#include "sd_spi_card.h"
#include "esphome/core/log.h"
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include "ff.h"  // For FATFS type detection

namespace esphome {
namespace sd_spi_card {

static const char *TAG = "sd_spi_card";

void SdSpiCard::setup() {
  ESP_LOGI(TAG, "Setting up SD card over SPI...");
  
  if (clk_pin_ == GPIO_NUM_NC || mosi_pin_ == GPIO_NUM_NC ||
      miso_pin_ == GPIO_NUM_NC || cs_pin_ == GPIO_NUM_NC) {
    ESP_LOGE(TAG, "Pins not configured!");
    this->mark_failed();
    return;
  }
  
  if (auto_mount_) {
    if (!this->mount_card_()) {
      ESP_LOGW(TAG, "Initial mount failed - will retry periodically");
      // Don't mark as failed - allow loop() to retry mounting
    }
  } else {
    ESP_LOGI(TAG, "Auto-mount disabled - use mount action to mount card");
  }
}

bool SdSpiCard::mount_card_() {
  if (mounted_) {
    ESP_LOGW(TAG, "Card already mounted");
    return true;
  }
  
  ESP_LOGI(TAG, "Mounting SD card...");
  
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = spi_host_;
  
  // Only initialize SPI bus once
  if (!spi_initialized_) {
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = mosi_pin_;
    bus_cfg.miso_io_num = miso_pin_;
    bus_cfg.sclk_io_num = clk_pin_;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;
    
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
      return false;
    }
    spi_initialized_ = true;
  }
  
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = cs_pin_;
  slot_config.host_id = (spi_host_device_t)host.slot;
  
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024};
  
  esp_err_t ret = esp_vfs_fat_sdspi_mount(mount_point_.c_str(), &host, 
                                          &slot_config, &mount_config, &card_);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem.");
      ESP_LOGE(TAG, "Card may not be formatted, or uses unsupported format.");
      ESP_LOGE(TAG, "Supported formats: FAT16, FAT32");
      ESP_LOGE(TAG, "NOT supported: exFAT, NTFS");
      ESP_LOGE(TAG, "Please format the card as FAT32 (will erase all data)");
    } else {
      ESP_LOGE(TAG, "Failed to mount card: %s", esp_err_to_name(ret));
    }
    return false;
  }
  
  ESP_LOGI(TAG, "SD card mounted at %s", mount_point_.c_str());
  mounted_ = true;
  failed_writes_ = 0;
  
  // Detect and log filesystem type
  FATFS *fs;
  DWORD free_clusters;
  if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
    const char *fs_type = "Unknown";
    switch (fs->fs_type) {
    case FS_FAT12: fs_type = "FAT12"; break;
    case FS_FAT16: fs_type = "FAT16"; break;
    case FS_FAT32: fs_type = "FAT32"; break;
    case FS_EXFAT: fs_type = "exFAT (unsupported)"; break;
    }
    ESP_LOGI(TAG, "Filesystem type: %s", fs_type);
    
    if (fs->fs_type == FS_EXFAT) {
      ESP_LOGW(TAG, "exFAT detected! This may cause issues.");
      ESP_LOGW(TAG, "Consider reformatting as FAT32 for best compatibility.");
    }
  }
  
  // Trigger on_mount callbacks
  this->mount_callback_.call();
  
  return true;
}

void SdSpiCard::unmount_card_() {
  if (!mounted_) {
    return;
  }
  
  ESP_LOGI(TAG, "Unmounting SD card...");
  esp_vfs_fat_sdcard_unmount(mount_point_.c_str(), card_);
  mounted_ = false;
  card_ = nullptr;
  
  // Set timer to try remounting sooner (30 seconds)
  last_check_millis_ = millis() - check_interval_ms_ + remount_retry_interval_ms_;
}

void SdSpiCard::loop() {
  // Skip periodic checks if auto_mount is disabled
  if (!auto_mount_) {
    return;
  }
  
  uint32_t now = millis();
  
  // Periodic card presence check
  if (now - last_check_millis_ > check_interval_ms_) {
    last_check_millis_ = now;
    
    if (!mounted_) {
      // Try to remount if we're not mounted
      ESP_LOGD(TAG, "Attempting to remount card...");
      this->mount_card_();
    } else {
      // Verify card is still accessible via direct SDMMC status check
      esp_err_t err = sdmmc_get_status(card_);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Card appears to be removed (err=%s), unmounting...", esp_err_to_name(err));
        this->unmount_card_();
      }
    }
  }
  
  // If too many failed writes, try remounting
  if (failed_writes_ >= MAX_FAILED_WRITES && mounted_) {
    ESP_LOGW(TAG, "Too many failed writes, attempting remount...");
    this->unmount_card_();
    delay(100);
    this->mount_card_();
  }
}

