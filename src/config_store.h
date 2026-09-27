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

struct HubBinding {
  std::string hub_id;
  std::string fingerprint;
  std::string tls_sha256;

  bool complete() const;
};

esp_err_t load_device_config(DeviceConfig *config);
esp_err_t save_device_config(const DeviceConfig &config);
esp_err_t load_hub_binding(HubBinding *binding);
esp_err_t save_hub_binding(const HubBinding &binding);
esp_err_t clear_hub_binding();

}  // namespace stagecore
