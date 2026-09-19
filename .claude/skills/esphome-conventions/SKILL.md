---
name: esphome-conventions
description: >-
  ESPHome custom-component coding conventions and code-generation patterns for
  this repo. Use when writing, editing, or reviewing ESPHome component code —
  C++ headers/implementations (.h/.cpp) or Python codegen/schema (__init__.py) —
  under components/. Covers naming, C++ field visibility, preprocessor rules,
  embedded memory/STL container choices, callback managers, config schemas,
  automations, and Python state management. Reconciled with upstream
  esphome/esphome AGENTS.md (dev branch, through PR #18941); local examples
  take precedence.
---

# ESPHome Component Conventions

Conventions for the custom components in `components/` (loaded via
`external_components`). This is **not** a fork of ESPHome — but the components
follow ESPHome's own conventions so they behave like first-party code and stay
easy to upstream later.

Write C++ as Linus would: direct, no unnecessary abstraction, no defensive
boilerplate for cases that cannot happen. When in doubt, err toward simpler.

## Reference files

Load these when the task touches their area:

- **`references/memory-and-containers.md`** — heap-allocation policy, STL
  container selection (`std::array`/`StaticVector`/`FixedVector`), and callback
  managers. Read before adding any container, buffer, or callback.
- **`references/codegen-patterns.md`** — Python config schema, C++ class
  skeleton, automations (triggers/actions/conditions), validators, platform
  sub-components, and `CORE.data` state management. Read before editing
  `__init__.py` or adding a new component/platform.

Upstream <https://developers.esphome.io> is the authoritative reference for the
component lifecycle and the loop primitives; go there when this skill is silent.

---

## Formatting

- **Python:** PEP 8. Follow `ruff` / `flake8` conventions even without a
  pre-commit hook.
- **C++:** Google C++ Style Guide with the ESPHome specifics below. Format with
  `clang-format` if available.

## Naming conventions

- **Python:** `snake_case` for functions and variables, `UpperCamelCase` for
  classes.
- **C++** (clang-tidy conventions):
  - Functions, methods, variables: `lower_snake_case`
  - Classes, structs, enums: `UpperCamelCase`
  - Top-level constants (global/namespace scope): `UPPER_SNAKE_CASE`
  - Function-local constants: `lower_snake_case`
  - Protected/private fields: `lower_snake_case_with_trailing_underscore_`
  - Favor descriptive names over abbreviations

## C++ additional conventions

- **Member access:** Prefix all class member access with `this->`
  (e.g. `this->value_`, not `value_`).
- **Indentation:** Spaces (two per level), not tabs.
- **Type aliases:** Prefer `using type_t = int;` over `typedef int type_t;`.
- **Line length:** Wrap at no more than 120 characters.
- **Constructor params vs setters:** Properties that are both required and
  invariant should be constructor parameters, not setter methods.

  ```cpp
  class SourceTextSensor : public text_sensor::TextSensor, public Component {
   public:
    explicit SourceTextSensor(text::Text *source) : source_(source) {}
   protected:
    text::Text *source_;
  };
  ```

- **Timing in `loop()`:** Never call `millis()` in a `loop()` body. The current
  tick's timestamp is already cached — use `App.get_loop_component_start_time()`
  (from `esphome/core/application.h`). This applies to `Component::loop()` only;
  a FreeRTOS task with its own `while` loop, like `sound_level_meter`'s, still
  calls `millis()`.

- **Rate-limiting gates:** The main loop runs every ~16 ms, so a gate shorter
  than that never fires early and is dead weight. Use an interval comfortably
  longer than 16 ms, or drop the gate.

  ```cpp
  // Bad - 10ms gate can never trigger; millis() in a loop body
  static constexpr uint32_t POLL_INTERVAL_MS = 10;
  const uint32_t now = millis();
  if (now - this->last_poll_ < POLL_INTERVAL_MS)
    return;
  this->last_poll_ = now;

  // Good
  static constexpr uint32_t POLL_INTERVAL_MS = 100;
  const uint32_t now = App.get_loop_component_start_time();
  if (now - this->last_poll_ < POLL_INTERVAL_MS)
    return;
  this->last_poll_ = now;
  ```

- **No redundant overrides:** Don't override a base-class method just to return
  what it already returns. `Component::get_setup_priority()` already returns
  `setup_priority::DATA`; overriding it to return the same value is noise.

- **Logging string literals:** Wrap a string literal passed as a `%s` argument in
  `LOG_STR_LITERAL()` so it lives in flash rather than RAM.

  ```cpp
  ESP_LOGCONFIG(TAG, "  Mode: %s", LOG_STR_LITERAL("continuous"));
  ```

## C++ preprocessor directives

- **Avoid `#define` for constants:** Use `const` variables or enums instead.
- **Use `#define` only for:**
  - Conditional compilation (`#ifdef`, `#ifndef`)
  - Compile-time sizes set during Python code generation via `cg.add_define()`

## C++ field visibility

**Prefer `protected`** for most class fields to enable extensibility and
testing. Fields follow `lower_snake_case_with_trailing_underscore_`.

**Use `private` for safety-critical cases** — when direct field access could
introduce bugs or violate invariants:

1. **Pointer lifetime issues:** setters validate and store pointers from known
   lists to prevent dangling references.

   ```cpp
   // Helper to find matching string in vector and return its pointer
   inline const char *vector_find(const std::vector<const char *> &vec, const char *value) {
     for (const char *item : vec) {
       if (strcmp(item, value) == 0)
         return item;
     }
     return nullptr;
   }

   class ClimateDevice {
    public:
     void set_custom_fan_modes(std::initializer_list<const char *> modes) {
       this->custom_fan_modes_ = modes;
       this->active_custom_fan_mode_ = nullptr;  // Reset when modes change
     }
     bool set_custom_fan_mode(const char *mode) {
       // Find mode in supported list and store that pointer (not the input pointer)
       const char *validated_mode = vector_find(this->custom_fan_modes_, mode);
       if (validated_mode != nullptr) {
         this->active_custom_fan_mode_ = validated_mode;
         return true;
       }
       return false;
     }
    private:
     std::vector<const char *> custom_fan_modes_;  // Pointers to string literals in flash
     const char *active_custom_fan_mode_{nullptr};  // Must point to entry in custom_fan_modes_
   };
   ```

2. **Invariant coupling:** when multiple fields must stay synchronized to prevent
   buffer overflows or data corruption.

   ```cpp
   class Buffer {
    public:
     void resize(size_t new_size) {
       auto new_data = std::make_unique<uint8_t[]>(new_size);
       if (this->data_) {
         std::memcpy(new_data.get(), this->data_.get(), std::min(this->size_, new_size));
       }
       this->data_ = std::move(new_data);
       this->size_ = new_size;  // Must stay in sync with data_
     }
    private:
     std::unique_ptr<uint8_t[]> data_;
     size_t size_{0};  // Must match allocated size of data_
   };
   ```

3. **Resource management:** when setters perform cleanup or registration that
   derived classes might skip.

**Provide `protected` accessor methods** when derived classes need controlled
access to `private` members.

## Python idioms

**Walrus operator (PEP 572):** prefer `:=` wherever it removes a redundant
lookup or a throwaway temporary. Don't contort code to use it.

```python
# Bad - looks up CONF_BLAH twice
if CONF_BLAH in config:
    cg.add(var.set_blah(config[CONF_BLAH]))

# Good - single lookup, value bound inline
if (blah := config.get(CONF_BLAH)) is not None:
    cg.add(var.set_blah(blah))
```

## Component structure

```
components/[component_name]/
├── __init__.py          # Configuration schema and code generation
├── [component].h        # C++ header
├── [component].cpp      # C++ implementation
└── [platform]/          # Platform-specific sub-components (sensor, number, etc.)
    ├── __init__.py
    ├── [platform].h
    └── [platform].cpp
```

**Component metadata** (in `__init__.py`):
- `DEPENDENCIES`: required components (e.g. `["i2c"]`, `["uart"]`)
- `AUTO_LOAD`: components to load automatically (e.g. `["json"]`)
- `CONFLICTS_WITH`: incompatible components
- `MULTI_CONF`: `True` if multiple instances are allowed
- `CODEOWNERS`: GitHub usernames (only relevant if upstreaming)

Keep dependencies minimal — add to `DEPENDENCIES` only what's actually used.

## Best practices

- Prefer ESP-IDF framework for ESP32 devices (the default here).
- Do not allocate heap after `setup()` — see `references/memory-and-containers.md`.
- Use `dump_config()` to log configured parameters at startup.
- Use `ESP_LOGD` / `ESP_LOGI` / `ESP_LOGE` for logging.
