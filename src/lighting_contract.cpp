#include "lighting_contract.h"

#include <cmath>
#include <cstring>
#include <set>
#include <utility>

#include "cJSON.h"

namespace stagecore {
namespace {

constexpr size_t kMaxChannels = 12;
constexpr int64_t kMaxFadeMS = 600000;
constexpr int64_t kMinIdentifyMS = 100;
constexpr int64_t kMaxIdentifyMS = 10000;

bool valid_channel_key(const std::string &key) {
  if (key.empty() || key.size() > 64) return false;
  if (!(key[0] >= 'a' && key[0] <= 'z')) return false;
  for (char ch : key) {
    if ((ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '_') {
      continue;
    }
    return false;
  }
  return true;
}

bool valid_kind(const std::string &kind) {
  static constexpr const char *kKinds[] = {
      "DIMMER","WARM_WHITE","COLD_WHITE","RED",
      "GREEN","BLUE","UNUSED",
  };
  for (const char *candidate : kKinds) {
    if (kind == candidate) return true;
  }
  return false;
}

bool finite_level(const cJSON *item, double *value) {
  if (!cJSON_IsNumber(item) || value == nullptr ||
      !std::isfinite(item->valuedouble) ||
      item->valuedouble < 0.0 || item->valuedouble > 100.0) {
    return false;
  }
  *value = item->valuedouble;
  return true;
}

bool integer_number(const cJSON *item, int64_t min_value, int64_t max_value,
                    int64_t *value) {
  if (!cJSON_IsNumber(item) || value == nullptr ||
      !std::isfinite(item->valuedouble)) {
    return false;
  }
  const double rounded = std::floor(item->valuedouble);
  if (rounded != item->valuedouble ||
      rounded < static_cast<double>(min_value) ||
      rounded > static_cast<double>(max_value)) {
    return false;
  }
  *value = static_cast<int64_t>(rounded);
  return true;
}

bool exactly_keys(const cJSON *object,
                  const std::vector<std::string> &allowed,
                  const std::vector<std::string> &required) {
  if (!cJSON_IsObject(object)) return false;
  std::set<std::string> seen;
  const cJSON *child = nullptr;
  cJSON_ArrayForEach(child, object) {
    if (child->string == nullptr) return false;
    const std::string key = child->string;
    bool permitted = false;
    for (const auto &candidate : allowed) {
      if (key == candidate) {
        permitted = true;
        break;
      }
    }
    if (!permitted || !seen.insert(key).second) return false;
  }
  for (const auto &key : required) {
    if (seen.find(key) == seen.end()) return false;
  }
  return true;
}

bool parse_channels(const cJSON *channels,
                    std::vector<ChannelLevelV1> *out) {
  if (!cJSON_IsObject(channels) || out == nullptr) return false;
  out->clear();
  std::set<std::string> seen;
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, channels) {
    if (item->string == nullptr) return false;
    const std::string key = item->string;
    double level = 0;
    if (!valid_channel_key(key) || !seen.insert(key).second ||
        !finite_level(item, &level)) {
      return false;
    }
    out->push_back(ChannelLevelV1{key, level});
    if (out->size() > kMaxChannels) return false;
  }
  return !out->empty();
}

bool parse_optional_fade(const cJSON *root, bool zero_allowed,
                         int64_t *fade_ms) {
  if (fade_ms == nullptr) return false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "fade_ms");
  if (item == nullptr) {
    *fade_ms = 0;
    return zero_allowed;
  }
  const int64_t minimum = zero_allowed ? 0 : 1;
  return integer_number(item, minimum, kMaxFadeMS, fade_ms);
}

bool parse_configuration(const cJSON *configuration,
                         std::vector<LightingChannelConfigV1> *out) {
  if (!cJSON_IsObject(configuration) || out == nullptr ||
      !exactly_keys(configuration,
                    {"schema_version","channels"},
                    {"schema_version","channels"})) {
    return false;
  }
  const cJSON *schema =
      cJSON_GetObjectItemCaseSensitive(configuration, "schema_version");
  int64_t schema_version = 0;
  if (!integer_number(schema, 1, 1, &schema_version)) return false;

  const cJSON *channels =
      cJSON_GetObjectItemCaseSensitive(configuration, "channels");
  if (!cJSON_IsArray(channels)) return false;
  const int count = cJSON_GetArraySize(channels);
  if (count < 1 || count > static_cast<int>(kMaxChannels)) return false;

  std::set<std::string> keys;
  std::set<int> numbers;
  out->clear();
  for (int i = 0; i < count; ++i) {
    const cJSON *item = cJSON_GetArrayItem(channels, i);
    if (!cJSON_IsObject(item) ||
        !exactly_keys(
            item,
            {"channel_key","channel_number","display_name","kind",
             "physical_zone","minimum_level","maximum_level",
             "inverted","enabled"},
            {"channel_key","channel_number","display_name","kind",
             "minimum_level","maximum_level","inverted","enabled"})) {
      return false;
    }

    const cJSON *channel_key =
        cJSON_GetObjectItemCaseSensitive(item, "channel_key");
    const cJSON *channel_number =
        cJSON_GetObjectItemCaseSensitive(item, "channel_number");
    const cJSON *display_name =
        cJSON_GetObjectItemCaseSensitive(item, "display_name");
    const cJSON *kind =
        cJSON_GetObjectItemCaseSensitive(item, "kind");
    const cJSON *physical_zone =
        cJSON_GetObjectItemCaseSensitive(item, "physical_zone");
    const cJSON *minimum_level =
        cJSON_GetObjectItemCaseSensitive(item, "minimum_level");
    const cJSON *maximum_level =
        cJSON_GetObjectItemCaseSensitive(item, "maximum_level");
    const cJSON *inverted =
        cJSON_GetObjectItemCaseSensitive(item, "inverted");
    const cJSON *enabled =
        cJSON_GetObjectItemCaseSensitive(item, "enabled");

    if (!cJSON_IsString(channel_key) || channel_key->valuestring == nullptr ||
        !cJSON_IsString(display_name) || display_name->valuestring == nullptr ||
        !cJSON_IsString(kind) || kind->valuestring == nullptr ||
        !cJSON_IsBool(inverted) || !cJSON_IsBool(enabled)) {
      return false;
    }
    if (physical_zone != nullptr &&
        (!cJSON_IsString(physical_zone) ||
         physical_zone->valuestring == nullptr)) {
      return false;
    }

    LightingChannelConfigV1 cfg;
    cfg.channel_key = channel_key->valuestring;
    cfg.display_name = display_name->valuestring;
    cfg.kind = kind->valuestring;
    cfg.physical_zone =
        physical_zone != nullptr ? physical_zone->valuestring : "";

    int64_t channel_number_value = 0;
    if (!valid_channel_key(cfg.channel_key) ||
        cfg.display_name.empty() || cfg.display_name.size() > 96 ||
        cfg.physical_zone.size() > 96 ||
        !valid_kind(cfg.kind) ||
        !integer_number(channel_number, 1, 12, &channel_number_value) ||
        !finite_level(minimum_level, &cfg.minimum_level) ||
        !finite_level(maximum_level, &cfg.maximum_level) ||
        cfg.minimum_level > cfg.maximum_level) {
      return false;
    }
    cfg.channel_number = static_cast<int>(channel_number_value);
    cfg.inverted = cJSON_IsTrue(inverted);
    cfg.enabled = cJSON_IsTrue(enabled);
    if (cfg.kind == "UNUSED" && cfg.enabled) return false;
    if (!keys.insert(cfg.channel_key).second ||
        !numbers.insert(cfg.channel_number).second) {
      return false;
    }
    out->push_back(std::move(cfg));
  }
  return true;
}

}  // namespace

