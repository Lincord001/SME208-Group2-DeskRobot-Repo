#include "wifi_network.h"

#include <cstdio>
#include <string>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "display.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_network";
static constexpr TickType_t INITIAL_CONNECT_TIMEOUT = pdMS_TO_TICKS(45000);

static bool s_initialized;
static bool s_started;
static bool s_started_with_saved_config;
static volatile bool s_connected;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_status[32] = "WiFi init";

static void wifi_network_start_station_task(void *arg);

static void wifi_network_set_status(const char *status)
{
    if (status == nullptr) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    std::snprintf(s_status, sizeof(s_status), "%s", status);
    portEXIT_CRITICAL(&s_status_lock);

    display_set_wifi_status(status);
}

static esp_err_t wifi_network_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase before WiFi configuration can be stored");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void wifi_network_event_handler(WifiEvent event, const std::string& data)
{
    switch (event) {
    case WifiEvent::Scanning:
        s_connected = false;
        wifi_network_set_status("scanning");
        ESP_LOGI(TAG, "WiFi scanning");
        break;

    case WifiEvent::Connecting:
        s_connected = false;
        wifi_network_set_status("connecting");
        ESP_LOGI(TAG, "WiFi connecting to SSID: %s", data.c_str());
        break;

    case WifiEvent::Connected: {
        s_connected = true;
        auto& wifi = WifiManager::GetInstance();
        wifi_network_set_status("connected");
        ESP_LOGI(TAG, "WiFi connected: SSID=%s IP=%s RSSI=%d",
                 data.c_str(),
                 wifi.GetIpAddress().c_str(),
                 wifi.GetRssi());
        break;
    }

    case WifiEvent::Disconnected:
        s_connected = false;
        wifi_network_set_status("reconnecting");
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting; reason=%s",
                 data.empty() ? "unknown" : data.c_str());
        break;

    case WifiEvent::ConfigModeEnter: {
        s_connected = false;
        auto& wifi = WifiManager::GetInstance();
        wifi_network_set_status("config mode");
        ESP_LOGI(TAG, "WiFi config mode: AP=%s URL=%s",
                 wifi.GetApSsid().c_str(),
                 wifi.GetApWebUrl().c_str());
        break;
    }

    case WifiEvent::ConfigModeExit:
        wifi_network_set_status("connecting");
        ESP_LOGI(TAG, "WiFi config mode exited");
        s_started_with_saved_config = true;
        if (xTaskCreate(wifi_network_start_station_task,
                        "wifi_start_sta",
                        3072,
                        nullptr,
                        3,
                        nullptr) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create WiFi station start task");
        }
        break;
    }
}

static void wifi_network_initial_connect_watchdog(void *arg)
{
    (void)arg;

    vTaskDelay(INITIAL_CONNECT_TIMEOUT);

    if (s_started_with_saved_config && !wifi_network_is_connected()) {
        auto& wifi = WifiManager::GetInstance();
        auto& ssids = SsidManager::GetInstance().GetSsidList();
        if (!ssids.empty() && !wifi.IsConfigMode()) {
            wifi_network_set_status("failed");
            ESP_LOGW(TAG, "Initial WiFi connection failed; entering config mode");
            wifi.StartConfigAp();
        }
    }

    vTaskDelete(nullptr);
}

static void wifi_network_start_station_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(500));

    auto& wifi = WifiManager::GetInstance();
    auto& ssids = SsidManager::GetInstance().GetSsidList();
    if (!ssids.empty() && !wifi.IsConnected() && !wifi.IsConfigMode()) {
        ESP_LOGI(TAG, "Starting station after WiFi config mode exit");
        wifi.StartStation();
    }

    vTaskDelete(nullptr);
}

esp_err_t wifi_network_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    wifi_network_set_status("init");

    esp_err_t err = wifi_network_init_nvs();
    if (err != ESP_OK) {
        wifi_network_set_status("failed");
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        wifi_network_set_status("failed");
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        wifi_network_set_status("failed");
        return err;
    }

    WifiManagerConfig config;
    config.ssid_prefix = "SME208-ESP32";
    config.language = "zh-CN";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 120;

    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback(wifi_network_event_handler);
    if (!wifi.Initialize(config)) {
        wifi_network_set_status("failed");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi network module initialized");
    return ESP_OK;
}

esp_err_t wifi_network_start(void)
{
    esp_err_t err = wifi_network_init();
    if (err != ESP_OK) {
        return err;
    }

    if (s_started) {
        return ESP_OK;
    }

    auto& ssids = SsidManager::GetInstance().GetSsidList();
    auto& wifi = WifiManager::GetInstance();

    if (ssids.empty()) {
        s_started_with_saved_config = false;
        wifi_network_set_status("config mode");
        ESP_LOGI(TAG, "No saved WiFi credentials; starting config AP");
        wifi.StartConfigAp();
    } else {
        s_started_with_saved_config = true;
        wifi_network_set_status("connecting");
        ESP_LOGI(TAG, "Found %u saved WiFi credential(s); starting station",
                 (unsigned)ssids.size());
        wifi.StartStation();

        if (xTaskCreate(wifi_network_initial_connect_watchdog,
                        "wifi_init_watchdog",
                        3072,
                        nullptr,
                        3,
                        nullptr) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create WiFi initial connection watchdog");
        }
    }

    s_started = true;
    return ESP_OK;
}

bool wifi_network_is_connected(void)
{
    return s_connected && WifiManager::GetInstance().IsConnected();
}

const char *wifi_network_get_status_string(void)
{
    return s_status;
}
