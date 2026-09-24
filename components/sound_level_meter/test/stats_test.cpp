// Host test for stats_window.h: sliding window, histogram quantiles and energy mean, checked
// against a brute-force sort of the same window and an exact double-precision energy average.
//
// Run from the repo root:
//   c++ -std=c++17 -O2 -Wall -Wextra -o /tmp/stats_test components/sound_level_meter/test/stats_test.cpp \
//     && /tmp/stats_test
// (If Apple clang reports 'cmath' file not found, add -isystem $(xcrun --show-sdk-path)/usr/include/c++/v1.)
//
// ESPHome does not compile this file: it copies only the top level of an external component.

#include "../stats_window.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

using esphome::sound_level_meter::StatsWindow;

static int failures = 0;
static int checks = 0;

static void expect(bool ok, const char *what, int step, double got, double want) {
  checks++;
  if (!ok) {
    if (failures < 20)
      std::printf("FAIL %s at step %d: got %.6f, want %.6f\n", what, step, got, want);
    failures++;
  }
}

static bool same(float a, float b, float tol) {
  if (std::isnan(a) || std::isnan(b))
    return std::isnan(a) && std::isnan(b);
  return std::fabs(a - b) <= tol;
}

// Brute force over the raw (unquantised) window, mirroring ESPHome's QuantileFilter
struct Reference {
  size_t capacity;
  std::deque<float> window;

  void push(float v) {
    window.push_back(v);
    if (window.size() > capacity)
      window.pop_front();
  }
  std::vector<float> valid(bool quantised) const {
    std::vector<float> out;
    for (float v : window) {
      if (std::isnan(v) || std::isinf(v))
        continue;
      out.push_back(quantised ? StatsWindow::decode(StatsWindow::encode(v)) : std::min(std::max(v, 0.f), 140.f));
    }
    return out;
  }
  float quantile(float q, bool quantised) const {
    std::vector<float> values = valid(quantised);
    if (values.empty())
      return NAN;
    std::sort(values.begin(), values.end());
    size_t position = ceilf(values.size() * q) - 1;
    return values[position];
  }
  float energy_mean(bool quantised) const {
    std::vector<float> values = valid(quantised);
    if (values.empty())
      return NAN;
    double sum = 0.;
    for (float v : values)
      sum += std::pow(10.0, v / 10.0);
    return 10.0 * std::log10(sum / values.size());
  }
};

static const float QS[] = {0.10f, 0.50f, 0.90f, 0.95f, 1.0f, 0.001f};

static void compare(const StatsWindow &w, const Reference &r, int step) {
  expect(w.size() == r.window.size(), "size", step, w.size(), r.window.size());
  expect(w.valid() == r.valid(true).size(), "valid", step, w.valid(), r.valid(true).size());
  for (float q : QS) {
    // Exact against the quantised reference; within 0.05 dB of the raw one
    float got = w.quantile(q);
    expect(same(got, r.quantile(q, true), 1e-6f), "quantile exact", step, got, r.quantile(q, true));
    expect(same(got, r.quantile(q, false), 0.05f + 1e-4f), "quantile bound", step, got, r.quantile(q, false));
  }
  float got = w.energy_mean();
  expect(same(got, r.energy_mean(true), 2e-3f), "energy exact", step, got, r.energy_mean(true));
  expect(same(got, r.energy_mean(false), 0.05f + 2e-3f), "energy bound", step, got, r.energy_mean(false));
}

// Push a sequence through both and compare after every push
static void run(const char *name, uint16_t capacity, const std::vector<float> &levels) {
  int before = failures;
  StatsWindow w;
  w.init(capacity);
  Reference r{capacity, {}};
  compare(w, r, -1);
  for (size_t i = 0; i < levels.size(); i++) {
    w.push(levels[i]);
    r.push(levels[i]);
    compare(w, r, i);
  }
  std::printf("%-28s cap %5u, %6zu pushes: %s\n", name, capacity, levels.size(), failures == before ? "ok" : "FAILED");
}

int main() {
  std::mt19937 rng(20260924);
  std::uniform_real_distribution<float> office(30.f, 80.f);
  std::uniform_real_distribution<float> wide(-20.f, 170.f);
  std::uniform_real_distribution<float> unit(0.f, 1.f);

  // Partial window, never full
  {
    std::vector<float> v;
    for (int i = 0; i < 100; i++)
      v.push_back(office(rng));
    run("partial window", 2400, v);
  }
  // Full-size window, several wraparounds, 2% NaN blocks
  {
    std::vector<float> v;
    for (int i = 0; i < 9000; i++)
      v.push_back(unit(rng) < 0.02f ? NAN : office(rng));
    run("2400 wraparound + NaN", 2400, v);
  }
  // Small window, many wraparounds, clamped extremes, inf and NaN
  {
    std::vector<float> v;
    for (int i = 0; i < 5000; i++) {
      float u = unit(rng);
      v.push_back(u < 0.05f ? NAN : u < 0.07f ? INFINITY : u < 0.09f ? -INFINITY : wide(rng));
    }
    run("clamped extremes", 7, v);
  }
  // All NaN: every statistic is NaN
  run("all NaN", 16, std::vector<float>(50, NAN));
  // NaN blocks evicting a valid window back to empty, then refilling
  {
    std::vector<float> v(10, 55.f);
    v.insert(v.end(), 12, NAN);
    v.insert(v.end(), 3, 60.f);
    run("drain to empty and refill", 10, v);
  }
  // Ties: few distinct levels, so quantile positions land inside runs of equal values
  {
    std::vector<float> v;
    for (int i = 0; i < 3000; i++)
      v.push_back(40.f + 5.f * (rng() % 3));
    run("ties", 240, v);
  }
  // Rounding boundaries: values straddling a 0.1 dB bin edge
  {
    std::vector<float> v;
    for (int i = 0; i < 2000; i++)
      v.push_back(50.05f + (unit(rng) - 0.5f) * 0.002f);
    run("bin edges", 100, v);
  }
  // Capacity 1
  {
    std::vector<float> v;
    for (int i = 0; i < 200; i++)
      v.push_back(unit(rng) < 0.2f ? NAN : office(rng));
    run("capacity 1", 1, v);
  }
  // clear() empties both ring and histogram
  {
    int before = failures;
    StatsWindow w;
    w.init(8);
    for (int i = 0; i < 20; i++)
      w.push(70.f);
    w.clear();
    expect(w.size() == 0 && w.valid() == 0, "clear size", 0, w.size(), 0);
    expect(std::isnan(w.quantile(0.5f)) && std::isnan(w.energy_mean()), "clear NaN", 0, w.energy_mean(), NAN);
    w.push(42.f);
    expect(same(w.quantile(0.9f), 42.f, 1e-6f) && same(w.energy_mean(), 42.f, 1e-3f), "after clear", 1,
           w.energy_mean(), 42.f);
    std::printf("%-28s %s\n", "clear", failures == before ? "ok" : "FAILED");
  }
  // Energy mean is not the arithmetic mean: equal time at 40 and 80 dB is 77.0 dB (10 log10 of 50005000), not 60
  {
    int before = failures;
    StatsWindow w;
    w.init(2);
    w.push(40.f);
    w.push(80.f);
    expect(same(w.energy_mean(), 76.9901f, 1e-3f), "energy 40/80", 0, w.energy_mean(), 76.9901f);
    std::printf("%-28s %s\n", "energy vs arithmetic", failures == before ? "ok" : "FAILED");
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
