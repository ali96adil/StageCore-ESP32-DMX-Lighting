#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"

namespace stagecore {

esp_err_t lighting_output_init();
esp_err_t lighting_output_start();
struct DmxSlotValue {
  uint8_t channel = 0;
  uint8_t value = 0;
};

esp_err_t lighting_output_apply_slots(
    const std::vector<DmxSlotValue> &updates);
esp_err_t lighting_output_blackout_immediate();
// Fresh output-task-backed logical DMX slot snapshot; not a physical decoder measurement.
esp_err_t lighting_output_read_slots(std::vector<uint8_t> *levels);
bool lighting_output_dmx_healthy();

}  // namespace stagecore
