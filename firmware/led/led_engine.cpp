// Ported from logled's text protocol; timing and platform I/O are independent.
#include "led_engine.h"
#include <algorithm>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#else
#include <strings.h>
#endif
namespace vibeled {
namespace {
constexpr size_t MAX_SAVED_EFFECTS = 8, LEVEL_COUNT = 5, COMMAND_SIZE = 256,
                 STORE_SIZE = 768;
struct saved_effect_t {
  bool used;
  effect_t effect;
};
struct level_state_t {
  bool active;
  effect_t effect;
  uint64_t started;
};
saved_effect_t s_saved[MAX_SAVED_EFFECTS]{};
level_state_t s_levels[LEVEL_COUNT]{};
Platform s_platform{};
effect_t s_current{};
uint8_t s_current_level = 0;
bool s_running = false;
const effect_t BUILTIN_EFFECTS[] = {{0x003c00, 1000, 0, 300000, "green", 0},
                                    {0x3b1e00, 1000, 0, 0, "yellow", 0},
                                    {0x770000, 1000, 0, 0, "red", 0},
                                    {0x000077, 1000, 0, 0, "blue", 0},
                                    {0x000077, 500, 500, 0, "slow-blue", 0},
                                    {0x770000, 120, 880, 0, "heartbeat", 0}};
void off() {
  for (auto &s : s_levels)
    s = {};
}
void play(const effect_t &effect, uint8_t level, uint64_t now) {
  auto &s = s_levels[level - 1];
  s = {effect.rgb != 0 || effect.alt_rgb != 0, effect, now};
}
void copy_name(char (&destination)[EFFECT_NAME_SIZE], const char *source) {
  snprintf(destination, sizeof(destination), "%s", source ? source : "");
}

void set_response(char *response, size_t response_size, const char *format,
                  ...) {
  if (!response || response_size == 0)
    return;
  va_list args;
  va_start(args, format);
  vsnprintf(response, response_size, format, args);
  va_end(args);
}

bool valid_name(const char *name) {
  if (!name || !*name || strlen(name) >= EFFECT_NAME_SIZE)
    return false;
  for (const char *p = name; *p; ++p) {
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
      return false;
  }
  return true;
}

bool parse_u32(const char *text, uint32_t *value) {
  if (!text || !*text || *text == '-')
    return false;
  char *end = nullptr;
  errno = 0;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || *end != '\0' || parsed > UINT32_MAX)
    return false;
  *value = (uint32_t)parsed;
  return true;
}

bool extract_level_option(char **words, size_t *count, uint8_t *level,
                          char *response, size_t response_size) {
  *level = DEFAULT_LEVEL;
  bool found = false;
  for (size_t index = 1; index < *count;) {
    if (strcasecmp(words[index], "--level") != 0) {
      ++index;
      continue;
    }
    if (found) {
      set_response(response, response_size,
                   "error --level specified more than once");
      return false;
    }
    if (index + 1 >= *count) {
      set_response(response, response_size,
                   "error --level requires a value from %u to %u",
                   (unsigned)MIN_LEVEL, (unsigned)MAX_LEVEL);
      return false;
    }
    uint32_t parsed = 0;
    if (!parse_u32(words[index + 1], &parsed) || parsed < MIN_LEVEL ||
        parsed > MAX_LEVEL) {
      set_response(response, response_size, "error level must be from %u to %u",
                   (unsigned)MIN_LEVEL, (unsigned)MAX_LEVEL);
      return false;
    }
    *level = (uint8_t)parsed;
    found = true;
    for (size_t move = index; move + 2 < *count; ++move)
      words[move] = words[move + 2];
    *count -= 2;
  }
  return true;
}

bool parse_color(const char *text, uint32_t *rgb) {
  struct named_color_t {
    const char *name;
    uint32_t rgb;
  };
  static constexpr named_color_t colors[] = {
      {"black", 0x000000},  {"white", 0xffffff},   {"red", 0xff0000},
      {"green", 0x00ff00},  {"blue", 0x0000ff},    {"yellow", 0xffff00},
      {"cyan", 0x00ffff},   {"magenta", 0xff00ff}, {"orange", 0xff8000},
      {"purple", 0x8000ff},
  };
  for (const auto &color : colors) {
    if (strcasecmp(text, color.name) == 0) {
      *rgb = color.rgb;
      return true;
    }
  }

  if (*text == '#')
    ++text;
  if (strlen(text) != 6)
    return false;
  for (const char *p = text; *p; ++p)
    if (!isxdigit((unsigned char)*p))
      return false;
  char *end = nullptr;
  unsigned long parsed = strtoul(text, &end, 16);
  if (*end != '\0' || parsed > 0xffffff)
    return false;
  *rgb = (uint32_t)parsed;
  return true;
}

bool extract_altcolor_option(char **words, size_t *count, uint32_t *alt_rgb,
                             bool *found, char *response,
                             size_t response_size) {
  *alt_rgb = 0;
  *found = false;
  for (size_t index = 1; index < *count;) {
    if (strcasecmp(words[index], "--altcolor") != 0) {
      ++index;
      continue;
    }
    if (*found) {
      set_response(response, response_size,
                   "error --altcolor specified more than once");
      return false;
    }
    if (index + 1 >= *count) {
      set_response(response, response_size,
                   "error --altcolor requires a color");
      return false;
    }
    if (!parse_color(words[index + 1], alt_rgb)) {
      set_response(response, response_size,
                   "error invalid altcolor: use #RRGGBB or a color name");
      return false;
    }
    *found = true;
    for (size_t move = index; move + 2 < *count; ++move)
      words[move] = words[move + 2];
    *count -= 2;
  }
  return true;
}

const effect_t *find_effect(const char *name) {
  for (const auto &saved : s_saved) {
    if (saved.used && strcasecmp(saved.effect.name, name) == 0)
      return &saved.effect;
  }
  for (const auto &builtin : BUILTIN_EFFECTS) {
    if (strcasecmp(builtin.name, name) == 0)
      return &builtin;
  }
  return nullptr;
}

bool serialize_saved_effects() {
  char buffer[STORE_SIZE]{};
  size_t used = 0;
  for (const auto &saved : s_saved) {
    if (!saved.used)
      continue;
    int written = snprintf(
        buffer + used, sizeof(buffer) - used, "%s,%06lx,%lu,%lu,%lu,%06lx\n",
        saved.effect.name, (unsigned long)saved.effect.rgb,
        (unsigned long)saved.effect.on_ms, (unsigned long)saved.effect.off_ms,
        (unsigned long)saved.effect.duration_ms,
        (unsigned long)saved.effect.alt_rgb);
    if (written < 0 || (size_t)written >= sizeof(buffer) - used)
      return false;
    used += (size_t)written;
  }
  return s_platform.save && s_platform.save(buffer);
}

void load_saved_effects() {
  char buffer[STORE_SIZE]{};
  if (!s_platform.load || !s_platform.load(buffer, sizeof(buffer)))
    return;

  char *save_line = nullptr;
  for (char *line = strtok_r(buffer, "\n", &save_line); line;
       line = strtok_r(nullptr, "\n", &save_line)) {
    if (!*line)
      continue;
    char *fields[6]{};
    char *save_field = nullptr;
    size_t count = 0;
    for (char *field = strtok_r(line, ",", &save_field); field && count < 6;
         field = strtok_r(nullptr, ",", &save_field)) {
      fields[count++] = field;
    }
    if ((count != 5 && count != 6) || !valid_name(fields[0]))
      continue;

    effect_t effect{};
    char color[8] = "#";
    snprintf(color + 1, sizeof(color) - 1, "%s", fields[1]);
    if (!parse_color(color, &effect.rgb) ||
        !parse_u32(fields[2], &effect.on_ms) ||
        !parse_u32(fields[3], &effect.off_ms) ||
        !parse_u32(fields[4], &effect.duration_ms) || effect.on_ms == 0) {
      continue;
    }
    if (count == 6) {
      char alt_color[8] = "#";
      snprintf(alt_color + 1, sizeof(alt_color) - 1, "%s", fields[5]);
      if (!parse_color(alt_color, &effect.alt_rgb))
        continue;
    }
    copy_name(effect.name, fields[0]);
    for (auto &saved : s_saved) {
      if (!saved.used) {
        saved.used = true;
        saved.effect = effect;
        break;
      }
    }
  }
}

bool parse_effect_arguments(char **words, size_t count, size_t color_index,
                            effect_t *effect, uint32_t option_alt_rgb,
                            bool has_option_altcolor, char *response,
                            size_t response_size) {
  if (count <= color_index) {
    set_response(response, response_size,
                 "error usage: set COLOR [ON_MS [OFF_MS [DURATION_MS]]] "
                 "[--altcolor COLOR]");
    return false;
  }
  effect->on_ms = 1000;
  effect->off_ms = 0;
  effect->duration_ms = 0;
  effect->alt_rgb = 0;
  if (!parse_color(words[color_index], &effect->rgb)) {
    set_response(response, response_size,
                 "error invalid color: use #RRGGBB or a color name");
    return false;
  }

  // Keep the original positional timing syntax, but also accept a
  // second color immediately after COLOR for callers that do not
  // use named options. The explicit --altcolor form is preferred.
  size_t timing_index = color_index + 1;
  if (count > timing_index) {
    uint32_t positional_alt_rgb = 0;
    if (strspn(words[timing_index], "0123456789") !=
            strlen(words[timing_index]) &&
        parse_color(words[timing_index], &positional_alt_rgb)) {
      if (has_option_altcolor) {
        set_response(response, response_size,
                     "error altcolor specified more than once");
        return false;
      }
      effect->alt_rgb = positional_alt_rgb;
      ++timing_index;
    }
  }
  if (has_option_altcolor)
    effect->alt_rgb = option_alt_rgb;

  if (count > timing_index + 3) {
    set_response(response, response_size,
                 "error usage: set COLOR [ALTCOLOR] [ON_MS [OFF_MS "
                 "[DURATION_MS]]] [--altcolor COLOR]");
    return false;
  }
  if (count > timing_index && !parse_u32(words[timing_index], &effect->on_ms)) {
    set_response(response, response_size, "error invalid ON_MS");
    return false;
  }
  if (count > timing_index + 1 &&
      !parse_u32(words[timing_index + 1], &effect->off_ms)) {
    set_response(response, response_size, "error invalid OFF_MS");
    return false;
  }
  if (count > timing_index + 2 &&
      !parse_u32(words[timing_index + 2], &effect->duration_ms)) {
    set_response(response, response_size, "error invalid DURATION_MS");
    return false;
  }
  if (effect->on_ms == 0) {
    set_response(response, response_size,
                 "error ON_MS must be greater than zero");
    return false;
  }
  return true;
}

void format_effect_response(const char *prefix, const effect_t &effect,
                            uint8_t level, char *response,
                            size_t response_size) {
  set_response(response, response_size,
               "%s level=%u name=%s color=#%06lx altcolor=#%06lx on=%lu "
               "off=%lu duration=%lu",
               prefix, (unsigned)level, effect.name, (unsigned long)effect.rgb,
               (unsigned long)effect.alt_rgb, (unsigned long)effect.on_ms,
               (unsigned long)effect.off_ms, (unsigned long)effect.duration_ms);
}

void trim(char *text) {
  char *begin = text;
  while (isspace((unsigned char)*begin))
    ++begin;
  if (begin != text)
    memmove(text, begin, strlen(begin) + 1);
  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
}
} // namespace

