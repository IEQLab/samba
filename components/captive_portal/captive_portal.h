#pragma once
// Local copy of ESPHome 2026.11.0 components/captive_portal, changed only to add WPA2-Enterprise (PEAP/TTLS with a
// username and password) to the portal form. Re-diff against upstream on every ESPHome upgrade.
#include "esphome/core/defines.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/web_server_base/captive_dns.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome::captive_portal {

/// Enterprise half of a portal-saved network. WiFi's own record keeps the SSID with an empty password; this one
/// holds the rest under a fixed key, so it survives OTA the way WiFi's record does when no network is in YAML.
struct SavedEapSettings {
  char ssid[33];
  char username[65];
  char password[65];
  uint8_t ttls_phase_2;
} PACKED;  // NOLINT

class CaptivePortal final : public AsyncWebHandler, public Component, public wifi::WiFiConnectStateListener {
 public:
  CaptivePortal(web_server_base::WebServerBase *base);
  void setup() override;
  void dump_config() override;
  void loop() override { this->dns_.loop(); }
  float get_setup_priority() const override;
  void start();
  bool is_active() const { return this->active_; }
  void end() {
    this->active_ = false;
    this->disable_loop();  // Stop processing DNS requests
    this->base_->deinit();
    this->dns_.stop();
  }

  bool canHandle(AsyncWebServerRequest *request) const override {
    // Handle all GET requests when captive portal is active
    // This allows us to respond with the portal page for any URL,
    // triggering OS captive portal detection
    return this->active_ && request->method() == HTTP_GET;
  }

  void handle_config(AsyncWebServerRequest *request);

  void handle_wifisave(AsyncWebServerRequest *request);

  void handleRequest(AsyncWebServerRequest *req) override;

  void on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) override;

 protected:
  void apply_eap_();

  web_server_base::WebServerBase *base_;
  bool initialized_{false};
  bool active_{false};
  ESPPreferenceObject eap_pref_;
  web_server_base::CaptiveDNS dns_;
};

extern CaptivePortal *global_captive_portal;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::captive_portal

#endif
