#include "influxdb.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

#include <cmath>
#include <string>
#include <list>
#include <cstring>  // for strcasecmp

#include "esp_http_client.h"
#include "esp_crt_bundle.h"

namespace esphome {
namespace influxdb {

static const char *const TAG = "influxdb";

void InfluxDB::setup() {
  ESP_LOGCONFIG(TAG, "Setting up InfluxDB");
  
  // Consolidated validation
  if (!this->validate_required_config_()) {
    this->mark_failed();
    return;
  }
  
  if (this->http_request_ == nullptr) {
    ESP_LOGE(TAG, "HTTP request component is required");
    this->mark_failed();
    return;
  }
  
  this->build_url_();
  this->setup_headers_();
  this->collect_sensors_();
  
  if (this->send_mac_) {
    this->mac_address_ = get_mac_address();
    ESP_LOGD(TAG, "MAC address: %s", this->mac_address_.c_str());
  }
  
  ESP_LOGI(TAG, "InfluxDB setup complete");
}

bool InfluxDB::validate_required_config_() {
  const std::vector<std::pair<const std::string*, const char*>> required = {
    {&this->host_, "Host"},
    {&this->token_, "Token"},
    {&this->bucket_, "Bucket"},
    {&this->org_, "Organization"}
  };
  
  for (const auto& [field, name] : required) {
    if (field->empty()) {
      ESP_LOGE(TAG, "%s is required", name);
      return false;
    }
  }
  return true;
}

// --- ESP-IDF HTTP POST with full compliance ---
bool InfluxDB::post_raw_idf_(const std::string &url,
                             const std::string &body,
                             const std::list<esphome::http_request::Header> &headers,
                             bool verify_ssl) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.keep_alive_enable = false;     // force short-lived connection
  cfg.timeout_ms = 12000;            // 12s timeout
  if (verify_ssl) {
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // use IDF cert bundle
  }
  
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    return false;
  }
  
  // Apply headers from ESPHome list
  bool has_conn_close = false;
  bool has_content_type = false;
  for (const auto &h : headers) {
    esp_http_client_set_header(client, h.name.c_str(), h.value.c_str());
    if (!has_conn_close &&
        strcasecmp(h.name.c_str(), "Connection") == 0 &&
        strcasecmp(h.value.c_str(), "close") == 0) {
      has_conn_close = true;
    }
    if (!has_content_type &&
        strcasecmp(h.name.c_str(), "Content-Type") == 0) {
      has_content_type = true;
    }
  }
  if (!has_conn_close) {
    esp_http_client_set_header(client, "Connection", "close");
  }
  if (!has_content_type) {
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
  }
  
