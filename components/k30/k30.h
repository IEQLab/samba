#pragma once

#include <cstdint>
#include <utility>

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace k30 {

enum ErrorCode {
  COMMUNICATION_FAILED,
  FIRMWARE_IDENTIFICATION_FAILED,
  MEASUREMENT_INIT_FAILED,
  FORCE_RECALIBRATION_FAILED,
  UNKNOWN
};

enum ReadingStatus {
  IDLE,
  REQUEST_SEND,
};

class K30Component : public PollingComponent, public i2c::I2CDevice {
 public:
  // Setters called by python. This runs before setup().
  void set_automatic_self_calibration(bool asc) { enable_abc_ = asc; }
  void set_automatic_self_calibration_update_interval(uint32_t interval) { abc_update_interval_ = interval; }
  void set_co2_sensor(sensor::Sensor *co2_sensor) { co2_sensor_ = co2_sensor; }
  void set_update_interval(uint16_t interval) { update_interval_ = interval; }

  // Called by the framework.
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override;

 protected:
  /**
   * @brief Calculate the checksum of an array given. This is used to verify the data
   * received from the sensor.
   *
   * @param array pointer to array of bytes.
   * @param array_length length of the array.
   * @return uint8_t calculation of the checksum.
   */
  uint8_t calculate_checksum_(uint8_t *array, uint8_t array_length);

  ErrorCode error_code_{UNKNOWN};
  // Status of the reading process. This is used to avoid blocking the update() loop.
  ReadingStatus reading_status_{IDLE};

  // Flag to enable automatic background calibration.
  bool enable_abc_{true};
  // Configurations for CO2 sensor.
  sensor::Sensor *co2_sensor_{nullptr};
  /// Update interval in seconds.
  uint16_t update_interval_{0xFFFF};
  uint32_t abc_update_interval_{0xFFFFFFFF};
  uint32_t start_time_{};
  bool read_started_{false};

  unsigned read_count_;
  void read_data_();
  void restart_read_();
};

}  // namespace k30
}  // namespace esphome
