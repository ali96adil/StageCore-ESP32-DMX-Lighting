#include <string>

#include "config_store.h"
#include "device_identity.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_discovery.h"
#include "hub_security.h"
#include "lighting_configuration.h"
#include "lighting_output.h"
#include "nvs_flash.h"
#include "provisioning.h"
#include "stage_device_runtime.h"

#ifndef STAGECORE_FW_VERSION
#define STAGECORE_FW_VERSION "0.2.0-dev"
#endif

namespace {

const char *kTag = "stagecore-light";

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

[[noreturn]] void hold_safe_failure(const char *reason) {
  ESP_LOGE(kTag, "safe failure: %s", reason);
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

std::string default_display_name(const std::string &device_id) {
  std::string suffix;
  for (char ch : device_id) {
    if (ch != '-') suffix.push_back(ch);
  }
  if (suffix.size() > 6) suffix = suffix.substr(suffix.size() - 6);
  return "StageCore Lighting " + suffix;
}

}  // namespace

extern "C" void app_main(void) {
  init_nvs();

  ESP_LOGI(kTag, "StageCore ESP32 DMX Lighting Node %s", STAGECORE_FW_VERSION);
  ESP_LOGI(kTag, "safe boot: blackout");

  if (stagecore::lighting_output_init() != ESP_OK) {
    hold_safe_failure("DMX initialization failed");
  }
  if (stagecore::lighting_output_start() != ESP_OK) {
    hold_safe_failure("DMX task startup failed");
  }

#ifdef STAGECORE_RECOVERY_CLEAR_HUB_BINDING_ON_BOOT
  ESP_LOGW(kTag,
           "RECOVERY BUILD: clearing remembered Hub trust binding only");
  if (stagecore::lighting_blackout(true) != ESP_OK) {
    hold_safe_failure("recovery blackout failed");
  }
  if (stagecore::clear_hub_binding() != ESP_OK) {
    hold_safe_failure("Hub trust binding reset failed");
  }
  ESP_LOGW(kTag,
           "Hub trust binding cleared; Wi-Fi, project and device identity "
           "preserved");
  hold_safe_failure(
      "Hub trust reset complete; flash the normal diagnostic firmware");
#endif

  const esp_err_t lighting_config_err =
      stagecore::lighting_configuration_init();
  if (lighting_config_err != ESP_OK) {
    ESP_LOGW(kTag,
             "stored lighting configuration unavailable (%s); "
             "physical-zero failsafe retained until CONFIG_APPLY",
             esp_err_to_name(lighting_config_err));
  }

  stagecore::DeviceIdentity identity;
  if (identity.LoadOrCreate() != ESP_OK) {
    hold_safe_failure("persistent P-256 identity unavailable");
  }
  ESP_LOGI(kTag, "device_id=%s", identity.device_id().c_str());

  stagecore::DeviceConfig config;
  if (stagecore::load_device_config(&config) != ESP_OK) {
    hold_safe_failure("configuration storage unavailable");
  }

  const std::string fallback_name = default_display_name(identity.device_id());
  if (!config.complete()) {
    stagecore::run_provisioning_portal(identity.device_id(), fallback_name);
  }

  if (stagecore::connect_station(config.wifi_ssid, config.wifi_password, 30000) !=
      ESP_OK) {
    stagecore::run_provisioning_portal(identity.device_id(),
                                       config.display_name.empty()
                                           ? fallback_name
                                           : config.display_name);
  }

  ESP_LOGI(kTag, "provisioned for project %s as %s",
           config.project_id.c_str(), config.display_name.c_str());

  while (true) {
    stagecore::VerifiedHub hub;
    if (stagecore::discover_and_verify_hub(&hub) != ESP_OK) {
      ESP_LOGW(kTag, "no verified StageCore Hub yet; DMX remains blackout");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    ESP_LOGI(kTag, "verified Hub %s (%s)",
             hub.display_name.c_str(), hub.hub_id.c_str());

    stagecore::RuntimeCredential credential;
    if (stagecore::ensure_paired_and_authenticate(
            hub, &identity, config.display_name, &credential) != ESP_OK) {
      ESP_LOGW(kTag,
               "StageCore pairing/auth unavailable; DMX remains blackout");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    const esp_err_t runtime_err = stagecore::run_stage_device_runtime(
        hub, credential, identity, config);
    const esp_err_t failsafe_err = stagecore::lighting_blackout(true);
    if (failsafe_err != ESP_OK) {
      ESP_LOGE(kTag, "failsafe blackout failed after runtime exit: %s",
               esp_err_to_name(failsafe_err));
    }
    ESP_LOGW(kTag,
             "Stage Device runtime ended (%s); failsafe blackout requested "
             "before re-authentication",
             esp_err_to_name(runtime_err));
    credential = stagecore::RuntimeCredential{};
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
