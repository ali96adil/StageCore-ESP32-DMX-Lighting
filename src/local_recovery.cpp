#include "local_recovery.h"

#include <cstdint>
#include <string>

#include "cJSON.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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
constexpr uint32_t kEmergencyHoldMS = 2000;
constexpr uint32_t kTrustResetHoldMS = 10000;
constexpr uint32_t kPollMS = 50;
constexpr uint32_t kDiagnosticsWindowMS = 60000;
constexpr uint16_t kDiagnosticsPort = 8088;

TaskHandle_t g_recovery_task = nullptr;
httpd_handle_t g_diagnostics_server = nullptr;
TickType_t g_diagnostics_expires = 0;

std::string print_json(cJSON *root) {
  if (root == nullptr) return {};
  char *text = cJSON_PrintUnformatted(root);
  std::string out = text != nullptr ? text : "";
  if (text != nullptr) cJSON_free(text);
  return out;
}

esp_err_t diagnostics_status_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "authority", lighting_authority().c_str());
  cJSON_AddBoolToObject(root, "local_emergency_blackout_latched",
                        lighting_local_emergency_blackout_latched());
  cJSON_AddBoolToObject(root, "dmx_healthy", lighting_output_dmx_healthy());
  cJSON_AddBoolToObject(root, "configuration_ready",
                        lighting_configuration_ready());
  cJSON_AddStringToObject(root, "configuration_hash",
                          lighting_configuration_hash().c_str());
  cJSON_AddNumberToObject(
      root, "uptime_seconds",
      static_cast<double>(esp_timer_get_time() / 1000000LL));
  cJSON_AddNumberToObject(root, "reset_reason",
                          static_cast<int>(esp_reset_reason()));

  wifi_ap_record_t access_point{};
  if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
    cJSON_AddNumberToObject(root, "wifi_rssi_dbm", access_point.rssi);
  }

  HubBinding binding;
  const esp_err_t binding_err = load_hub_binding(&binding);
  cJSON_AddBoolToObject(root, "hub_trust_configured",
                        binding_err == ESP_OK && binding.complete());

  cJSON *levels = cJSON_CreateObject();
  if (levels == nullptr) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_ERR_NO_MEM;
  }
  for (const auto &entry : lighting_current_levels()) {
    cJSON_AddNumberToObject(levels, entry.channel_key.c_str(), entry.level);
  }
  cJSON_AddItemToObject(root, "logical_levels", levels);
  cJSON_AddStringToObject(
      root, "measurement_boundary",
      "software logical/output-task state only; not independent decoder or fixture measurement");

  const std::string response = print_json(root);
  cJSON_Delete(root);
  if (response.empty()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "status serialization failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, response.c_str(), response.size());
}

esp_err_t diagnostics_root_handler(httpd_req_t *req) {
  static constexpr char kPage[] =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>StageCore Lighting Diagnostics</title>"
      "<style>body{font-family:system-ui;max-width:720px;margin:32px auto;"
      "padding:0 18px}pre{white-space:pre-wrap;background:#111;color:#eee;"
      "padding:16px;border-radius:8px}</style></head><body>"
      "<h1>StageCore Lighting Diagnostics</h1>"
      "<p>Physical-presence window. Read-only. Emergency blackout remains "
      "latched until reboot.</p>"
      "<pre id='s'>Loading...</pre>"
      "<script>fetch('/status',{cache:'no-store'}).then(r=>r.json()).then("
      "j=>s.textContent=JSON.stringify(j,null,2)).catch(e=>s.textContent=e)"
      "</script></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

void stop_diagnostics_window() {
  if (g_diagnostics_server == nullptr) return;
  httpd_stop(g_diagnostics_server);
  g_diagnostics_server = nullptr;
  g_diagnostics_expires = 0;
  ESP_LOGI(kTag, "protected local diagnostics window closed");
}

esp_err_t start_or_refresh_diagnostics_window() {
  g_diagnostics_expires =
      xTaskGetTickCount() + pdMS_TO_TICKS(kDiagnosticsWindowMS);
  if (g_diagnostics_server != nullptr) {
    ESP_LOGI(kTag, "protected local diagnostics window refreshed");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kDiagnosticsPort;
  config.max_uri_handlers = 2;
  config.lru_purge_enable = true;

  esp_err_t err = httpd_start(&g_diagnostics_server, &config);
  if (err != ESP_OK) {
    g_diagnostics_server = nullptr;
    return err;
  }

  httpd_uri_t root{};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = diagnostics_root_handler;
  err = httpd_register_uri_handler(g_diagnostics_server, &root);
  if (err != ESP_OK) {
    stop_diagnostics_window();
    return err;
  }

  httpd_uri_t status{};
  status.uri = "/status";
  status.method = HTTP_GET;
  status.handler = diagnostics_status_handler;
  err = httpd_register_uri_handler(g_diagnostics_server, &status);
  if (err != ESP_OK) {
    stop_diagnostics_window();
    return err;
  }

  ESP_LOGW(kTag,
           "protected read-only diagnostics armed for %u ms on port %u",
           static_cast<unsigned>(kDiagnosticsWindowMS),
           static_cast<unsigned>(kDiagnosticsPort));
  return ESP_OK;
}

void expire_diagnostics_if_needed() {
  if (g_diagnostics_server == nullptr) return;
  const TickType_t now = xTaskGetTickCount();
  if (static_cast<int32_t>(now - g_diagnostics_expires) >= 0) {
    stop_diagnostics_window();
  }
}

esp_err_t enforce_local_emergency_blackout() {
  const esp_err_t err = lighting_local_emergency_blackout();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "local emergency blackout not confirmed: %s",
             esp_err_to_name(err));
    return err;
  }
  ESP_LOGW(kTag,
           "LOCAL_WEB emergency blackout latched until attended reboot");
  return ESP_OK;
}

void recovery_task(void *) {
  LocalRecoveryHoldPolicy emergency_policy(kEmergencyHoldMS);
  LocalRecoveryHoldPolicy trust_reset_policy(kTrustResetHoldMS);

  while (true) {
    const bool pressed =
        gpio_get_level(static_cast<gpio_num_t>(STAGECORE_LOCAL_RECOVERY_GPIO)) == 0;

    if (emergency_policy.sample(pressed, kPollMS)) {
      if (enforce_local_emergency_blackout() == ESP_OK) {
        const esp_err_t diagnostics_err = start_or_refresh_diagnostics_window();
        if (diagnostics_err != ESP_OK) {
          ESP_LOGE(kTag, "local diagnostics unavailable: %s",
                   esp_err_to_name(diagnostics_err));
        }
      }
    }

    if (trust_reset_policy.sample(pressed, kPollMS)) {
      ESP_LOGW(kTag,
               "local Hub trust reset requested after long physical hold");

      if (enforce_local_emergency_blackout() != ESP_OK) {
        ESP_LOGE(kTag,
                 "Hub trust reset refused because blackout was not confirmed");
      } else {
        const esp_err_t err = clear_hub_binding();
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

    expire_diagnostics_if_needed();
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

  if (xTaskCreate(&recovery_task, "stagecore-recovery", 5120, nullptr, 3,
                  &g_recovery_task) != pdPASS) {
    g_recovery_task = nullptr;
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(kTag,
           "local recovery armed gpio=%d emergency_hold_ms=%u "
           "trust_reset_hold_ms=%u",
           STAGECORE_LOCAL_RECOVERY_GPIO,
           static_cast<unsigned>(kEmergencyHoldMS),
           static_cast<unsigned>(kTrustResetHoldMS));
  return ESP_OK;
}

}  // namespace stagecore
