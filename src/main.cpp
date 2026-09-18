#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_dmx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#ifndef STAGECORE_FW_VERSION
#define STAGECORE_FW_VERSION "0.2.0-dev"
#endif

#ifndef STAGECORE_DMX_TX_GPIO
#define STAGECORE_DMX_TX_GPIO 17
#endif

#ifndef STAGECORE_DMX_RTS_GPIO
#define STAGECORE_DMX_RTS_GPIO 21
#endif

#ifndef STAGECORE_DMX_UNIVERSE_SIZE
#define STAGECORE_DMX_UNIVERSE_SIZE 12
#endif

namespace {

constexpr dmx_port_t kDmxPort = DMX_NUM_1;
constexpr int kDmxTxPin = STAGECORE_DMX_TX_GPIO;
constexpr int kDmxRtsPin = STAGECORE_DMX_RTS_GPIO;
constexpr std::size_t kChannelCount = STAGECORE_DMX_UNIVERSE_SIZE;
constexpr std::size_t kFrameBytes = kChannelCount + 1;  // start code + slots
constexpr TickType_t kFramePeriod = pdMS_TO_TICKS(25);   // ~40 Hz

static_assert(kChannelCount >= 1 && kChannelCount <= 12,
              "StageCore lighting node supports 1..12 physical channels");

const char *kTag = "stagecore-light";
std::array<uint8_t, DMX_PACKET_SIZE> g_dmx_data{};

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

bool init_dmx_safe_blackout() {
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_personality_t personalities[] = {
      {static_cast<uint16_t>(kChannelCount), "StageCore Lighting"},
  };

  if (!dmx_driver_install(kDmxPort, &config, personalities, 1)) {
    ESP_LOGE(kTag, "dmx_driver_install failed");
    return false;
  }

  if (!dmx_set_pin(kDmxPort, kDmxTxPin, DMX_PIN_NO_CHANGE, kDmxRtsPin)) {
    ESP_LOGE(kTag, "dmx_set_pin failed (tx=%d rts=%d)", kDmxTxPin, kDmxRtsPin);
    return false;
  }

  // DMX slot 0 is the NULL start code. Slots 1..12 start at blackout.
  g_dmx_data.fill(0);
  dmx_write(kDmxPort, g_dmx_data.data(), kFrameBytes);
  return true;
}

[[noreturn]] void hold_safe_failure() {
  ESP_LOGE(kTag, "firmware stopped in safe failure state");
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

extern "C" void app_main(void) {
  init_nvs();

  ESP_LOGI(kTag, "StageCore ESP32 DMX Lighting Node %s", STAGECORE_FW_VERSION);
  ESP_LOGI(kTag, "safe boot: blackout; DMX TX GPIO=%d RTS GPIO=%d channels=%u",
           kDmxTxPin, kDmxRtsPin, static_cast<unsigned>(kChannelCount));

  if (!init_dmx_safe_blackout()) {
    hold_safe_failure();
  }

  TickType_t last_wake = xTaskGetTickCount();
  while (true) {
    // Continuous DMX output is independent of future Wi-Fi/runtime work.
    // Until an authenticated StageCore command is implemented, all slots stay 0.
    dmx_send_num(kDmxPort, kFrameBytes);
    dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    vTaskDelayUntil(&last_wake, kFramePeriod);
  }
}