void init(Platform platform) {
  s_platform = platform;
  memset(s_saved, 0, sizeof s_saved);
  off();
  s_current = {};
  s_current_level = 0;
  s_running = false;
  load_saved_effects();
}
Frame tick(uint64_t now) {
  Frame frame{0, UINT32_MAX};
  s_current = {};
  s_current_level = 0;
  s_running = false;
  for (size_t i = 0; i < LEVEL_COUNT; ++i) {
    auto &s = s_levels[i];
    if (!s.active)
      continue;
    const uint64_t elapsed = now - s.started;
    if (s.effect.duration_ms && elapsed >= s.effect.duration_ms) {
      s.active = false;
      continue;
    }
    if (s.effect.duration_ms)
      frame.wait_ms =
          std::min(frame.wait_ms, uint32_t(std::min<uint64_t>(UINT32_MAX - 1,
                                             s.effect.duration_ms - elapsed)));
    uint64_t phase =
        s.effect.off_ms ? elapsed % (uint64_t(s.effect.on_ms) + s.effect.off_ms)
                        : 0;
    const bool on = phase < s.effect.on_ms;
    if (s.effect.off_ms) {
      uint64_t wait = on ? s.effect.on_ms - phase
                         : uint64_t(s.effect.on_ms) + s.effect.off_ms - phase;
      frame.wait_ms = std::min(frame.wait_ms, uint32_t(std::min<uint64_t>(UINT32_MAX - 1, wait)));
    }
    frame.rgb = on ? s.effect.rgb : s.effect.alt_rgb;
    s_current = s.effect;
    s_current_level = uint8_t(i + 1);
    s_running = true;
  }
  return frame;
}
bool execute_command(const char *command, char *response, size_t response_size,
                     uint64_t now_ms, bool allow_save) {
  tick(now_ms);
  if (!command)
    return false;
  char buffer[COMMAND_SIZE]{};
  if (strlen(command) >= sizeof(buffer)) {
    set_response(response, response_size, "error command too long");
    return true;
  }
  snprintf(buffer, sizeof(buffer), "%s", command);
  trim(buffer);
  if (!*buffer) {
    set_response(response, response_size, "error empty command");
    return true;
  }

  char *input = buffer;
  if (strncasecmp(input, "XGAI_LED", strlen("XGAI_LED")) == 0) {
    input += strlen("XGAI_LED");
    while (*input == ':' || isspace((unsigned char)*input))
      ++input;
  }

  char *words[16]{};
  size_t count = 0;
  char *save = nullptr;
  for (char *word = strtok_r(input, " \t", &save); word;
       word = strtok_r(nullptr, " \t", &save)) {
    if (count >= sizeof(words) / sizeof(words[0])) {
      set_response(response, response_size, "error too many command arguments");

      return true;
    }
    words[count++] = word;
  }
  if (count == 0) {
    set_response(response, response_size, "error empty command");

    return true;
  }

  if (!allow_save &&
      (!strcasecmp(words[0], "save") || !strcasecmp(words[0], "delete"))) {
    set_response(response, response_size,
                 "error busy; retry saving when voice/update is idle");
    return true;
  }
  if (strcasecmp(words[0], "off") == 0) {
    if (count != 1)
      set_response(response, response_size, "error usage: off");
    else {
      off();

      set_response(response, response_size, "ok off");
    }
  } else if (strcasecmp(words[0], "set") == 0) {
    effect_t effect{};
    copy_name(effect.name, "direct");
    uint8_t level = DEFAULT_LEVEL;
    uint32_t alt_rgb = 0;
    bool has_altcolor = false;
    if (extract_level_option(words, &count, &level, response, response_size) &&
        extract_altcolor_option(words, &count, &alt_rgb, &has_altcolor,
                                response, response_size) &&
        parse_effect_arguments(words, count, 1, &effect, alt_rgb, has_altcolor,
                               response, response_size)) {
      play(effect, level, now_ms);

      format_effect_response("ok", effect, level, response, response_size);
    }
  } else if (strcasecmp(words[0], "save") == 0) {
    uint8_t level = DEFAULT_LEVEL;
    if (!extract_level_option(words, &count, &level, response, response_size)) {
    } else if (count < 3 || !valid_name(words[1])) {
      set_response(response, response_size,
                   "error usage: save NAME COLOR [ON_MS [OFF_MS "
                   "[DURATION_MS]]] [--altcolor COLOR] [--level LEVEL]");
    } else {
      effect_t effect{};
      copy_name(effect.name, words[1]);
      uint32_t alt_rgb = 0;
      bool has_altcolor = false;
      if (extract_altcolor_option(words, &count, &alt_rgb, &has_altcolor,
                                  response, response_size) &&
          parse_effect_arguments(words, count, 2, &effect, alt_rgb,
                                 has_altcolor, response, response_size)) {
        saved_effect_t *slot = nullptr;
        for (auto &saved : s_saved) {
          if (saved.used && strcasecmp(saved.effect.name, effect.name) == 0) {
            slot = &saved;
            break;
          }
          if (!saved.used && !slot)
            slot = &saved;
        }
        if (!slot) {
          set_response(response, response_size,
                       "error saved-effect limit is %u",
                       (unsigned)MAX_SAVED_EFFECTS);
        } else {
          saved_effect_t old = *slot;
          slot->used = true;
          slot->effect = effect;
          if (!serialize_saved_effects()) {
            *slot = old;
            set_response(response, response_size,
                         "error could not persist effect");
          } else {
            play(effect, level, now_ms);

            format_effect_response("ok saved", effect, level, response,
                                   response_size);
          }
        }
      }
    }
  } else if (strcasecmp(words[0], "play") == 0) {
    uint8_t level = DEFAULT_LEVEL;
    if (!extract_level_option(words, &count, &level, response, response_size)) {
    } else if (count != 2) {
      set_response(response, response_size,
                   "error usage: play NAME [--level LEVEL]");
    } else {
      const effect_t *effect = find_effect(words[1]);
      if (!effect)
        set_response(response, response_size, "error effect not found: %s",
                     words[1]);
      else {
        play(*effect, level, now_ms);

        format_effect_response("ok", *effect, level, response, response_size);
      }
    }
  } else if (strcasecmp(words[0], "delete") == 0) {
    if (count != 2) {
      set_response(response, response_size, "error usage: delete NAME");
    } else {
      saved_effect_t *found = nullptr;
      for (auto &saved : s_saved) {
        if (saved.used && strcasecmp(saved.effect.name, words[1]) == 0) {
          found = &saved;
          break;
        }
      }
      if (!found)
        set_response(response, response_size,
                     "error saved effect not found: %s", words[1]);
      else {
        saved_effect_t old = *found;
        *found = {};
        if (!serialize_saved_effects()) {
          *found = old;
          set_response(response, response_size,
                       "error could not persist deletion");
        } else {
          set_response(response, response_size, "ok deleted %s", words[1]);
        }
      }
    }
  } else if (strcasecmp(words[0], "list") == 0) {
    set_response(response, response_size,
                 "ok builtin=green,yellow,red,blue,slow-blue,heartbeat saved=");
    size_t used = response ? strlen(response) : 0;
    for (const auto &saved : s_saved) {
      if (!saved.used || used >= response_size)
        continue;
      int written = snprintf(
          response + used, response_size - used, "%s%s",
          used > strlen("ok builtin=green,yellow,red,blue,slow-blue,heartbeat "
                        "saved=")
              ? ","
              : "",
          saved.effect.name);
      if (written > 0)
        used += (size_t)written;
    }
  } else if (strcasecmp(words[0], "status") == 0) {
    effect_t current{};

    current = s_current;
    uint8_t current_level = s_current_level;
    bool running = s_running;

    if (!running)
      set_response(response, response_size, "ok off");
    else
      format_effect_response("ok active", current, current_level, response,
                             response_size);
  } else if (strcasecmp(words[0], "getip") == 0) {
    if (count != 1)
      set_response(response, response_size, "error usage: getip");
    else
      set_response(response, response_size, "no ip");
  } else if (strcasecmp(words[0], "help") == 0) {
    set_response(response, response_size,
                 "ok commands: set/save [--altcolor COLOR], play [--level "
                 "1..5], delete/list/status/getip/off");
  } else {
    uint8_t level = DEFAULT_LEVEL;
    bool valid_options =
        extract_level_option(words, &count, &level, response, response_size);
    const effect_t *shortcut =
        valid_options && count == 1 ? find_effect(words[0]) : nullptr;
    if (!shortcut) {
      if (valid_options)
        set_response(response, response_size,
                     "error unknown command; try help");
    } else {
      play(*shortcut, level, now_ms);

      format_effect_response("ok", *shortcut, level, response, response_size);
    }
  }

  tick(now_ms);
  return true;
}
} // namespace vibeled
