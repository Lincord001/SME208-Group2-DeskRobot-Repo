#ifndef WIFI_NETWORK_H
#define WIFI_NETWORK_H

#include <stdbool.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_network_init(void);
esp_err_t wifi_network_start(void);
esp_err_t wifi_network_start_config_mode(bool clear_saved);
esp_err_t wifi_network_set_power_save(bool enable);
bool wifi_network_is_connected(void);
bool wifi_network_is_power_save_enabled(void);
const char *wifi_network_get_status_string(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_NETWORK_H
