#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "config_store.h"
#include "device_identity.h"
#include "esp_dmx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_discovery.h"
#include "hub_security.h"
#include "nvs_flash.h"
#include "provisioning.h"

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
constexpr std::size_t kFrameBytes = kChannelCount + 1;
constexpr TickType_t kFramePeriod = pdMS_TO_TICKS(25);

static_assert(kChannelCount >= 1 && kChannelCount <= 12,
              "StageCore lighting node supports 1..12 physical channels");

const char *kTag = "stagecore-light";
std::array<uint8_t, DMX_PACKET_SIZE> g_dmx_data{};

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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

  g_dmx_data.fill(0);
  dmx_write(kDmxPort, g_dmx_data.data(), kFrameBytes);
  return true;
}

void dmx_task(void *) {
  TickType_t last_wake = xTaskGetTickCount();
  while (true) {
    dmx_send_num(kDmxPort, kFrameBytes);
    dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    vTaskDelayUntil(&last_wake, kFramePeriod);
  }
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
  ESP_LOGI(kTag,
           "safe boot: blackout; DMX TX GPIO=%d RTS GPIO=%d channels=%u",
           kDmxTxPin, kDmxRtsPin, static_cast<unsigned>(kChannelCount));

  if (!init_dmx_safe_blackout()) {
    hold_safe_failure("DMX initialization failed");
  }
  if (xTaskCreate(&dmx_task, "stagecore-dmx", 4096, nullptr, 10, nullptr) !=
      pdPASS) {
    hold_safe_failure("DMX task creation failed");
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

  stagecore::VerifiedHub hub;
  while (stagecore::discover_and_verify_hub(&hub) != ESP_OK) {
    ESP_LOGW(kTag, "no verified StageCore Hub yet; DMX remains blackout");
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  ESP_LOGI(kTag, "verified Hub %s (%s)",
           hub.display_name.c_str(), hub.hub_id.c_str());

  stagecore::RuntimeCredential credential;
  while (stagecore::ensure_paired_and_authenticate(
             hub, &identity, config.display_name, &credential) != ESP_OK) {
    ESP_LOGW(kTag,
             "StageCore pairing/auth unavailable; DMX remains blackout");
    credential = stagecore::RuntimeCredential{};
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  ESP_LOGI(kTag,
           "authenticated runtime session ready; WebSocket is the next sub-slice");

  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
