#pragma once

#include <cstdint>
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace senseair_i2c {

/**
 * @brief SenseairI2CSensor
 *   - Platform sensor for K30/K33 (and compatible) CO₂ sensors over I²C.
 *   - Robust, non-blocking state-machine for configuration and measurement.
 *   - Automatic baseline correction (ABC) configurable at boot.
 */
class SenseairI2CSensor : public sensor::Sensor, public PollingComponent, public i2c::I2CDevice {
 public:
  // --- Configurable setters called by Python codegen ---
  void set_abc_interval(uint32_t interval) { abc_interval_ = interval; }
  void set_update_interval(uint16_t interval) { update_interval_ = interval; }
  void set_retry_delay_ms(int delay) { retry_delay_ms_ = delay; }
  void set_max_retries(int retries) { max_retries_ = retries; }

  // --- Component interface ---
  void setup() override;
  void update() override;
  void reset_read_flag();
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  // --- User options with sensible defaults ---
  uint32_t abc_interval_{648000};   // 180h default ABC interval
  uint16_t update_interval_{60};   // 60s sensor polling
  int retry_delay_ms_ = 200;       // Retry wait (ms)
  int max_retries_ = 5;            // Max I²C retries per state
  bool read_started_{false};
  bool measuring_ = false;
  uint32_t start_time_{0};

  // --- Setup state machine for ABC configuration ---
  enum SetupStep { SETUP_IDLE, SETUP_READ_METER, SETUP_CONFIGURE_ABC, SETUP_DONE };
  SetupStep setup_step_{SETUP_IDLE};
  int setup_retry_count_{0};
  uint8_t setup_data_[3];
  void setup_read_meter_control_();
  void setup_configure_abc_();

  // --- Measurement state machine ---
  enum MeasureStep { MEASURE_IDLE, MEASURE_WRITE, MEASURE_READ };
  MeasureStep measure_step_{MEASURE_IDLE};
  int measure_retry_count_{0};
  uint8_t measure_data_[4];
  void attempt_measurement_();
};

}  // namespace senseair_i2c
}  // namespace esphome