bool SdSpiCard::check_and_remount() {
  if (mounted_) {
    return true;
  }
  return this->mount_card_();
}

bool SdSpiCard::mount() {
  if (mounted_) {
    ESP_LOGI(TAG, "Card already mounted");
    return true;
  }
  
  ESP_LOGI(TAG, "Manually mounting SD card...");
  return this->mount_card_();
}

void SdSpiCard::dump_config() {
  ESP_LOGCONFIG(TAG, "SD SPI Card:");
  ESP_LOGCONFIG(TAG, "  Mount Point: %s", mount_point_.c_str());
  ESP_LOGCONFIG(TAG, "  Auto Mount: %s", auto_mount_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Mounted: %s", mounted_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  CLK: GPIO%d", clk_pin_);
  ESP_LOGCONFIG(TAG, "  MOSI: GPIO%d", mosi_pin_);
  ESP_LOGCONFIG(TAG, "  MISO: GPIO%d", miso_pin_);
  ESP_LOGCONFIG(TAG, "  CS: GPIO%d", cs_pin_);
  if (this->is_failed()) {
    ESP_LOGCONFIG(TAG, "  Status: FAILED");
  }
  
  // Dump card info if mounted
  if (mounted_) {
    this->dump_info();
  }
}

void SdSpiCard::dump_info() {
  if (!mounted_ || !card_) {
    ESP_LOGW(TAG, "No card mounted");
    return;
  }
  
  ESP_LOGCONFIG(TAG, "SD Card Info:");
  ESP_LOGCONFIG(TAG, "  Name: %s", card_->cid.name);
  
  // Calculate actual capacity properly
  uint64_t capacity_bytes = ((uint64_t)card_->csd.capacity) * card_->csd.sector_size;
  uint64_t size_mb = capacity_bytes / (1024 * 1024);
  uint64_t size_gb = capacity_bytes / (1024 * 1024 * 1024);
  
  // Determine card type based on capacity
  std::string type;
  if (size_gb >= 32) {
    type = "SDXC";
  } else if (size_mb > 2048) {
    type = "SDHC";
  } else {
    type = "SDSC";
  }
  ESP_LOGCONFIG(TAG, "  Type: %s", type.c_str());
  
  // Speed in MHz
  ESP_LOGCONFIG(TAG, "  Speed: %.2f MHz", (double)card_->csd.tr_speed / 1000000.0);
  
  // Display size in appropriate units
  if (size_gb > 0) {
    ESP_LOGCONFIG(TAG, "  Size: %llu GB (%llu MB)", 
                  (unsigned long long)size_gb, (unsigned long long)size_mb);
  } else {
    ESP_LOGCONFIG(TAG, "  Size: %llu MB", (unsigned long long)size_mb);
  }
  
  ESP_LOGCONFIG(TAG, "  Sector size: %d bytes", card_->csd.sector_size);
  ESP_LOGCONFIG(TAG, "  Read block length: %d bytes", 1 << card_->csd.read_block_len);
}

WriteResult SdSpiCard::write_file(const std::string &path, 
                                  const std::vector<uint8_t> &data) {
  if (!mounted_) {
    ESP_LOGE(TAG, "Card not mounted, cannot write file");
    failed_writes_++;
    return WriteResult::NOT_MOUNTED;
  }
  
  std::string full_path = mount_point_ + path;
  FILE *f = fopen(full_path.c_str(), "w");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path.c_str());
    failed_writes_++;
    return WriteResult::FILE_ERROR;
  }
  
  size_t written = fwrite(data.data(), 1, data.size(), f);
  
  // Ensure data is flushed to card and check for errors
  int flush_result = fflush(f);
  int sync_result = fsync(fileno(f));
  fclose(f);
  
  if (written != data.size()) {
    ESP_LOGE(TAG, "Write incomplete: %d/%d bytes to %s", 
             (int)written, (int)data.size(), path.c_str());
    failed_writes_++;
    return WriteResult::WRITE_ERROR;
  }
  
  if (flush_result != 0 || sync_result != 0) {
    ESP_LOGE(TAG, "Failed to flush/sync data to %s (flush=%d, sync=%d)", 
             path.c_str(), flush_result, sync_result);
    failed_writes_++;
    return WriteResult::WRITE_ERROR;
  }
  
  ESP_LOGI(TAG, "Wrote %d bytes to %s", (int)data.size(), path.c_str());
  
  failed_writes_ = 0;
  return WriteResult::SUCCESS;
}

