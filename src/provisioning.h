#pragma once

#include <string>

#include "esp_err.h"

namespace stagecore {

esp_err_t init_network_stack();
esp_err_t connect_station(const std::string &ssid, const std::string &password,
                          int timeout_ms);
[[noreturn]] void run_provisioning_portal(const std::string &device_id,
                                          const std::string &default_display_name);

}  // namespace stagecore
