#include "influxdb.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include <cmath>
#include <sstream>

namespace esphome {
namespace influxdb {

static const char *const TAG = "influxdb";

void InfluxDB::setup() {
  ESP_LOGCONFIG(TAG, "Setting up InfluxDB");

  // Validate required configuration
  if (this->host_.empty()) {
    ESP_LOGE(TAG, "Host is required");
    this->mark_failed();
    return;
  }
  if (this->token_.empty()) {
    ESP_LOGE(TAG, "Token is required");
    this->mark_failed();
    return;
  }
  if (this->bucket_.empty()) {
    ESP_LOGE(TAG, "Bucket is required");
    this->mark_failed();
    return;
  }
  if (this->org_.empty()) {
    ESP_LOGE(TAG, "Organization is required");
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

void InfluxDB::loop() {
  if (this->should_publish_()) {
    this->publish_now();
  }
}

void InfluxDB::build_url_() {
  // Validate required components
  if (this->host_.empty() || this->org_.empty() || this->bucket_.empty()) {
    ESP_LOGE(TAG, "Cannot build URL: missing host, org, or bucket");
    this->url_ = "";
    return;
  }
  
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
  
  // Validate token before using it
  if (this->token_.empty()) {
    ESP_LOGE(TAG, "Token is empty, cannot set Authorization header");
    return;
  }
  
  // Build headers with validation
  std::string content_type = "text/plain; charset=utf-8";
  std::string auth_header = "Token " + this->token_;
  
  // Validate header values
  if (content_type.empty() || auth_header.empty() || auth_header == "Token ") {
    ESP_LOGE(TAG, "Invalid header values");
    return;
  }
  
  this->headers_.emplace_back("Content-Type", content_type);
  this->headers_.emplace_back("Authorization", auth_header);
  
  // Log headers for debugging
  ESP_LOGD(TAG, "Setting up headers:");
  for (const auto& header : this->headers_) {
    ESP_LOGD(TAG, "  %s: %s", header.name.c_str(), 
             header.name == "Authorization" ? "[REDACTED]" : header.value.c_str());
  }
}

void InfluxDB::collect_sensors_() {
  ESP_LOGD(TAG, "Configured sensor names:");
  
  // Collect regular sensors
  for (auto *sensor : App.get_sensors()) {
    if (sensor == nullptr) continue;
    
    std::string obj_id = sensor->get_object_id();
    std::string name = sensor->get_name();
    
    ESP_LOGD(TAG, "[detected] object_id=%s, name=%s", obj_id.c_str(), name.c_str());
    
    if (this->has_sensor_mapping_(obj_id)) {
      ESP_LOGD(TAG, "Matched to sensor_names");
      this->sensors_.push_back(sensor);
    } else {
      ESP_LOGW(TAG, "No match for object_id=%s", obj_id.c_str());
    }
  }
  
  // Collect text sensors
  for (auto *text_sensor : App.get_text_sensors()) {
    if (text_sensor == nullptr) continue;
    std::string obj_id = text_sensor->get_object_id();
    ESP_LOGD(TAG, "[text_sensor] object_id=%s", obj_id.c_str());
    if (this->has_sensor_mapping_(obj_id)) {
      this->text_sensors_.push_back(text_sensor);
    }
  }
  
#ifdef USE_BINARY_SENSOR
  // Collect binary sensors with state tracking
  for (auto *binary_sensor : App.get_binary_sensors()) {
    if (binary_sensor == nullptr) continue;
    std::string obj_id = binary_sensor->get_object_id();
    ESP_LOGD(TAG, "[binary_sensor] object_id=%s", obj_id.c_str());
    if (this->has_sensor_mapping_(obj_id)) {
      this->binary_sensor_states_[obj_id] = binary_sensor->state;
      binary_sensor->add_on_state_callback([this, obj_id](bool state) {
        this->binary_sensor_states_[obj_id] = state;
      });
    }
  }
#endif
  
  ESP_LOGI(TAG, "Final count: %zu sensors, %zu text sensors, %zu binary sensors",
           this->sensors_.size(), this->text_sensors_.size(),
#ifdef USE_BINARY_SENSOR
           this->binary_sensor_states_.size()
#else
             0
#endif
  );
}

bool InfluxDB::should_publish_() const {
  // Don't publish if component failed, publish in progress, or interval set to "never"
  if (this->is_failed() || this->publish_in_progress_) {
    return false;
  }
  
  // Check if update_interval is set to "never" (UINT32_MAX in ESPHome)
  if (this->update_interval_ == UINT32_MAX) {
    return false;  // Never auto-publish when interval is "never"
  }
  
  // Don't publish if interval is 0 (should update every loop, but we'll skip for safety)
  if (this->update_interval_ == 0) {
    return false;
  }
  
  uint32_t now = millis();
  return (now - this->last_publish_) >= this->update_interval_;
}

void InfluxDB::publish_now() {
  if (this->is_failed() || this->publish_in_progress_) {
    ESP_LOGW(TAG, "Cannot publish: component failed or publish in progress");
    return;
  }
  
  // DEBUG LOGGING
  ESP_LOGD(TAG, "Evaluating sensors before upload:");
  for (auto *s : this->sensors_) {
    if (s != nullptr) {
      ESP_LOGD(TAG, "  [%s] has_state = %s, state = %s",
               s->get_object_id().c_str(),
               s->has_state() ? "true" : "false",
               std::isnan(s->state) ? "NaN" : to_string(s->state).c_str());
    } else {
      ESP_LOGW(TAG, "  Null sensor pointer encountered");
    }
  }
  
  std::string body;
  size_t data_points = 0;

  // Process regular sensors
  for (auto *sensor : this->sensors_) {
    if (!std::isnan(sensor->state)) {
      std::string sensor_id = sensor->get_object_id();
      body += this->build_line_protocol_line_(sensor_id, to_string(sensor->state));
      data_points++;
    }
  }

  // Process text sensors
  for (auto *text_sensor : this->text_sensors_) {
    if (!text_sensor->state.empty()) {
      std::string sensor_id = text_sensor->get_object_id();
      body += this->build_line_protocol_line_(sensor_id, text_sensor->state, true);
      data_points++;
    }
  }

#ifdef USE_BINARY_SENSOR
  // Process binary sensors
  for (const auto &pair : this->binary_sensor_states_) {
    body += this->build_line_protocol_line_(pair.first, std::to_string(pair.second ? 1 : 0));
    data_points++;
  }
#endif

  if (data_points == 0) {
    ESP_LOGD(TAG, "No valid sensor data to publish");
    return;
  }

  ESP_LOGI(TAG, "Publishing %zu data points to InfluxDB", data_points);
  
  ESP_LOGD(TAG, "Line protocol payload (%d data points):", data_points);
  std::istringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    ESP_LOGD(TAG, "  %s", line.c_str());
  }

  ESP_LOGD(TAG, "URL: %s", this->url_.c_str());
  ESP_LOGVV(TAG, "Request body:\n%s", body.c_str());

  // Validate headers before sending
  if (this->headers_.empty()) {
    ESP_LOGE(TAG, "Headers not set up properly");
    return;
  }

  // Validate URL
  if (this->url_.empty()) {
    ESP_LOGE(TAG, "URL is empty");
    return;
  }

  // Set publish state and timestamp
  this->publish_in_progress_ = true;
  this->last_publish_ = millis();

  // Send HTTP request - ESPHome handles the response internally
  // No exception handling needed as ESP-IDF doesn't use exceptions
  this->http_request_->post(this->url_, body, this->headers_);
  
  // Reset the publish flag immediately since we can't track completion
  this->publish_in_progress_ = false;
}

std::string InfluxDB::build_line_protocol_line_(const std::string &sensor_id, const std::string &value, bool is_string_value) {
  std::string line;
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
  std::string measurement = (it != this->sensor_measurements_.end()) ? it->second : sensor_id;
  return this->escape_influx_key_(measurement);
}

std::string InfluxDB::build_tags_(const std::string &sensor_id) const {
  std::string tags;
  
  // Add global tags first (applied to all measurements)
  for (const auto &pair : this->global_tags_) {
    tags += "," + this->escape_influx_key_(pair.first) + "=" + this->escape_influx_key_(pair.second);
  }
  
  // Get static tags for this specific sensor
  auto static_it = this->static_tags_.find(sensor_id);
  if (static_it != this->static_tags_.end()) {
    for (const auto &pair : static_it->second) {
      tags += "," + this->escape_influx_key_(pair.first) + "=" + this->escape_influx_key_(pair.second);
    }
  }

  // Add MAC address tag if enabled
  if (this->send_mac_ && !this->mac_address_.empty()) {
    tags += ",device=" + this->escape_influx_key_(this->mac_address_);
  }

  return tags;
}

std::string InfluxDB::build_fields_(const std::string &sensor_id, const std::string &value, bool is_string_value) const {
  std::string field_name = this->get_field_name_(sensor_id);
  std::string field_value = is_string_value ? 
    "\"" + this->escape_influx_string_value_(value) + "\"" : 
    value;
  
  return field_name + "=" + field_value;
}

std::string InfluxDB::build_timestamp_() const {
  if (this->time_source_ != nullptr) {
    auto now = this->time_source_->now();
    if (now.is_valid()) {
      return " " + to_string(now.timestamp);
    }
  }
  return "";  // InfluxDB will use server timestamp if not provided
}

std::string InfluxDB::escape_influx_key_(const std::string &input) const {
  std::string output;
  output.reserve(input.length() * 1.2);  // Reserve some extra space for escaping
  
  for (char c : input) {
    if (c == ' ' || c == ',' || c == '=') {
      output += '\\';
    }
    output += c;
  }
  return output;
}

std::string InfluxDB::escape_influx_string_value_(const std::string &input) const {
  std::string output;
  output.reserve(input.length() * 1.2);
  
  for (char c : input) {
    if (c == '"' || c == '\\') {
      output += '\\';
    }
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

void InfluxDB::add_sensor_mapping(const std::string &sensor_id, const std::string &measurement_name) {
  this->sensor_measurements_[sensor_id] = measurement_name;
}

void InfluxDB::add_static_tag(const std::string &sensor_id, const std::string &tag_key, const std::string &tag_value) {
  this->static_tags_[sensor_id][tag_key] = tag_value;
}

void InfluxDB::add_global_tag(const std::string &tag_key, const std::string &tag_value) {
  this->global_tags_[tag_key] = tag_value;
}

void InfluxDB::set_field_name(const std::string &sensor_id, const std::string &field_name) {
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