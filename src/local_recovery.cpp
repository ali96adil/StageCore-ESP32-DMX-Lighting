#include "local_recovery.h"

#include "config_store.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lighting_configuration.h"
#include "lighting_output.h"
#include "recovery_policy.h"

#ifndef STAGECORE_LOCAL_RECOVERY_GPIO
#define STAGECORE_LOCAL_RECOVERY_GPIO 0
#endif

namespace stagecore {
namespace {

constexpr char kTag[] = "stagecore-recovery";
constexpr uint32_t kRequiredHoldMS = 10000;
constexpr uint32_t kPollMS = 50;
TaskHandle_t g_recovery_task = nullptr;

void recovery_task(void *) {
  LocalRecoveryHoldPolicy policy(kRequiredHoldMS);

  while (true) {
    const bool pressed =
        gpio_get_level(static_cast<gpio_num_t>(STAGECORE_LOCAL_RECOVERY_GPIO)) == 0;

    if (policy.sample(pressed, kPollMS)) {
      ESP_LOGW(kTag,
               "local Hub trust reset requested; forcing confirmed blackout first");

      esp_err_t err = lighting_blackout(true);
      if (err == ESP_OK) err = lighting_output_blackout_immediate();
      if (err != ESP_OK || !lighting_output_dmx_healthy()) {
        ESP_LOGE(kTag,
                 "Hub trust reset refused because blackout was not confirmed");
      } else {
        err = clear_hub_binding();
        if (err != ESP_OK) {
          ESP_LOGE(kTag, "Hub trust reset failed: %s", esp_err_to_name(err));
        } else {
          ESP_LOGW(kTag,
                   "remembered Hub trust cleared; Wi-Fi, device identity and "
                   "configuration preserved; rebooting for normal discovery");
          vTaskDelay(pdMS_TO_TICKS(500));
          esp_restart();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(kPollMS));
  }
}

}  // namespace

esp_err_t start_local_hub_trust_reset_monitor() {
  if (g_recovery_task != nullptr) return ESP_OK;

  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << STAGECORE_LOCAL_RECOVERY_GPIO;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;

  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) return err;

  if (xTaskCreate(&recovery_task, "stagecore-recovery", 3072, nullptr, 3,
                  &g_recovery_task) != pdPASS) {
    g_recovery_task = nullptr;
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(kTag,
           "local Hub trust reset armed on GPIO %d (hold %u ms)",
           STAGECORE_LOCAL_RECOVERY_GPIO,
           static_cast<unsigned>(kRequiredHoldMS));
  return ESP_OK;
}

}  // namespace stagecore
