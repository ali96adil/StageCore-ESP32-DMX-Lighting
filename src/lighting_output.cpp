#include "lighting_output.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "esp_dmx.h"
#include "dmx_driver_diagnostics.h"
#include "esp_log.h"
#include "hal/uart_ll.h"
#include "soc/uart_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef STAGECORE_DMX_TX_GPIO
#define STAGECORE_DMX_TX_GPIO 17
#endif

#ifndef STAGECORE_DMX_RTS_GPIO
#define STAGECORE_DMX_RTS_GPIO 21
#endif

#ifndef STAGECORE_DMX_UNIVERSE_SIZE
#define STAGECORE_DMX_UNIVERSE_SIZE 12
#endif

namespace stagecore {
namespace {

constexpr char kTag[] = "lighting-output";
constexpr dmx_port_t kDmxPort = DMX_NUM_1;
constexpr int kDmxTxPin = STAGECORE_DMX_TX_GPIO;
constexpr int kDmxRtsPin = STAGECORE_DMX_RTS_GPIO;
constexpr std::size_t kChannelCount = STAGECORE_DMX_UNIVERSE_SIZE;
constexpr std::size_t kFrameBytes = kChannelCount + 1;
constexpr TickType_t kFramePeriod = pdMS_TO_TICKS(25);
constexpr TickType_t kApplyTimeout = pdMS_TO_TICKS(150);

static_assert(kChannelCount >= 1 && kChannelCount <= 12,
              "StageCore lighting node supports 1..12 physical channels");

std::array<uint8_t, DMX_PACKET_SIZE> g_frame{};
SemaphoreHandle_t g_frame_lock = nullptr;
TaskHandle_t g_task = nullptr;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_healthy{false};
std::atomic<uint32_t> g_requested_generation{0};
std::atomic<uint32_t> g_sent_generation{0};

bool generation_reached(uint32_t sent, uint32_t target) {
  return static_cast<int32_t>(sent - target) >= 0;
}

bool wait_for_generation(uint32_t target, TickType_t timeout) {
  const TickType_t started = xTaskGetTickCount();
  while (!generation_reached(g_sent_generation.load(), target)) {
    if ((xTaskGetTickCount() - started) >= timeout) return false;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

void dmx_task(void *) {
  TickType_t last_wake = xTaskGetTickCount();
  std::array<uint8_t, DMX_PACKET_SIZE> local{};
  constexpr TickType_t kDiagnosticInterval = pdMS_TO_TICKS(60000);
  TickType_t last_failure_log = 0;
  uint32_t consecutive_failures = 0;
  bool failure_reported = false;
  const char *last_failure_stage = nullptr;

  // Diagnostics only: never alter the DMX frame, physical levels, timing,
  // failsafe, or g_healthy semantics when classifying a failed TX attempt.
  const auto record_failure = [&](const char *stage, size_t written,
                                  size_t sent, bool wait_done) {
    if (consecutive_failures != UINT32_MAX) ++consecutive_failures;
    const TickType_t now = xTaskGetTickCount();
    // Capture the UART registers only on the first failure, a stage change,
    // or the existing 60-second diagnostic interval. No register writes or
    // driver reinitialization; this diagnostic does not claim physical TX.
    const bool stage_changed =
        last_failure_stage == nullptr || std::strcmp(stage, last_failure_stage) != 0;
    if (!failure_reported || stage_changed ||
        static_cast<TickType_t>(now - last_failure_log) >=
            kDiagnosticInterval) {
      const uart_dev_t *const uart = UART_LL_GET_HW(kDmxPort);
      const uint32_t int_raw = uart->int_raw.val;
      const uint32_t int_st = uart->int_st.val;
      const uint32_t int_ena = uart->int_ena.val;
      const uint32_t status = uart->status.val;
      stagecore_dmx_driver_snapshot_t driver_snapshot{};
      const bool have_driver_snapshot =
          stagecore_dmx_capture_driver_snapshot(
              static_cast<int>(kDmxPort), &driver_snapshot);
      ESP_LOGW(kTag,
               "DMX TX unhealthy stage=%s written=%u sent=%u wait_done=%d "
               "expected=%u consecutive_failures=%lu",
               stage, static_cast<unsigned>(written),
               static_cast<unsigned>(sent), static_cast<int>(wait_done),
               static_cast<unsigned>(kFrameBytes),
               static_cast<unsigned long>(consecutive_failures));
      ESP_LOGW(kTag,
               "DMX UART1 snapshot stage=%s int_raw=0x%08lx "
               "int_st=0x%08lx int_ena=0x%08lx status=0x%08lx",
               stage, static_cast<unsigned long>(int_raw),
               static_cast<unsigned long>(int_st),
               static_cast<unsigned long>(int_ena),
               static_cast<unsigned long>(status));
      if (have_driver_snapshot) {
        ESP_LOGW(
            kTag,
            "DMX driver snapshot stage=%s enabled=%d controller=%d "
            "status=%d progress=%d head=%d size=%d task_waiting=0x%08lx "
            "eop_us=%lld",
            stage, static_cast<int>(driver_snapshot.enabled),
            static_cast<int>(driver_snapshot.is_controller),
            driver_snapshot.status, driver_snapshot.progress,
            driver_snapshot.head, driver_snapshot.size,
            static_cast<unsigned long>(driver_snapshot.task_waiting),
            static_cast<long long>(driver_snapshot.controller_eop_timestamp));
      } else {
        ESP_LOGW(kTag, "DMX driver snapshot stage=%s unavailable", stage);
      }
      last_failure_log = now;
    }
    last_failure_stage = stage;
    failure_reported = true;
  };

  while (true) {
    uint32_t generation = 0;
    if (xSemaphoreTake(g_frame_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
      local = g_frame;
      generation = g_requested_generation.load();
      xSemaphoreGive(g_frame_lock);
    } else {
      g_healthy.store(false);
      record_failure("frame_lock", 0, 0, false);
      vTaskDelayUntil(&last_wake, kFramePeriod);
      continue;
    }

    const size_t written =
        dmx_write(kDmxPort, local.data(), kFrameBytes);
    const size_t sent =
        written == kFrameBytes ? dmx_send_num(kDmxPort, kFrameBytes) : 0;
    const bool wait_done =
        sent == kFrameBytes && dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    const bool completed = sent == kFrameBytes && wait_done;

    g_healthy.store(completed);
    if (completed) {
      g_sent_generation.store(generation);
      if (failure_reported) {
        ESP_LOGI(kTag, "DMX TX recovered after %lu failed frames",
                 static_cast<unsigned long>(consecutive_failures));
        consecutive_failures = 0;
        failure_reported = false;
        last_failure_stage = nullptr;
      }
    } else {
      const char *stage = written != kFrameBytes
                              ? "write"
                              : sent != kFrameBytes ? "send" : "wait_sent";
      record_failure(stage, written, sent, wait_done);
    }

    vTaskDelayUntil(&last_wake, kFramePeriod);
  }
}

}  // namespace

esp_err_t lighting_output_init() {
  if (g_initialized.load()) return ESP_OK;

  g_frame_lock = xSemaphoreCreateMutex();
  if (g_frame_lock == nullptr) return ESP_ERR_NO_MEM;

  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_personality_t personalities[] = {
      {static_cast<uint16_t>(kChannelCount), "StageCore Lighting"},
  };

  if (!dmx_driver_install(kDmxPort, &config, personalities, 1)) {
    vSemaphoreDelete(g_frame_lock);
    g_frame_lock = nullptr;
    return ESP_FAIL;
  }
  if (!dmx_set_pin(kDmxPort, kDmxTxPin, DMX_PIN_NO_CHANGE, kDmxRtsPin)) {
    vSemaphoreDelete(g_frame_lock);
    g_frame_lock = nullptr;
    return ESP_FAIL;
  }

  g_frame.fill(0);
  g_requested_generation.store(1);
  g_sent_generation.store(0);
  g_healthy.store(false);
  g_initialized.store(true);

  ESP_LOGI(kTag, "safe blackout initialized tx=%d rts=%d channels=%u",
           kDmxTxPin, kDmxRtsPin,
           static_cast<unsigned>(kChannelCount));
  return ESP_OK;
}

// This reports the currently requested/sent software frame only. It is not
// independent verification of the connected DMX decoder or LED voltage.
esp_err_t lighting_output_read_slots(std::vector<uint8_t> *levels) {
  if (levels == nullptr || !g_initialized.load() || g_frame_lock == nullptr ||
      g_task == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(g_frame_lock, pdMS_TO_TICKS(25)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  const uint32_t wanted = g_requested_generation.load();
  const bool current = g_healthy.load() &&
                       g_sent_generation.load() == wanted;
  std::vector<uint8_t> snapshot;
  if (current) {
    snapshot.reserve(kChannelCount);
    for (size_t i = 1; i <= kChannelCount; ++i) {
      snapshot.push_back(g_frame[i]);
    }
  }
  xSemaphoreGive(g_frame_lock);
  if (!current) return ESP_ERR_INVALID_STATE;
  *levels = std::move(snapshot);
  return ESP_OK;
}

esp_err_t lighting_output_start() {
  if (!g_initialized.load() || g_frame_lock == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (g_task != nullptr) return ESP_OK;

  if (xTaskCreate(&dmx_task, "stagecore-dmx", 4096, nullptr, 10, &g_task) !=
      pdPASS) {
    g_task = nullptr;
    return ESP_ERR_NO_MEM;
  }

  const uint32_t target = g_requested_generation.load();
  if (!wait_for_generation(target, kApplyTimeout)) {
    g_healthy.store(false);
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

esp_err_t lighting_output_apply_slots(
    const std::vector<DmxSlotValue> &updates) {
  if (!g_initialized.load() || g_frame_lock == nullptr || g_task == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (updates.empty() || updates.size() > kChannelCount) {
    return ESP_ERR_INVALID_ARG;
  }

  std::array<bool, kChannelCount + 1> seen{};
  for (const auto &update : updates) {
    if (update.channel < 1 || update.channel > kChannelCount ||
        seen[update.channel]) {
      return ESP_ERR_INVALID_ARG;
    }
    seen[update.channel] = true;
  }

  std::vector<DmxSlotValue> previous;
  previous.reserve(updates.size());

  if (xSemaphoreTake(g_frame_lock, pdMS_TO_TICKS(25)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  for (const auto &update : updates) {
    previous.push_back(DmxSlotValue{update.channel, g_frame[update.channel]});
    g_frame[update.channel] = update.value;
  }
  const uint32_t target = g_requested_generation.fetch_add(1) + 1;
  xSemaphoreGive(g_frame_lock);

  const bool sent =
      wait_for_generation(target, kApplyTimeout) && g_healthy.load();
  if (sent) return ESP_OK;

  g_healthy.store(false);

  // A command that was reported FAILED must not remain staged for a later
  // refresh. Restore the exact previous slot values and request a rollback
  // generation. This is best-effort if the bus itself is unhealthy.
  if (xSemaphoreTake(g_frame_lock, pdMS_TO_TICKS(25)) == pdTRUE) {
    for (const auto &entry : previous) {
      g_frame[entry.channel] = entry.value;
    }
    const uint32_t rollback =
        g_requested_generation.fetch_add(1) + 1;
    xSemaphoreGive(g_frame_lock);
    (void)wait_for_generation(rollback, kApplyTimeout);
  }
  return ESP_ERR_TIMEOUT;
}

esp_err_t lighting_output_blackout_immediate() {
  std::vector<DmxSlotValue> updates;
  updates.reserve(kChannelCount);
  for (std::size_t i = 1; i <= kChannelCount; ++i) {
    updates.push_back(
        DmxSlotValue{static_cast<uint8_t>(i), static_cast<uint8_t>(0)});
  }
  return lighting_output_apply_slots(updates);
}

bool lighting_output_dmx_healthy() {
  return g_healthy.load();
}

}  // namespace stagecore
