// Runtime I2C bus recovery for ESP-IDF
// ESPHome only performs bus recovery once during boot, so a device left
// mid-transaction (e.g. a clock-stretching target aborted by the 13ms
// hardware timeout) wedges the bus until it gives up on its own. This
// component exposes the IDF i2c_master_bus_reset() routine, which clocks
// out a stuck target (up to 9 SCL pulses + STOP) and resets the peripheral.

#pragma once

#include "esphome/core/component.h"

#include <driver/i2c_master.h>

namespace esphome::i2c_recovery {

class I2CRecovery : public Component {
 public:
  void dump_config() override;
  void set_port(uint8_t port) { this->port_ = port; }
  void set_min_interval(uint32_t min_interval_ms) { this->min_interval_ms_ = min_interval_ms; }

  // Clock out any device stuck mid-transaction and reset the I2C peripheral.
  // Rate-limited by min_interval. Returns true if a reset ran and succeeded.
  bool reset_bus();

 protected:
  uint8_t port_{0};
  uint32_t min_interval_ms_{10000};
  uint32_t last_reset_ms_{0};
  bool has_reset_{false};
};

}  // namespace esphome::i2c_recovery
