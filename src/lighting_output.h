#pragma once

#include <cstdint>

#include "esp_err.h"

namespace stagecore {

esp_err_t lighting_output_init();
esp_err_t lighting_output_start();
esp_err_t lighting_output_blackout_immediate();
bool lighting_output_dmx_healthy();

}  // namespace stagecore
