#pragma once

#include "esphome/core/automation.h"
#include "sd_spi_card.h"

namespace esphome {
namespace sd_spi_card {

// Trigger that fires when SD card is mounted
class SdSpiCardMountTrigger : public Trigger<> {
public:
  explicit SdSpiCardMountTrigger(SdSpiCard *parent) {
    parent->add_on_mount_callback([this]() { this->trigger(); });
  }
};

template<typename... Ts>
class AppendFileAction : public Action<Ts...> {
public:
  explicit AppendFileAction(SdSpiCard *parent) : parent_(parent) {}
  
  TEMPLATABLE_VALUE(std::string, path)
    TEMPLATABLE_VALUE(std::string, content)
    
    void play(const Ts &...x) override {
      auto path_str = this->path_.value(x...);
      auto content_str = this->content_.value(x...);
      
      if (path_str.empty()) {
        ESP_LOGW("sd_spi_card.action", "Path is empty, skipping write");
        return;
      }
      
      std::vector<uint8_t> data(content_str.begin(), content_str.end());
      auto result = this->parent_->append_file(path_str, data);
      
      if (result != WriteResult::SUCCESS) {
        ESP_LOGW("sd_spi_card.action", "Failed to append to file: %s", path_str.c_str());
      }
    }
  
protected:
  SdSpiCard *parent_;
};

template<typename... Ts>
class WriteFileAction : public Action<Ts...> {
public:
  explicit WriteFileAction(SdSpiCard *parent) : parent_(parent) {}
  
  TEMPLATABLE_VALUE(std::string, path)
    TEMPLATABLE_VALUE(std::string, content)
    
    void play(const Ts &...x) override {
      auto path_str = this->path_.value(x...);
      auto content_str = this->content_.value(x...);
      
      if (path_str.empty()) {
        ESP_LOGW("sd_spi_card.action", "Path is empty, skipping write");
        return;
      }
      
      std::vector<uint8_t> data(content_str.begin(), content_str.end());
      auto result = this->parent_->write_file(path_str, data);
      
      if (result != WriteResult::SUCCESS) {
        ESP_LOGW("sd_spi_card.action", "Failed to write file: %s", path_str.c_str());
      }
    }
  
protected:
  SdSpiCard *parent_;
};

template<typename... Ts>
class SyncAction : public Action<Ts...> {
public:
  explicit SyncAction(SdSpiCard *parent) : parent_(parent) {}
  
  void play(const Ts &...x) override {
    this->parent_->sync();
  }
  
protected:
  SdSpiCard *parent_;
};

template<typename... Ts>
class CreateFileAction : public Action<Ts...> {
public:
  explicit CreateFileAction(SdSpiCard *parent) : parent_(parent) {}
  
  TEMPLATABLE_VALUE(std::string, path)
    TEMPLATABLE_VALUE(std::string, content)
    
    void play(const Ts &...x) override {
      auto path_str = this->path_.value(x...);
      auto content_str = this->content_.value(x...);
      
      if (path_str.empty()) {
        ESP_LOGW("sd_spi_card.action", "Path is empty, skipping file creation");
        return;
      }
      
      std::vector<uint8_t> data(content_str.begin(), content_str.end());
      auto result = this->parent_->create_file(path_str, data);
      
      if (result != WriteResult::SUCCESS && result != WriteResult::NOT_MOUNTED) {
        ESP_LOGW("sd_spi_card.action", "Failed to create file: %s", path_str.c_str());
      }
    }
  
protected:
  SdSpiCard *parent_;
};

template<typename... Ts>
class MountAction : public Action<Ts...> {
public:
  explicit MountAction(SdSpiCard *parent) : parent_(parent) {}
  
  void play(const Ts &...x) override {
    if (!this->parent_->mount()) {
      ESP_LOGW("sd_spi_card.action", "Failed to mount SD card");
    }
  }
  
protected:
  SdSpiCard *parent_;
};

}  // namespace sd_spi_card
}  // namespace esphome