esp_err_t validate_lighting_payload(const CommandEnvelopeV1 &command,
                                    LightingPayloadV1 *payload,
                                    std::string *error_message) {
  if (payload == nullptr || error_message == nullptr ||
      command.payload_json.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  *payload = LightingPayloadV1{};
  error_message->clear();

  cJSON *root =
      cJSON_ParseWithLength(command.payload_json.data(),
                            command.payload_json.size());
  if (root == nullptr || !cJSON_IsObject(root)) {
    if (root != nullptr) cJSON_Delete(root);
    *error_message = "Payload must be a JSON object";
    return ESP_ERR_INVALID_ARG;
  }

  bool valid = false;

  if (command.command_type == "LIGHTING_CHANNELS_SET") {
    valid = exactly_keys(root, {"channels"}, {"channels"}) &&
            parse_channels(cJSON_GetObjectItemCaseSensitive(root, "channels"),
                           &payload->channels);
  } else if (command.command_type == "LIGHTING_CHANNELS_FADE") {
    valid = exactly_keys(root, {"fade_ms","channels"},
                         {"fade_ms","channels"}) &&
            parse_optional_fade(root, false, &payload->fade_ms) &&
            parse_channels(cJSON_GetObjectItemCaseSensitive(root, "channels"),
                           &payload->channels);
  } else if (command.command_type == "LIGHTING_BLACKOUT") {
    valid = exactly_keys(root, {"fade_ms"}, {}) &&
            parse_optional_fade(root, true, &payload->fade_ms);
  } else if (command.command_type == "LIGHTING_STATE_READ" ||
             command.command_type == "LIGHTING_CONFIG_READ") {
    valid = exactly_keys(root, {}, {});
  } else if (command.command_type == "LIGHTING_IDENTIFY") {
    valid = exactly_keys(root,
                         {"channel_key","level","duration_ms"},
                         {"channel_key","level","duration_ms"});
    const cJSON *key =
        cJSON_GetObjectItemCaseSensitive(root, "channel_key");
    const cJSON *level =
        cJSON_GetObjectItemCaseSensitive(root, "level");
    const cJSON *duration =
        cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    int64_t duration_ms = 0;
    if (valid &&
        cJSON_IsString(key) && key->valuestring != nullptr &&
        valid_channel_key(key->valuestring) &&
        finite_level(level, &payload->level) &&
        integer_number(duration, kMinIdentifyMS, kMaxIdentifyMS,
                       &duration_ms)) {
      payload->channel_key = key->valuestring;
      payload->duration_ms = duration_ms;
    } else {
      valid = false;
    }
  } else if (command.command_type == "LIGHTING_CONFIG_APPLY") {
    valid = exactly_keys(root, {"configuration"}, {"configuration"}) &&
            parse_configuration(
                cJSON_GetObjectItemCaseSensitive(root, "configuration"),
                &payload->configuration);
  }

  cJSON_Delete(root);
  if (!valid) {
    *error_message =
        "Lighting payload does not match the canonical command schema";
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

}  // namespace stagecore
