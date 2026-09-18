#include "config_store.h"

#include <algorithm>
#include <vector>

#include "nvs.h"

namespace stagecore {
namespace {

constexpr char kNamespace[] = "stagecore";
constexpr char kSSIDKey[] = "wifi_ssid";
constexpr char kPasswordKey[] = "wifi_pass";
constexpr char kProjectKey[] = "project_id";
constexpr char kDisplayKey[] = "display_name";

esp_err_t read_string(nvs_handle_t handle, const char *key, std::string *value) {
  size_t length = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &length);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    value->clear();
    return ESP_OK;
  }
  if (err != ESP_OK) return err;
  if (length == 0) {
    value->clear();
    return ESP_OK;
  }

  std::vector<char> buffer(length);
  err = nvs_get_str(handle, key, buffer.data(), &length);
  if (err != ESP_OK) return err;
  *value = buffer.data();
  return ESP_OK;
}

esp_err_t write_string(nvs_handle_t handle, const char *key, const std::string &value) {
  return nvs_set_str(handle, key, value.c_str());
}

}  // namespace

bool DeviceConfig::complete() const {
  return !wifi_ssid.empty() && wifi_password.size() >= 8 &&
         project_id.size() == 36 && !display_name.empty();
}

esp_err_t load_device_config(DeviceConfig *config) {
  if (config == nullptr) return ESP_ERR_INVALID_ARG;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *config = DeviceConfig{};
    return ESP_OK;
  }
  if (err != ESP_OK) return err;

  DeviceConfig loaded;
  err = read_string(handle, kSSIDKey, &loaded.wifi_ssid);
  if (err == ESP_OK) err = read_string(handle, kPasswordKey, &loaded.wifi_password);
  if (err == ESP_OK) err = read_string(handle, kProjectKey, &loaded.project_id);
  if (err == ESP_OK) err = read_string(handle, kDisplayKey, &loaded.display_name);
  nvs_close(handle);

  if (err == ESP_OK) *config = std::move(loaded);
  return err;
}

esp_err_t save_device_config(const DeviceConfig &config) {
  if (!config.complete()) return ESP_ERR_INVALID_ARG;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;

  err = write_string(handle, kSSIDKey, config.wifi_ssid);
  if (err == ESP_OK) err = write_string(handle, kPasswordKey, config.wifi_password);
  if (err == ESP_OK) err = write_string(handle, kProjectKey, config.project_id);
  if (err == ESP_OK) err = write_string(handle, kDisplayKey, config.display_name);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

}  // namespace stagecore
