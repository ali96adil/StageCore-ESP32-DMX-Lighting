#include "provisioning.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <string>

#include "config_store.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#ifndef STAGECORE_EXPERIMENTAL_DEVICE_V2
#define STAGECORE_EXPERIMENTAL_DEVICE_V2 0
#endif

namespace stagecore {
namespace {

constexpr char kTag[] = "stagecore-net";
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
constexpr int kMaxRetries = 8;

EventGroupHandle_t g_wifi_events = nullptr;
int g_retry_count = 0;

struct PortalContext {
  std::string default_display_name;
};

void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (g_retry_count < kMaxRetries) {
      ++g_retry_count;
      esp_wifi_connect();
    } else if (g_wifi_events != nullptr) {
      xEventGroupSetBits(g_wifi_events, kFailedBit);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    g_retry_count = 0;
    if (g_wifi_events != nullptr) xEventGroupSetBits(g_wifi_events, kConnectedBit);
  }
}

std::string suffix_from_id(const std::string &device_id) {
  std::string compact;
  for (char ch : device_id) {
    if (std::isxdigit(static_cast<unsigned char>(ch))) compact.push_back(ch);
  }
  if (compact.size() > 6) compact = compact.substr(compact.size() - 6);
  return compact;
}

std::string random_ap_password() {
  static constexpr char kAlphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  std::array<char, 13> out{};
  for (size_t i = 0; i < out.size() - 1; ++i) {
    out[i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
  }
  return out.data();
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

std::string url_decode(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      out.push_back(' ');
    } else if (input[i] == '%' && i + 2 < input.size()) {
      int hi = hex_value(input[i + 1]);
      int lo = hex_value(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(input[i]);
      }
    } else {
      out.push_back(input[i]);
    }
  }
  return out;
}

std::string form_value(const std::string &body, const std::string &key) {
  size_t start = 0;
  while (start < body.size()) {
    const size_t end = body.find('&', start);
    const std::string pair =
        body.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) {
      return url_decode(pair.substr(eq + 1));
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

#if !STAGECORE_EXPERIMENTAL_DEVICE_V2
bool valid_project_id(const std::string &value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash) {
      if (value[i] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}
#endif

esp_err_t root_handler(httpd_req_t *req) {
  auto *ctx = static_cast<PortalContext *>(req->user_ctx);
  std::string page =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>StageCore Lighting Setup</title>"
      "<style>body{font-family:system-ui;max-width:620px;margin:40px auto;padding:0 18px}"
      "label{display:block;margin:14px 0 5px}input{width:100%;padding:10px;box-sizing:border-box}"
      "button{margin-top:20px;padding:11px 18px}small{color:#666}</style></head><body>"
      "<h1>StageCore Lighting Node</h1>"
      "<p>First-run provisioning. DMX remains at blackout.</p>"
      "<form method='post' action='/save'>"
      "<label>Wi-Fi SSID</label><input name='ssid' maxlength='32' required>"
      "<label>Wi-Fi password</label><input name='password' type='password' minlength='8' maxlength='63' required>";
#if !STAGECORE_EXPERIMENTAL_DEVICE_V2
  page +=
      "<label>StageCore Project ID</label><input name='project_id' maxlength='36' required "
      "placeholder='xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'>";
#endif
  page += "<label>Display name</label><input name='display_name' maxlength='64' value='";
  page += ctx ? ctx->default_display_name : "StageCore Lighting";
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  page +=
      "' required><small>Device identity is persistent. Assign this node to a Show inside StageCore after pairing. Output stays black until qualified.</small>"
      "<button type='submit'>Save and restart</button></form></body></html>";
#else
  page +=
      "' required><small>The Project ID is bootstrap-only. Daily cue authoring stays in StageCore.</small>"
      "<button type='submit'>Save and restart</button></form></body></html>";
#endif

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, page.c_str(), page.size());
}

esp_err_t save_handler(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    return ESP_FAIL;
  }

  std::string body(req->content_len, '\0');
  size_t received = 0;
  while (received < req->content_len) {
    const int rc = httpd_req_recv(req, body.data() + received,
                                  req->content_len - received);
    if (rc <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "receive failed");
      return ESP_FAIL;
    }
    received += static_cast<size_t>(rc);
  }

  DeviceConfig config;
  config.wifi_ssid = form_value(body, "ssid");
  config.wifi_password = form_value(body, "password");
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  // Reject rather than trust a forged Project claim in an experimental v2
  // setup request. The Operator assigns Projects using Hub authentication.
  if (!form_value(body, "project_id").empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Project ID is Hub-owned");
    return ESP_FAIL;
  }
#else
  config.project_id = form_value(body, "project_id");
#endif
  config.display_name = form_value(body, "display_name");

  if (config.wifi_ssid.empty() || config.wifi_ssid.size() > 32 ||
      config.wifi_password.size() < 8 || config.wifi_password.size() > 63 ||
#if !STAGECORE_EXPERIMENTAL_DEVICE_V2
      !valid_project_id(config.project_id) ||
#endif
      config.display_name.empty() ||
      config.display_name.size() > 64) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid configuration");
    return ESP_FAIL;
  }

