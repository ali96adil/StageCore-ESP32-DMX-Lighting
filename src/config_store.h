#pragma once

#include <string>

#include "esp_err.h"
#include "foundation_store.h"

namespace stagecore {

struct DeviceConfig {
  std::string wifi_ssid;
  std::string wifi_password;
  std::string project_id;
  std::string display_name;

  bool complete() const;
};

FoundationStore &foundation_store();

esp_err_t load_device_config(DeviceConfig *config);
esp_err_t save_device_config(const DeviceConfig &config);
esp_err_t load_setup_ap_password(std::string *password);
esp_err_t save_setup_ap_password(const std::string &password);
esp_err_t clear_setup_ap_password();
esp_err_t load_hub_binding(HubBinding *binding);
esp_err_t save_hub_binding(const HubBinding &binding);
esp_err_t clear_hub_binding();

}  // namespace stagecore