bool SdSpiCard::file_exists(const std::string &path) {
  if (!mounted_) {
    return false;
  }
  
  std::string full_path = mount_point_ + path;
  struct stat st;
  return (stat(full_path.c_str(), &st) == 0);
}

WriteResult SdSpiCard::create_file(const std::string &path, 
                                   const std::vector<uint8_t> &data) {
  if (!mounted_) {
    ESP_LOGE(TAG, "Card not mounted, cannot create file");
    failed_writes_++;
    return WriteResult::NOT_MOUNTED;
  }
  
  // Check if file already exists
  if (file_exists(path)) {
    ESP_LOGI(TAG, "File already exists, skipping creation: %s", path.c_str());
    return WriteResult::SUCCESS;
  }
  
  // File doesn't exist, create it
  std::string full_path = mount_point_ + path;
  FILE *f = fopen(full_path.c_str(), "w");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create file: %s", full_path.c_str());
    failed_writes_++;
    return WriteResult::FILE_ERROR;
  }
  
  size_t written = fwrite(data.data(), 1, data.size(), f);
  
  // Ensure data is flushed to card and check for errors
  int flush_result = fflush(f);
  int sync_result = fsync(fileno(f));
  fclose(f);
  
  if (written != data.size()) {
    ESP_LOGE(TAG, "Write incomplete during file creation: %d/%d bytes to %s", 
             (int)written, (int)data.size(), path.c_str());
    failed_writes_++;
    return WriteResult::WRITE_ERROR;
  }
  
  if (flush_result != 0 || sync_result != 0) {
    ESP_LOGE(TAG, "Failed to flush/sync data to %s (flush=%d, sync=%d)", 
             path.c_str(), flush_result, sync_result);
    failed_writes_++;
    return WriteResult::WRITE_ERROR;
  }
  
  ESP_LOGI(TAG, "Created file %s: %d bytes", path.c_str(), (int)data.size());
  
  failed_writes_ = 0;
  return WriteResult::SUCCESS;
}

WriteResult SdSpiCard::append_file(const std::string &path, 
                                   const std::vector<uint8_t> &data) {
  if (!mounted_) {
    ESP_LOGE(TAG, "Card not mounted, cannot append file");
    failed_writes_++;
    return WriteResult::NOT_MOUNTED;
  }
  
  std::string full_path = mount_point_ + path;
  FILE *f = fopen(full_path.c_str(), "a");
  if (!f) {
    ESP_LOGD(TAG, "File not found, creating new: %s", path.c_str());
    f = fopen(full_path.c_str(), "w");
  }
  
  if (!f) {
    ESP_LOGE(TAG, "Failed to open file for appending: %s", path.c_str());
    failed_writes_++;
    return WriteResult::FILE_ERROR;
  }
  
  size_t written = fwrite(data.data(), 1, data.size(), f);
  
  // Ensure data is flushed to card and check for errors
  int flush_result = fflush(f);
  int sync_result = fsync(fileno(f));
  fclose(f);
  
  if (written != data.size()) {
    ESP_LOGE(TAG, "Append incomplete: %d/%d bytes to %s", 
             (int)written, (int)data.size(), path.c_str());
    failed_writes_++;
    return WriteResult::WRITE_ERROR;
  }
  
  if (flush_result != 0 || sync_result != 0) {
    ESP_LOGE(TAG, "Failed to flush/sync data to %s (flush=%d, sync=%d)", 
             path.c_str(), flush_result, sync_result);
    failed_writes_++;
    return WriteResult::WRITE_ERROR;
  }
  
  ESP_LOGI(TAG, "Appended %d bytes to %s", (int)data.size(), path.c_str());
  
  failed_writes_ = 0;
  return WriteResult::SUCCESS;
}

void SdSpiCard::sync() {
  if (mounted_) {
    ::sync();  // Force all buffers to disk
    ESP_LOGD(TAG, "Filesystem synced");
  }
}

}  // namespace sd_spi_card
}  // namespace esphome