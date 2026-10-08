#include <string>

#include "config_store.h"
#include "connection_loss_failsafe_policy.h"
#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_discovery.h"
#include "hub_security.h"
#include "lighting_configuration.h"
#include "lighting_output.h"
#include "local_recovery.h"
#include "nvs_flash.h"
#include "provisioning.h"
#include "stage_device_runtime.h"

#ifndef STAGECORE_EXPERIMENTAL_DEVICE_V2
#define STAGECORE_EXPERIMENTAL_DEVICE_V2 0
#endif

#ifndef STAGECORE_FW_VERSION
#define STAGECORE_FW_VERSION "0.2.0-dev"
#endif

#ifndef STAGECORE_BUILD_REVISION
#define STAGECORE_BUILD_REVISION "unknown"
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

stagecore::FoundationDeviceDescriptor foundation_descriptor() {
  stagecore::FoundationDeviceDescriptor descriptor;
  descriptor.hostname_prefix = "stagecore-light-";
  descriptor.platform = "esp32";
  descriptor.architecture = "xtensa";
  descriptor.firmware_version = STAGECORE_FW_VERSION;
  descriptor.capabilities = {
      "lighting.channels.set",
      "lighting.channels.fade",
      "lighting.blackout",
      "lighting.state.read",
      "lighting.identify",
      "lighting.config.read",
      "lighting.config.apply",
  };
  return descriptor;
}

}  // namespace

extern "C" void app_main(void) {
  init_nvs();

  const esp_app_desc_t *app = esp_app_get_description();
  ESP_LOGI(kTag,
           "StageCore ESP32 DMX Lighting Node %s source=%s app=%s",
           STAGECORE_FW_VERSION,
           STAGECORE_BUILD_REVISION,
           app != nullptr ? app->version : "unknown");
  ESP_LOGI(kTag, "safe boot: blackout");

  if (stagecore::lighting_output_init() != ESP_OK) {
    hold_safe_failure("DMX initialization failed");
  }
  if (stagecore::lighting_output_start() != ESP_OK) {
    hold_safe_failure("DMX task startup failed");
  }

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

  stagecore::FoundationStore &foundation = stagecore::foundation_store();
  const stagecore::FoundationDeviceDescriptor descriptor =
      foundation_descriptor();

  stagecore::DeviceConfig config;
  if (stagecore::load_device_config(&config) != ESP_OK) {
    hold_safe_failure("configuration storage unavailable");
  }
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  // Never reuse a legacy NVS Project as Hub-owned v2 assignment authority.
  // The old persisted value remains intact for a possible v1 rollback.
  config.project_id.clear();
#endif

  const std::string fallback_name = default_display_name(identity.device_id());
  if (!config.complete()) {
    stagecore::run_provisioning_portal(identity.device_id(), fallback_name);
  }

  esp_err_t station_err =
      stagecore::connect_station(config.wifi_ssid, config.wifi_password, 30000);
  if (station_err != ESP_OK && station_err != ESP_ERR_TIMEOUT) {
    hold_safe_failure("configured Stage LAN initialization failed");
  }
  if (station_err == ESP_ERR_TIMEOUT) {
    ESP_LOGW(kTag,
             "configured Stage LAN still unavailable; DMX remains blackout "
             "while automatic reconnect continues");
    station_err = stagecore::wait_for_station_connection_with_recovery(
        identity.device_id(), config, 30000);
  }
  if (station_err != ESP_OK) {
    hold_safe_failure("configured Stage LAN recovery failed");
  }

  const esp_err_t recovery_err =
      stagecore::start_local_hub_trust_reset_monitor();
  if (recovery_err != ESP_OK) {
    ESP_LOGE(kTag, "local recovery surface unavailable: %s",
             esp_err_to_name(recovery_err));
  }

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
  ESP_LOGW(kTag,
           "EXPERIMENTAL v2 ACTIVE candidate: projectless; Hub-owned exact "
           "scope required before commands; device=%s",
           identity.device_id().c_str());
#elif STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
  ESP_LOGW(kTag,
           "EXPERIMENTAL v2 read-only probe: projectless, blackout-only; "
           "device=%s",
           identity.device_id().c_str());
#else
  ESP_LOGW(kTag,
           "EXPERIMENTAL v2 blackout-only: projectless; device=%s",
           identity.device_id().c_str());
#endif
#else
  ESP_LOGI(kTag, "provisioned for project %s as %s",
           config.project_id.c_str(), config.display_name.c_str());
#endif

  while (true) {
    if (stagecore::wait_for_station_connection(0) != ESP_OK) {
      ESP_LOGW(kTag,
               "Stage LAN disconnected; DMX remains blackout while "
               "reconnect/recovery continues");
      const esp_err_t recovery_err =
          stagecore::wait_for_station_connection_with_recovery(
              identity.device_id(), config);
      if (recovery_err != ESP_OK) {
        ESP_LOGE(kTag, "Stage LAN recovery failed: %s",
                 esp_err_to_name(recovery_err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    stagecore::VerifiedHub hub;
    if (stagecore::discover_and_verify_hub(&foundation, &hub) != ESP_OK) {
      ESP_LOGW(kTag, "no verified StageCore Hub yet; DMX remains blackout");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    ESP_LOGI(kTag, "verified Hub %s (%s)",
             hub.display_name.c_str(), hub.hub_id.c_str());

    stagecore::RuntimeCredential credential;
    if (stagecore::ensure_paired_and_authenticate(
            hub, &identity, descriptor, config.display_name, &credential) !=
        ESP_OK) {
      ESP_LOGW(kTag,
               "StageCore pairing/auth unavailable; DMX remains blackout");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    const esp_err_t runtime_err = stagecore::run_stage_device_runtime(
        hub, credential, identity, config);
    esp_err_t failsafe_err = stagecore::lighting_connection_loss_failsafe(
        stagecore::kConnectionLossHoldMs,
        stagecore::kConnectionLossFadeMs);
    if (failsafe_err != ESP_OK) {
      ESP_LOGE(kTag,
               "bounded connection-loss fade failed (%s); requesting "
               "immediate blackout fallback",
               esp_err_to_name(failsafe_err));
      failsafe_err = stagecore::lighting_blackout(true);
    }
    if (failsafe_err != ESP_OK) {
      ESP_LOGE(kTag, "failsafe blackout fallback failed after runtime exit: %s",
               esp_err_to_name(failsafe_err));
    }
    if (failsafe_err == ESP_OK) {
      ESP_LOGW(kTag,
               "Stage Device runtime ended (%s); failsafe policy hold=%lldms "
               "fade=%lldms reached blackout before re-authentication",
               esp_err_to_name(runtime_err),
               static_cast<long long>(stagecore::kConnectionLossHoldMs),
               static_cast<long long>(stagecore::kConnectionLossFadeMs));
    } else {
      ESP_LOGE(kTag,
               "Stage Device runtime ended (%s); local failsafe could not "
               "confirm blackout before re-authentication",
               esp_err_to_name(runtime_err));
    }
    credential = stagecore::RuntimeCredential{};
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
