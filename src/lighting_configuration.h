#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "lighting_contract.h"

namespace stagecore {

esp_err_t lighting_configuration_init();
bool lighting_configuration_ready();
std::string lighting_configuration_hash();
std::string lighting_configuration_json();
std::vector<ChannelLevelV1> lighting_current_levels();
std::string lighting_authority();

struct LightingCommandEvent {
  std::string command_id;
  std::string status;
  std::string error_code;
  std::string category;
  std::string message;
  std::vector<ChannelLevelV1> levels;
};

struct LightingActiveFade {
  std::string command_id;
  int64_t duration_ms = 0;
  int64_t elapsed_ms = 0;
  std::vector<ChannelLevelV1> targets;
};

bool lighting_pop_command_event(LightingCommandEvent *event);
bool lighting_active_fade(LightingActiveFade *fade);

esp_err_t lighting_configuration_apply(
    const std::vector<LightingChannelConfigV1> &configuration,
    std::string *configuration_hash);

esp_err_t lighting_channels_set(
    const std::vector<ChannelLevelV1> &requested,
    std::vector<ChannelLevelV1> *normalized,
    std::string *error_message);

esp_err_t lighting_channels_fade(
    const std::string &command_id,
    const std::vector<ChannelLevelV1> &requested,
    int64_t fade_ms,
    std::vector<ChannelLevelV1> *normalized,
    std::string *error_message);

esp_err_t lighting_blackout(bool failsafe);

esp_err_t lighting_blackout_fade(
    const std::string &command_id,
    int64_t fade_ms,
    std::string *error_message);

}  // namespace stagecore
