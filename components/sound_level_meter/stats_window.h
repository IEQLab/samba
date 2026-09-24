#pragma once

// Sliding-window level statistics with fixed memory: no ESPHome or ESP-IDF dependency, so
// test/stats_test.cpp can exercise it on the host.
//
// Levels are held as uint16_t deci-dB in a ring of `capacity` blocks, and a histogram of 0.1 dB
// bins over 0.0-140.0 dB is kept in step with it (add on insert, remove on evict). Both live in
// one allocation made by init(), which the sound level meter calls once from setup(); push(),
// quantile() and energy_mean() never allocate, sort or copy.
//
// Quantisation: a level is rounded to the nearest 0.1 dB, so each stored value is within 0.05 dB
// of the true one. Rounding is monotonic, so a quantile of the stored values is the rounded
// quantile of the true values, and an energy mean of values each within +-0.05 dB is itself within
// +-0.05 dB (every term is scaled by a factor in [10^-0.005, 10^0.005]). Levels outside 0-140 dB
// are clamped to the range edge, and that is the only other error.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace esphome::sound_level_meter {

class StatsWindow {
 public:
  static constexpr uint16_t INVALID = UINT16_MAX;  // a NaN block; excluded from every statistic
  static constexpr uint16_t MAX_LEVEL = 1400;      // 140.0 dB in deci-dB
  static constexpr size_t NUM_BINS = MAX_LEVEL + 1;

  static uint16_t encode(float level_db) {
    if (!std::isfinite(level_db))
      return INVALID;
    if (level_db <= 0.0f)
      return 0;
    if (level_db >= MAX_LEVEL * 0.1f)
      return MAX_LEVEL;
    return static_cast<uint16_t>(lroundf(level_db * 10.0f));
  }
  static float decode(uint16_t v) { return v * 0.1f; }

  // Capacity is at most 65535 blocks, so every count fits a uint16_t.
  void init(uint16_t capacity) {
    this->storage_.reset(new uint16_t[capacity + NUM_BINS]);
    this->ring_ = this->storage_.get();
    this->hist_ = this->ring_ + capacity;
    this->capacity_ = capacity;
    this->clear();
  }

  void clear() {
    for (size_t i = 0; i < NUM_BINS; i++)
      this->hist_[i] = 0;
    this->next_ = this->size_ = this->valid_ = 0;
  }

  void push(float level_db) {
    uint16_t v = encode(level_db);
    if (this->size_ == this->capacity_) {
      // Full: the slot about to be written holds the oldest block
      uint16_t old = this->ring_[this->next_];
      if (old != INVALID) {
        this->hist_[old]--;
        this->valid_--;
      }
    } else {
      this->size_++;
    }
    this->ring_[this->next_] = v;
    if (v != INVALID) {
      this->hist_[v]++;
      this->valid_++;
    }
    if (++this->next_ == this->capacity_)
      this->next_ = 0;
  }

  uint16_t capacity() const { return this->capacity_; }
  uint16_t size() const { return this->size_; }
  uint16_t valid() const { return this->valid_; }

  // Same as ESPHome's `quantile` filter over the valid values: sorted ascending, the value at
  // index ceil(n * q) - 1, computed in float exactly as QuantileFilter::compute_result() does.
  // q must be in (0, 1].
  float quantile(float q) const {
    if (this->valid_ == 0)
      return NAN;
    size_t position = ceilf(this->valid_ * q) - 1;
    size_t seen = 0;
    for (size_t bin = 0; bin < NUM_BINS; bin++) {
      seen += this->hist_[bin];
      if (seen > position)
        return decode(bin);
    }
    return NAN;  // unreachable: the histogram holds valid_ values
  }

  // Energy average 10 log10(mean(10^(L/10))) of the valid blocks, an exact Leq of equal-length
  // blocks. Summed per histogram bin rather than per ring slot: it is the same multiset of values,
  // recomputed from scratch each call so nothing drifts, with at most 1401 exponentials instead of
  // one per block and only for occupied bins.
  float energy_mean() const {
    if (this->valid_ == 0)
      return NAN;
    static constexpr float DECI_DB_TO_LN = 0.023025851f;  // ln(10) / 100
    double sum = 0.;
    for (size_t bin = 0; bin < NUM_BINS; bin++) {
      if (this->hist_[bin] != 0)
        sum += this->hist_[bin] * static_cast<double>(expf(bin * DECI_DB_TO_LN));
    }
    return 10.0f * log10f(static_cast<float>(sum / this->valid_));
  }

 protected:
  std::unique_ptr<uint16_t[]> storage_;
  uint16_t *ring_{nullptr};
  uint16_t *hist_{nullptr};
  uint16_t capacity_{0};
  uint16_t next_{0};   // next slot to write; the oldest block once the ring is full
  uint16_t size_{0};   // blocks held, valid or not
  uint16_t valid_{0};  // blocks held that are not INVALID
};

}  // namespace esphome::sound_level_meter
