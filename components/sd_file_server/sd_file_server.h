#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <esp_http_server.h>

#include "esphome/core/component.h"
#include "esphome/components/sd_spi_card/sd_spi_card.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/button/button.h"
#include "esphome/components/switch/switch.h"

namespace esphome::sd_file_server {

/// HTTP file service over the SD card log for the SAMBA Home app, docs/home-sync.md section 2:
/// `GET /sd` lists the card, `GET /sd/<name>` streams a log (open-ended Range supported),
/// `HEAD /sd/<name>` gives its size. `GET`/`POST /settings` reads and sets the switches named in
/// YAML, `POST /action/<key>` presses a button. Digest auth as user `samba`. The listener runs
/// only while a password is set; a fleet unit with none never opens port 80 outside the captive
/// portal.
class SdFileServer : public AsyncWebHandler, public Component {
 public:
  SdFileServer(web_server_base::WebServerBase *base, sd_spi_card::SdSpiCard *card);

  void setup() override;
  void dump_config() override;
  // after wifi has brought up the network stack, before the web OTA handler registers
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  /// At most this many settings and this many actions: each is one bit of a uint8_t.
  static constexpr size_t MAX_ENTRIES = 8;

  /// `key` is a string literal from codegen. Call before registration. While `available` returns
  /// false the setting reads off and turning it on is refused; it runs on the httpd task, so it
  /// must read only state that is safe there (a bool, not a std::string the loop may reassign).
  void add_setting(const char *key, switch_::Switch *sw, std::function<bool()> available = nullptr);
  void add_action(const char *key, button::Button *button);

  /// Sets the pairing password (12 to 32 printable ASCII, validated by the caller) and starts
  /// the listener; an empty string stops it. Safe to call before setup().
  void set_password(const std::string &password);
  bool is_running() const { return this->running_; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

 protected:
  void start_();
  void stop_();
  void register_head_();
  void handle_list_(httpd_req_t *req, bool head);
  void handle_file_(httpd_req_t *req, const char *name, bool head);
  void handle_settings_(httpd_req_t *req, http_method method);
  void handle_action_(httpd_req_t *req, http_method method, const char *key);
  void send_status_(httpd_req_t *req, const char *status, const char *retry_after = nullptr);
  static bool valid_name_(const char *name);
  void send_settings_(httpd_req_t *req, uint8_t set, uint8_t on);
  void apply_settings_(uint8_t set, uint8_t on);

  struct Setting {
    const char *key;
    switch_::Switch *sw;
    std::function<bool()> available;
  };
  bool available_(size_t i) const { return !this->settings_[i].available || this->settings_[i].available(); }
  std::array<Setting, MAX_ENTRIES> settings_{};
  uint8_t settings_count_{0};
  struct Action {
    const char *key;
    button::Button *button;
  };
  std::array<Action, MAX_ENTRIES> actions_{};
  uint8_t actions_count_{0};

  web_server_base::WebServerBase *base_;
  sd_spi_card::SdSpiCard *card_;
  char password_[33]{};  // the auth middleware holds a pointer to this, so it never moves
  bool running_{false};
  bool setup_done_{false};
  std::array<char, 1460> buf_{};  // one TCP segment; the only I/O buffer, static for life
};

}  // namespace esphome::sd_file_server
