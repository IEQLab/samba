// Implementation based on:
//  - k30: https://cdn.shopify.com/s/files/1/0019/5952/files/AN102-K30-Sensor-Arduino-I2C.zip?v=1653007039
//  - Official Datasheet (cn):
//  https://rmtplusstoragesenseair.blob.core.windows.net/docs/Dev/publicerat/TDE4700.pdf
//

#include "k30.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace k30 {

static const char *const TAG = "K30";

// Command to measure CO2.
static const uint8_t K30_MEASURE_CMD[] = {0x22, 0x00, 0x08, 0x2A};
// Command to read meter control byte.
static const uint8_t K30_READ_METER_CMD[] = {0x41, 0x00, 0x3E, 0x7F};

void K30Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up K30...");
  // Set the automatic background calibration. This is done in EEPROM.

  // Read meter control.
  i2c::ErrorCode error = this->write(K30_READ_METER_CMD, sizeof(K30_READ_METER_CMD));
  if (error != i2c::ERROR_OK) {
    ESP_LOGD(TAG, "Attempt to read meter control byte failed");
    this->mark_failed();
    return;
  }
  // Wait for the sensor to process the command.
  uint8_t data[4];
  error = this->read(data, 3);
  if (error != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Error reading data from K30");
    this->mark_failed();
    return;
  }
  // Calculate the checksum. The checksum is stripped to 8 bits.
  uint8_t checksum = this->calculate_checksum_(data, 2);
  if (checksum != data[2]) {
    ESP_LOGE(TAG, "Checksum error when reading meter control byte");
    this->mark_failed();
    return;
  }

  // Check if ABC is already enabled.
  bool is_abc_enabled_ = data[1] & 0x02;
  // Only update if values are different.
  if (this->enable_abc_ != is_abc_enabled_) {
    // Create the command to configure the ABC.
    uint8_t configure_abc_command[] = {0x31, 0x00, 0x3E, 0x00, 0x00};
    if (this->enable_abc_) {
      // Mask the ABC bit to 1.
      configure_abc_command[3] = data[2] | 0x02;
    } else {
      // Mask the ABC bit to 0.
      configure_abc_command[3] = data[2] & 0xFD;
    }
    // Calculate the checksum for the new command.
    checksum = this->calculate_checksum_(configure_abc_command, 4);
    configure_abc_command[4] = checksum;
    // Send command. Bear in mind that, in order to change the ABC configuration,
    // the sensor must be restarted.
    error = this->write(configure_abc_command, sizeof(configure_abc_command));
    if (error != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "Error setting  meter control byte");
      this->mark_failed();
      return;
    }
  }
  ESP_LOGCONFIG(TAG, "K30 initialized");
}

void K30Component::update() {
  if (!this->read_started_) {
    this->start_time_ = millis();
    this->read_started_ = true;
    return;
  }

  uint32_t elapsed = (millis() - this->start_time_) / 1000;
  // Time has not passed.
  if (elapsed < this->update_interval_) {
    return;
  }
  // Reset flag for the next reading.
  this->read_started_ = false;

  // send the command to start a measurement.
  i2c::ErrorCode error = this->write(K30_MEASURE_CMD, sizeof(K30_MEASURE_CMD));
  if (error != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Error sending command to K30");
    this->status_set_warning("Error sending command to K30");
  }
  // Wait for the sensor to process the command.
  this->set_timeout(20, [this]() {
    // Read the data from the sensor.
    uint8_t data[4];
    i2c::ErrorCode error = this->read(data, 4);
    this->reading_status_ = IDLE;
    if (error != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "Error reading data from K30");
      this->status_set_warning("Error reading data from K30");
      return;
    }

    // Check if the measuring process is finished.
    if ((data[0] & 0x01) != 0x01) {
      ESP_LOGE(TAG, "Measuring process not finished");
      this->status_set_warning("Measuring process not finished");
      return;
    }

    // Calculate the checksum. The checksum is stripped to 8 bits.
    uint8_t checksum = this->calculate_checksum_(data, 3);
    if (checksum != data[3]) {
      ESP_LOGE(TAG, "Checksum error!");
      this->status_set_warning("Checksum error!");
      return;
    }

    // Calculate the CO2 value.
    uint16_t temp_c_o2_u32 = (((uint16_t(data[1])) << 8) | (uint16_t(data[2])));
    float co2 = static_cast<float>(temp_c_o2_u32);
    // Publish result.
    if (this->co2_sensor_ != nullptr) {
      this->co2_sensor_->publish_state(co2);
    }
    this->status_clear_warning();
  });
}

void K30Component::dump_config() {
  ESP_LOGCONFIG(TAG, "K30:");
  LOG_I2C_DEVICE(this);

  ESP_LOGCONFIG(TAG, "  Automatic self calibration: %s", ONOFF(this->enable_abc_));
  ESP_LOGCONFIG(TAG, "  Automatic self calibration interval: %dh", this->abc_update_interval_ / 3600);
  ESP_LOGCONFIG(TAG, "  Update interval: %ds", this->update_interval_);
  LOG_SENSOR("  ", "CO2", this->co2_sensor_);
}

float K30Component::get_setup_priority() const { return setup_priority::DATA; }

uint8_t K30Component::calculate_checksum_(uint8_t *array, uint8_t len) {
  uint32_t checksum = 0;
  for (uint8_t i = 0; i < len; i++) {
    checksum += array[i];
  }
  return checksum & 0xFF;
}

}  // namespace k30
}  // namespace esphome
