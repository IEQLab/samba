#include "i2c_recovery.h"

#include <cinttypes>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::i2c_recovery {

static const char *const TAG = "i2c_recovery";

void I2CRecovery::dump_config() {
  ESP_LOGCONFIG(TAG, "I2C Recovery:");
  ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG, "  Min interval: %" PRIu32 "ms", this->min_interval_ms_);
}

bool I2CRecovery::reset_bus() {
  const uint32_t now = millis();
  if (this->has_reset_ && (now - this->last_reset_ms_) < this->min_interval_ms_) {
    ESP_LOGD(TAG, "Skipping reset, last was %" PRIu32 "ms ago", now - this->last_reset_ms_);
    return false;
  }
  this->has_reset_ = true;
  this->last_reset_ms_ = now;

  i2c_master_bus_handle_t handle = nullptr;
  esp_err_t err = i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(this->port_), &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get bus handle for port %u: %s", this->port_, esp_err_to_name(err));
    return false;
  }

  ESP_LOGW(TAG, "Resetting I2C bus on port %u", this->port_);
  err = i2c_master_bus_reset(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bus reset failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "Bus reset complete");
  return true;
}

}  // namespace esphome::i2c_recovery
