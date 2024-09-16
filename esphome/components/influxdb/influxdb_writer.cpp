#include <string>
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "influxdb_writer.h"

#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome
{
    namespace influxdb
    {
        static const char *TAG = "influxdb_jab";

        void InfluxDBWriter::setup()
        {
            ESP_LOGCONFIG(TAG, "Setting up InfluxDB Writer...");
            std::vector<EntityBase *> objs;
            for (auto fun : setup_callbacks)
                objs.push_back(fun());

            if (this->use_ssl) this->service_url = "https://";
            else this->service_url = "http://";

            this->service_url += this->host+":"+to_string(this->port)+"/api/v2/write?bucket="+this->bucket+"&precision="+this->precision;
            if (this->org.length() > 0) this->service_url += "&org="+this->org;
            if (this->orgId.length() > 0) this->service_url += "&orgID="+this->orgId;

            this->request_ = new http_request::HttpRequestComponent();
            this->request_->setup();

            std::list<http_request::Header> headers;

            http_request::Header auth_header;
            auth_header.name = "Authorization";
            auth_header.value = this->auth_string.c_str();
            headers.push_back(auth_header);

            http_request::Header content_header;
            content_header.name = "Content-Type";
            content_header.value = "text/plain";
            headers.push_back(content_header);

            this->request_->set_headers(headers);
            this->request_->set_method("POST");
            this->request_->set_useragent("ESPHome InfluxDB Bot");
            this->request_->set_timeout(this->send_timeout);

            if (publish_all)
            {
#ifdef USE_BINARY_SENSOR
                for (auto *obj : App.get_binary_sensors())
                {
                    if (!obj->is_internal() && std::none_of(objs.begin(), objs.end(), [&obj](EntityBase *o) { return o == obj; }))
                        obj->add_on_state_callback([this, obj](bool state) { this->on_sensor_update(obj, obj->get_object_id(), tags, state); });
                }
#endif
#ifdef USE_SENSOR
                for (auto *obj : App.get_sensors())
                {
                    if (!obj->is_internal() && std::none_of(objs.begin(), objs.end(), [&obj](EntityBase *o) { return o == obj; }))
                        obj->add_on_state_callback([this, obj](float state) { this->on_sensor_update(obj, obj->get_object_id(), tags, state); });
                }
#endif
#ifdef USE_TEXT_SENSOR
                for (auto *obj : App.get_text_sensors())
                {
                    if (!obj->is_internal() && std::none_of(objs.begin(), objs.end(), [&obj](EntityBase *o) { return o == obj; }))
                        obj->add_on_state_callback([this, obj](std::string state) { this->on_sensor_update(obj, obj->get_object_id(), tags, state); });
                }
#endif
            }
        }

        float InfluxDBWriter::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

        void InfluxDBWriter::loop() {}

        void InfluxDBWriter::write(std::string measurement, std::string tags, const std::string value, bool is_string)
        {
            std::string line = measurement + tags + " value=" + (is_string ? ("\"" + value + "\"") : value);
            this->request_->set_url(this->service_url);
            this->request_->set_body(line.c_str());
            this->request_->send(
                    std::vector<
                    esphome::http_request::HttpRequestResponseTrigger *>{});

//             String response = this->request_->get_string();
//             if (response.length() == 0) {
//                 ESP_LOGD(TAG, "Got empty response for method POST.");
//             } else {
//                 ESP_LOGD(TAG, "Response: %s", response.c_str());
//             }

            this->request_->close();

            ESP_LOGD(TAG, "InfluxDB packet: %s", line.c_str());
        }

        void InfluxDBWriter::dump_config()
        {
            ESP_LOGCONFIG(TAG, "InfluxDB Writer:");
            ESP_LOGCONFIG(TAG, "  Address: %s:%u", host.c_str(), port);
            ESP_LOGCONFIG(TAG, "  Bucket: %s", bucket.c_str());
        }

#ifdef USE_BINARY_SENSOR
        void InfluxDBWriter::on_sensor_update(binary_sensor::BinarySensor *obj, std::string measurement, std::string tags, bool state)
        {
            write(measurement, tags, state ? "t" : "f", false);
        }
#endif

#ifdef USE_SENSOR
        void InfluxDBWriter::on_sensor_update(sensor::Sensor *obj, std::string measurement, std::string tags, float state)
        {
            if(!isnan(state))
                write(measurement, tags, to_string(state), false);
        }
#endif

#ifdef USE_TEXT_SENSOR
        void InfluxDBWriter::on_sensor_u    pdate(text_sensor::TextSensor *obj, std::string measurement, std::string tags, std::string state)
        {
            write(measurement, tags, state, true);
        }
#endif

    } // namespace influxdb
} // namespace esphome
