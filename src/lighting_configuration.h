#pragma once

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

esp_err_t lighting_configuration_apply(
    const std::vector<LightingChannelConfigV1> &configuration,
    std::string *configuration_hash);

esp_err_t lighting_channels_set(
    const std::vector<ChannelLevelV1> &requested,
    std::vector<ChannelLevelV1> *normalized,
    std::string *error_message);

esp_err_t lighting_blackout(bool failsafe);

}  // namespace stagecore
