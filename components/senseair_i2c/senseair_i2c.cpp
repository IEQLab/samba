// Implementation based on:
//   - Senseair K30/K33: https://cdn.shopify.com/s/files/1/0019/5952/files/AN102-K30-Sensor-Arduino-I2C.zip?v=1653007039
//   - Official Datasheet (cn): https://rmtplusstoragesenseair.blob.core.windows.net/docs/Dev/publicerat/TDE4700.pdf

#include "senseair_i2c.h"
#include "esphome/core/log.h"

namespace esphome {
namespace senseair_i2c {

static const char *const TAG = "senseair_i2c";
// Command for K30/K33 measurement (returns CO2 PPM)
static const uint8_t SENSEAIR_MEASURE_CMD[] = {0x22, 0x00, 0x08, 0x2A};

// --- Setup (boot-time configuration, including ABC state machine) ---

void SenseairI2CSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair Kxx I2C (sensor platform)...");
  this->setup_step_ = SETUP_READ_METER;
  this->setup_retry_count_ = 0;
  this->setup_read_meter_control_();
}

void SenseairI2CSensor::setup_read_meter_control_() {
  // Read meter control register to determine if ABC matches config
  static const uint8_t READ_METER_CMD[] = {0x41, 0x00, 0x3E, 0x7F};
  auto error = this->write(READ_METER_CMD, sizeof(READ_METER_CMD));
  if (error == i2c::ERROR_OK) {
    this->set_timeout(25, [this]() {
      auto read_err = this->read(this->setup_data_, 3);
      if (read_err == i2c::ERROR_OK) {
        uint8_t checksum = (this->setup_data_[0] + this->setup_data_[1]) & 0xFF;
        if (checksum != this->setup_data_[2]) {
          ESP_LOGE(TAG, "Checksum error when reading meter control byte");
          this->setup_step_ = SETUP_DONE;
          return;
        }
        bool abc_should_enable = (this->abc_interval_ > 0);
        bool is_abc_enabled = this->setup_data_[1] & 0x02;
        ESP_LOGCONFIG(TAG, "K30: Config wants ABC %s (interval %u). Sensor currently: %s.",
          abc_should_enable ? "ENABLED" : "DISABLED", this->abc_interval_,
          is_abc_enabled ? "ENABLED" : "DISABLED");
        if (abc_should_enable != is_abc_enabled) {
          this->setup_step_ = SETUP_CONFIGURE_ABC;
          this->setup_retry_count_ = 0;
          this->setup_configure_abc_();
        } else {
          this->setup_step_ = SETUP_DONE;
          ESP_LOGCONFIG(TAG, "K30 initialized (no ABC update needed)");
        }
      } else {
        if (++this->setup_retry_count_ < this->max_retries_) {
          ESP_LOGW(TAG, "K30: I2C read (meter control) failed (error %d), retry %d/%d",
              read_err, this->setup_retry_count_, this->max_retries_);
          this->set_timeout(this->retry_delay_ms_, [this]() { this->setup_read_meter_control_(); });
        } else {
          ESP_LOGE(TAG, "K30: I2C read (meter control) failed after %d retries. Aborting.", this->max_retries_);
          this->setup_step_ = SETUP_DONE;
        }
      }
    });
  } else {
    if (++this->setup_retry_count_ < this->max_retries_) {
      ESP_LOGW(TAG, "K30: I2C write (meter control) failed (error %d), retry %d/%d",
          error, this->setup_retry_count_, this->max_retries_);
      this->set_timeout(this->retry_delay_ms_, [this]() { this->setup_read_meter_control_(); });
    } else {
      ESP_LOGE(TAG, "K30: I2C write (meter control) failed after %d retries. Aborting.", this->max_retries_);
      this->setup_step_ = SETUP_DONE;
    }
  }
}

void SenseairI2CSensor::setup_configure_abc_() {
  // Write ABC enable/disable to meter control register
  bool abc_should_enable = (this->abc_interval_ > 0);
  uint8_t configure_abc_command[] = {0x31, 0x00, 0x3E, 0x00, 0x00};
  if (abc_should_enable) {
    configure_abc_command[3] = this->setup_data_[1] | 0x02;
  } else {
    configure_abc_command[3] = this->setup_data_[1] & 0xFD;
  }
  configure_abc_command[4] = (configure_abc_command[0] + configure_abc_command[1] +
                              configure_abc_command[2] + configure_abc_command[3]) & 0xFF;

  ESP_LOGCONFIG(TAG, "Requesting sensor to %s ABC (Automatic Baseline Correction)...",
                abc_should_enable ? "ENABLE" : "DISABLE");

  auto error = this->write(configure_abc_command, sizeof(configure_abc_command));
  if (error == i2c::ERROR_OK) {
    this->setup_step_ = SETUP_DONE;
    ESP_LOGCONFIG(TAG, "K30 initialized (ABC updated to %s)", abc_should_enable ? "ENABLED" : "DISABLED");
    if (abc_should_enable) {
      ESP_LOGCONFIG(TAG, "Requesting sensor to set ABC interval to %u seconds...", this->abc_interval_);
      uint8_t interval_bytes[2] = {
        static_cast<uint8_t>((this->abc_interval_ >> 8) & 0xFF),
        static_cast<uint8_t>(this->abc_interval_ & 0xFF)
      };
      uint8_t interval_cmd[] = {0x01, 0x40, interval_bytes[0], interval_bytes[1]};
      if (this->write(interval_cmd, sizeof(interval_cmd)) == i2c::ERROR_OK) {
        ESP_LOGCONFIG(TAG, "ABC interval updated successfully.");
      } else {
        ESP_LOGE(TAG, "Failed to update ABC interval!");
      }
    }
  } else {
    if (++this->setup_retry_count_ < this->max_retries_) {
      ESP_LOGW(TAG, "K30: I2C write (ABC config) failed (error %d), retry %d/%d",
          error, this->setup_retry_count_, this->max_retries_);
      this->set_timeout(this->retry_delay_ms_, [this]() { this->setup_configure_abc_(); });
    } else {
      ESP_LOGE(TAG, "K30: I2C write (ABC config) failed after %d retries. Aborting.", this->max_retries_);
      this->setup_step_ = SETUP_DONE;
    }
  }
}

// --- Polling/Measurement state machine (non-blocking) ---

void SenseairI2CSensor::update() {
  // Non-blocking, wait for update_interval_ seconds
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
        ESP_LOGW(TAG, "Senseair: I2C write (measure) failed (error %d), retry %d/%d",
            error, this->measure_retry_count_, this->max_retries_);
        this->set_timeout(this->retry_delay_ms_, [this]() { this->attempt_measurement_(); });
        return;
      } else {
        ESP_LOGE(TAG, "Senseair: I2C write (measure) failed after %d retries. Aborting.", this->max_retries_);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
    }
  }

  if (this->measure_step_ == MEASURE_READ) {
    auto error = this->read(this->measure_data_, 4);
    if (error == i2c::ERROR_OK) {
      if ((this->measure_data_[0] & 0x01) != 0x01) {
        ESP_LOGE(TAG, "Senseair: Measuring process not finished (status: 0x%02X)", this->measure_data_[0]);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
      uint8_t checksum = (this->measure_data_[0] + this->measure_data_[1] + this->measure_data_[2]) & 0xFF;
      if (checksum != this->measure_data_[3]) {
        ESP_LOGE(TAG, "Senseair: Checksum error! (expected: 0x%02X, got: 0x%02X)", checksum, this->measure_data_[3]);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
      uint16_t co2 = ((uint16_t)this->measure_data_[1] << 8) | this->measure_data_[2];
      this->publish_state(static_cast<float>(co2));
      this->measure_step_ = MEASURE_IDLE;
      return;
    } else {
      if (++this->measure_retry_count_ < this->max_retries_) {
        ESP_LOGW(TAG, "Senseair: I2C read (measure) failed (error %d), retry %d/%d",
            error, this->measure_retry_count_, this->max_retries_);
        this->set_timeout(this->retry_delay_ms_, [this]() { this->attempt_measurement_(); });
        return;
      } else {
        ESP_LOGE(TAG, "Senseair: I2C read (measure) failed after %d retries. Aborting.", this->max_retries_);
        this->measure_step_ = MEASURE_IDLE;
        return;
      }
    }
  }
}

// --- Config and status output (shown in ESPHome logs) ---
void SenseairI2CSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair Kxx CO2 Sensor (I2C platform):");
  LOG_I2C_DEVICE(this);
  LOG_SENSOR("  ", "CO2", this);
}

}  // namespace senseair_i2c
}  // namespace esphome