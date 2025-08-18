// Implementation based on:
//  - k30: https://cdn.shopify.com/s/files/1/0019/5952/files/AN102-K30-Sensor-Arduino-I2C.zip?v=1653007039

#include "senseair_i2c.h"
#include "esphome/core/log.h"

namespace esphome {
namespace senseair_i2c {

static const char *const TAG = "senseair_i2c";

// Command constants
static const uint8_t SENSEAIR_MEASURE_CMD[] = {0x22, 0x00, 0x08, 0x2A};
static const uint8_t READ_METER_CMD[] = {0x41, 0x00, 0x3E, 0x7F};

// Timing constants
static const uint32_t I2C_RESPONSE_DELAY_MS = 25;
static const uint8_t ABC_ENABLE_MASK = 0x02;

void SenseairI2CSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair I2C sensor");
  this->setup_step_ = SETUP_READ_METER;
  this->setup_retry_count_ = 0;
  this->setup_read_meter_control_();
}

void SenseairI2CSensor::handle_retry_(std::function<void()> operation, uint8_t& retry_count, 
                                    const char* operation_name, std::function<void()> on_failure) {
  if (++retry_count < this->max_retries_) {
    this->set_timeout(this->retry_delay_ms_, std::move(operation));
  } else {
    ESP_LOGW(TAG, "%s failed after %d retries", operation_name, this->max_retries_);
    on_failure();
  }
}

void SenseairI2CSensor::setup_read_meter_control_() {
  auto error = this->write(READ_METER_CMD, sizeof(READ_METER_CMD));
  if (error != i2c::ERROR_OK) {
    this->handle_retry_([this]() { this->setup_read_meter_control_(); }, 
                       this->setup_retry_count_, "Meter control write",
                       [this]() { this->setup_step_ = SETUP_DONE; });
    return;
  }

  // Schedule read after I2C response delay
  this->set_timeout(I2C_RESPONSE_DELAY_MS, [this]() {
    auto read_err = this->read(this->setup_data_, 3);
    if (read_err != i2c::ERROR_OK) {
      this->handle_retry_([this]() { this->setup_read_meter_control_(); }, 
                         this->setup_retry_count_, "Meter control read",
                         [this]() { this->setup_step_ = SETUP_DONE; });
      return;
    }

    // Validate checksum
    if (!this->validate_checksum_(this->setup_data_, 2, this->setup_data_[2])) {
      ESP_LOGE(TAG, "Meter control checksum mismatch");
      this->setup_step_ = SETUP_DONE;
      return;
    }

    // Check if ABC configuration change is needed
    bool abc_should_enable = (this->abc_interval_ > 0);
    bool abc_is_enabled = this->setup_data_[1] & ABC_ENABLE_MASK;
    
    ESP_LOGCONFIG(TAG, "ABC Status - Requested: %s (%us), Sensor: %s",
                  abc_should_enable ? "ENABLED" : "DISABLED", 
                  this->abc_interval_,
                  abc_is_enabled ? "ENABLED" : "DISABLED");

    if (abc_should_enable != abc_is_enabled) {
      this->setup_step_ = SETUP_CONFIGURE_ABC;
      this->setup_retry_count_ = 0;
      this->setup_configure_abc_();
    } else {
      this->setup_step_ = SETUP_DONE;
      ESP_LOGD(TAG, "ABC configuration matches request, no changes needed");
    }
  });
}

void SenseairI2CSensor::setup_configure_abc_() {
  bool abc_enable = (this->abc_interval_ > 0);
  uint8_t configure_abc_command[] = {0x31, 0x00, 0x3E, 0x00, 0x00};
  
  // Set ABC enable/disable bit
  configure_abc_command[3] = abc_enable ? 
    (this->setup_data_[1] | ABC_ENABLE_MASK) : 
    (this->setup_data_[1] & ~ABC_ENABLE_MASK);
  
  // Calculate checksum
  configure_abc_command[4] = this->calculate_checksum_(configure_abc_command, 4);

  ESP_LOGD(TAG, "Configuring ABC: %s", abc_enable ? "ENABLED" : "DISABLED");

  auto error = this->write(configure_abc_command, sizeof(configure_abc_command));
  if (error != i2c::ERROR_OK) {
    this->handle_retry_([this]() { this->setup_configure_abc_(); }, 
                       this->setup_retry_count_, "ABC configuration write",
                       [this]() { this->setup_step_ = SETUP_DONE; });
    return;
  }

  // Set ABC interval if enabling
  if (abc_enable) {
    ESP_LOGD(TAG, "Setting ABC interval to %us", this->abc_interval_);
    uint8_t interval_cmd[] = {
      0x01, 0x40,
      static_cast<uint8_t>((this->abc_interval_ >> 8) & 0xFF),
      static_cast<uint8_t>(this->abc_interval_ & 0xFF)
    };
    
    if (this->write(interval_cmd, sizeof(interval_cmd)) == i2c::ERROR_OK) {
      ESP_LOGD(TAG, "ABC interval configured successfully");
    } else {
      ESP_LOGW(TAG, "Failed to set ABC interval, using sensor default");
    }
  }

  this->setup_step_ = SETUP_DONE;
}

void SenseairI2CSensor::update() {
  if (this->setup_step_ != SETUP_DONE) {
    ESP_LOGV(TAG, "Setup not complete, skipping measurement");
    return;
  }

  if (this->measure_step_ != MEASURE_IDLE) {
    ESP_LOGV(TAG, "Measurement already in progress, skipping update");
    return;
  }

  ESP_LOGV(TAG, "Starting CO2 measurement");
  this->measure_step_ = MEASURE_WRITE;
  this->measure_retry_count_ = 0;
  this->attempt_measurement_();
}

void SenseairI2CSensor::attempt_measurement_() {
  if (this->measure_step_ == MEASURE_WRITE) {
    auto error = this->write(SENSEAIR_MEASURE_CMD, sizeof(SENSEAIR_MEASURE_CMD));
    if (error != i2c::ERROR_OK) {
      this->handle_retry_([this]() { this->attempt_measurement_(); }, 
                         this->measure_retry_count_, "Measurement write",
                         [this]() { this->measure_step_ = MEASURE_IDLE; });
      return;
    }

    this->measure_step_ = MEASURE_READ;
    this->set_timeout(I2C_RESPONSE_DELAY_MS, [this]() { this->attempt_measurement_(); });
    return;
  }

  if (this->measure_step_ == MEASURE_READ) {
    auto error = this->read(this->measure_data_, 4);
    if (error != i2c::ERROR_OK) {
      this->handle_retry_([this]() { this->attempt_measurement_(); }, 
                         this->measure_retry_count_, "Measurement read",
                         [this]() { this->measure_step_ = MEASURE_IDLE; });
      return;
    }

    // Check if measurement is ready
    if ((this->measure_data_[0] & 0x01) != 0x01) {
      ESP_LOGW(TAG, "Measurement not ready (status: 0x%02X)", this->measure_data_[0]);
      this->measure_step_ = MEASURE_IDLE;
      return;
    }

    // Validate checksum
    if (!this->validate_checksum_(this->measure_data_, 3, this->measure_data_[3])) {
      ESP_LOGE(TAG, "Measurement checksum validation failed");
      this->measure_step_ = MEASURE_IDLE;
      return;
    }

    // Extract and publish CO2 value
    uint16_t co2_ppm = (static_cast<uint16_t>(this->measure_data_[1]) << 8) | this->measure_data_[2];
    ESP_LOGV(TAG, "CO2 measurement: %u ppm", co2_ppm);
    
    this->publish_state(static_cast<float>(co2_ppm));
    this->measure_step_ = MEASURE_IDLE;
  }
}

bool SenseairI2CSensor::validate_checksum_(const uint8_t *data, size_t data_len, uint8_t received_checksum) const {
  uint8_t calculated_checksum = this->calculate_checksum_(data, data_len);
  return calculated_checksum == received_checksum;
}

uint8_t SenseairI2CSensor::calculate_checksum_(const uint8_t *data, size_t len) const {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum += data[i];
  }
  return sum & 0xFF;
}

void SenseairI2CSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair I2C CO2 Sensor:");
  LOG_I2C_DEVICE(this);
  LOG_SENSOR("  ", "CO2", this);
  ESP_LOGCONFIG(TAG, "  ABC: %s (%us), Retries: %u, Delay: %ums", 
                this->abc_interval_ > 0 ? "enabled" : "disabled",
                this->abc_interval_, this->max_retries_, this->retry_delay_ms_);
}

}  // namespace senseair_i2c
}  // namespace esphome