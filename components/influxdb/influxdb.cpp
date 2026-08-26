#include "influxdb.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#endif

namespace esphome::influxdb {

static const char *const TAG = "influxdb";

// InfluxDB explains a rejected write in the response body; this much is enough for its message.
static constexpr size_t ERROR_BODY_LOG_BYTES = 200;

void InfluxDB::setup() {
  // Assign each distinct measurement a line, in order of first appearance
  for (auto &f : this->fields_) {
    uint8_t line = 0;
    while (line < this->line_count_ && strcmp(this->line_names_[line], f.measurement) != 0)
      line++;
    if (line == this->line_count_) {
      if (this->line_count_ == MAX_MEASUREMENTS) {
        ESP_LOGE(TAG, "More than %u measurements configured", (unsigned) MAX_MEASUREMENTS);
        this->mark_failed();
        return;
      }
      this->line_names_[this->line_count_++] = f.measurement;
    }
    f.line = line;
  }

  // A sensor is uploaded only when it has produced a filtered value since its last upload.
  // Anything that already has a state (published before this component was set up) starts fresh.
  for (size_t i = 0; i < this->fields_.size(); i++) {
    auto *sensor = this->fields_[i].sensor;
    this->fields_[i].fresh = sensor->has_state();
    sensor->add_on_state_callback([this, i](float) { this->fields_[i].fresh = true; });
  }

  if (strcmp(this->timestamp_unit_, "ms") == 0) {
    this->timestamp_scale_ = 1000;
  } else if (strcmp(this->timestamp_unit_, "us") == 0) {
    this->timestamp_scale_ = 1000000;
  } else if (strcmp(this->timestamp_unit_, "ns") == 0) {
    this->timestamp_scale_ = 1000000000;
  }

  this->url_ = this->use_ssl_ ? "https://" : "http://";
  this->url_ += this->host_;
  this->url_ += ':';
  this->url_ += std::to_string(this->port_);
  this->url_ += "/api/v2/write?org=";
  append_url_encoded_(this->url_, this->org_);
  this->url_ += "&bucket=";
  append_url_encoded_(this->url_, this->bucket_);
  this->url_ += "&precision=";
  this->url_ += this->timestamp_unit_;

  this->headers_.reserve(2);
  this->headers_.push_back({"Content-Type", "text/plain; charset=utf-8"});
  this->headers_.push_back({"Authorization", std::string("Token ") + this->token_});

  if (this->send_mac_)
    get_mac_address_into_buffer(this->mac_);

  // Reserve the payload buffer once. Its capacity is kept between uploads, so steady-state
  // publishes do not touch the heap. Generous bound: tag values are unknown until runtime.
  this->body_.reserve(128 + this->line_count_ * (64 + this->tags_.size() * 64) + this->fields_.size() * 40);

  if (this->update_interval_ != UPDATE_INTERVAL_NEVER && this->update_interval_ > 0) {
    this->set_interval(this->update_interval_, [this]() { this->publish(); });
  }
}

void InfluxDB::dump_config() {
  ESP_LOGCONFIG(TAG,
                "InfluxDB:\n"
                "  URL: %s\n"
                "  Fields: %u in %u measurement(s)\n"
                "  Tags: %u%s\n"
                "  Timestamps: %s",
                this->url_.c_str(), (unsigned) this->fields_.size(), (unsigned) this->line_count_,
                (unsigned) this->tags_.size(), this->send_mac_ ? " + device MAC" : "",
                this->time_source_ != nullptr ? "device clock" : "server");
  if (this->update_interval_ == UPDATE_INTERVAL_NEVER || this->update_interval_ == 0) {
    ESP_LOGCONFIG(TAG, "  Update interval: never (publish action only)");
  } else {
    ESP_LOGCONFIG(TAG, "  Update interval: %" PRIu32 " ms", this->update_interval_);
  }
}

void InfluxDB::publish_(const std::vector<sensor::Sensor *> *only) {
  if (this->is_failed())
    return;

  if (this->in_flight_) {
    // A retry of the previous payload is still pending; the new data supersedes it.
    ESP_LOGW(TAG, "Dropping unsent payload from previous publish");
    this->cancel_timeout("retry");
    this->in_flight_ = false;
  }

  if (!network::is_connected()) {
    // Fresh flags are left set, so these values go out with the next publish.
    ESP_LOGW(TAG, "Not publishing: network is down");
    return;
  }

  size_t fields = this->build_body_(only);
  if (fields == 0) {
    ESP_LOGD(TAG, "Nothing to publish: no sensor has a fresh, finite value");
    return;
  }

  ESP_LOGI(TAG, "Publishing %u field(s), %u bytes", (unsigned) fields, (unsigned) this->body_.size());
  this->attempt_ = 0;
  this->in_flight_ = true;
  this->send_();
}

size_t InfluxDB::build_body_(const std::vector<sensor::Sensor *> *only) {
  auto selected = [only](const sensor::Sensor *sensor) {
    if (only == nullptr)
      return true;
    for (const auto *s : *only) {
      if (s == sensor)
        return true;
    }
    return false;
  };

  this->body_.clear();
  size_t total = 0;

  for (uint8_t line = 0; line < this->line_count_; line++) {
    const size_t line_start = this->body_.size();
    this->body_ += this->line_names_[line];
    this->append_tags_();
    this->body_ += ' ';

    size_t count = 0;
    for (auto &f : this->fields_) {
      if (f.line != line || !selected(f.sensor))
        continue;
      // A sensor with nothing new, or with NaN/inf, is left out of the line: InfluxDB rejects
      // both, and re-sending its previous value would fabricate data. Warn every time so a
      // dead sensor is visible in the log as well as by its missing field.
      if (!f.fresh) {
        ESP_LOGW(TAG, "Skipping %s: no new value since last upload", f.sensor->get_name().c_str());
        continue;
      }
      f.fresh = false;
      if (!std::isfinite(f.sensor->state)) {
        ESP_LOGW(TAG, "Skipping %s: value is %f", f.sensor->get_name().c_str(), f.sensor->state);
        continue;
      }
      if (count++ > 0)
        this->body_ += ',';
      this->body_ += f.field;
      this->body_ += '=';
      char value[32];
      snprintf(value, sizeof(value), "%f", f.sensor->state);
      this->body_ += value;
    }

    if (count == 0) {
      // No fields for this measurement; remove the partial line
      this->body_.resize(line_start);
      continue;
    }
    this->append_timestamp_();
    this->body_ += '\n';
    total += count;
  }
  return total;
}

void InfluxDB::append_tags_() {
  for (const auto &t : this->tags_) {
    const size_t start = this->body_.size();
    this->body_ += ',';
    this->body_ += t.key;
    this->body_ += '=';
    const size_t value_start = this->body_.size();
    if (t.value != nullptr) {
      append_escaped_(this->body_, t.value);
    } else {
      append_escaped_(this->body_, t.fn().c_str());
    }
    // An empty tag value is a line protocol error; leave the tag out instead
    if (this->body_.size() == value_start)
      this->body_.resize(start);
  }
  if (this->send_mac_) {
    this->body_ += ",device=";
    this->body_ += this->mac_;
  }
}

void InfluxDB::append_timestamp_() {
  if (this->time_source_ == nullptr)
    return;
  auto now = this->time_source_->now();
  if (!now.is_valid())
    return;  // InfluxDB uses its own receive time
  char buf[24];
  snprintf(buf, sizeof(buf), " %" PRId64, static_cast<int64_t>(now.timestamp) * this->timestamp_scale_);
  this->body_ += buf;
}

void InfluxDB::send_() {
  this->attempt_++;
#ifdef USE_ESP32
  ESP_LOGI(TAG, "POST attempt %u/%u; heap free %u, largest block %u", this->attempt_, MAX_ATTEMPTS,
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
  ESP_LOGI(TAG, "POST attempt %u/%u", this->attempt_, MAX_ATTEMPTS);
#endif

  // http_request applies its configured socket timeout and widens the task watchdog for the
  // duration of the request, so a dead host cannot trip the watchdog and reboot the device.
  auto response = this->http_request_->post(this->url_, this->body_, this->headers_);
  if (response == nullptr) {
    this->retry_or_drop_("connection failed");
    return;
  }

  const int status = response->status_code;
  if (http_request::is_success(status)) {
    response->end();
    ESP_LOGI(TAG, "Published (HTTP %d in %" PRIu32 " ms)", status, response->duration_ms);
    this->in_flight_ = false;
    return;
  }

  char message[ERROR_BODY_LOG_BYTES + 1];
  int len = response->read(reinterpret_cast<uint8_t *>(message), ERROR_BODY_LOG_BYTES);
  response->end();
  if (len < 0)
    len = 0;
  message[len] = '\0';
  for (int i = 0; i < len; i++) {
    if (message[i] == '\n' || message[i] == '\r')
      message[i] = ' ';
  }
  ESP_LOGW(TAG, "InfluxDB rejected write: HTTP %d %s", status, message);

  // 5xx and 429 are the server's problem and worth a retry. Any other 4xx means the payload,
  // token, org or bucket is wrong, and resending the same request cannot succeed.
  if (status >= 500 || status == 429) {
    this->retry_or_drop_("server error");
  } else {
    this->in_flight_ = false;
  }
}

void InfluxDB::retry_or_drop_(const char *reason) {
  if (this->attempt_ >= MAX_ATTEMPTS) {
    ESP_LOGW(TAG, "Upload failed after %u attempts (%s); dropping %u bytes", this->attempt_, reason,
             (unsigned) this->body_.size());
    this->in_flight_ = false;
    return;
  }
  const uint32_t backoff = RETRY_BACKOFF_MIN_MS + random_uint32() % RETRY_BACKOFF_RANGE_MS;
  ESP_LOGW(TAG, "Upload failed (%s); retrying in %" PRIu32 " ms", reason, backoff);
  this->set_timeout("retry", backoff, [this]() { this->send_(); });
}

void InfluxDB::append_escaped_(std::string &out, const char *value) {
  for (const char *p = value; *p != '\0'; p++) {
    const char c = *p;
    if (static_cast<unsigned char>(c) < 0x20)
      continue;  // line protocol has no escape for control characters
    if (c == ' ' || c == ',' || c == '=')
      out += '\\';
    out += c;
  }
}

void InfluxDB::append_url_encoded_(std::string &out, const char *value) {
  for (const char *p = value; *p != '\0'; p++) {
    const auto c = static_cast<unsigned char>(*p);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
}

}  // namespace esphome::influxdb
