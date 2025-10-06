#pragma once

#include <cstdint>
#include <functional>
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace senseair_i2c {

/**
 * @brief SenseairI2CSensor
 *   - Platform sensor for K30/K33 (and compatible) CO₂ sensors over I²C.
 *   - Robust, non-blocking state-machine for configuration and measurement.
 *   - Automatic baseline correction (ABC) configurable at boot.
 *   - ABC settings written to EEPROM (requires power cycle to take effect)
 *   - Manual calibration actions available
 */
class SenseairI2CSensor : public sensor::Sensor, public PollingComponent, public i2c::I2CDevice {
public:
  // --- Configurable setters called by Python codegen ---
  void set_abc_interval(uint32_t interval) { abc_interval_ = interval; }
  void set_retry_delay_ms(uint32_t delay) { retry_delay_ms_ = delay; }
  void set_max_retries(uint8_t retries) { max_retries_ = retries; }
  void set_read_delay_ms(uint32_t delay) { read_delay_ms_ = delay; }
  
  // --- Component interface ---
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  
  // --- Manual calibration actions ---
  void background_calibration();
  void background_calibration_with_ppm(uint16_t target_ppm);
  void abc_get_period();
  
protected:
  // --- User options with sensible defaults ---
  uint32_t abc_interval_{648000};   // 180h default ABC interval (seconds)
  uint32_t retry_delay_ms_{200};    // Retry wait (ms)
  uint8_t max_retries_{5};          // Max I²C retries per state
  uint32_t read_delay_ms_{50};      // Delay before reading response (ms)
  
  // --- Setup state machine for ABC configuration ---
  enum SetupStep { 
    SETUP_IDLE, 
    SETUP_READ_METER, 
    SETUP_CONFIGURE_ABC, 
    SETUP_WRITE_ABC_INTERVAL,
    SETUP_READ_DIAGNOSTICS,
    SETUP_DONE 
  };
  SetupStep setup_step_{SETUP_IDLE};
  uint8_t setup_retry_count_{0};
  bool setup_success_{false};  // Track if setup actually succeeded
  bool abc_config_pending_{false};  // Track if ABC changes need power cycle
  uint8_t setup_data_[3];
  void setup_read_meter_control_();
  void setup_configure_abc_();
  void setup_write_abc_interval_();
  void setup_failed_();
  
  // --- Measurement state machine ---
  enum MeasureStep { MEASURE_IDLE, MEASURE_WRITE, MEASURE_READ };
  MeasureStep measure_step_{MEASURE_IDLE};
  uint8_t measure_write_retry_count_{0};
  uint8_t measure_read_retry_count_{0};
  uint8_t measure_data_[4];
  void attempt_measurement_();
  
  // --- Calibration state machine ---
  enum CalibrationStep { 
    CAL_IDLE, 
    CAL_WRITE, 
    CAL_READ_RESPONSE,
    CAL_READ_ABC_PERIOD
  };
  CalibrationStep calibration_step_{CAL_IDLE};
  uint8_t calibration_retry_count_{0};
  uint8_t calibration_data_[4];
  void perform_calibration_command_(uint16_t address, uint16_t command, const char* name);
  void read_abc_period_();
  uint16_t get_calibration_address_() const;
  
  // ZeroTrim register for calibration offset (TDE4700 Table 18)
  static const uint16_t ZEROTRIM_ADDR = 0x0048;
  
  // --- Diagnostic data read during setup ---
  struct DiagnosticData {
    bool valid{false};
    uint8_t firmware_type{0};
    uint8_t firmware_main{0};
    uint8_t firmware_sub{0};
    uint8_t error_status{0};
    uint32_t serial_number{0};
    uint8_t memory_map_id{0};
  } diagnostic_data_;
  
  void read_diagnostics_();
  
  // --- Helper methods ---
  void handle_retry_(std::function<void()> operation, uint8_t& retry_count, 
                     const char* operation_name, std::function<void()> on_failure);
  bool validate_checksum_(const uint8_t *data, size_t data_len, uint8_t received_checksum) const;
  uint8_t calculate_checksum_(const uint8_t *data, size_t len) const;
  bool validate_co2_value_(int16_t co2_ppm) const;
};

/** Action to trigger background calibration */
template<typename... Ts> 
class BackgroundCalibrationAction : public Action<Ts...> {
public:
  BackgroundCalibrationAction(SenseairI2CSensor *parent) : parent_(parent) {}
  
  void play(Ts... x) override { this->parent_->background_calibration(); }
  
protected:
  SenseairI2CSensor *parent_;
};

/** Action to trigger background calibration with custom target ppm */
template<typename... Ts> 
class BackgroundCalibrationWithPPMAction : public Action<Ts...> {
public:
  BackgroundCalibrationWithPPMAction(SenseairI2CSensor *parent) : parent_(parent) {}
  
  TEMPLATABLE_VALUE(uint16_t, target_ppm)
  
  void play(Ts... x) override { 
    auto ppm = this->target_ppm_.value(x...);
    this->parent_->background_calibration_with_ppm(ppm);
  }
  
protected:
  SenseairI2CSensor *parent_;
};

/** Action to read ABC period from sensor */
template<typename... Ts> 
class ABCGetPeriodAction : public Action<Ts...> {
public:
  ABCGetPeriodAction(SenseairI2CSensor *parent) : parent_(parent) {}
  
  void play(Ts... x) override { this->parent_->abc_get_period(); }
  
protected:
  SenseairI2CSensor *parent_;
};

}  // namespace senseair_i2c
}  // namespace esphome