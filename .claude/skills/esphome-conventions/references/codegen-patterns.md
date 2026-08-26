# Code Generation Patterns

Python parses YAML and generates C++. These are the standard shapes for schema,
classes, automations, and state.

## Configuration schema

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CONF_PARAM = "param"  # local constant if not in esphome.const

my_component_ns = cg.esphome_ns.namespace("my_component")
MyComponent = my_component_ns.class_("MyComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MyComponent),
    cv.Optional(CONF_PARAM, default=42): cv.int_,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_param(config[CONF_PARAM]))
```

## C++ class

```cpp
namespace esphome::my_component {

class MyComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_param(int param) { this->param_ = param; }

 protected:
  int param_{0};
};

}  // namespace esphome::my_component
```

## Schema extensions

```python
CONFIG_SCHEMA = cv.Schema({ ... })
 .extend(cv.COMPONENT_SCHEMA)
 .extend(cv.polling_component_schema("60s"))
 .extend(uart.UART_DEVICE_SCHEMA)
 .extend(i2c.i2c_device_schema(0x33))
 .extend(spi.spi_device_schema(cs_pin_required=True))
```

## Validators

- `cv.int_`, `cv.float_`, `cv.string`, `cv.boolean`
- `cv.int_range(min=0, max=100)`, `cv.positive_int`, `cv.percentage`
- `cv.float_range(min=0.0, max=1.0)`
- `cv.All(cv.string, cv.Length(min=1, max=50))`, `cv.Any(cv.int_, cv.string)`
- Platform: `cv.only_on(["esp32", "esp8266"])`, `cv.only_on_esp32`,
  `cv.only_on_esp8266`, `cv.only_on_rp2040`, `esp32.only_on_variant(...)`
- Framework: `cv.only_with_framework(...)`, `cv.only_with_arduino`,
  `cv.only_with_esp_idf`

## Platform sub-components

```python
# Sensor platform
from esphome.components import sensor
CONFIG_SCHEMA = sensor.sensor_schema(MySensor).extend(cv.polling_component_schema("60s"))
async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

# Number platform
from esphome.components import number
CONFIG_SCHEMA = number.number_schema(MyNumber).extend({ ... })
async def to_code(config):
    var = await number.new_number(config, min_value=0, max_value=100, step=1)
```

## Automations (triggers, actions, conditions)

Three building blocks: **triggers** fire when something happens, **actions** do
something, **conditions** check whether something is true.

### Triggers — callback method (preferred)

Use `build_callback_automation()` for simple triggers. No `CONF_TRIGGER_ID`
needed in the schema.

```python
from esphome import automation

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MyComponent),
    cv.Optional(CONF_ON_STATE): automation.validate_automation({}),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for conf in config.get(CONF_ON_STATE, []):
        await automation.build_callback_automation(
            var, "add_on_state_callback", [(bool, "x")], conf
        )
```

The C++ registration method must be templatized (see callback managers in
`memory-and-containers.md`):

```cpp
template<typename F> void add_on_state_callback(F &&callback) {
  this->state_callback_.add(std::forward<F>(callback));
}
```

Use `build_automation()` with a `Trigger<Ts...>` subclass only when the
forwarder needs mutable state beyond a single `Automation*` pointer (edge
detection, timing logic).

### Actions

```cpp
template<typename... Ts> class MyAction : public Action<Ts...> {
 public:
  explicit MyAction(MyComponent *parent) : parent_(parent) {}
  void play(const Ts &...) override { this->parent_->do_something(); }
 protected:
  MyComponent *parent_;
};
```

Register with `@automation.register_action(...)`. Use `synchronous=True` when
the action completes inside `play()`; `synchronous=False` if it may
suspend/defer.

### Conditions

```cpp
template<typename... Ts> class MyCondition : public Condition<Ts...> {
 public:
  explicit MyCondition(MyComponent *parent) : parent_(parent) {}
  bool check(const Ts &...) override { return this->parent_->is_active(); }
 protected:
  MyComponent *parent_;
};
```

Register with `@automation.register_condition(...)`.

## State management (Python codegen)

Use `CORE.data` for state that must persist during configuration generation.
Avoid module-level mutable globals (they persist between compilation runs in
long-running dashboard processes) and avoid flat keys (namespace under the
component `DOMAIN` to prevent collisions).

```python
# Bad - module-level global persists between runs
_use_feature = None

# Bad - flat key, collides across components
CORE.data["my_component_feature"] = True
```

```python
# Good - dataclass namespaced under DOMAIN, auto-cleared between runs
from dataclasses import dataclass, field
from esphome.core import CORE

DOMAIN = "my_component"

@dataclass
class MyComponentData:
    feature_enabled: bool = False
    items: list[str] = field(default_factory=list)

def _get_data() -> MyComponentData:
    if DOMAIN not in CORE.data:
        CORE.data[DOMAIN] = MyComponentData()
    return CORE.data[DOMAIN]
```

`CORE.data` clears between runs; `@dataclass` gives type safety and clean
attribute access. `TypedDict` is also acceptable.
