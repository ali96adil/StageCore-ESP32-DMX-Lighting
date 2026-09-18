#pragma once

#include <string>

#include "esp_err.h"

namespace stagecore {

struct DeviceConfig {
  std::string wifi_ssid;
  std::string wifi_password;
  std::string project_id;
  std::string display_name;

  bool complete() const;
};

esp_err_t load_device_config(DeviceConfig *config);
esp_err_t save_device_config(const DeviceConfig &config);

}  // namespace stagecore
