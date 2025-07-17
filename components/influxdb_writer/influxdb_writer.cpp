#include "influxdb_writer.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace influxdb_writer {

static const char *const TAG = "influxdb_writer";

void InfluxDBWriter::setup() {
  ESP_LOGI(TAG, "Setting up InfluxDBWriter");

  if (this->use_ssl_) this->url_ = "https://";
  else this->url_ = "http://";

  this->url_ += this->host_ +":"+this->port_+"/api/v2/write?org="+this->org_ +"&bucket="+this->bucket_+"&precision="+this->timestampUnit_;

  for (auto *sensor : App.get_sensors()) {
    if (sensor != nullptr){
      this->sensors_.push_back(sensor);
    }
  }
#ifdef USE_BINARY_SENSOR
  for (auto *binary_sensor : App.get_binary_sensors()) {
    if (binary_sensor != nullptr){    
      this->binarySensorsStates_[binary_sensor->get_name()] = binary_sensor->state;
      binary_sensor->add_on_state_callback([this, binary_sensor](bool state) {
        this->binarySensorsStates_[binary_sensor->get_name()] = state;
      });
    }
  }
#endif
  for (auto *text_sensor : App.get_text_sensors()) {
    if (text_sensor != nullptr){
      this->textSensors_.push_back(text_sensor);
    }
  }

if (this->send_mac_) {
  this->mac_addr_ = esphome::get_mac_address(); //get_mac_address_pretty()
  ESP_LOGV(TAG, "MAC Addr: %s", this->mac_addr_.c_str());
}

  this->headers_.push_back({"Content-Type", "text/plain;charset=utf-8"}); 
  this->headers_.push_back({"Authorization", "Token "+this->token_}); 
}

void InfluxDBWriter::publish_now() {
  std::string body;

  if (this->time_ != nullptr) {
    this->timestamp = this->time_->now().timestamp;
    this->has_time = (this->timestamp > 1000000000);
    if (this->has_time) {
      ESP_LOGD(TAG, "Got timestamp: %ld", this->timestamp);
    }
  } else {
    this->has_time = false;
  }

  // Floating sensors
  for (auto *sensor : this->sensors_) {
    float value = sensor->state;
    if (isnan(value)) continue;
    auto it = this->sensorNamesWithId_.find(sensor->get_name());
    if (it == this->sensorNamesWithId_.end()) continue;
    body += build_line(it->second, value);
  }

  // Binary sensors
  for (const auto& binary_sens : this->binarySensorsStates_) {
    auto it = this->sensorNamesWithId_.find(binary_sens.first);
    if (it == this->sensorNamesWithId_.end()) continue;
    body += build_line(it->second, binary_sens.second);
  }

  // Text sensors
  for (auto *text_sensor : this->textSensors_) {
    auto it = this->sensorNamesWithId_.find(text_sensor->get_name());
    if (it == this->sensorNamesWithId_.end()) continue;
    body += build_line(it->second, text_sensor->state);
  }

  ESP_LOGV(TAG, "HTTP Request Body: %s", body.c_str());
  this->http_request_->post(this->url_, body, this->headers_);
}

void InfluxDBWriter::add_static_tag(const std::string &sensor, const std::string &tag, const std::string &value) {
  this->static_tags_[sensor][tag] = value;
}

void InfluxDBWriter::set_dynamic_tag(const std::string &sensor, const std::string &tag, const std::string &value) {
  this->dynamic_tags_[sensor][tag] = value;
}

void InfluxDBWriter::set_field_name(const std::string &sensor, const std::string &name) {
  this->fieldNames_[sensor] = name;
}

void InfluxDBWriter::add_sensor_name_id(const std::string &sensor_id, const std::string &name) {
  this->sensorNamesWithId_[name] = sensor_id;
}

std::string InfluxDBWriter::escape_tags(const std::string &input) {
  std::string output;
  for (char c : input) {
    if (c == ' ' || c == ',' || c == '=') {
      output += '\\';
    }
    output += c;
  }
  return output;
}

std::string InfluxDBWriter::build_tags(const std::string& id) {
  std::string tags;

  // Merge static and dynamic tags
  std::map<std::string, std::string> merged_tags;

  auto static_it = this->static_tags_.find(id);
  if (static_it != this->static_tags_.end()) {
    merged_tags = static_it->second;
  }

  auto dynamic_it = this->dynamic_tags_.find(id);
  if (dynamic_it != this->dynamic_tags_.end()) {
    for (const auto& pair : dynamic_it->second) {
      merged_tags[pair.first] = pair.second;
    }
  }

  for (const auto& pair : merged_tags) {
    tags += "," + this->escape_tags(pair.first) + "=" + this->escape_tags(pair.second);
  }

  if (this->send_mac_) {
    tags += ",device=" + this->escape_tags(this->mac_addr_);
  }

  return tags;
}

std::string InfluxDBWriter::get_field_name(const std::string& id) {
  auto field_name = this->fieldNames_.find(id);
  return (field_name != this->fieldNames_.end()) ? field_name->second : "value";
}

std::string InfluxDBWriter::build_line(const std::string &id, float &value) {
  std::string line = this->escape_tags(id);
  line += this->build_tags(id);
  std::string field = this->get_field_name(id);
  line += " " + field + "=" + to_string(value);
  line += this->has_time ? " " + to_string(timestamp) + "\n" : "\n";
  return line;
}

std::string InfluxDBWriter::build_line(const std::string &id, bool state) {
  std::string line = this->escape_tags(id);
  line += this->build_tags(id);
  std::string field = this->get_field_name(id);
  line += " " + field + "=" + std::to_string(state ? 1 : 0);
  line += this->has_time ? " " + to_string(timestamp) + "\n" : "\n";
  return line;
}

std::string InfluxDBWriter::build_line(const std::string &id, const std::string &value) {
  std::string line = this->escape_tags(id);
  line += this->build_tags(id);
  std::string field = this->get_field_name(id);
  line += " " + field + "=\"" + value + "\"";
  line += this->has_time ? " " + to_string(timestamp) + "\n" : "\n";
  return line;
}

void InfluxDBWriter::dump_config(){
  ESP_LOGCONFIG(TAG, "Host: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "Port: %s", this->port_.c_str());
  ESP_LOGCONFIG(TAG, "Organization: %s", this->org_.c_str());
  ESP_LOGCONFIG(TAG, "Bucket: %s", this->bucket_.c_str());
}

}  // namespace influxdb_writer
}  // namespace esphome