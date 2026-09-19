# Embedded Memory & STL Containers

ESP devices run for months with small heaps shared between Wi-Fi, BLE, LWIP, and
application code. Over time, repeated allocations of different sizes fragment the
heap. ESPHome treats runtime heap allocation as a long-term reliability bug, not
a performance issue — crashes happen when the largest contiguous free block
shrinks even when total free heap is adequate.

## Heap allocation policy

**Heap allocation after `setup()` should be avoided unless absolutely
unavoidable.** Every allocation/deallocation cycle contributes to fragmentation.

Helpers that hide allocation (`std::string`, `std::to_string`, string-returning
helpers) are being deprecated upstream in favor of buffer and view-based APIs.
Prefer fixed buffers and views over dynamic strings in new code.

### Strings set once from config → `StringRef`

A `std::string` member set once from the YAML config heap-copies a literal that
already lives in flash. Use `StringRef` (`esphome/core/string_ref.h`) instead: it
is a pointer plus a length, and the codegen literal it points at is static.

```cpp
// Bad - heap copy of a flash literal, held for the life of the device
void set_mount_point(const std::string &mount_point) { this->mount_point_ = mount_point; }
std::string mount_point_;

// Good - no allocation
void set_mount_point(const StringRef &mount_point) { this->mount_point_ = mount_point; }
StringRef mount_point_;
```

This applies only to values that are **invariant after `setup()`**. A string
built or mutated at runtime still needs to own its storage.

## STL container guidelines

### 1. Compile-time-known sizes → `std::array`

```cpp
// Bad - generates STL realloc code
std::vector<int> values;

// Good - no dynamic allocation
std::array<int, MAX_VALUES> values;
```

Use `cg.add_define("MAX_VALUES", count)` to pass the size from Python.

**For byte buffers:** avoid `std::vector<uint8_t>` unless the buffer must grow.
Prefer `std::unique_ptr<uint8_t[]>` (no bounds checking, minimal overhead) or
`std::array<uint8_t, N>` (fixed size). Note that `std::unique_ptr<uint8_t[]>`
gives you no bounds checking or iterators — use it only when you don't need them.

```cpp
// Bad - STL overhead for a simple byte buffer
std::vector<uint8_t> buffer;
buffer.resize(256);

// Good - minimal overhead, single allocation
std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(256);
// Or if size is constant:
std::array<uint8_t, 256> buffer;
```

### 2. Compile-time fixed size with vector-like API → `StaticVector`

From `esphome/core/helpers.h`. Like `std::array` but with `push_back()` /
`size()` and no STL reallocation code.

```cpp
// Bad - includes STL reallocation machinery
std::vector<ServiceRecord> services;
services.reserve(5);

// Good - compile-time fixed size, no reallocation code
StaticVector<ServiceRecord, MAX_SERVICES> services;
services.push_back(record1);
```

Use `cg.add_define("MAX_SERVICES", count)` to set the size from Python.

### 3. Runtime-known sizes → `FixedVector`

From `esphome/core/helpers.h`. Single allocation at init, no reallocation.

```cpp
// Bad - includes STL reallocation machinery
std::vector<TxtRecord> txt_records;
txt_records.reserve(5);

// Good - runtime size, single allocation, no reallocation
FixedVector<TxtRecord> txt_records;
txt_records.init(record_count);
```

**Benefits:** eliminates `_M_realloc_insert` / `_M_default_append` instantiations
(saves 200–500 bytes per instance), single allocation, no upper bound needed,
compatible with protobuf codegen via the `[(fixed_vector) = true]` option.

### 4. Small datasets (1–16 elements)

Use `std::vector` or `std::array` of simple structs with linear search instead
of `std::map` / `std::set` / `std::unordered_map` (2 KB+ overhead each for the
red-black tree or hash table).

```cpp
// Bad - 2KB+ overhead for a tree/hash table
std::map<std::string, int> small_lookup;

// Good - simple struct, linear search
struct LookupEntry {
  const char *key;
  int value;
};
std::array<LookupEntry, 3> small_lookup = {{ {"key1", 10}, {"key2", 20}, {"key3", 30} }};
```

Linear search on small datasets is usually faster than hashing/tree overhead —
but frequency and access patterns matter. For frequent lookups in hot code
paths, O(1) vs O(n) may still matter. `std::vector`/`std::array` with simple
structs is usually fine; avoid heavy containers for small datasets unless
profiling shows otherwise.

### 5. Avoid `std::deque`

Allocates in 512-byte blocks regardless of element size — guarantees at least
512 bytes immediately. A major crash source on memory-constrained devices.

### 6. Detection

Watch compiler output for these — they indicate expensive STL overhead:
- `_M_realloc_insert`, `_M_default_append` (vector reallocation)
- `rb_tree` / `_Rb_tree` (red-black tree from map/set)
- `unordered_map` / `hash` (hash table infrastructure)
- `alloc` / `realloc` / `dealloc` in symbol names

**Prioritize container optimization effort** on core components (API, network,
logger), widely-used components (mdns, wifi, ble), and components causing flash
size complaints. (Avoiding heap allocation after `setup()` is always required
regardless of priority.)

## Callback managers

Two types in `esphome/core/helpers.h` for the observer pattern. Both accept
`std::function`, lambdas, and lightweight forwarder structs via a templatized
`add()`.

| Type | Idle overhead (32-bit) | When to use |
|------|------------------------|-------------|
| `CallbackManager<void(Ts...)>` | 12 bytes (empty `std::vector`) | Callbacks always or almost always registered |
| `LazyCallbackManager<void(Ts...)>` | 4 bytes (`nullptr`) | Callbacks often not registered |

`LazyCallbackManager` is a drop-in replacement that defers allocation until the
first callback is added. Prefer it for entity-level callbacks where most
instances have no subscribers.

**Registration methods that add to a callback manager must always be
templatized** — never take `std::function` in the signature, or you force a heap
allocation for pointer-sized forwarder structs:

```cpp
// Bad -- forces heap allocation for forwarder structs
void add_on_state_callback(std::function<void(bool)> &&callback) {
  this->state_callback_.add(std::move(callback));
}

// Good -- accepts any callable without forcing std::function wrapping
template<typename F> void add_on_state_callback(F &&callback) {
  this->state_callback_.add(std::forward<F>(callback));
}
```
