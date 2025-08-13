#pragma once

#include <map>
#include <string>
#include <vector>
#include "esp_wifi.h"

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/sntp/sntp_component.h"

namespace esphome {
namespace influxdb_writer {

class InfluxDBWriter : public Component {
   public:
      InfluxDBWriter() : Component(){};  
      void setup() override;
      void publish_now();
      void dump_config() override;
      
      void set_host(const std::string &host) { host_ = host; }
      void set_token(const std::string &token) { token_ = token; }
      void set_bucket(const std::string &bucket) { bucket_ = bucket; }
      void set_org(const std::string &org) { org_ = org; }
      void set_port(const std::string &port) { port_ = port; }
      void set_influxdb_timestamp_unit(const std::string &unit) { timestampUnit_ = unit; }
      void set_http_request(http_request::HttpRequestComponent *request) { http_request_ = request; }
      void add_static_tag(const std::string &sensor, const std::string &tag, const std::string &value);
      void set_dynamic_tag(const std::string &sensor, const std::string &tag, const std::string &value);
      void set_time(esphome::sntp::SNTPComponent *time) { this->time_ = time; }
      void set_field_name(const std::string &sensor, const std::string &fieldName);
      void set_use_ssl(bool use_ssl) { this->use_ssl_ = use_ssl; }
      void add_sensor_name_id(const std::string &sensor_id, const std::string &name);
      void set_send_mac(bool send_mac) { this->send_mac_ = send_mac; }

   protected:
      std::string host_;
      std::string token_;
      std::string bucket_;
      std::string org_;
      std::string port_;
      std::string timestampUnit_;
      std::string url_;
      std::string mac_addr_;
      time_t timestamp;
      bool has_time = false;
      bool use_ssl_, send_mac_;

      std::string build_tags(const std::string& id);
      std::string escape_tags(const std::string &input);
      std::string get_field_name(const std::string& id);
      std::string build_line(const std::string &id, float &value);
      std::string build_line(const std::string &id, bool state);
      std::string build_line(const std::string &id, const std::string &value);

      std::list<esphome::http_request::Header> headers_;
      
      std::map<std::string, std::string> sensorNamesWithId_;
      std::map<std::string, std::map<std::string, std::string>> static_tags_;
      std::map<std::string, std::map<std::string, std::string>> dynamic_tags_;
      std::map<std::string, std::string> fieldNames_;
      
      esphome::sntp::SNTPComponent *time_{nullptr};

      std::map<std::string, bool> binarySensorsStates_;
      std::vector<esphome::sensor::Sensor *> sensors_; 
      std::vector<esphome::text_sensor::TextSensor *> textSensors_; 

      http_request::HttpRequestComponent *http_request_{nullptr};
};

}  // namespace influxdb_writer
}  // namespace esphome