#pragma once
#include <stddef.h>
#include <stdint.h>

// The text protocol and presets originate in ../logled/lib/xgai_led.cpp.
// Platform-independent engine; callers serialize init/execute/tick.
namespace vibeled {
constexpr size_t EFFECT_NAME_SIZE = 16;
constexpr uint8_t MIN_LEVEL = 1, MAX_LEVEL = 5, DEFAULT_LEVEL = 3;
struct effect_t {
  uint32_t rgb, on_ms, off_ms, duration_ms;
  char name[EFFECT_NAME_SIZE];
  uint32_t alt_rgb;
};
struct Platform {
  bool (*load)(char *text, size_t capacity);
  bool (*save)(const char *text);
};
struct Frame {
  uint32_t rgb;
  // UINT32_MAX means no timer. Otherwise wake after at least 1 ms.
  uint32_t wait_ms;
};
void init(Platform platform);
Frame tick(uint64_t now_ms);
bool execute_command(const char *command, char *response, size_t capacity,
                     uint64_t now_ms, bool allow_save = true);
} // namespace vibeled
