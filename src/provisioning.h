#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "config_store.h"

namespace stagecore {

esp_err_t init_network_stack();
esp_err_t connect_station(const std::string &ssid, const std::string &password,
                          int timeout_ms);
esp_err_t wait_for_station_connection(int timeout_ms);
esp_err_t wait_for_station_connection_with_recovery(
    const std::string &device_id, const DeviceConfig &current_config,
    uint32_t already_offline_ms = 0);
[[noreturn]] void run_provisioning_portal(const std::string &device_id,
                                          const std::string &default_display_name);

}  // namespace stagecore
