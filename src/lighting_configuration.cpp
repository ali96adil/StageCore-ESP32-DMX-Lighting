#include "lighting_configuration.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lighting_output.h"
#include "nvs.h"
#include "sha/sha_core.h"

namespace stagecore {
namespace {

constexpr char kNamespace[] = "stagecore_light";
constexpr char kConfigKey[] = "config_v1";
constexpr size_t kMaxStoredConfig = 8192;
constexpr size_t kPhysicalChannels = 12;

SemaphoreHandle_t g_lock = nullptr;
SemaphoreHandle_t g_operation_lock = nullptr;
TaskHandle_t g_fade_task = nullptr;
bool g_ready = false;
std::vector<LightingChannelConfigV1> g_configuration;
std::vector<ChannelLevelV1> g_levels;
std::string g_canonical;
std::string g_hash;
std::string g_authority = "FAILSAFE";
std::vector<LightingCommandEvent> g_events;

struct ActiveFadeInternal {
  bool active = false;
  uint32_t generation = 0;
  std::string command_id;
  int64_t started_us = 0;
  int64_t duration_ms = 0;
  std::vector<ChannelLevelV1> from;
  std::vector<ChannelLevelV1> targets;
  bool blackout = false;
};

ActiveFadeInternal g_fade;
uint32_t g_next_fade_generation = 1;

struct ActiveIdentifyInternal {
  bool active = false;
  uint32_t generation = 0;
  std::string command_id;
  int64_t started_us = 0;
  int64_t duration_ms = 0;
  std::string channel_key;
  double requested_level = 0;
  double previous_level = 0;
  uint8_t channel_number = 0;
  uint8_t restore_value = 0;
};

ActiveIdentifyInternal g_identify;
uint32_t g_next_identify_generation = 1;

esp_err_t ensure_lock() {
  if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
  if (g_operation_lock == nullptr) g_operation_lock = xSemaphoreCreateMutex();
  return g_lock != nullptr && g_operation_lock != nullptr
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

std::string json_escape(const std::string &value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '<': out += "\\u003c"; break;
      case '>': out += "\\u003e"; break;
      case '&': out += "\\u0026"; break;
      default:
        if (ch < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(ch >> 4) & 0x0f]);
          out.push_back(kHex[ch & 0x0f]);
        } else if (ch == 0xE2 && i + 2 < value.size() &&
                   static_cast<unsigned char>(value[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(value[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
          out += static_cast<unsigned char>(value[i + 2]) == 0xA8
                     ? "\\u2028"
                     : "\\u2029";
          i += 2;
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string normalize_exponent(std::string value) {
  size_t e = value.find_first_of("eE");
  if (e == std::string::npos) return value;
  value[e] = 'e';
  size_t pos = e + 1;
  if (pos < value.size() && (value[pos] == '+' || value[pos] == '-')) ++pos;
  while (pos + 1 < value.size() && value[pos] == '0') {
    value.erase(pos, 1);
  }
  return value;
}

std::string expand_scientific(const std::string &value) {
  const size_t e = value.find_first_of("eE");
  if (e == std::string::npos) return value;

  bool negative = false;
  size_t start = 0;
  if (!value.empty() && value[0] == '-') {
    negative = true;
    start = 1;
  }

  std::string mantissa = value.substr(start, e - start);
  int exponent = 0;
  const char *begin = value.data() + e + 1;
  const char *end = value.data() + value.size();
  const auto parsed = std::from_chars(begin, end, exponent);
  if (parsed.ec != std::errc() || parsed.ptr != end) return value;

  const size_t dot = mantissa.find('.');
  const int digits_before =
      dot == std::string::npos ? static_cast<int>(mantissa.size())
                               : static_cast<int>(dot);
  if (dot != std::string::npos) mantissa.erase(dot);
  const int decimal_pos = digits_before + exponent;

  std::string out;
  if (negative) out.push_back('-');
  if (decimal_pos <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-decimal_pos), '0');
    out += mantissa;
  } else if (decimal_pos >= static_cast<int>(mantissa.size())) {
    out += mantissa;
    out.append(static_cast<size_t>(decimal_pos -
                                   static_cast<int>(mantissa.size())),
               '0');
  } else {
    out.append(mantissa.data(), static_cast<size_t>(decimal_pos));
    out.push_back('.');
    out.append(mantissa.data() + decimal_pos,
               mantissa.size() - static_cast<size_t>(decimal_pos));
  }
  return out;
}

std::string go_json_number(double value) {
  if (value == 0.0) return std::signbit(value) ? "-0" : "0";

  std::array<char, 64> buffer{};
  auto converted = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general);
  if (converted.ec != std::errc()) return {};
  std::string out(buffer.data(),
                  static_cast<size_t>(converted.ptr - buffer.data()));

  const double abs_value = std::fabs(value);
  if (abs_value >= 1e-6 && abs_value < 1e21) {
    out = expand_scientific(out);
  } else {
    out = normalize_exponent(out);
  }
  return out;
}

std::string canonical_configuration(
    const std::vector<LightingChannelConfigV1> &configuration) {
  std::vector<LightingChannelConfigV1> sorted = configuration;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto &a, const auto &b) {
              return a.channel_number < b.channel_number;
            });

  std::string out = "{\"schema_version\":1,\"channels\":[";
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto &channel = sorted[i];
    const std::string minimum = go_json_number(channel.minimum_level);
    const std::string maximum = go_json_number(channel.maximum_level);
    if (minimum.empty() || maximum.empty()) return {};

    out += "{\"channel_key\":" + json_escape(channel.channel_key);
    out += ",\"channel_number\":" + std::to_string(channel.channel_number);
    out += ",\"display_name\":" + json_escape(channel.display_name);
    out += ",\"kind\":" + json_escape(channel.kind);
    if (!channel.physical_zone.empty()) {
      out += ",\"physical_zone\":" + json_escape(channel.physical_zone);
    }
    out += ",\"minimum_level\":" + minimum;
    out += ",\"maximum_level\":" + maximum;
    out += ",\"inverted\":";
    out += channel.inverted ? "true" : "false";
    out += ",\"enabled\":";
    out += channel.enabled ? "true" : "false";
    out.push_back('}');
  }
  out += "]}";
  return out;
}

std::string sha256_hex(const std::string &value) {
  unsigned char digest[32] = {};
  esp_sha(SHA2_256, reinterpret_cast<const unsigned char *>(value.data()),
          value.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2] = kHex[(digest[i] >> 4) & 0x0f];
    out[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  return out;
}

uint8_t level_to_dmx(const LightingChannelConfigV1 &channel,
                     double logical_level) {
  double level = logical_level;
  if (level < channel.minimum_level) level = channel.minimum_level;
  if (level > channel.maximum_level) level = channel.maximum_level;
  int value = static_cast<int>(std::lround((level / 100.0) * 255.0));
  if (channel.inverted) value = 255 - value;
  if (value < 0) value = 0;
  if (value > 255) value = 255;
  return static_cast<uint8_t>(value);
}

std::vector<DmxSlotValue> blackout_slots(
    const std::vector<LightingChannelConfigV1> &configuration) {
  std::vector<DmxSlotValue> slots;
  slots.reserve(kPhysicalChannels);
  for (size_t channel = 1; channel <= kPhysicalChannels; ++channel) {
    slots.push_back(DmxSlotValue{static_cast<uint8_t>(channel), 0});
  }
  for (const auto &cfg : configuration) {
    if (!cfg.enabled || cfg.kind == "UNUSED") continue;
    slots[static_cast<size_t>(cfg.channel_number - 1)].value =
        level_to_dmx(cfg, 0.0);
  }
  return slots;
}

std::vector<ChannelLevelV1> blackout_levels(
    const std::vector<LightingChannelConfigV1> &configuration) {
  std::vector<ChannelLevelV1> levels;
  for (const auto &cfg : configuration) {
    if (cfg.enabled && cfg.kind != "UNUSED") {
      levels.push_back(ChannelLevelV1{cfg.channel_key, 0.0});
    }
  }
  return levels;
}

void push_event_locked(LightingCommandEvent event) {
  constexpr size_t kEventCapacity = 32;
  if (g_events.size() >= kEventCapacity) {
    g_events.erase(g_events.begin());
  }
  g_events.push_back(std::move(event));
}

void cancel_active_locked(const char *reason, bool emit_event) {
  if (!g_fade.active) return;
  if (emit_event) {
    LightingCommandEvent event;
    event.command_id = g_fade.command_id;
    event.status = "CANCELLED";
    event.error_code = "COMMAND_SUPERSEDED";
    event.category = "CANCELLED";
    event.message = reason != nullptr ? reason : "superseded";
    push_event_locked(std::move(event));
  }
  g_fade.active = false;
}

bool cancel_identify_locked(const char *reason, bool emit_event,
                            DmxSlotValue *restore) {
  if (!g_identify.active) return false;
  if (restore != nullptr) {
    *restore = DmxSlotValue{
        g_identify.channel_number,
        g_identify.restore_value,
    };
  }
  if (emit_event) {
    LightingCommandEvent event;
    event.command_id = g_identify.command_id;
    event.status = "CANCELLED";
    event.error_code = "COMMAND_SUPERSEDED";
    event.category = "CANCELLED";
    event.message = reason != nullptr ? reason : "superseded";
    event.identify = true;
    event.channel_key = g_identify.channel_key;
    event.level = g_identify.requested_level;
    event.duration_ms = g_identify.duration_ms;
    push_event_locked(std::move(event));
  }
  g_identify.active = false;
  return true;
}

double current_level_for(const std::vector<ChannelLevelV1> &levels,
                         const std::string &key) {
  auto it = std::find_if(
      levels.begin(), levels.end(),
      [&](const auto &entry) { return entry.channel_key == key; });
  return it != levels.end() ? it->level : 0.0;
}

bool normalize_requested(
    const std::vector<LightingChannelConfigV1> &configuration,
    const std::vector<ChannelLevelV1> &requested,
    bool clamp_targets,
    std::vector<ChannelLevelV1> *values,
    std::vector<DmxSlotValue> *updates,
    std::string *error_message) {
  if (values == nullptr || updates == nullptr || error_message == nullptr ||
      requested.empty()) {
    return false;
  }

  values->clear();
  updates->clear();
  for (const auto &request : requested) {
    auto it = std::find_if(
        configuration.begin(), configuration.end(),
        [&](const auto &cfg) { return cfg.channel_key == request.channel_key; });
    if (it == configuration.end() || !it->enabled || it->kind == "UNUSED") {
      *error_message = "Requested lighting channel is unknown or disabled";
      return false;
    }

    double level = request.level;
    if (clamp_targets) {
      if (level < it->minimum_level) level = it->minimum_level;
      if (level > it->maximum_level) level = it->maximum_level;
    }
    values->push_back(ChannelLevelV1{it->channel_key, level});
    updates->push_back(DmxSlotValue{
        static_cast<uint8_t>(it->channel_number),
        level_to_dmx(*it, level),
    });
  }
  return true;
}

void update_levels_locked(const std::vector<ChannelLevelV1> &values) {
  for (const auto &value : values) {
    auto level_it = std::find_if(
        g_levels.begin(), g_levels.end(),
        [&](const auto &existing) {
          return existing.channel_key == value.channel_key;
        });
    if (level_it != g_levels.end()) {
      level_it->level = value.level;
    } else {
      g_levels.push_back(value);
    }
  }
}

void fade_task(void *) {
  TickType_t last_wake = xTaskGetTickCount();
  while (true) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));

