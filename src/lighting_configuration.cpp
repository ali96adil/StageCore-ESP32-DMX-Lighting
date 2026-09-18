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

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
bool g_ready = false;
std::vector<LightingChannelConfigV1> g_configuration;
std::vector<ChannelLevelV1> g_levels;
std::string g_canonical;
std::string g_hash;
std::string g_authority = "FAILSAFE";

esp_err_t ensure_lock() {
  if (g_lock != nullptr) return ESP_OK;
  g_lock = xSemaphoreCreateMutex();
  return g_lock != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
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

  // Force the candidate configuration to its semantic logical blackout before
  // it becomes persistent or authoritative.
  err = lighting_output_apply_slots(blackout_slots(configuration));
  if (err != ESP_OK) return err;

  err = write_config_blob(canonical);
  if (err != ESP_OK) return err;

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  g_configuration = configuration;
  g_levels = blackout_levels(g_configuration);
  g_canonical = canonical;
  g_hash = hash;
  g_authority = "STAGECORE";
  g_ready = true;
  xSemaphoreGive(g_lock);

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

  std::vector<LightingChannelConfigV1> configuration;
  if (!copy_state(&configuration, nullptr, nullptr, nullptr, nullptr)) {
    *error_message = "No validated lighting configuration is installed";
    return ESP_ERR_INVALID_STATE;
  }

  std::vector<DmxSlotValue> updates;
  std::vector<ChannelLevelV1> values;
  for (const auto &request : requested) {
    auto it = std::find_if(
        configuration.begin(), configuration.end(),
        [&](const auto &cfg) { return cfg.channel_key == request.channel_key; });
    if (it == configuration.end() || !it->enabled || it->kind == "UNUSED") {
      *error_message = "Requested lighting channel is unknown or disabled";
      return ESP_ERR_NOT_FOUND;
    }

    double level = request.level;
    if (level < it->minimum_level) level = it->minimum_level;
    if (level > it->maximum_level) level = it->maximum_level;
    values.push_back(ChannelLevelV1{it->channel_key, level});
    updates.push_back(DmxSlotValue{
        static_cast<uint8_t>(it->channel_number),
        level_to_dmx(*it, level),
    });
  }

  esp_err_t err = lighting_output_apply_slots(updates);
  if (err != ESP_OK) {
    *error_message = "DMX frame was not confirmed by the output task";
    return err;
  }

  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
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
  g_authority = "STAGECORE";
  xSemaphoreGive(g_lock);

  *normalized = std::move(values);
  return ESP_OK;
}

esp_err_t lighting_blackout(bool failsafe) {
  std::vector<LightingChannelConfigV1> configuration;
  const bool ready =
      copy_state(&configuration, nullptr, nullptr, nullptr, nullptr);

  esp_err_t err = ready
      ? lighting_output_apply_slots(blackout_slots(configuration))
      : lighting_output_blackout_immediate();
  if (err != ESP_OK) return err;

  if (g_lock == nullptr ||
      xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (g_ready) g_levels = blackout_levels(g_configuration);
  g_authority = failsafe ? "FAILSAFE" : "STAGECORE";
  xSemaphoreGive(g_lock);
  return ESP_OK;
}

}  // namespace stagecore
