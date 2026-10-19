#include "captive_portal.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/wifi/scan_list.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_PROVISIONING
#include "esphome/components/provisioning/provisioning.h"
#endif
#include "captive_index.h"

namespace esphome::captive_portal {

static const char *const TAG = "captive_portal";

void CaptivePortal::handle_config(AsyncWebServerRequest *request) {
  AsyncResponseStream *stream = request->beginResponseStream(ESPHOME_F("application/json"));
  stream->addHeader(ESPHOME_F("cache-control"), ESPHOME_F("public, max-age=0, must-revalidate"));
  char mac_s[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  const char *mac_str = get_mac_address_pretty_into_buffer(mac_s);
#ifdef USE_ESP8266
  stream->print(ESPHOME_F("{\"mac\":\""));
  stream->print(mac_str);
  stream->print(ESPHOME_F("\",\"name\":\""));
  stream->print(App.get_name().c_str());
  stream->print(ESPHOME_F("\",\"aps\":[{}"));
#else
  stream->printf(R"({"mac":"%s","name":"%s","aps":[{})", mac_str, App.get_name().c_str());
#endif

  // An SSID can contain a " or \ that would break the JSON, so escape it before writing it out. An SSID is at most
  // 32 bytes (IEEE 802.11), so this is large enough that nothing is ever dropped. Reused for every scan result.
  char escaped_ssid[32 * JSON_ESCAPE_MAX_EXPANSION + 1];
  {
    // Invariant: only bounded in-memory work under the lock; the network send
    // happens later in request->send()
    wifi::ScanResultsLock lock(wifi::global_wifi_component);
    const auto &results = wifi::global_wifi_component->get_scan_result();
    for (const auto &scan : results) {
      bool with_auth = false;
      if (!wifi::should_show_scan_entry(results, scan, with_auth))
        continue;

      json_escape_into_buffer(escaped_ssid, scan.get_ssid());
#ifdef USE_ESP8266
      stream->print(ESPHOME_F(",{\"ssid\":\""));
      stream->print(escaped_ssid);
      stream->print(ESPHOME_F("\",\"rssi\":"));
      stream->print(scan.get_rssi());
      stream->print(ESPHOME_F(",\"lock\":"));
      stream->print(with_auth);
      stream->print(ESPHOME_F("}"));
#else
      stream->printf(R"(,{"ssid":"%s","rssi":%d,"lock":%d})", escaped_ssid, scan.get_rssi(), with_auth);
#endif
    }
  }
  stream->print(ESPHOME_F("]}"));
  request->send(stream);
}

// Form value to the TTLS phase 2 method; unknown values fall back to MSCHAPv2, ESP-IDF's own default
static esp_eap_ttls_phase2_types parse_ttls_phase_2(const std::string &value) {
  if (value == "pap")
    return ESP_EAP_TTLS_PHASE2_PAP;
  if (value == "chap")
    return ESP_EAP_TTLS_PHASE2_CHAP;
  if (value == "mschap")
    return ESP_EAP_TTLS_PHASE2_MSCHAP;
  if (value == "eap")
    return ESP_EAP_TTLS_PHASE2_EAP;
  return ESP_EAP_TTLS_PHASE2_MSCHAPV2;
}

void CaptivePortal::handle_wifisave(AsyncWebServerRequest *request) {
  const auto &ssid = request->arg("ssid");
  const auto &psk = request->arg("psk");
  // A username marks the network as enterprise; the form sends none for a PSK network
  const auto &username = request->arg("username");
  const auto ttls_phase_2 = parse_ttls_phase_2(request->arg("ttls"));
  const bool enterprise = !username.empty();
  if (enterprise && (username.size() >= sizeof(SavedEapSettings::username) ||
                     psk.size() >= sizeof(SavedEapSettings::password))) {
    request->send(400, ESPHOME_F("text/plain"), ESPHOME_F("Username and password are limited to 64 characters."));
    return;
  }
  ESP_LOGI(TAG,
           "Requested WiFi Settings Change:\n"
           "  SSID='%s'\n"
           "  Username=" LOG_SECRET("'%s'") "\n"
           "  Password=" LOG_SECRET("'%s'"),
           ssid.c_str(), username.c_str(), psk.c_str());
  // Defer save to main loop thread to avoid NVS operations from HTTP thread
  this->defer([this, ssid, psk, username, ttls_phase_2, enterprise]() {
    // Written for a PSK network too, so a previous enterprise record cannot reattach to it
    SavedEapSettings eap{};
    if (enterprise) {
      strncpy(eap.ssid, ssid.c_str(), sizeof(eap.ssid) - 1);
      strncpy(eap.username, username.c_str(), sizeof(eap.username) - 1);
      strncpy(eap.password, psk.c_str(), sizeof(eap.password) - 1);
      eap.ttls_phase_2 = ttls_phase_2;
    }
    this->eap_pref_.save(&eap);
    // Syncs both records to flash
    wifi::global_wifi_component->save_wifi_sta(ssid.c_str(), enterprise ? "" : psk.c_str());
    this->apply_eap_();
  });
  request->send(200, ESPHOME_F("text/plain"), ESPHOME_F("Saved. Connecting..."));
}

void CaptivePortal::apply_eap_() {
  SavedEapSettings save{};
  if (!this->eap_pref_.load(&save) || save.ssid[0] == '\0')
    return;
  wifi::WiFiAP sta = wifi::global_wifi_component->get_sta();
  if (strcmp(sta.get_ssid().c_str(), save.ssid) != 0)
    return;
  wifi::EAPAuth eap{};
  eap.identity = save.username;
  eap.username = save.username;
  eap.password = save.password;
  // The connect path calls strlen() on all three, so they must be empty strings, not null
  eap.ca_cert = "";
  eap.client_cert = "";
  eap.client_key = "";
  eap.ttls_phase_2 = static_cast<esp_eap_ttls_phase2_types>(save.ttls_phase_2);
  sta.set_eap(eap);
  wifi::global_wifi_component->set_sta(sta);
  ESP_LOGI(TAG, "Enterprise credentials applied to " LOG_SECRET("'%s'"), save.ssid);
}

void CaptivePortal::on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) {
  // Empty SSID is a disconnect
  wifi::global_wifi_component->set_keep_scan_results(ssid.empty());
}

void CaptivePortal::setup() {
  // Disable loop by default - will be enabled when captive portal starts
  this->disable_loop();
  this->eap_pref_ = global_preferences->make_preference<SavedEapSettings>(fnv1_hash("captive_portal_eap"), true);
  // Scans are filtered to the saved SSID unless results are wanted, so a unit whose saved network is out of range
  // opened the portal with an empty list until a full scan 2-3 min later. Keep every scan whole while offline.
  wifi::global_wifi_component->set_keep_scan_results(true);
  wifi::global_wifi_component->add_connect_state_listener(this);
  // WiFi loads its saved network in its own setup, after this one, and connects only after a scan
  this->defer([this]() { this->apply_eap_(); });
#ifdef USE_PROVISIONING
  // The captive portal is a provisioning surface: once the provisioning window
  // has closed, stop serving it. WiFi's own closed-callback shuts down the
  // access point the portal runs on, and the gated fallback in WiFiComponent's
  // loop() ensures neither is started again afterwards.
  if (provisioning::global_provisioning_manager != nullptr) {
    provisioning::global_provisioning_manager->add_on_closed_callback([this]() {
      if (this->active_) {
        ESP_LOGD(TAG, "Provisioning window closed; stopping captive portal");
        this->end();
      }
    });
  }
#endif
}
void CaptivePortal::start() {
  this->base_->init();
  if (!this->initialized_) {
    this->base_->add_handler_without_auth(this);
  }

  network::IPAddress ip = wifi::global_wifi_component->wifi_soft_ap_ip();

#if defined(USE_ESP32)
  // Create DNS server instance for ESP-IDF
  this->dns_server_ = make_unique<DNSServer>();
  this->dns_server_->start(ip);
#elif defined(USE_ARDUINO)
  this->dns_server_ = make_unique<DNSServer>();
  this->dns_server_->setErrorReplyCode(DNSReplyCode::NoError);
  this->dns_server_->start(53, ESPHOME_F("*"), ip);
#endif

  this->initialized_ = true;
  this->active_ = true;

  // Enable loop() now that captive portal is active
  this->enable_loop();

  ESP_LOGV(TAG, "Captive portal started");
}

void CaptivePortal::handleRequest(AsyncWebServerRequest *req) {
#ifdef USE_ESP32
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = req->url_to(url_buf);
#else
  const auto &url = req->url();
#endif
  if (url == ESPHOME_F("/config.json")) {
    this->handle_config(req);
    return;
  } else if (url == ESPHOME_F("/wifisave")) {
    this->handle_wifisave(req);
    return;
  }

  // All other requests get the captive portal page
  // This includes OS captive portal detection endpoints which will trigger
  // the captive portal when they don't receive their expected responses
#ifndef USE_ESP8266
  auto *response = req->beginResponse(200, ESPHOME_F("text/html"), INDEX_GZ, sizeof(INDEX_GZ));
#else
  auto *response = req->beginResponse_P(200, ESPHOME_F("text/html"), INDEX_GZ, sizeof(INDEX_GZ));
#endif
#ifdef USE_CAPTIVE_PORTAL_GZIP
  response->addHeader(ESPHOME_F("Content-Encoding"), ESPHOME_F("gzip"));
#else
  response->addHeader(ESPHOME_F("Content-Encoding"), ESPHOME_F("br"));
#endif
  req->send(response);
}

CaptivePortal::CaptivePortal(web_server_base::WebServerBase *base) : base_(base) { global_captive_portal = this; }
float CaptivePortal::get_setup_priority() const {
  // Before WiFi
  return setup_priority::WIFI + 1.0f;
}
void CaptivePortal::dump_config() { ESP_LOGCONFIG(TAG, "Captive Portal:"); }

CaptivePortal *global_captive_portal = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::captive_portal

#endif