    if (g_operation_lock == nullptr ||
        xSemaphoreTake(g_operation_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
      continue;
    }
    if (g_lock == nullptr ||
        xSemaphoreTake(g_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
      xSemaphoreGive(g_operation_lock);
      continue;
    }

    if (!g_ready) {
      xSemaphoreGive(g_lock);
      xSemaphoreGive(g_operation_lock);
      continue;
    }

    if (g_identify.active) {
      const ActiveIdentifyInternal identify = g_identify;
      const std::vector<LightingChannelConfigV1> configuration =
          g_configuration;
      const int64_t elapsed_us =
          std::max<int64_t>(0, esp_timer_get_time() - identify.started_us);
      if (elapsed_us < identify.duration_ms * 1000LL) {
        xSemaphoreGive(g_lock);
        xSemaphoreGive(g_operation_lock);
        continue;
      }
      xSemaphoreGive(g_lock);

      const std::vector<DmxSlotValue> restore = {
          DmxSlotValue{identify.channel_number, identify.restore_value},
      };
      const esp_err_t restore_err = lighting_output_apply_slots(restore);

      esp_err_t blackout_err = ESP_OK;
      if (restore_err != ESP_OK) {
        blackout_err =
            lighting_output_apply_slots(blackout_slots(configuration));
      }

      if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (g_identify.active &&
            g_identify.generation == identify.generation) {
          LightingCommandEvent event;
          event.command_id = identify.command_id;
          event.identify = true;
          event.channel_key = identify.channel_key;
          event.level = identify.requested_level;
          event.duration_ms = identify.duration_ms;
          if (restore_err == ESP_OK) {
            event.status = "COMPLETED";
            g_authority = "STAGECORE";
          } else {
            event.status = "FAILED";
            event.error_code = "DMX_OUTPUT_FAILED";
            event.category = "DEVICE";
            event.message =
                "Identify restore frame was not confirmed by the output task";
            g_authority = "FAILSAFE";
            if (blackout_err == ESP_OK) {
              g_levels = blackout_levels(g_configuration);
            }
          }
          push_event_locked(std::move(event));
          g_identify.active = false;
        }
        xSemaphoreGive(g_lock);
      }
      xSemaphoreGive(g_operation_lock);
      continue;
    }

    if (!g_fade.active) {
      xSemaphoreGive(g_lock);
      xSemaphoreGive(g_operation_lock);
      continue;
    }

    const ActiveFadeInternal fade = g_fade;
    const std::vector<LightingChannelConfigV1> configuration =
        g_configuration;
    const int64_t now_us = esp_timer_get_time();
    const int64_t duration_us = fade.duration_ms * 1000LL;
    double fraction = duration_us <= 0
                          ? 1.0
                          : static_cast<double>(now_us - fade.started_us) /
                                static_cast<double>(duration_us);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    std::vector<ChannelLevelV1> levels;
    levels.reserve(fade.targets.size());
    std::vector<DmxSlotValue> updates;
    updates.reserve(fade.targets.size());

    bool valid = true;
    for (size_t i = 0; i < fade.targets.size(); ++i) {
      const auto &target = fade.targets[i];
      const double start =
          i < fade.from.size() ? fade.from[i].level : 0.0;
      const double level = start + (target.level - start) * fraction;
      auto cfg = std::find_if(
          configuration.begin(), configuration.end(),
          [&](const auto &item) {
            return item.channel_key == target.channel_key;
          });
      if (cfg == configuration.end() || !cfg->enabled ||
          cfg->kind == "UNUSED") {
        valid = false;
        break;
      }
      levels.push_back(ChannelLevelV1{target.channel_key, level});
      updates.push_back(DmxSlotValue{
          static_cast<uint8_t>(cfg->channel_number),
          level_to_dmx(*cfg, level),
      });
    }
    xSemaphoreGive(g_lock);

    const esp_err_t output_err =
        !valid ? ESP_ERR_INVALID_STATE
               : (updates.empty() ? ESP_OK
                                  : lighting_output_apply_slots(updates));

    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (g_fade.active && g_fade.generation == fade.generation) {
        if (output_err != ESP_OK) {
          LightingCommandEvent event;
          event.command_id = fade.command_id;
          event.status = "FAILED";
          event.error_code = "DMX_OUTPUT_FAILED";
          event.category = "DEVICE";
          event.message = "DMX fade frame was not confirmed by the output task";
          push_event_locked(std::move(event));
          g_fade.active = false;
          g_authority = "FAILSAFE";
        } else {
          update_levels_locked(levels);
          g_authority = "STAGECORE";
          if (fraction >= 1.0) {
            LightingCommandEvent event;
            event.command_id = fade.command_id;
            event.status = "COMPLETED";
            event.levels = fade.targets;
            event.blackout = fade.blackout;
            event.fade_ms = fade.duration_ms;
            push_event_locked(std::move(event));
            g_fade.active = false;
          }
        }
      }
      xSemaphoreGive(g_lock);
    }
    xSemaphoreGive(g_operation_lock);
  }
}

