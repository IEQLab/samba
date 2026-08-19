#include "copy_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace copy {

static const char *const TAG = "copy.sensor";

void CopySensor::setup() {
  // Vendored from core ESPHome to add this null guard.
  //
  // safe_mode calls App.setup() from inside should_enter_safe_mode() and then returns
  // early from the generated setup(). The copy platform's to_code() registers the
  // component before awaiting cg.get_variable(source_id), so when the source belongs to
  // a late-processed component (sound_level_meter) the set_source() call is emitted
  // after that early return. Those copy sensors are therefore set up with a null source
  // whenever safe mode is entered, and dereferencing it panics (LoadProhibited) before
  // the boot counter is ever synced to flash - an unrecoverable boot loop.
  //
  // Verified still present in ESPHome 2026.7.4. Remove this guard once fixed upstream.
  if (this->source_ == nullptr) {
    this->mark_failed(LOG_STR("source not set"));
    return;
  }

  source_->add_on_state_callback([this](float value) { this->publish_state(value); });
  if (source_->has_state())
    this->publish_state(source_->state);
}

void CopySensor::dump_config() { LOG_SENSOR("", "Copy Sensor", this); }

}  // namespace copy
}  // namespace esphome
