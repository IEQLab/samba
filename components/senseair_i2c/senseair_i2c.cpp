// Implementation based on:
//  - K30 I2C Protocol: TDE4700.pdf from Senseair AB
//  - Arduino example: AN102-K30-Sensor-Arduino-I2C.pdf

#include "senseair_i2c.h"
#include "esphome/core/log.h"

namespace esphome {
namespace senseair_i2c {

static const char *const TAG = "senseair_i2c";

// Command constants
static const uint8_t SENSEAIR_MEASURE_CMD[] = {0x22, 0x00, 0x08, 0x2A};
static const uint8_t READ_METER_CMD[] = {0x41, 0x00, 0x3E, 0x7F};

// Status byte bit masks
static const uint8_t STATUS_COMPLETE_BIT = 0x01;

// ABC enable mask (bit 1 in MeterControl register)
// Per TDE4700 Table 16: bit 1 = 1 means DISABLED, bit 1 = 0 means ENABLED
static const uint8_t ABC_ENABLE_MASK = 0x02;

// CO2 value validation ranges (per sensor documentation)
// Negative values are possible during calibration/nitrogen test
static const int16_t CO2_MIN_VALID = -2000;  // Allow negative during calibration
static const int16_t CO2_MAX_VALID = 10000;  // Sensor max range

// ABC interval limits (16-bit register, units in hours)
static const uint32_t ABC_INTERVAL_MAX_HOURS = 65535;  // Max value for 16-bit register

void SenseairI2CSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Senseair I2C sensor");
  this->setup_step_ = SETUP_READ_METER;
  this->setup_retry_count_ = 0;
  this->setup_success_ = false;
  this->abc_config_pending_ = false;
  this->setup_read_meter_control_();
}

void SenseairI2CSensor::handle_retry_(std::function<void()> operation, uint8_t& retry_count, 
                                      const char* operation_name, std::function<void()> on_failure) {
  if (++retry_count < this->max_retries_) {
    ESP_LOGV(TAG, "%s retry %d/%d", operation_name, retry_count, this->max_retries_);
    this->set_timeout(this->retry_delay_ms_, std::move(operation));
  } else {
    ESP_LOGW(TAG, "%s failed after %d retries", operation_name, this->max_retries_);
    on_failure();
  }
}

void SenseairI2CSensor::setup_failed_() {
  this->setup_step_ = SETUP_DONE;
  this->setup_success_ = false;
  ESP_LOGE(TAG, "Sensor setup failed - measurements will be skipped");
}

void SenseairI2CSensor::setup_read_meter_control_() {
  // Read MeterControl register from EEPROM using Read EEPROM command (0x41)
  // Command format: [cmd, addr_msb, addr_lsb, checksum]
  auto error = this->write(READ_METER_CMD, sizeof(READ_METER_CMD));
  if (error != i2c::ERROR_OK) {
    ESP_LOGV(TAG, "Meter control write error: %d", error);
    this->handle_retry_([this]() { this->setup_read_meter_control_(); }, 
                        this->setup_retry_count_, "Meter control write",
                        [this]() { this->setup_failed_(); });
    return;
  }
  
  // Schedule read after delay to allow sensor to prepare response
  this->set_timeout(this->read_delay_ms_, [this]() {
    // Response format: [status, data, checksum]
    auto read_err = this->read(this->setup_data_, 3);
    if (read_err != i2c::ERROR_OK) {
      ESP_LOGV(TAG, "Meter control read error: %d", read_err);
      this->handle_retry_([this]() { this->setup_read_meter_control_(); }, 
                          this->setup_retry_count_, "Meter control read",
                          [this]() { this->setup_failed_(); });
      return;
    }
    
    ESP_LOGV(TAG, "Meter control read: status=0x%02X data=0x%02X checksum=0x%02X",
             this->setup_data_[0], this->setup_data_[1], this->setup_data_[2]);
    
    // Validate checksum
    if (!this->validate_checksum_(this->setup_data_, 2, this->setup_data_[2])) {
      ESP_LOGE(TAG, "Meter control checksum mismatch");
      this->setup_failed_();
      return;
    }
    
    // Check if read was complete
    if ((this->setup_data_[0] & STATUS_COMPLETE_BIT) != STATUS_COMPLETE_BIT) {
      ESP_LOGW(TAG, "Meter control read incomplete (status: 0x%02X)", this->setup_data_[0]);
      this->setup_failed_();
      return;
    }
    
    // Check if ABC configuration change is needed
    bool abc_should_enable = (this->abc_interval_ > 0);
    bool abc_is_enabled = !(this->setup_data_[1] & ABC_ENABLE_MASK);  // Bit 1=0 means enabled
    
    ESP_LOGI(TAG, "ABC Status - Current: %s, Requested: %s (%u hours)",
             abc_is_enabled ? "ENABLED" : "DISABLED",
             abc_should_enable ? "ENABLED" : "DISABLED",
             this->abc_interval_ / 3600);
    ESP_LOGD(TAG, "MeterControl byte: 0x%02X", this->setup_data_[1]);
    
    if (abc_should_enable != abc_is_enabled) {
      ESP_LOGI(TAG, "ABC configuration mismatch - will update EEPROM");
      this->setup_step_ = SETUP_CONFIGURE_ABC;
      this->setup_retry_count_ = 0;
      this->setup_configure_abc_();
    } else {
      // ABC matches, proceed to read diagnostics
      this->setup_step_ = SETUP_READ_DIAGNOSTICS;
      this->setup_retry_count_ = 0;
      this->read_diagnostics_();
    }
  });
}

void SenseairI2CSensor::setup_configure_abc_() {
  bool abc_enable = (this->abc_interval_ > 0);
  
  // Write to MeterControl register in EEPROM (address 0x3E)
  // Command format: [cmd, addr_msb, addr_lsb, data, checksum]
  // 0x31 = Write EEPROM (1 byte) - requires power cycle to take effect per TDE4700 Table 16
  uint8_t configure_abc_command[] = {0x31, 0x00, 0x3E, 0x00, 0x00};
  
  // Set ABC enable/disable bit (bit 1)
  // Per TDE4700 Table 16: bit 1 = 0 means ENABLED, bit 1 = 1 means DISABLED
  if (abc_enable) {
    configure_abc_command[3] = this->setup_data_[1] & ~ABC_ENABLE_MASK;  // Clear bit 1 to enable
  } else {
    configure_abc_command[3] = this->setup_data_[1] | ABC_ENABLE_MASK;   // Set bit 1 to disable
  }
  
  // Calculate checksum: sum of bytes after address+direction (bytes 0-3 in our command)
  configure_abc_command[4] = this->calculate_checksum_(configure_abc_command, 4);
  
  ESP_LOGI(TAG, "Writing ABC %s to EEPROM", abc_enable ? "ENABLE" : "DISABLE");
  ESP_LOGD(TAG, "ABC Command: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
           configure_abc_command[0], configure_abc_command[1], 
           configure_abc_command[2], configure_abc_command[3], configure_abc_command[4]);
  
  auto error = this->write(configure_abc_command, sizeof(configure_abc_command));
  if (error != i2c::ERROR_OK) {
    ESP_LOGV(TAG, "ABC configuration write error: %d", error);
    this->handle_retry_([this]() { this->setup_configure_abc_(); }, 
                        this->setup_retry_count_, "ABC configuration write",
                        [this]() { this->setup_failed_(); });
    return;
  }
  
  // Wait for sensor to process the command
  this->set_timeout(this->read_delay_ms_, [this]() {
    // Read the response to verify write succeeded
    // Response format for Write EEPROM: [status, checksum]
    uint8_t response_data[2];
    auto read_err = this->read(response_data, 2);
    if (read_err != i2c::ERROR_OK) {
      ESP_LOGV(TAG, "ABC configuration response read error: %d", read_err);
      this->handle_retry_([this]() { this->setup_configure_abc_(); }, 
                          this->setup_retry_count_, "ABC configuration response read",
                          [this]() { this->setup_failed_(); });
      return;
    }
    
    ESP_LOGV(TAG, "ABC Response: status=0x%02X checksum=0x%02X", 
             response_data[0], response_data[1]);
    
    // Validate response checksum
    if (!this->validate_checksum_(response_data, 1, response_data[1])) {
      ESP_LOGE(TAG, "ABC configuration response checksum mismatch (calc=0x%02X, recv=0x%02X)",
               response_data[0], response_data[1]);
      this->setup_failed_();
      return;
    }
    
    // Check operation status bit (bit 0 should be 1 for success)
    // For Write EEPROM: 0x31 = success, 0x30 = incomplete
    if ((response_data[0] & STATUS_COMPLETE_BIT) != STATUS_COMPLETE_BIT) {
      ESP_LOGW(TAG, "ABC configuration write incomplete (status: 0x%02X)", response_data[0]);
      this->setup_failed_();
      return;
    }
    
    ESP_LOGI(TAG, "ABC enable/disable written to EEPROM successfully");
    this->abc_config_pending_ = true;
    
    // Set ABC interval if enabling
    if (this->abc_interval_ > 0) {
      this->setup_step_ = SETUP_WRITE_ABC_INTERVAL;
      this->setup_retry_count_ = 0;
      this->setup_write_abc_interval_();
    } else {
      this->setup_step_ = SETUP_READ_DIAGNOSTICS;
      this->setup_retry_count_ = 0;
      this->read_diagnostics_();
    }
  });
}

void SenseairI2CSensor::setup_write_abc_interval_() {
  // Convert seconds to hours (register expects hours per TDE4700 Table 16)
  uint32_t abc_hours = this->abc_interval_ / 3600;
  
  // Validate ABC interval range (16-bit register max)
  if (abc_hours == 0) {
    ESP_LOGW(TAG, "ABC interval too short (%us), minimum is 1 hour. Using 1 hour.", this->abc_interval_);
    abc_hours = 1;
  } else if (abc_hours > ABC_INTERVAL_MAX_HOURS) {
    ESP_LOGW(TAG, "ABC interval too large (%u hours), maximum is %u hours. Using maximum.", 
             abc_hours, ABC_INTERVAL_MAX_HOURS);
    abc_hours = ABC_INTERVAL_MAX_HOURS;
  }
  
  ESP_LOGI(TAG, "Setting ABC period to %u hours", abc_hours);
  
  // Write to EEPROM address 0x40 (ABC Period register - 2 bytes)
  // Format per TDE4700 Table 16: 2 bytes unsigned word, MS byte at LOWER address (big-endian)
  // Command format: [cmd, addr_msb, addr_lsb, data_msb, data_lsb, checksum]
  // 0x32 = Write EEPROM, 2 bytes (0x3 high nibble = WriteEEPROM, 0x2 low nibble = 2 bytes)
  uint8_t interval_cmd[] = {
    0x32,  // Write EEPROM command for 2 bytes
    0x00,  // Address MSB
    0x40,  // Address LSB - ABC Period register
    static_cast<uint8_t>((abc_hours >> 8) & 0xFF),  // Data MSB (hours high byte)
    static_cast<uint8_t>(abc_hours & 0xFF),         // Data LSB (hours low byte)
    0x00   // Checksum placeholder
  };
  interval_cmd[5] = this->calculate_checksum_(interval_cmd, 5);
  
  ESP_LOGD(TAG, "ABC Interval Command: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
           interval_cmd[0], interval_cmd[1], interval_cmd[2], 
           interval_cmd[3], interval_cmd[4], interval_cmd[5]);
  
  auto error = this->write(interval_cmd, sizeof(interval_cmd));
  if (error != i2c::ERROR_OK) {
    ESP_LOGV(TAG, "ABC interval write error: %d", error);
    this->handle_retry_([this]() { this->setup_write_abc_interval_(); }, 
                        this->setup_retry_count_, "ABC interval write",
                        [this]() { this->setup_failed_(); });
    return;
  }
  
  // Wait and read response
  this->set_timeout(this->read_delay_ms_, [this]() {
    uint8_t response_data[2];
    auto read_err = this->read(response_data, 2);
    if (read_err != i2c::ERROR_OK) {
      ESP_LOGV(TAG, "ABC interval response read error: %d", read_err);
      this->handle_retry_([this]() { this->setup_write_abc_interval_(); }, 
                          this->setup_retry_count_, "ABC interval response read",
                          [this]() { this->setup_failed_(); });
      return;
    }
    
    ESP_LOGV(TAG, "ABC Interval Response: status=0x%02X checksum=0x%02X",
             response_data[0], response_data[1]);
    
    // Validate response checksum
    if (!this->validate_checksum_(response_data, 1, response_data[1])) {
      ESP_LOGE(TAG, "ABC interval response checksum mismatch (calc=0x%02X, recv=0x%02X)",
               response_data[0], response_data[1]);
      this->setup_failed_();
      return;
    }
    
    // Check operation status (0x31 = success for Write EEPROM)
    if ((response_data[0] & STATUS_COMPLETE_BIT) != STATUS_COMPLETE_BIT) {
      ESP_LOGW(TAG, "ABC interval write incomplete (status: 0x%02X)", response_data[0]);
      this->setup_failed_();
      return;
    }
    
    ESP_LOGI(TAG, "ABC period written to EEPROM successfully");
    
    // Proceed to read diagnostics
    this->setup_step_ = SETUP_READ_DIAGNOSTICS;
    this->setup_retry_count_ = 0;
    this->read_diagnostics_();
  });
}

void SenseairI2CSensor::read_diagnostics_() {
  ESP_LOGD(TAG, "Reading sensor diagnostics...");
  
  // Read firmware identification (0x62-0x64 in RAM, 3 bytes)
  uint8_t fw_cmd[] = {0x23, 0x00, 0x62, 0x88};  // Read RAM, 3 bytes from 0x62
  auto error = this->write(fw_cmd, sizeof(fw_cmd));
  if (error != i2c::ERROR_OK) {
    ESP_LOGD(TAG, "Firmware read command failed, skipping diagnostics");
    this->setup_step_ = SETUP_DONE;
    this->setup_success_ = true;
    if (this->abc_config_pending_) {
      ESP_LOGW(TAG, "ABC changes written - POWER CYCLE SENSOR to activate!");
    }
    return;
  }
  
  this->set_timeout(this->read_delay_ms_, [this]() {
    uint8_t fw_response[5];  // status + 3 data bytes + checksum
    if (this->read(fw_response, 5) == i2c::ERROR_OK && 
        this->validate_checksum_(fw_response, 4, fw_response[4]) &&
        (fw_response[0] & STATUS_COMPLETE_BIT)) {
      this->diagnostic_data_.firmware_type = fw_response[1];
      this->diagnostic_data_.firmware_main = fw_response[2];
      this->diagnostic_data_.firmware_sub = fw_response[3];
      this->diagnostic_data_.valid = true;
      ESP_LOGD(TAG, "Firmware: Type 0x%02X, Version %u.%u",
               this->diagnostic_data_.firmware_type,
               this->diagnostic_data_.firmware_main,
               this->diagnostic_data_.firmware_sub);
    }
    
    // Read error status (0x1E in RAM, 1 byte)
    uint8_t err_cmd[] = {0x21, 0x00, 0x1E, 0x3F};
    if (this->write(err_cmd, sizeof(err_cmd)) == i2c::ERROR_OK) {
      this->set_timeout(this->read_delay_ms_, [this]() {
        uint8_t err_response[3];  // status + 1 data byte + checksum
        if (this->read(err_response, 3) == i2c::ERROR_OK && 
            this->validate_checksum_(err_response, 2, err_response[2]) &&
            (err_response[0] & STATUS_COMPLETE_BIT)) {
          this->diagnostic_data_.error_status = err_response[1];
          if (this->diagnostic_data_.error_status != 0) {
            ESP_LOGW(TAG, "Error status: 0x%02X (errors detected!)", this->diagnostic_data_.error_status);
          } else {
            ESP_LOGD(TAG, "Error status: OK");
          }
        }
        
        // Read memory map ID (0x2F in RAM, 1 byte)
        uint8_t map_cmd[] = {0x21, 0x00, 0x2F, 0x50};
        if (this->write(map_cmd, sizeof(map_cmd)) == i2c::ERROR_OK) {
          this->set_timeout(this->read_delay_ms_, [this]() {
            uint8_t map_response[3];
            if (this->read(map_response, 3) == i2c::ERROR_OK && 
                this->validate_checksum_(map_response, 2, map_response[2]) &&
                (map_response[0] & STATUS_COMPLETE_BIT)) {
              this->diagnostic_data_.memory_map_id = map_response[1];
              ESP_LOGD(TAG, "Memory map: 0x%02X", this->diagnostic_data_.memory_map_id);
            }
            
            // Read serial number (0x28-0x2B in RAM, 4 bytes)
            uint8_t serial_cmd[] = {0x24, 0x00, 0x28, 0x50};
            if (this->write(serial_cmd, sizeof(serial_cmd)) == i2c::ERROR_OK) {
              this->set_timeout(this->read_delay_ms_, [this]() {
                uint8_t serial_response[6];  // status + 4 data bytes + checksum
                if (this->read(serial_response, 6) == i2c::ERROR_OK && 
                    this->validate_checksum_(serial_response, 5, serial_response[5]) &&
                    (serial_response[0] & STATUS_COMPLETE_BIT)) {
                  this->diagnostic_data_.serial_number = 
                    (static_cast<uint32_t>(serial_response[1]) << 24) |
                    (static_cast<uint32_t>(serial_response[2]) << 16) |
                    (static_cast<uint32_t>(serial_response[3]) << 8) |
                    static_cast<uint32_t>(serial_response[4]);
                  ESP_LOGD(TAG, "Serial number: %08X", this->diagnostic_data_.serial_number);
                }
                
                // Diagnostics complete
                this->setup_step_ = SETUP_DONE;
                this->setup_success_ = true;
                if (this->abc_config_pending_) {
                  ESP_LOGW(TAG, "ABC changes written - POWER CYCLE SENSOR to activate!");
                }
              });
            } else {
              this->setup_step_ = SETUP_DONE;
              this->setup_success_ = true;
              if (this->abc_config_pending_) {
                ESP_LOGW(TAG, "ABC changes written - POWER CYCLE SENSOR to activate!");
              }
            }
          });
        } else {
          this->setup_step_ = SETUP_DONE;
          this->setup_success_ = true;
          if (this->abc_config_pending_) {
            ESP_LOGW(TAG, "ABC changes written - POWER CYCLE SENSOR to activate!");
          }
        }
      });
    } else {
      this->setup_step_ = SETUP_DONE;
      this->setup_success_ = true;
      if (this->abc_config_pending_) {
        ESP_LOGW(TAG, "ABC changes written - POWER CYCLE SENSOR to activate!");
      }
    }
  });
}

void SenseairI2CSensor::update() {
  // Check if setup completed successfully
  if (!this->setup_success_) {
    ESP_LOGW(TAG, "Setup incomplete or failed, skipping measurement");
    return;
  }
  
  // Warn about pending ABC configuration on every update
  if (this->abc_config_pending_) {
    ESP_LOGW(TAG, "ABC configuration pending - power cycle sensor to activate");
  }
  
  if (this->measure_step_ != MEASURE_IDLE) {
    ESP_LOGV(TAG, "Measurement already in progress, skipping update");
    return;
  }
  
  ESP_LOGV(TAG, "Starting CO2 measurement");
  this->measure_step_ = MEASURE_WRITE;
  this->measure_write_retry_count_ = 0;
  this->attempt_measurement_();
}

void SenseairI2CSensor::attempt_measurement_() {
  if (this->measure_step_ == MEASURE_WRITE) {
    // Send measurement command
    auto error = this->write(SENSEAIR_MEASURE_CMD, sizeof(SENSEAIR_MEASURE_CMD));
    if (error != i2c::ERROR_OK) {
      ESP_LOGV(TAG, "Measurement write error: %d", error);
      this->handle_retry_([this]() { this->attempt_measurement_(); }, 
                          this->measure_write_retry_count_, "Measurement write",
                          [this]() { 
                            this->measure_step_ = MEASURE_IDLE;
                            this->publish_state(NAN);  // Publish NaN on failure
                          });
      return;
    }
    
    // Reset read retry counter when starting read phase
    this->measure_read_retry_count_ = 0;
    this->measure_step_ = MEASURE_READ;
    // Use configurable delay to allow sensor time to prepare response
    this->set_timeout(this->read_delay_ms_, [this]() { this->attempt_measurement_(); });
    return;
  }
  
  if (this->measure_step_ == MEASURE_READ) {
    // Read measurement response: [status, data_msb, data_lsb, checksum]
    auto error = this->read(this->measure_data_, 4);
    if (error != i2c::ERROR_OK) {
      ESP_LOGV(TAG, "Measurement read error: %d", error);
      this->handle_retry_([this]() { 
        // Retry the read without resending command
        this->attempt_measurement_(); 
      }, this->measure_read_retry_count_, "Measurement read",
      [this]() { 
        this->measure_step_ = MEASURE_IDLE;
        this->publish_state(NAN);  // Publish NaN on failure
      });
      return;
    }
    
    ESP_LOGVV(TAG, "Measurement raw: 0x%02X 0x%02X 0x%02X 0x%02X",
              this->measure_data_[0], this->measure_data_[1], 
              this->measure_data_[2], this->measure_data_[3]);
    
    // Check if measurement is ready (bit 0 of status should be 1)
    if ((this->measure_data_[0] & STATUS_COMPLETE_BIT) != STATUS_COMPLETE_BIT) {
      ESP_LOGW(TAG, "Measurement not ready (status: 0x%02X)", this->measure_data_[0]);
      this->measure_step_ = MEASURE_IDLE;
      return;
    }
    
    // Validate checksum (sum of first 3 bytes should equal 4th byte)
    if (!this->validate_checksum_(this->measure_data_, 3, this->measure_data_[3])) {
      ESP_LOGE(TAG, "Measurement checksum validation failed (calc=0x%02X, recv=0x%02X)",
               this->calculate_checksum_(this->measure_data_, 3), this->measure_data_[3]);
      this->measure_step_ = MEASURE_IDLE;
      this->publish_state(NAN);
      return;
    }
    
    // Extract CO2 value as signed integer (big-endian: MSB first)
    // Per documentation: negative values possible during calibration/nitrogen test
    int16_t co2_ppm = (static_cast<int16_t>(this->measure_data_[1]) << 8) | this->measure_data_[2];
    
    // Validate CO2 value range
    if (!this->validate_co2_value_(co2_ppm)) {
      ESP_LOGW(TAG, "CO2 value out of valid range: %d ppm", co2_ppm);
      this->measure_step_ = MEASURE_IDLE;
      this->publish_state(NAN);
      return;
    }
    
    ESP_LOGD(TAG, "CO2: %d ppm", co2_ppm);
    
    this->publish_state(static_cast<float>(co2_ppm));
    this->measure_step_ = MEASURE_IDLE;
  }
}

// ============================================================================
// Manual Calibration Actions
// ============================================================================

void SenseairI2CSensor::background_calibration() {
  if (this->calibration_step_ != CAL_IDLE) {
    ESP_LOGW(TAG, "Calibration already in progress, ignoring request");
    return;
  }
  
  ESP_LOGW(TAG, "Starting background calibration - sensor assumes current environment is 400ppm");
  ESP_LOGW(TAG, "Ensure sensor is in fresh air for accurate calibration!");
  
  // Background calibration command: write 0x7C06 to address 0x67 (K30)
  this->perform_calibration_command_(CALIBRATION_ADDR_K30, 0x7C06, "background calibration");
}

void SenseairI2CSensor::abc_get_period() {
  if (this->calibration_step_ != CAL_IDLE) {
    ESP_LOGW(TAG, "Calibration operation in progress, ignoring request");
    return;
  }
  
  ESP_LOGI(TAG, "Reading ABC period from sensor");
  this->calibration_step_ = CAL_READ_ABC_PERIOD;
  this->calibration_retry_count_ = 0;
  this->read_abc_period_();
}

void SenseairI2CSensor::perform_calibration_command_(uint16_t address, uint16_t command, 
                                                     const char* name) {
  // Build write command: [cmd, addr_msb, addr_lsb, data_msb, data_lsb, checksum]
  // 0x12 = Write RAM, 2 bytes
  uint8_t cal_cmd[] = {
    0x12,  // Write RAM, 2 bytes
    static_cast<uint8_t>((address >> 8) & 0xFF),   // Address MSB
    static_cast<uint8_t>(address & 0xFF),          // Address LSB
    static_cast<uint8_t>((command >> 8) & 0xFF),   // Command MSB
    static_cast<uint8_t>(command & 0xFF),          // Command LSB
    0x00   // Checksum placeholder
  };
  cal_cmd[5] = this->calculate_checksum_(cal_cmd, 5);
  
  ESP_LOGD(TAG, "Calibration command (%s): 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
           name, cal_cmd[0], cal_cmd[1], cal_cmd[2], cal_cmd[3], cal_cmd[4], cal_cmd[5]);
  
  this->calibration_step_ = CAL_WRITE;
  
  auto error = this->write(cal_cmd, sizeof(cal_cmd));
  if (error != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Calibration command write failed: %d", error);
    this->calibration_step_ = CAL_IDLE;
    return;
  }
  
  // Wait and read response
  this->calibration_step_ = CAL_READ_RESPONSE;
  this->set_timeout(this->read_delay_ms_, [this, name]() {
    uint8_t response_data[2];
    auto read_err = this->read(response_data, 2);
    if (read_err != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "Calibration response read failed: %d", read_err);
      this->calibration_step_ = CAL_IDLE;
      return;
    }
    
    ESP_LOGD(TAG, "Calibration response: status=0x%02X checksum=0x%02X",
             response_data[0], response_data[1]);
    
    // Validate checksum
    if (!this->validate_checksum_(response_data, 1, response_data[1])) {
      ESP_LOGE(TAG, "Calibration response checksum mismatch");
      this->calibration_step_ = CAL_IDLE;
      return;
    }
    
    // Check status
    if ((response_data[0] & STATUS_COMPLETE_BIT) != STATUS_COMPLETE_BIT) {
      ESP_LOGW(TAG, "Calibration command incomplete (status: 0x%02X)", response_data[0]);
      this->calibration_step_ = CAL_IDLE;
      return;
    }
    
    ESP_LOGI(TAG, "Calibration command (%s) completed successfully", name);
    ESP_LOGI(TAG, "Wait for sensor measurement cycle (~8 seconds) before taking readings");
    this->calibration_step_ = CAL_IDLE;
  });
}

