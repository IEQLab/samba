#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace esphome {
namespace sd_spi_card {

class SdSpiCardMountTrigger;  // Forward declaration

enum class WriteResult {
  SUCCESS,
  NOT_MOUNTED,
  FILE_ERROR,
  WRITE_ERROR
};

class SdSpiCard : public Component {
public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }
  
  bool is_mounted() const { return mounted_; }
  void dump_info();
  
  // Check if card is still present and remount if needed
  bool check_and_remount();
  
  void set_pins(int clk, int mosi, int miso, int cs) {
    clk_pin_ = (gpio_num_t)clk;
    mosi_pin_ = (gpio_num_t)mosi;
    miso_pin_ = (gpio_num_t)miso;
    cs_pin_   = (gpio_num_t)cs;
  }
  
  void set_mount_point(const std::string &path) { mount_point_ = path; }
  
  // Register mount trigger
  void add_on_mount_callback(std::function<void()> &&callback) {
    this->mount_callback_.add(std::move(callback));
  }
  
  // Improved file helpers with error reporting
  WriteResult write_file(const std::string &path, const std::vector<uint8_t> &data);
  WriteResult append_file(const std::string &path, const std::vector<uint8_t> &data);
  WriteResult create_file(const std::string &path, const std::vector<uint8_t> &data);
  
  // Check if file exists
  bool file_exists(const std::string &path);
  
  // Ensure data is flushed to card
  void sync();
  
protected:
  bool mount_card_();
  void unmount_card_();
  
private:
  bool mounted_{false};
  std::string mount_point_{"/sd"};
  sdmmc_card_t *card_{nullptr};
  
  uint32_t last_check_millis_{0};
  uint32_t check_interval_ms_{300000};  // Check card every 5 minutes
  uint32_t failed_writes_{0};
  static constexpr uint32_t MAX_FAILED_WRITES = 3;
  
  gpio_num_t clk_pin_{GPIO_NUM_NC};
  gpio_num_t mosi_pin_{GPIO_NUM_NC};
  gpio_num_t miso_pin_{GPIO_NUM_NC};
  gpio_num_t cs_pin_{GPIO_NUM_NC};
  
  spi_host_device_t spi_host_{HSPI_HOST};
  bool spi_initialized_{false};
  
  CallbackManager<void()> mount_callback_;
};

}  // namespace sd_spi_card
}  // namespace esphome