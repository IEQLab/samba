#pragma once

#include <map>
#include <string>
#include <list>
#include <vector>
#include <unordered_map>

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/time/real_time_clock.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome {
namespace influxdb {

/**
 * @brief InfluxDB Component
 * 
 * Collects sensor data from ESPHome and sends it to InfluxDB v2 via HTTP API.
 * Supports automatic sensor discovery, custom tags, field names, and timestamping.
 */
class InfluxDB : public Component {
 public:
  // --- Component lifecycle ---
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // --- Public API ---
  void publish_now();

  // --- Configuration setters (called by Python codegen) ---
  void set_host(const std::string &host) { host_ = host; }
  void set_port(const std::string &port) { port_ = port; }
  void set_token(const std::string &token) { token_ = token; }
  void set_bucket(const std::string &bucket) { bucket_ = bucket; }
  void set_org(const std::string &org) { org_ = org; }
  void set_timestamp_unit(const std::string &unit) { timestamp_unit_ = unit; }
  void set_use_ssl(bool use_ssl) { use_ssl_ = use_ssl; }
  void set_send_mac(bool send_mac) { send_mac_ = send_mac; }
  void set_update_interval(uint32_t interval_ms) { 
    update_interval_ = interval_ms;
  }
  
  // --- Component dependencies ---
  void set_http_request(http_request::HttpRequestComponent *request) { http_request_ = request; }
  void set_time_source(time::RealTimeClock *time_source) { time_source_ = time_source; }

  // --- Sensor configuration ---
  void add_sensor_mapping(const std::string &sensor_id, const std::string &measurement_name);
  void add_static_tag(const std::string &sensor_id, const std::string &tag_key, const std::string &tag_value);
  void add_global_tag(const std::string &tag_key, const std::string &tag_value);
  void set_field_name(const std::string &sensor_id, const std::string &field_name);

 protected:
  // --- Configuration ---
  std::string host_;  // InfluxDB host
  std::string port_{"8086"};
  std::string token_;  // InfluxDB token
  std::string bucket_;  // InfluxDB bucket
  std::string org_;  // InfluxDB org
  std::string timestamp_unit_{"s"};
  bool use_ssl_{false};  // Use HTTPS if true
  bool send_mac_{false};  // Include MAC address tag
  uint32_t update_interval_{60000};  // 60 seconds default

  // --- ESP-IDF HTTP client implementation ---
  bool post_raw_idf_(const std::string &url,
                     const std::string &body,
                     const std::list<esphome::http_request::Header> &headers,
                     bool verify_ssl);
  
  // --- Runtime state ---
  std::string url_;
  std::string mac_address_;
  std::list<http_request::Header> headers_;
  uint32_t last_publish_{0};
  bool publish_in_progress_{false};
  
  // --- Component dependencies ---
  http_request::HttpRequestComponent *http_request_{nullptr};
  time::RealTimeClock *time_source_{nullptr};

  // --- Sensor management (removed unused sensor_names_) ---
  std::unordered_map<std::string, std::string> sensor_measurements_;  // sensor_id -> measurement_name
  std::unordered_map<std::string, std::string> field_names_;          // sensor_id -> field_name
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> static_tags_;   // sensor_id -> {tag_key -> tag_value}
  std::unordered_map<std::string, std::string> global_tags_;          // tag_key -> tag_value (applied to all measurements)

  // --- Sensor collections ---
  std::vector<sensor::Sensor *> sensors_;
  std::vector<text_sensor::TextSensor *> text_sensors_;
#ifdef USE_BINARY_SENSOR
  std::unordered_map<std::string, bool> binary_sensor_states_;
#endif

  // --- Helper methods ---
  bool validate_required_config_();
  void collect_sensors_();
  void build_url_();
  void setup_headers_();
  size_t estimate_payload_size_() const;
  
  std::string build_line_protocol_line_(const std::string &sensor_id, const std::string &value, bool is_string_value = false);
  std::string build_measurement_name_(const std::string &sensor_id) const;
  std::string build_tags_(const std::string &sensor_id) const;
  std::string build_fields_(const std::string &sensor_id, const std::string &value, bool is_string_value = false) const;
  std::string build_timestamp_() const;
  
  std::string escape_influx_key_(const std::string &input) const;
  std::string escape_influx_string_value_(const std::string &input) const;
  
  std::string get_field_name_(const std::string &sensor_id) const;
  bool has_sensor_mapping_(const std::string &sensor_id) const;
  bool should_publish_() const;
  
  // --- Constants ---
  static constexpr size_t BASE_LINE_SIZE = 64;  // Estimated base size per line
  static constexpr size_t MIN_BUFFER_SIZE = 256;
  static constexpr uint32_t BASE_BACKOFF_MS = 500;
  static constexpr uint32_t BACKOFF_RANGE_MS = 1500;
};

}  // namespace influxdb
}  // namespace esphome