void SenseairI2CSensor::read_abc_period_() {
  // Read ABC period from EEPROM address 0x40 (2 bytes)
  // Command: [cmd, addr_msb, addr_lsb, checksum]
  // 0x42 = Read EEPROM, 2 bytes
  uint8_t read_cmd[] = {0x42, 0x00, 0x40, 0x82};
  
  auto error = this->write(read_cmd, sizeof(read_cmd));
  if (error != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "ABC period read command failed: %d", error);
    this->calibration_step_ = CAL_IDLE;
    return;
  }
  
  // Wait and read response: [status, data_msb, data_lsb, checksum]
  this->set_timeout(this->read_delay_ms_, [this]() {
    uint8_t response_data[4];
    auto read_err = this->read(response_data, 4);
    if (read_err != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "ABC period read response failed: %d", read_err);
      this->calibration_step_ = CAL_IDLE;
      return;
    }
    
    ESP_LOGD(TAG, "ABC period response: 0x%02X 0x%02X 0x%02X 0x%02X",
             response_data[0], response_data[1], response_data[2], response_data[3]);
    
    // Validate checksum
    if (!this->validate_checksum_(response_data, 3, response_data[3])) {
      ESP_LOGE(TAG, "ABC period response checksum mismatch");
      this->calibration_step_ = CAL_IDLE;
      return;
    }
    
    // Check status
    if ((response_data[0] & STATUS_COMPLETE_BIT) != STATUS_COMPLETE_BIT) {
      ESP_LOGW(TAG, "ABC period read incomplete (status: 0x%02X)", response_data[0]);
      this->calibration_step_ = CAL_IDLE;
      return;
    }
    
    // Extract ABC period (big-endian, units in hours)
    uint16_t abc_hours = (static_cast<uint16_t>(response_data[1]) << 8) | response_data[2];
    
    if (abc_hours == 0) {
      ESP_LOGI(TAG, "ABC Period: DISABLED (0 hours)");
    } else {
      ESP_LOGI(TAG, "ABC Period: %u hours (%.1f days)", abc_hours, abc_hours / 24.0);
    }
    
    this->calibration_step_ = CAL_IDLE;
  });
}