  const esp_err_t err = save_device_config(config);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "save failed");
    return err;
  }

  const char *response =
      "<html><body><h1>Saved</h1><p>Restarting StageCore Lighting Node.</p></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(800));
  esp_restart();
  return ESP_OK;
}

}  // namespace

esp_err_t init_network_stack() {
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  return ESP_OK;
}

esp_err_t connect_station(const std::string &ssid, const std::string &password,
                          int timeout_ms) {
  if (ssid.empty() || password.size() < 8) return ESP_ERR_INVALID_ARG;
  esp_err_t err = init_network_stack();
  if (err != ESP_OK) return err;

  if (g_wifi_events == nullptr) g_wifi_events = xEventGroupCreate();
  if (g_wifi_events == nullptr) return ESP_ERR_NO_MEM;
  xEventGroupClearBits(g_wifi_events, kConnectedBit | kFailedBit);
  g_retry_count = 0;

  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK) return err;

  esp_event_handler_instance_t wifi_instance;
  esp_event_handler_instance_t ip_instance;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
      &wifi_instance));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
      &ip_instance));

  wifi_config_t wifi{};
  std::snprintf(reinterpret_cast<char *>(wifi.sta.ssid), sizeof(wifi.sta.ssid),
                "%s", ssid.c_str());
  std::snprintf(reinterpret_cast<char *>(wifi.sta.password),
                sizeof(wifi.sta.password), "%s", password.c_str());
  wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
  if (err == ESP_OK) err = esp_wifi_start();
  if (err != ESP_OK) return err;

  const EventBits_t bits = xEventGroupWaitBits(
      g_wifi_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(timeout_ms));

  esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        ip_instance);
  esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_instance);

  if (bits & kConnectedBit) {
    ESP_LOGI(kTag, "connected to configured Stage LAN");
    return ESP_OK;
  }

  ESP_LOGW(kTag, "station connection failed; provisioning will resume");
  esp_wifi_stop();
  esp_wifi_deinit();
  return ESP_ERR_TIMEOUT;
}

[[noreturn]] void run_provisioning_portal(
    const std::string &device_id, const std::string &default_display_name) {
  ESP_ERROR_CHECK(init_network_stack());

  const std::string ssid = "StageCore-Light-" + suffix_from_id(device_id);
  const std::string password = random_ap_password();

  esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));

  wifi_config_t wifi{};
  std::snprintf(reinterpret_cast<char *>(wifi.ap.ssid), sizeof(wifi.ap.ssid),
                "%s", ssid.c_str());
  wifi.ap.ssid_len = static_cast<uint8_t>(ssid.size());
  std::snprintf(reinterpret_cast<char *>(wifi.ap.password),
                sizeof(wifi.ap.password), "%s", password.c_str());
  wifi.ap.max_connection = 2;
  wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());

  static PortalContext context;
  context.default_display_name = default_display_name;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 4;
  httpd_handle_t server = nullptr;
  ESP_ERROR_CHECK(httpd_start(&server, &config));

  httpd_uri_t root{};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = &root_handler;
  root.user_ctx = &context;
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));

  httpd_uri_t save{};
  save.uri = "/save";
  save.method = HTTP_POST;
  save.handler = &save_handler;
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));

  ESP_LOGW(kTag, "PROVISIONING REQUIRED");
  ESP_LOGW(kTag, "join Wi-Fi SSID: %s", ssid.c_str());
  ESP_LOGW(kTag, "temporary AP password: %s", password.c_str());
  ESP_LOGW(kTag, "open http://192.168.4.1/");

  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

}  // namespace stagecore
