#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "command_contract.h"
#include "esp_err.h"

namespace stagecore {

struct ChannelLevelV1 {
  std::string channel_key;
  double level = 0;
};

struct LightingChannelConfigV1 {
  std::string channel_key;
  int channel_number = 0;
  std::string display_name;
  std::string kind;
  std::string physical_zone;
  double minimum_level = 0;
  double maximum_level = 100;
  bool inverted = false;
  bool enabled = false;
};

struct LightingPayloadV1 {
  std::vector<ChannelLevelV1> channels;
  int64_t fade_ms = 0;
  std::string channel_key;
  double level = 0;
  int64_t duration_ms = 0;
  std::vector<LightingChannelConfigV1> configuration;
};

esp_err_t validate_lighting_payload(const CommandEnvelopeV1 &command,
                                    LightingPayloadV1 *payload,
                                    std::string *error_message);

}  // namespace stagecore