// ============================================================================
// Helper Methods
// ============================================================================

bool SenseairI2CSensor::validate_co2_value_(int16_t co2_ppm) const {
  // Per documentation: negative values are possible during calibration
  // but should be within reasonable bounds
  if (co2_ppm < CO2_MIN_VALID || co2_ppm > CO2_MAX_VALID) {
    return false;
  }
  return true;
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
  return sum & 0xFF;  // Truncate to 8 bits (byte addition with overflow)
}

void SenseairI2CSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Senseair I2C CO2 Sensor:");
  LOG_I2C_DEVICE(this);
  LOG_SENSOR("  ", "CO2", this);
  
  if (this->setup_success_) {
    ESP_LOGCONFIG(TAG, "  Setup: SUCCESS");
  } else {
    ESP_LOGCONFIG(TAG, "  Setup: FAILED - measurements disabled");
  }
  
  // Display diagnostics if available
  if (this->diagnostic_data_.valid) {
    ESP_LOGCONFIG(TAG, "  Diagnostics:");
    ESP_LOGCONFIG(TAG, "    Firmware: Type 0x%02X, Version %u.%u", 
                  this->diagnostic_data_.firmware_type,
                  this->diagnostic_data_.firmware_main,
                  this->diagnostic_data_.firmware_sub);
    ESP_LOGCONFIG(TAG, "    Serial Number: %08X", this->diagnostic_data_.serial_number);
    ESP_LOGCONFIG(TAG, "    Memory Map: 0x%02X", this->diagnostic_data_.memory_map_id);
    
    if (this->diagnostic_data_.error_status == 0) {
      ESP_LOGCONFIG(TAG, "    Error Status: OK (0x00)");
    } else {
      ESP_LOGCONFIG(TAG, "    Error Status: 0x%02X (errors detected!)", 
                    this->diagnostic_data_.error_status);
    }
  } else {
    ESP_LOGCONFIG(TAG, "  Diagnostics: Not available");
  }
  
  ESP_LOGCONFIG(TAG, "  ABC: %s", 
                this->abc_interval_ > 0 ? "enabled" : "disabled");
  if (this->abc_interval_ > 0) {
    uint32_t abc_hours = this->abc_interval_ / 3600;
    if (abc_hours > ABC_INTERVAL_MAX_HOURS) {
      ESP_LOGCONFIG(TAG, "    Interval: %u hours (clamped from %u)", 
                    ABC_INTERVAL_MAX_HOURS, abc_hours);
    } else {
      ESP_LOGCONFIG(TAG, "    Interval: %u hours", abc_hours);
    }
  }
  
  if (this->abc_config_pending_) {
    ESP_LOGCONFIG(TAG, "  ABC Config: PENDING - power cycle sensor to activate!");
  }
  
  ESP_LOGCONFIG(TAG, "  Timing:");
  ESP_LOGCONFIG(TAG, "    Read delay: %ums", this->read_delay_ms_);
  ESP_LOGCONFIG(TAG, "    Retry delay: %ums", this->retry_delay_ms_);
  ESP_LOGCONFIG(TAG, "    Max retries: %u", this->max_retries_);
  
}

}  // namespace senseair_i2c
}  // namespace esphome