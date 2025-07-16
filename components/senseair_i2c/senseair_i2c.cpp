// Implementation based on:
//  - k30: https://cdn.shopify.com/s/files/1/0019/5952/files/AN102-K30-Sensor-Arduino-I2C.zip?v=1653007039
//  - Official Datasheet (cn):
//  https://rmtplusstoragesenseair.blob.core.windows.net/docs/Dev/publicerat/TDE4700.pdf
//

#include "senseair_i2c.h"
#include "esphome/core/log.h"

namespace esphome {
namespace senseair_i2c {

static const char *const TAG = "senseair_i2c";
static const uint8_t SENSEAIR_MEASURE_CMD[] = {0x22, 0x00, 0x08, 0x2A};

void SenseairI2CSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair I2C sensor");
  this->setup_step_ = SETUP_READ_METER;
  this->setup_retry_count_ = 0;
  this->setup_read_meter_control_();
}

void SenseairI2CSensor::setup_read_meter_control_() {
  static const uint8_t READ_METER_CMD[] = {0x41, 0x00, 0x3E, 0x7F};
  auto error = this->write(READ_METER_CMD, sizeof(READ_METER_CMD));
  if (error == i2c::ERROR_OK) {
    this->set_timeout(25, [this]() {
      auto read_err = this->read(this->setup_data_, 3);
      if (read_err == i2c::ERROR_OK) {
        uint8_t checksum = (this->setup_data_[0] + this->setup_data_[1]) & 0xFF;
        if (checksum != this->setup_data_[2]) {
          ESP_LOGE(TAG, "Meter control checksum mismatch");
          this->setup_step_ = SETUP_DONE;
          return;
        }
        bool abc_should_enable = (this->abc_interval_ > 0);
        bool abc_is_enabled = this->setup_data_[1] & 0x02;
        ESP_LOGCONFIG(TAG, "Automatic Baseline Correction requested: %s (%us), sensor: %s",
          abc_should_enable ? "ENABLED" : "DISABLED", this->abc_interval_,
          abc_is_enabled ? "ENABLED" : "DISABLED");
        if (abc_should_enable != abc_is_enabled) {
          this->setup_step_ = SETUP_CONFIGURE_ABC;
          this->setup_retry_count_ = 0;
          this->setup_configure_abc_();
        } else {
          this->setup_step_ = SETUP_DONE;
          ESP_LOGI(TAG, "No ABC configuration change needed");
        }
      } else {
        if (++this->setup_retry_count_ < this->max_retries_) {
          ESP_LOGD(TAG, "Meter control read failed (%d), retry %d/%d",
            read_err, this->setup_retry_count_, this->max_retries_);
          this->set_timeout(this->retry_delay_ms_, [this]() { this->setup_read_meter_control_(); });
        } else {
          ESP_LOGW(TAG, "Meter control read failed after %d retries, skipping", this->max_retries_);
          this->setup_step_ = SETUP_DONE;
        }
      }
    });
  } else {
    if (++this->setup_retry_count_ < this->max_retries_) {
      ESP_LOGD(TAG, "Meter control write failed (%d), retry %d/%d",
        error, this->setup_retry_count_, this->max_retries_);
      this->set_timeout(this->retry_delay_ms_, [this]() { this->setup_read_meter_control_(); });
    } else {
      ESP_LOGW(TAG, "Meter control write failed after %d retries, skipping", this->max_retries_);
      this->setup_step_ = SETUP_DONE;
    }
  }
}

void SenseairI2CSensor::setup_configure_abc_() {
  bool abc_enable = (this->abc_interval_ > 0);
  uint8_t configure_abc_command[] = {0x31, 0x00, 0x3E, 0x00, 0x00};
  configure_abc_command[3] = abc_enable ? (this->setup_data_[1] | 0x02) : (this->setup_data_[1] & 0xFD);
  configure_abc_command[4] = (configure_abc_command[0] + configure_abc_command[1] +
                              configure_abc_command[2] + configure_abc_command[3]) & 0xFF;

  ESP_LOGI(TAG, "Setting ABC to %s", abc_enable ? "ENABLED" : "DISABLED");

  auto error = this->write(configure_abc_command, sizeof(configure_abc_command));
  if (error == i2c::ERROR_OK) {
    this->setup_step_ = SETUP_DONE;
    ESP_LOGI(TAG, "ABC configuration updated to %s", abc_enable ? "ENABLED" : "DISABLED");
    if (abc_enable) {
      ESP_LOGI(TAG, "Setting ABC interval to %us", this->abc_interval_);
      uint8_t interval_bytes[2] = {
        static_cast<uint8_t>((this->abc_interval_ >> 8) & 0xFF),
        static_cast<uint8_t>(this->abc_interval_ & 0xFF)
      };
      uint8_t interval_cmd[] = {0x01, 0x40, interval_bytes[0], interval_bytes[1]};
      if (this->write(interval_cmd, sizeof(interval_cmd)) == i2c::ERROR_OK) {
        ESP_LOGI(TAG, "ABC interval set successfully");
      } else {
        ESP_LOGW(TAG, "Failed to set ABC interval");
      }
    }
  } else {
    if (++this->setup_retry_count_ < this->max_retries_) {
      ESP_LOGD(TAG, "ABC configuration write failed (%d), retry %d/%d",
        error, this->setup_retry_count_, this->max_retries_);
      this->set_timeout(this->retry_delay_ms_, [this]() { this->setup_configure_abc_(); });
    } else {
      ESP_LOGW(TAG, "ABC configuration write failed after %d retries, aborting", this->max_retries_);
      this->setup_step_ = SETUP_DONE;
    }
  }
}

void SenseairI2CSensor::update() {
  if (!this->read_started_) {
    this->start_time_ = millis();
    this->read_started_ = true;
    return;
  }
  uint32_t elapsed = (millis() - this->start_time_) / 1000;
  if (elapsed < this->update_interval_) {
    return;
  }
  this->read_started_ = false;
  this->measure_step_ = MEASURE_WRITE;
  this->measure_retry_count_ = 0;
  this->attempt_measurement_();
}

void SenseairI2CSensor::attempt_measurement_() {
  if (this->measure_step_ == MEASURE_WRITE) {
    auto error = this->write(SENSEAIR_MEASURE_CMD, sizeof(SENSEAIR_MEASURE_CMD));
    if (error == i2c::ERROR_OK) {
      this->measure_step_ = MEASURE_READ;
      this->set_timeout(25, [this]() { this->attempt_measurement_(); });
      return;
    } else {
      if (++this->measure_retry_count_ < this->max_retries_) {
        ESP_LOGD(TAG, "Measurement write failed (%d), retry %d/%d",
          error, this->measure_retry_count_, this->max_retries_);
        this->set_timeout(this->retry_delay_ms_, [this]() { this->attempt_measurement_(); });
        return;
      } else {
        ESP_LOGW(TAG, "Measurement write failed after %d retries, aborting", this->max_retries_);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
    }
  }

  if (this->measure_step_ == MEASURE_READ) {
    auto error = this->read(this->measure_data_, 4);
    if (error == i2c::ERROR_OK) {
      if ((this->measure_data_[0] & 0x01) != 0x01) {
        ESP_LOGW(TAG, "Measurement process not finished (0x%02X)", this->measure_data_[0]);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
      uint8_t checksum = (this->measure_data_[0] + this->measure_data_[1] + this->measure_data_[2]) & 0xFF;
      if (checksum != this->measure_data_[3]) {
        ESP_LOGE(TAG, "Measurement checksum mismatch (%02X != %02X)", checksum, this->measure_data_[3]);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
      uint16_t co2 = ((uint16_t)this->measure_data_[1] << 8) | this->measure_data_[2];
      // Consider logging this only at DEBUG level, or not at all if it spams
      // ESP_LOGD(TAG, "CO2: %u ppm", co2);
      this->publish_state(static_cast<float>(co2));
      this->measure_step_ = MEASURE_IDLE;
      return;
    } else {
      if (++this->measure_retry_count_ < this->max_retries_) {
        ESP_LOGD(TAG, "Measurement read failed (%d), retry %d/%d",
          error, this->measure_retry_count_, this->max_retries_);
        this->set_timeout(this->retry_delay_ms_, [this]() { this->attempt_measurement_(); });
        return;
      } else {
        ESP_LOGW(TAG, "Measurement read failed after %d retries, aborting", this->max_retries_);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
    }
  }
}

void SenseairI2CSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair I2C CO2 Sensor:");
  LOG_I2C_DEVICE(this);
  LOG_SENSOR("  ", "CO2", this);
}

}  // namespace senseair_i2c
}  // namespace esphome