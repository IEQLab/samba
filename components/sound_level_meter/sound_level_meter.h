#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include "esp_timer.h"

#include "esphome/core/application.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/microphone/microphone_source.h"

#include "stats_window.h"

#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif

#ifdef USE_ESP_DSP
#include "dsps_biquad.h"
#endif

namespace esphome::sound_level_meter {
class SoundLevelMeterProcessor;
class Filter;
template<typename T> class BufferStack;

class SoundLevelMeter : public Component
#ifdef USE_OTA_STATE_LISTENER
    ,
                        public ota::OTAGlobalStateListener
#endif
{
  friend class SoundLevelMeterProcessor;
  friend class SoundLevelMeterSensorMax;
  friend class SoundLevelMeterSensorMin;

 public:
  void set_update_interval(uint32_t update_interval);
  uint32_t get_update_interval();
  void set_ring_buffer_size(uint32_t ring_buffer_size);
  uint32_t get_ring_buffer_size();
  void set_microphone_source(microphone::MicrophoneSource *microphone_source);
  void set_warmup_interval(uint32_t warmup_interval);
  void set_task_stack_size(uint32_t task_stack_size);
  void set_task_priority(uint8_t task_priority);
  void set_task_core(uint8_t task_core);
  void set_mic_sensitivity(optional<float> mic_sensitivity);
  optional<float> get_mic_sensitivity();
  void set_mic_sensitivity_ref(optional<float> mic_sensitivity_ref);
  optional<float> get_mic_sensitivity_ref();
  void set_offset(optional<float> offset);
  optional<float> get_offset();
  void set_is_high_freq(bool is_high_freq);
  void set_is_auto_start(bool is_auto_start);
  void add_processor(SoundLevelMeterProcessor *processor);
  void add_dsp_filter(Filter *dsp_filter);
  virtual void setup() override;
  virtual void loop() override;
  virtual void dump_config() override;
  void start();
  void stop();
  bool is_running();

#ifdef USE_OTA_STATE_LISTENER
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif

 protected:
  microphone::MicrophoneSource *microphone_source_{nullptr};
  std::vector<Filter *> dsp_filters_;
  std::vector<SoundLevelMeterProcessor *> processors_;
  size_t ring_buffer_size_ms_{256};
  uint32_t warmup_interval_ms_{500};
  uint32_t task_stack_size_{1024};
  uint8_t task_priority_{1};
  uint8_t task_core_{1};
  optional<float> mic_sensitivity_{};
  optional<float> mic_sensitivity_ref_{};
  optional<float> offset_{};
  // Guards every hand-off slot between the audio task and loop(): each processor's latest
  // values and utilisation_ below. Nothing is queued, so nothing is allocated per value.
  std::mutex publish_mutex_;
  struct Utilisation {
    float cpu{0.f};
    float ring_buffer{0.f};
    uint8_t core{0};
    bool pending{false};
  } utilisation_;
  // Set by the microphone callback, logged and cleared by loop()
  std::atomic<bool> ring_buffer_overrun_{false};
  uint32_t last_overrun_log_{0};
  uint32_t update_interval_ms_{60000};
  bool is_running_{false};
  bool was_running_before_ota_{false};
  bool is_pending_stop_{false};
  bool is_high_freq_{false};
  bool is_auto_start_{true};
  HighFrequencyLoopRequester high_freq_;
  std::shared_ptr<ring_buffer::RingBuffer> ring_buffer_;
  std::weak_ptr<ring_buffer::RingBuffer> ring_buffer_weak_;
  size_t ring_buffer_stats_free_{SIZE_MAX};
  TaskHandle_t task_handle_{nullptr};

  audio::AudioStreamInfo get_audio_stream_info() const;
  uint32_t ms_to_frames(uint32_t ms);
  void sort_processors();
  size_t read_samples(std::vector<float> &data, TickType_t ticks_to_wait = portMAX_DELAY);
  void process(BufferStack<float> &buffers);
  void reset();

  static void task(void *param);
};

// Anything fed from the filter chain in the audio task. process() and reset() run in the audio
// task, publish_pending() in the main loop; they meet only in Slots guarded by the parent's
// publish_mutex_, so the audio task never touches the scheduler or a sensor's callbacks.
class SoundLevelMeterProcessor {
  friend SoundLevelMeter;

 public:
  void set_parent(SoundLevelMeter *parent);
  void set_update_interval(uint32_t update_interval);
  void add_dsp_filter(Filter *dsp_filter);
  // Main task, from SoundLevelMeter::setup(): the one place a processor may allocate
  virtual void setup() {}
  virtual void process(std::vector<float> &buffer) = 0;
  virtual void publish_pending() = 0;
  virtual void dump_config() = 0;

 protected:
  // Latest value wins: a value the main loop has not yet published is overwritten
  struct Slot {
    float value{NAN};
    bool pending{false};
  };

  SoundLevelMeter *parent_{nullptr};
  std::vector<Filter *> dsp_filters_;
  uint32_t update_samples_{0};
  uint32_t update_interval_ms_{60000};
  float adjust_dB(float dB, bool is_rms = true);
  void hand_off_(Slot &slot, float value);
  bool take_(Slot &slot, float &value);

