#include "influxdb.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

#include <cmath>
#include <string>
#include <list>
#include <cstring>  // for strcasecmp
#include <cinttypes>

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
  this->validate_field_uniqueness_();
  this->validate_tag_consistency_();

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

void InfluxDB::validate_field_uniqueness_() {
  // Check that no two sensors in the same measurement share a field name
  // Uses simple nested loop — only runs once at setup with ~15 sensors
  for (auto it1 = this->sensor_measurements_.begin(); it1 != this->sensor_measurements_.end(); ++it1) {
    for (auto it2 = std::next(it1); it2 != this->sensor_measurements_.end(); ++it2) {
      if (it1->second != it2->second) continue;  // different measurements
      std::string f1 = this->get_field_name_(it1->first);
      std::string f2 = this->get_field_name_(it2->first);
      if (f1 == f2) {
        ESP_LOGW(TAG, "Duplicate field '%s' in measurement '%s' (sensors '%s' and '%s') — one will overwrite the other!",
                 f1.c_str(), it1->second.c_str(), it1->first.c_str(), it2->first.c_str());
      }
    }
  }
}

void InfluxDB::validate_tag_consistency_() {
  // Warn if sensors in the same measurement group have different per-sensor tags
  for (auto it1 = this->sensor_measurements_.begin(); it1 != this->sensor_measurements_.end(); ++it1) {
    auto tags1 = this->static_tags_.find(it1->first);
    for (auto it2 = std::next(it1); it2 != this->sensor_measurements_.end(); ++it2) {
      if (it1->second != it2->second) continue;
      auto tags2 = this->static_tags_.find(it2->first);
      bool t1_has = (tags1 != this->static_tags_.end() && !tags1->second.empty());
      bool t2_has = (tags2 != this->static_tags_.end() && !tags2->second.empty());
      if (t1_has != t2_has || (t1_has && t2_has && tags1->second != tags2->second)) {
        ESP_LOGW(TAG, "Sensors '%s' and '%s' share measurement '%s' but have different per-sensor tags — "
                      "grouped line will use tags from the first sensor only",
                 it1->first.c_str(), it2->first.c_str(), it1->second.c_str());
        return;  // one warning is enough
      }
    }
  }
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
    ESP_LOGE(TAG, "HTTP write failed (wrote %d of %u bytes)", written, (unsigned)body.size());
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  
  // Fetch headers / status, then drain any response body
  (void) esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  ESP_LOGD(TAG, "HTTP status: %d", status);
  
  // Drain response body with safety limit to prevent infinite loop
  char tmp[256];
  int total_read = 0;
  while (total_read < MAX_HTTP_RESPONSE_SIZE) {
    int r = esp_http_client_read(client, tmp, sizeof(tmp));
    if (r <= 0) break;
    total_read += r;
  }
  
  if (total_read >= MAX_HTTP_RESPONSE_SIZE) {
    ESP_LOGW(TAG, "Response body exceeded %d bytes, truncated", MAX_HTTP_RESPONSE_SIZE);
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
  // Validation already done in setup, just build with URL encoding
  this->url_ = (this->use_ssl_ ? "https://" : "http://");
  this->url_ += this->host_ + ":" + this->port_;
  this->url_ += "/api/v2/write";
  this->url_ += "?org=" + this->url_encode_(this->org_);
  this->url_ += "&bucket=" + this->url_encode_(this->bucket_);
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
    
    char obj_id_buf[128];
    sensor->get_object_id_to(obj_id_buf);
    std::string obj_id(obj_id_buf);

    if (this->has_sensor_mapping_(obj_id)) {
      this->sensors_.push_back(sensor);
      ESP_LOGD(TAG, "Added sensor: %s", obj_id_buf);
    } else {
      ESP_LOGVV(TAG, "No mapping for sensor: %s", obj_id_buf);
    }
  }

  // Collect text sensors
  for (auto *text_sensor : App.get_text_sensors()) {
    if (text_sensor == nullptr) continue;
    char obj_id_buf[128];
    text_sensor->get_object_id_to(obj_id_buf);
    std::string obj_id(obj_id_buf);
    if (this->has_sensor_mapping_(obj_id)) {
      this->text_sensors_.push_back(text_sensor);
      ESP_LOGD(TAG, "Added text sensor: %s", obj_id_buf);
    }
  }

#ifdef USE_BINARY_SENSOR
  // Collect binary sensors with state tracking
  for (auto *binary_sensor : App.get_binary_sensors()) {
    if (binary_sensor == nullptr) continue;
    char obj_id_buf[128];
    binary_sensor->get_object_id_to(obj_id_buf);
    std::string obj_id(obj_id_buf);
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
  
  // Check if WiFi is connected
  if (!wifi::global_wifi_component->is_connected()) {
    return false;
  }
  
  // "Never" or invalid (0)
  if (this->update_interval_ == UPDATE_INTERVAL_NEVER || this->update_interval_ == 0) {
    return false;
  }
  
  uint32_t now = millis();
  return (now - this->last_publish_) >= this->update_interval_;
}

size_t InfluxDB::estimate_payload_size_() const {
  // With grouped output, we have ~2 lines instead of ~15
  // Each line: measurement + tags (~150 bytes) + all fields (~200 bytes) + timestamp
  size_t sensor_count = this->sensors_.size() + this->text_sensors_.size();
#ifdef USE_BINARY_SENSOR
  sensor_count += this->binary_sensor_states_.size();
#endif
  // Estimate: ~20 bytes per field, ~150 bytes overhead per group, assume 2 groups
  size_t estimated_size = (sensor_count * 20) + (MAX_MEASUREMENT_GROUPS * 150);
  return std::max(estimated_size, MIN_BUFFER_SIZE);
}

// Add these method implementations to your influxdb.cpp
// Replace your existing publish_now() method with these three methods

void InfluxDB::publish_now() {
  // Publish all configured sensors (no filter)
  this->publish_internal_(nullptr);
}

void InfluxDB::publish_sensors(const std::vector<std::string> &sensor_ids) {
  // Convert vector to unordered_set for O(1) lookup performance
  std::unordered_set<std::string> filter(sensor_ids.begin(), sensor_ids.end());
  this->publish_internal_(&filter);
}

void InfluxDB::publish_internal_(const std::unordered_set<std::string> *filter) {
  if (this->is_failed() || this->publish_in_progress_) {
    ESP_LOGW(TAG, "Cannot publish: component %s",
             this->is_failed() ? "has failed" : "publish already in progress");
    return;
  }

  if (!wifi::global_wifi_component->is_connected()) {
    ESP_LOGD(TAG, "Cannot publish: WiFi not connected");
    return;
  }

  // --- Phase 1: Group fields by measurement ---
  std::array<MeasurementGroup, MAX_MEASUREMENT_GROUPS> groups{};
  size_t group_count = 0;
  size_t field_count = 0;

  // Find existing group or create a new one (linear search over <=4 slots)
  auto find_or_create_group = [&](const std::string &measurement,
                                  const std::string &sensor_id) -> MeasurementGroup * {
    for (size_t i = 0; i < group_count; i++) {
      if (groups[i].measurement == measurement)
        return &groups[i];
    }
    if (group_count >= MAX_MEASUREMENT_GROUPS) {
      ESP_LOGW(TAG, "Too many measurement groups (max %zu), dropping data", MAX_MEASUREMENT_GROUPS);
      return nullptr;
    }
    auto &g = groups[group_count++];
    g.measurement = measurement;
    g.first_sensor = sensor_id;
    g.has_data = true;
    return &g;
  };

  // Append a field value to its measurement group
  auto append_field = [&](const std::string &sensor_id, const std::string &value, bool is_string) {
    auto it = this->sensor_measurements_.find(sensor_id);
    const std::string &measurement = (it != this->sensor_measurements_.end()) ? it->second : sensor_id;
    auto *group = find_or_create_group(measurement, sensor_id);
    if (group == nullptr)
      return;
    if (!group->fields.empty())
      group->fields += ",";
    group->fields += this->build_fields_(sensor_id, value, is_string);
    field_count++;
  };

  // Collect numeric sensors
  for (auto *sensor : this->sensors_) {
    if (std::isnan(sensor->state))
      continue;
    char buf[128];
    sensor->get_object_id_to(buf);
    std::string sensor_id(buf);
    if (filter != nullptr && filter->find(sensor_id) == filter->end())
      continue;
    append_field(sensor_id, to_string(sensor->state), false);
  }

  // Collect text sensors
  for (auto *text_sensor : this->text_sensors_) {
    if (text_sensor->state.empty())
      continue;
    char buf[128];
    text_sensor->get_object_id_to(buf);
    std::string sensor_id(buf);
    if (filter != nullptr && filter->find(sensor_id) == filter->end())
      continue;
    append_field(sensor_id, text_sensor->state, true);
  }

#ifdef USE_BINARY_SENSOR
  for (const auto &[sensor_id, state] : this->binary_sensor_states_) {
    if (filter != nullptr && filter->find(sensor_id) == filter->end())
      continue;
    append_field(sensor_id, std::to_string(state ? 1 : 0), false);
  }
#endif

  if (field_count == 0) {
    ESP_LOGD(TAG, "No valid sensor data to publish");
    return;
  }

  // --- Phase 2: Build line protocol payload (one line per group) ---
  std::string timestamp = this->build_timestamp_();
  std::string body;
  body.reserve(this->estimate_payload_size_());

  for (size_t i = 0; i < group_count; i++) {
    auto &g = groups[i];
    body += this->escape_influx_key_(g.measurement);
    body += this->build_tags_(g.first_sensor);
    body += " ";
    body += g.fields;
    body += timestamp;
    body += "\n";
  }

  body.shrink_to_fit();
  this->publish_in_progress_ = true;

  if (filter != nullptr) {
    ESP_LOGI(TAG, "Publishing %zu data points (%zu fields) to InfluxDB", group_count, field_count);
  } else {
    this->last_publish_ = millis();
    ESP_LOGI(TAG, "Publishing %zu data points (%zu fields) to InfluxDB", group_count, field_count);
  }
  ESP_LOGVV(TAG, "Request body length: %u bytes", (unsigned) body.size());

  // One-shot IDF client (verify SSL if URL is https)
  const bool verify_ssl = this->url_.rfind("https://", 0) == 0;

  // Retry logic with exponential backoff
  int attempts = 0;
  bool ok = false;
  do {
    ok = this->post_raw_idf_(this->url_, body, this->headers_, verify_ssl);
    if (!ok && attempts < MAX_RETRY_ATTEMPTS) {
      uint32_t backoff = BASE_BACKOFF_MS + (esp_random() % BACKOFF_RANGE_MS);
      ESP_LOGW(TAG, "POST failed, retrying in %" PRIu32 " ms (attempt %d/%d)",
               backoff, attempts + 1, MAX_RETRY_ATTEMPTS);
      delay(backoff);
    }
  } while (!ok && ++attempts <= MAX_RETRY_ATTEMPTS);

  // Free the request body's capacity immediately
  std::string().swap(body);

  if (!ok) {
    ESP_LOGW(TAG, "InfluxDB POST failed after %d attempts", MAX_RETRY_ATTEMPTS + 1);
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
  
  // Global static tags
  for (const auto &[key, value] : this->global_tags_) {
    tags += "," + this->escape_influx_key_(key) + "=" + this->escape_influx_key_(value);
  }
  
  // Global template tags (evaluated at runtime)
  // Note: Templates are assumed to be safe as they're generated by ESPHome's codegen
  for (const auto &[key, template_func] : this->global_tag_templates_) {
    std::string tag_value = template_func();  // Call the template function
    if (!tag_value.empty()) {  // Only add non-empty values
      tags += "," + this->escape_influx_key_(key) + "=" + this->escape_influx_key_(tag_value);
    }
  }
  
  // Static tags for this sensor
  auto static_it = this->static_tags_.find(sensor_id);
  if (static_it != this->static_tags_.end()) {
    for (const auto &[key, value] : static_it->second) {
      tags += "," + this->escape_influx_key_(key) + "=" + this->escape_influx_key_(value);
    }
  }
  
  // Template tags for this sensor (evaluated at runtime)
  // Note: Templates are assumed to be safe as they're generated by ESPHome's codegen
  auto template_it = this->static_tag_templates_.find(sensor_id);
  if (template_it != this->static_tag_templates_.end()) {
    for (const auto &[key, template_func] : template_it->second) {
      std::string tag_value = template_func();  // Call the template function
      if (!tag_value.empty()) {  // Only add non-empty values
        tags += "," + this->escape_influx_key_(key) + "=" + this->escape_influx_key_(tag_value);
      }
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
      int64_t timestamp = now.timestamp;
      
      // Scale timestamp to requested unit (default is seconds)
      if (this->timestamp_unit_ == "ms") {
        timestamp *= 1000;
      } else if (this->timestamp_unit_ == "us") {
        timestamp *= 1000000;
      } else if (this->timestamp_unit_ == "ns") {
        timestamp *= 1000000000;
      }
      // else: "s" (seconds) - no scaling needed
      
      return std::string(" ") + to_string(timestamp);
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

std::string InfluxDB::url_encode_(const std::string &value) const {
  std::string encoded;
  encoded.reserve(static_cast<size_t>(value.length() * 1.2f));
  
  for (unsigned char c : value) {
    // Unreserved characters per RFC 3986
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      // Percent-encode everything else
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    }
  }
  
  return encoded;
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

// --- Template support methods ---

void InfluxDB::add_static_tag_template(const std::string &sensor_id,
                                       const std::string &tag_key,
                                       std::function<std::string()> func) {
  this->static_tag_templates_[sensor_id][tag_key] = std::move(func);
}

void InfluxDB::add_global_tag_template(const std::string &tag_key,
                                       std::function<std::string()> func) {
  this->global_tag_templates_[tag_key] = std::move(func);
}

void InfluxDB::dump_config() {
  ESP_LOGCONFIG(TAG, "InfluxDB:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Organization: %s", this->org_.c_str());
  ESP_LOGCONFIG(TAG, "  Bucket: %s", this->bucket_.c_str());
  ESP_LOGCONFIG(TAG, "  Timestamp Unit: %s", this->timestamp_unit_.c_str());
  if (this->update_interval_ == UPDATE_INTERVAL_NEVER) {
    ESP_LOGCONFIG(TAG, "  Update Interval: never (manual only)");
  } else {
    ESP_LOGCONFIG(TAG, "  Update Interval: %u ms", this->update_interval_);
  }
  ESP_LOGCONFIG(TAG, "  SSL: %s", this->use_ssl_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Send MAC: %s", this->send_mac_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Configured sensors: %zu", this->sensor_measurements_.size());
  
  // Log template tag counts
  ESP_LOGCONFIG(TAG, "  Global static tags: %zu", this->global_tags_.size());
  ESP_LOGCONFIG(TAG, "  Global template tags: %zu", this->global_tag_templates_.size());
  
  size_t total_static_tags = 0;
  for (const auto &[sensor_id, tags] : this->static_tags_) {
    total_static_tags += tags.size();
  }
  ESP_LOGCONFIG(TAG, "  Sensor static tags: %zu", total_static_tags);
  
  size_t total_template_tags = 0;
  for (const auto &[sensor_id, tags] : this->static_tag_templates_) {
    total_template_tags += tags.size();
  }
  ESP_LOGCONFIG(TAG, "  Sensor template tags: %zu", total_template_tags);
  
  if (this->time_source_ == nullptr) {
    ESP_LOGCONFIG(TAG, "  Time source: Not configured (server timestamp will be used)");
  } else {
    ESP_LOGCONFIG(TAG, "  Time source: Configured");
  }
}

}  // namespace influxdb
}  // namespace esphome