esp_err_t start_fade_locked(
    const std::string &command_id,
    const std::vector<ChannelLevelV1> &targets,
    int64_t fade_ms,
    bool blackout,
    std::vector<ChannelLevelV1> *normalized,
    std::string *error_message) {
  if (command_id.empty() || (!blackout && targets.empty()) || fade_ms <= 0 ||
      normalized == nullptr || error_message == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (!g_ready) {
    xSemaphoreGive(g_lock);
    *error_message = "No validated lighting configuration is installed";
    return ESP_ERR_INVALID_STATE;
  }

  std::vector<DmxSlotValue> ignored_updates;
  std::vector<ChannelLevelV1> values;
  if (blackout && targets.empty()) {
    values.clear();
  } else if (!normalize_requested(g_configuration, targets, !blackout,
                                  &values, &ignored_updates, error_message)) {
    xSemaphoreGive(g_lock);
    return ESP_ERR_NOT_FOUND;
  }

  cancel_active_locked(
      blackout ? "superseded by blackout" : "superseded by newer fade",
      true);

  ActiveFadeInternal fade;
  fade.active = true;
  fade.generation = g_next_fade_generation++;
  fade.command_id = command_id;
  fade.started_us = esp_timer_get_time();
  fade.duration_ms = fade_ms;
  fade.targets = values;
  fade.blackout = blackout;
  fade.from.reserve(values.size());
  for (const auto &target : values) {
    fade.from.push_back(ChannelLevelV1{
        target.channel_key,
        current_level_for(g_levels, target.channel_key),
    });
  }
  g_fade = std::move(fade);
  g_authority = "STAGECORE";
  *normalized = values;
  xSemaphoreGive(g_lock);
  return ESP_OK;
}

esp_err_t write_config_blob(const std::string &canonical) {
  if (canonical.empty() || canonical.size() > kMaxStoredConfig) {
    return ESP_ERR_INVALID_SIZE;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, kConfigKey, canonical.data(), canonical.size());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

esp_err_t read_config_blob(std::string *canonical) {
  if (canonical == nullptr) return ESP_ERR_INVALID_ARG;
  canonical->clear();

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  size_t size = 0;
  err = nvs_get_blob(handle, kConfigKey, nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ESP_OK;
  }
  if (err != ESP_OK || size == 0 || size > kMaxStoredConfig) {
    nvs_close(handle);
    return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
  }

  std::vector<char> buffer(size);
  err = nvs_get_blob(handle, kConfigKey, buffer.data(), &size);
  nvs_close(handle);
  if (err != ESP_OK) return err;
  canonical->assign(buffer.data(), size);
  return ESP_OK;
}

esp_err_t decode_canonical(
    const std::string &canonical,
    std::vector<LightingChannelConfigV1> *configuration) {
  if (configuration == nullptr || canonical.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  CommandEnvelopeV1 command;
  command.command_type = "LIGHTING_CONFIG_APPLY";
  command.payload_json =
      "{\"configuration\":" + canonical + "}";

  LightingPayloadV1 payload;
  std::string error_message;
  esp_err_t err =
      validate_lighting_payload(command, &payload, &error_message);
  if (err != ESP_OK) return err;

  const std::string normalized =
      canonical_configuration(payload.configuration);
  if (normalized.empty() || normalized != canonical) {
    return ESP_ERR_INVALID_CRC;
  }

  *configuration = std::move(payload.configuration);
  return ESP_OK;
}

bool copy_state(std::vector<LightingChannelConfigV1> *configuration,
                std::vector<ChannelLevelV1> *levels,
                std::string *canonical,
                std::string *hash,
                std::string *authority) {
  if (g_lock == nullptr ||
      xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  const bool ready = g_ready;
  if (configuration != nullptr) *configuration = g_configuration;
  if (levels != nullptr) *levels = g_levels;
  if (canonical != nullptr) *canonical = g_canonical;
  if (hash != nullptr) *hash = g_hash;
  if (authority != nullptr) *authority = g_authority;
  xSemaphoreGive(g_lock);
  return ready;
}

}  // namespace

esp_err_t lighting_configuration_init() {
  esp_err_t err = ensure_lock();
  if (err != ESP_OK) return err;

  if (g_fade_task == nullptr) {
    if (xTaskCreate(&fade_task, "stagecore-fade", 6144, nullptr, 9,
                    &g_fade_task) != pdPASS) {
      g_fade_task = nullptr;
      return ESP_ERR_NO_MEM;
    }
  }

  std::string canonical;
  err = read_config_blob(&canonical);
  if (err != ESP_OK) return err;
  if (canonical.empty()) return ESP_OK;

  std::vector<LightingChannelConfigV1> configuration;
  err = decode_canonical(canonical, &configuration);
  if (err != ESP_OK) return err;

  err = lighting_output_apply_slots(blackout_slots(configuration));
  if (err != ESP_OK) return err;

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  g_configuration = std::move(configuration);
  g_levels = blackout_levels(g_configuration);
  g_canonical = canonical;
  g_hash = sha256_hex(canonical);
  g_authority = "FAILSAFE";
  g_ready = true;
  xSemaphoreGive(g_lock);
  return ESP_OK;
}

bool lighting_configuration_ready() {
  return copy_state(nullptr, nullptr, nullptr, nullptr, nullptr);
}

std::string lighting_configuration_hash() {
  std::string hash;
  (void)copy_state(nullptr, nullptr, nullptr, &hash, nullptr);
  return hash;
}

std::string lighting_configuration_json() {
  std::string canonical;
  (void)copy_state(nullptr, nullptr, &canonical, nullptr, nullptr);
  return canonical;
}

std::vector<ChannelLevelV1> lighting_current_levels() {
  std::vector<ChannelLevelV1> levels;
  (void)copy_state(nullptr, &levels, nullptr, nullptr, nullptr);
  return levels;
}

std::string lighting_authority() {
  std::string authority = "FAILSAFE";
  (void)copy_state(nullptr, nullptr, nullptr, nullptr, &authority);
  return authority;
}

bool lighting_pop_command_event(LightingCommandEvent *event) {
  if (event == nullptr || g_lock == nullptr ||
      xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
  }
  if (g_events.empty()) {
    xSemaphoreGive(g_lock);
    return false;
  }
  *event = std::move(g_events.front());
  g_events.erase(g_events.begin());
  xSemaphoreGive(g_lock);
  return true;
}

bool lighting_active_fade(LightingActiveFade *fade) {
  if (g_lock == nullptr ||
      xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
    return false;
  }
  const bool active = g_fade.active;
  if (active && fade != nullptr) {
    fade->command_id = g_fade.command_id;
    fade->duration_ms = g_fade.duration_ms;
    const int64_t elapsed_us =
        std::max<int64_t>(0, esp_timer_get_time() - g_fade.started_us);
    fade->elapsed_ms =
        std::min<int64_t>(g_fade.duration_ms, elapsed_us / 1000LL);
    fade->targets = g_fade.targets;
  }
  xSemaphoreGive(g_lock);
  return active;
}

esp_err_t lighting_configuration_apply(
    const std::vector<LightingChannelConfigV1> &configuration,
    std::string *configuration_hash) {
  if (configuration.empty() || configuration_hash == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = ensure_lock();
  if (err != ESP_OK) return err;

  const std::string canonical = canonical_configuration(configuration);
  if (canonical.empty() || canonical.size() > kMaxStoredConfig) {
    return ESP_ERR_INVALID_SIZE;
  }
  const std::string hash = sha256_hex(canonical);

  if (xSemaphoreTake(g_operation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  bool old_ready = false;
  std::vector<LightingChannelConfigV1> old_configuration;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
    old_ready = g_ready;
    old_configuration = g_configuration;
    cancel_active_locked("superseded by configuration apply", true);
    xSemaphoreGive(g_lock);
  } else {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }

  // The candidate configuration becomes persistent only after its semantic
  // blackout has been physically confirmed.
  err = lighting_output_apply_slots(blackout_slots(configuration));
  if (err != ESP_OK) {
    xSemaphoreGive(g_operation_lock);
    return err;
  }

  err = write_config_blob(canonical);
  if (err != ESP_OK) {
    // Persistence failed after the candidate blackout reached the bus.
    // Return to a known safe state under the previous configuration.
    const esp_err_t rollback_err =
        old_ready
            ? lighting_output_apply_slots(blackout_slots(old_configuration))
            : lighting_output_blackout_immediate();
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (old_ready) g_levels = blackout_levels(old_configuration);
      g_authority = "FAILSAFE";
      xSemaphoreGive(g_lock);
    }
    xSemaphoreGive(g_operation_lock);
    return rollback_err == ESP_OK ? err : rollback_err;
  }

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }
  g_configuration = configuration;
  g_levels = blackout_levels(g_configuration);
  g_canonical = canonical;
  g_hash = hash;
  g_authority = "STAGECORE";
  g_ready = true;
  xSemaphoreGive(g_lock);
  xSemaphoreGive(g_operation_lock);

  *configuration_hash = hash;
  return ESP_OK;
}

esp_err_t lighting_channels_set(
    const std::vector<ChannelLevelV1> &requested,
    std::vector<ChannelLevelV1> *normalized,
    std::string *error_message) {
  if (requested.empty() || normalized == nullptr ||
      error_message == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = ensure_lock();
  if (err != ESP_OK) return err;
  if (xSemaphoreTake(g_operation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  std::vector<LightingChannelConfigV1> configuration;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }
  if (!g_ready) {
    xSemaphoreGive(g_lock);
    xSemaphoreGive(g_operation_lock);
    *error_message = "No validated lighting configuration is installed";
    return ESP_ERR_INVALID_STATE;
  }
  configuration = g_configuration;
  cancel_active_locked("superseded by immediate channel set", true);
  xSemaphoreGive(g_lock);

  std::vector<DmxSlotValue> updates;
  std::vector<ChannelLevelV1> values;
  if (!normalize_requested(configuration, requested, true,
                           &values, &updates, error_message)) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_NOT_FOUND;
  }

  err = lighting_output_apply_slots(updates);
  if (err != ESP_OK) {
    *error_message = "DMX frame was not confirmed by the output task";
    xSemaphoreGive(g_operation_lock);
    return err;
  }

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }
  update_levels_locked(values);
  g_authority = "STAGECORE";
  xSemaphoreGive(g_lock);
  xSemaphoreGive(g_operation_lock);

  *normalized = std::move(values);
  return ESP_OK;
}

esp_err_t lighting_channels_fade(
    const std::string &command_id,
    const std::vector<ChannelLevelV1> &requested,
    int64_t fade_ms,
    std::vector<ChannelLevelV1> *normalized,
    std::string *error_message) {
  if (command_id.empty() || requested.empty() || fade_ms <= 0 ||
      normalized == nullptr || error_message == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = ensure_lock();
  if (err != ESP_OK) return err;
  if (xSemaphoreTake(g_operation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  err = start_fade_locked(command_id, requested, fade_ms, false,
                          normalized, error_message);
  xSemaphoreGive(g_operation_lock);
  return err;
}

esp_err_t lighting_blackout(bool failsafe) {
  esp_err_t err = ensure_lock();
  if (err != ESP_OK) return err;
  if (xSemaphoreTake(g_operation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  bool ready = false;
  std::vector<LightingChannelConfigV1> configuration;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }
  ready = g_ready;
  configuration = g_configuration;
  cancel_active_locked("superseded by blackout", !failsafe);
  if (failsafe) g_events.clear();
  xSemaphoreGive(g_lock);

  err = ready
      ? lighting_output_apply_slots(blackout_slots(configuration))
      : lighting_output_blackout_immediate();
  if (err != ESP_OK) {
    xSemaphoreGive(g_operation_lock);
    return err;
  }

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }
  if (g_ready) g_levels = blackout_levels(g_configuration);
  g_authority = failsafe ? "FAILSAFE" : "STAGECORE";
  xSemaphoreGive(g_lock);
  xSemaphoreGive(g_operation_lock);
  return ESP_OK;
}

esp_err_t lighting_blackout_fade(
    const std::string &command_id,
    int64_t fade_ms,
    std::string *error_message) {
  if (command_id.empty() || fade_ms <= 0 || error_message == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = ensure_lock();
  if (err != ESP_OK) return err;
  if (xSemaphoreTake(g_operation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  std::vector<ChannelLevelV1> targets;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    xSemaphoreGive(g_operation_lock);
    return ESP_ERR_TIMEOUT;
  }
  if (!g_ready) {
    xSemaphoreGive(g_lock);
    xSemaphoreGive(g_operation_lock);
    *error_message = "No validated lighting configuration is installed";
    return ESP_ERR_INVALID_STATE;
  }
  targets = blackout_levels(g_configuration);
  xSemaphoreGive(g_lock);

  std::vector<ChannelLevelV1> normalized;
  err = start_fade_locked(command_id, targets, fade_ms, true,
                          &normalized, error_message);
  xSemaphoreGive(g_operation_lock);
  return err;
}

}  // namespace stagecore