  // Open stream and write body
  esp_err_t err = esp_http_client_open(client, body.size());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  
  int written = esp_http_client_write(client, body.data(), body.size());
  if (written < 0 || static_cast<size_t>(written) != body.size()) {
    ESP_LOGE(TAG, "HTTP write failed: %d", written);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  
  // Fetch headers / status, then drain any response body
  (void) esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  ESP_LOGD(TAG, "HTTP status: %d", status);
  
  // Drain response body as per ESP-IDF documentation
  char tmp[256];
  while (true) {
    int r = esp_http_client_read(client, tmp, sizeof(tmp));
    if (r <= 0) break;
  }
  
  esp_http_client_close(client);
  esp_http_client_cleanup(client);   // frees TLS/HTTP buffers immediately
  
  return (status >= 200 && status < 300);
}

void InfluxDB::loop() {
  if (this->should_publish_()) {
    this->publish_now();
  }
}

void InfluxDB::build_url_() {
  // Validation already done in setup, just build
  this->url_ = (this->use_ssl_ ? "https://" : "http://");
  this->url_ += this->host_ + ":" + this->port_;
  this->url_ += "/api/v2/write";
  this->url_ += "?org=" + this->org_;
  this->url_ += "&bucket=" + this->bucket_;
  this->url_ += "&precision=" + this->timestamp_unit_;
  
  ESP_LOGD(TAG, "Built URL: %s", this->url_.c_str());
}

void InfluxDB::setup_headers_() {
  this->headers_.clear();
  
  // Token already validated in setup
  std::string auth_header = "Token " + this->token_;
  
  this->headers_.emplace_back("Content-Type", "text/plain; charset=utf-8");
  this->headers_.emplace_back("Authorization", std::move(auth_header));
  this->headers_.emplace_back("Connection", "close");
  
  ESP_LOGD(TAG, "Headers configured");
}

void InfluxDB::collect_sensors_() {
  // Collect regular sensors
  for (auto *sensor : App.get_sensors()) {
    if (sensor == nullptr) continue;
    
    const std::string &obj_id = sensor->get_object_id();
    
    if (this->has_sensor_mapping_(obj_id)) {
      this->sensors_.push_back(sensor);
      ESP_LOGD(TAG, "Added sensor: %s", obj_id.c_str());
    } else {
      ESP_LOGVV(TAG, "No mapping for sensor: %s", obj_id.c_str());
    }
  }
  
  // Collect text sensors
  for (auto *text_sensor : App.get_text_sensors()) {
    if (text_sensor == nullptr) continue;
    const std::string &obj_id = text_sensor->get_object_id();
    if (this->has_sensor_mapping_(obj_id)) {
      this->text_sensors_.push_back(text_sensor);
      ESP_LOGD(TAG, "Added text sensor: %s", obj_id.c_str());
    }
  }
  
#ifdef USE_BINARY_SENSOR
  // Collect binary sensors with state tracking
  for (auto *binary_sensor : App.get_binary_sensors()) {
    if (binary_sensor == nullptr) continue;
    const std::string &obj_id = binary_sensor->get_object_id();
    if (this->has_sensor_mapping_(obj_id)) {
      this->binary_sensor_states_[obj_id] = binary_sensor->state;
      binary_sensor->add_on_state_callback([this, obj_id](bool state) {
        this->binary_sensor_states_[obj_id] = state;
      });
      ESP_LOGD(TAG, "Added binary sensor: %s", obj_id.c_str());
    }
  }
#endif
  
  ESP_LOGI(TAG, "Collected %zu sensors, %zu text sensors, %zu binary sensors",
           this->sensors_.size(), this->text_sensors_.size(),
#ifdef USE_BINARY_SENSOR
           this->binary_sensor_states_.size()
#else
           (size_t)0
#endif
  );
}

bool InfluxDB::should_publish_() const {
  // Don't publish if component failed, publish in progress, or interval set to "never"
  if (this->is_failed() || this->publish_in_progress_) {
    return false;
  }
  
  // "Never" (UINT32_MAX) or invalid (0)
  if (this->update_interval_ == UINT32_MAX || this->update_interval_ == 0) {
    return false;
  }
  
  uint32_t now = millis();
  return (now - this->last_publish_) >= this->update_interval_;
}

size_t InfluxDB::estimate_payload_size_() const {
  size_t estimated_size = 0;
  
  // Calculate based on actual sensor count
  estimated_size += this->sensors_.size() * BASE_LINE_SIZE;
  estimated_size += this->text_sensors_.size() * BASE_LINE_SIZE;
#ifdef USE_BINARY_SENSOR
  estimated_size += this->binary_sensor_states_.size() * BASE_LINE_SIZE;
#endif
  
  // Add overhead for tags
  size_t tag_overhead = this->global_tags_.size() * 32;  // rough estimate per tag
  estimated_size += tag_overhead * (this->sensors_.size() + this->text_sensors_.size());
  
  return std::max(estimated_size, MIN_BUFFER_SIZE);
}

void InfluxDB::publish_now() {
  if (this->is_failed() || this->publish_in_progress_) {
    ESP_LOGW(TAG, "Cannot publish: component failed or publish in progress");
    return;
  }
  
  // Optimized payload building with better size estimation
  const size_t estimated_size = this->estimate_payload_size_();
  std::string body;
  body.reserve(estimated_size);
  size_t data_points = 0;
  
  // Build payload efficiently - no preflight checks
  for (auto *sensor : this->sensors_) {
    if (!std::isnan(sensor->state)) {
      const std::string &sensor_id = sensor->get_object_id();
      body += this->build_line_protocol_line_(sensor_id, to_string(sensor->state));
      data_points++;
    }
  }
  
  for (auto *text_sensor : this->text_sensors_) {
    if (!text_sensor->state.empty()) {
      const std::string &sensor_id = text_sensor->get_object_id();
      body += this->build_line_protocol_line_(sensor_id, text_sensor->state, true);
      data_points++;
    }
  }
  
#ifdef USE_BINARY_SENSOR
  for (const auto &pair : this->binary_sensor_states_) {
    body += this->build_line_protocol_line_(pair.first, std::to_string(pair.second ? 1 : 0));
    data_points++;
  }
#endif
  
  if (data_points == 0) {
    ESP_LOGD(TAG, "No valid sensor data to publish");
    return;
  }
  
  // Shrink buffer to actual size
  body.shrink_to_fit();
  
  ESP_LOGI(TAG, "Publishing %zu data points to InfluxDB", data_points);
  ESP_LOGVV(TAG, "Request body length: %u bytes", (unsigned) body.size());
  
  this->publish_in_progress_ = true;
  this->last_publish_ = millis();
  
  // One-shot IDF client (verify SSL if URL is https)
  const bool verify_ssl = this->url_.rfind("https://", 0) == 0;
  
  // Retry logic with exponential backoff
  int attempts = 0, max_retries = 2;
  bool ok = false;
  do {
    ok = this->post_raw_idf_(this->url_, body, this->headers_, verify_ssl);
    if (!ok && attempts < max_retries) {
      uint32_t backoff = BASE_BACKOFF_MS + (esp_random() % BACKOFF_RANGE_MS);
      ESP_LOGW(TAG, "POST failed, retrying in %u ms (attempt %d/%d)",
               backoff, attempts + 1, max_retries);
      delay(backoff);
    }
  } while (!ok && ++attempts <= max_retries);
  
  // Free the request body's capacity immediately
  std::string().swap(body);
  
  if (!ok) {
    ESP_LOGW(TAG, "InfluxDB POST failed after %d attempts", max_retries + 1);
  } else {
    ESP_LOGD(TAG, "Successfully published to InfluxDB");
  }
  
  this->publish_in_progress_ = false;
}

std::string InfluxDB::build_line_protocol_line_(const std::string &sensor_id,
                                                const std::string &value,
                                                bool is_string_value) {
  std::string line;
  line.reserve(128);  // Pre-allocate reasonable size
  
  line += this->build_measurement_name_(sensor_id);
  line += this->build_tags_(sensor_id);
  line += " ";
  line += this->build_fields_(sensor_id, value, is_string_value);
  line += this->build_timestamp_();
  line += "\n";
  
  return line;
}

std::string InfluxDB::build_measurement_name_(const std::string &sensor_id) const {
  auto it = this->sensor_measurements_.find(sensor_id);
  const std::string &measurement = (it != this->sensor_measurements_.end()) ? it->second : sensor_id;
  return this->escape_influx_key_(measurement);
}

std::string InfluxDB::build_tags_(const std::string &sensor_id) const {
  std::string tags;
  tags.reserve(128);  // Pre-allocate for typical tag size
  
  // Global tags
  for (const auto &pair : this->global_tags_) {
    tags += "," + this->escape_influx_key_(pair.first) + "=" + this->escape_influx_key_(pair.second);
  }
  
  // Static tags for this sensor
  auto static_it = this->static_tags_.find(sensor_id);
  if (static_it != this->static_tags_.end()) {
    for (const auto &pair : static_it->second) {
      tags += "," + this->escape_influx_key_(pair.first) + "=" + this->escape_influx_key_(pair.second);
    }
  }
  
  // MAC address tag (optional)
  if (this->send_mac_ && !this->mac_address_.empty()) {
    tags += ",device=" + this->escape_influx_key_(this->mac_address_);
  }
  
  return tags;
}

std::string InfluxDB::build_fields_(const std::string &sensor_id,
                                    const std::string &value,
                                    bool is_string_value) const {
  const std::string &field_name = this->get_field_name_(sensor_id);
  std::string field_value = is_string_value
  ? "\"" + this->escape_influx_string_value_(value) + "\""
  : value;
  return field_name + "=" + std::move(field_value);
}

std::string InfluxDB::build_timestamp_() const {
  if (this->time_source_ != nullptr) {
    auto now = this->time_source_->now();
    if (now.is_valid()) {
      return std::string(" ") + to_string(now.timestamp);
    }
  }
  return "";  // InfluxDB will use server timestamp if not provided
}

std::string InfluxDB::escape_influx_key_(const std::string &input) const {
  std::string output;
  output.reserve(static_cast<size_t>(input.length() * 1.2f));
  for (char c : input) {
    if (c == ' ' || c == ',' || c == '=') output += '\\';
    output += c;
  }
  return output;
}

std::string InfluxDB::escape_influx_string_value_(const std::string &input) const {
  std::string output;
  output.reserve(static_cast<size_t>(input.length() * 1.2f));
  for (char c : input) {
    if (c == '"' || c == '\\') output += '\\';
    output += c;
  }
  return output;
}

std::string InfluxDB::get_field_name_(const std::string &sensor_id) const {
  auto it = this->field_names_.find(sensor_id);
  return (it != this->field_names_.end()) ? it->second : "value";
}

bool InfluxDB::has_sensor_mapping_(const std::string &sensor_id) const {
  return this->sensor_measurements_.find(sensor_id) != this->sensor_measurements_.end();
}

// --- Configuration methods ---

void InfluxDB::add_sensor_mapping(const std::string &sensor_id,
                                  const std::string &measurement_name) {
  this->sensor_measurements_[sensor_id] = measurement_name;
}

void InfluxDB::add_static_tag(const std::string &sensor_id,
                              const std::string &tag_key,
                              const std::string &tag_value) {
  this->static_tags_[sensor_id][tag_key] = tag_value;
}

void InfluxDB::add_global_tag(const std::string &tag_key,
                              const std::string &tag_value) {
  this->global_tags_[tag_key] = tag_value;
}

void InfluxDB::set_field_name(const std::string &sensor_id,
                              const std::string &field_name) {
  this->field_names_[sensor_id] = field_name;
}

void InfluxDB::dump_config() {
  ESP_LOGCONFIG(TAG, "InfluxDB:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Organization: %s", this->org_.c_str());
  ESP_LOGCONFIG(TAG, "  Bucket: %s", this->bucket_.c_str());
  ESP_LOGCONFIG(TAG, "  Timestamp Unit: %s", this->timestamp_unit_.c_str());
  if (this->update_interval_ == UINT32_MAX) {
    ESP_LOGCONFIG(TAG, "  Update Interval: never (manual only)");
  } else {
    ESP_LOGCONFIG(TAG, "  Update Interval: %u ms", this->update_interval_);
  }
  ESP_LOGCONFIG(TAG, "  SSL: %s", this->use_ssl_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Send MAC: %s", this->send_mac_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Configured sensors: %zu", this->sensor_measurements_.size());
  
  if (this->time_source_ == nullptr) {
    ESP_LOGCONFIG(TAG, "  Time source: Not configured (server timestamp will be used)");
  }
}

}  // namespace influxdb
}  // namespace esphome