  virtual void reset() = 0;
};

class SoundLevelMeterSensor : public SoundLevelMeterProcessor, public sensor::Sensor {
 public:
  void publish_pending() override;
  void dump_config() override;

 protected:
  Slot slot_;

  void defer_publish_state(float state) { this->hand_off_(this->slot_, state); }
};

// LAeq, LA90, LA10 (and LA05) over a sliding window of block levels, one block per update
// interval. The window is owned by the audio task alone: it is fed every block and computes the
// statistics there, handing only the results to the main loop.
class SoundLevelMeterStats : public SoundLevelMeterProcessor {
 public:
  void set_window_blocks(uint16_t window_blocks) { this->window_blocks_ = window_blocks; }
  void set_send_every(uint16_t send_every) { this->send_every_ = send_every; }
  void set_send_first_at(uint16_t send_first_at) { this->send_first_at_ = send_first_at; }
  void set_leq_sensor(sensor::Sensor *sensor) { this->sensors_[STAT_LEQ] = sensor; }
  void set_l90_sensor(sensor::Sensor *sensor) { this->sensors_[STAT_L90] = sensor; }
  void set_l10_sensor(sensor::Sensor *sensor) { this->sensors_[STAT_L10] = sensor; }
  void set_l05_sensor(sensor::Sensor *sensor) { this->sensors_[STAT_L05] = sensor; }
  void setup() override;
  void process(std::vector<float> &buffer) override;
  void publish_pending() override;
  void dump_config() override;

 protected:
  enum Stat : uint8_t { STAT_LEQ, STAT_L90, STAT_L10, STAT_L05, STAT_COUNT };

  std::array<sensor::Sensor *, STAT_COUNT> sensors_{};
  std::array<Slot, STAT_COUNT> slots_{};
  StatsWindow window_;
  uint16_t window_blocks_{0};
  uint16_t send_every_{0};     // blocks between outputs
  uint16_t send_first_at_{0};  // blocks before the first output
  uint16_t blocks_to_send_{0};
  double sum_{0.};
  uint32_t count_{0};

  void publish_window_();
  void reset() override;
};

class SoundLevelMeterSensorEq : public SoundLevelMeterSensor {
 public:
  virtual void process(std::vector<float> &buffer) override;

 protected:
  double sum_{0.};
  uint32_t count_{0};

  virtual void reset() override;
};

class SoundLevelMeterSensorMax : public SoundLevelMeterSensor {
 public:
  void set_window_size(uint32_t window_size);
  virtual void process(std::vector<float> &buffer) override;

 protected:
  uint32_t window_samples_{0};
  float sum_{0.f};
  float max_{std::numeric_limits<float>::min()};
  uint32_t count_sum_{0}, count_max_{0};

  virtual void reset() override;
};

class SoundLevelMeterSensorMin : public SoundLevelMeterSensor {
 public:
  void set_window_size(uint32_t window_size);
  virtual void process(std::vector<float> &buffer) override;

 protected:
  uint32_t window_samples_{0};
  float sum_{0.f};
  float min_{std::numeric_limits<float>::max()};
  uint32_t count_sum_{0}, count_min_{0};

  virtual void reset() override;
};

class SoundLevelMeterSensorPeak : public SoundLevelMeterSensor {
 public:
  virtual void process(std::vector<float> &buffer) override;

 protected:
  float peak_{0.f};
  uint32_t count_{0};

  virtual void reset() override;
};

class Filter {
  friend SoundLevelMeter;

 public:
  virtual void process(std::vector<float> &data) = 0;

 protected:
  virtual void reset() = 0;
};

class SOS_Filter : public Filter {
 public:
  SOS_Filter(std::initializer_list<std::initializer_list<float>> &&coeffs);
  virtual void process(std::vector<float> &data) override;

 protected:
  std::vector<std::array<float, 5>> coeffs_;  // {b0, b1, b2, a1, a2}
  std::vector<std::array<float, 2>> state_;

  virtual void reset() override;
};

// All max_depth + 1 buffers are allocated up front, so push() never allocates
template<typename T> class BufferStack {
 public:
  BufferStack(uint32_t buffer_size, uint32_t max_depth);
  std::vector<T> &current();
  void push();
  void pop();
  void reset();
  operator std::vector<T> &();

 private:
  uint32_t buffer_size_;
  uint32_t index_{0};
  std::vector<std::vector<T>> buffers_;
};

template<typename... Ts> class StartAction : public Action<Ts...> {
 public:
  explicit StartAction(SoundLevelMeter *sound_level_meter) : sound_level_meter_(sound_level_meter) {}

  void play(const Ts &...x) override { this->sound_level_meter_->start(); }

 protected:
  SoundLevelMeter *sound_level_meter_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(SoundLevelMeter *sound_level_meter) : sound_level_meter_(sound_level_meter) {}

  void play(const Ts &...x) override { this->sound_level_meter_->stop(); }

 protected:
  SoundLevelMeter *sound_level_meter_;
};

}  // namespace esphome::sound_level_meter