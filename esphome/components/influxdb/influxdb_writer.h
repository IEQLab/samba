#pragma once

#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/controller.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#include "esphome/components/http_request/http_request.h"

namespace esphome
{
  namespace influxdb
  {
    class InfluxDBWriter : public Component
    {
    public:
      InfluxDBWriter(){};
      void setup() override;
      void loop() override;
      void dump_config() override;
#ifdef USE_BINARY_SENSOR
      void on_sensor_update(binary_sensor::BinarySensor *obj, std::string measurement, std::string tags, bool state);
#endif
#ifdef USE_SENSOR
      void on_sensor_update(sensor::Sensor *obj, std::string measurement, std::string tags, float state);
#endif
#ifdef USE_TEXT_SENSOR
      void on_sensor_update(text_sensor::TextSensor *obj, std::string measurement, std::string tags, std::string state);
#endif
      float get_setup_priority() const;

      void set_host(std::string host) { this->host = host; };
      void set_use_ssl(bool use_ssl) { this->use_ssl = use_ssl; }
      void set_port(uint16_t port) { this->port = port; };

      void set_bucket(std::string bucket) { this->bucket = bucket; };
      void set_token(std::string token) { this->token = token; this->auth_string = "Token " + token; };
      void set_org(std::string org) { this->org = org; };
      void set_orgId(std::string orgId) { this->orgId = orgId; };
      void set_precision(std::string precision) { this->precision = precision; };
      void set_send_timeout(int timeout) { send_timeout = timeout; };

      void set_tags(std::string tags) { this->tags = tags; };
      void set_publish_all(bool all) { publish_all = all; };
      void add_setup_callback(std::function<EntityBase *()> fun) { setup_callbacks.push_back(fun); };

    protected:
      void write(std::string measurement, std::string tags, const std::string value, bool is_string);

      uint16_t port;
      std::string host;
      bool use_ssl;

      std::string bucket;
      std::string token;
      std::string auth_string;
      std::string org;
      std::string orgId;
      std::string precision;
      std::string service_url;

      int send_timeout;
      std::string tags;
      bool publish_all;

      std::vector<std::function<EntityBase *()>> setup_callbacks;

      http_request::HttpRequestComponent *request_;
    };

  } // namespace influxdb
} // namespace esphome
