#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t wifi_init_common(void);
esp_err_t wifi_start_sta(void);
esp_err_t wifi_start_ap(const char *ssid, const char *password);
esp_err_t wifi_stop_ap(void);
bool wifi_is_connected(void);
bool wifi_wait_connected(TickType_t timeout_ticks);

/* Background AP scan used to populate the provisioning portal's network list.
 * wifi_scan_start() is idempotent and runs the scan on its own task so it can
 * overlap the boot animation; it leaves the radio stopped when done. */
void wifi_scan_start(void);
bool wifi_scan_wait(TickType_t timeout_ticks);
size_t wifi_scan_count(void);
bool wifi_scan_get(size_t index, char *ssid_out, size_t ssid_len,
                   int8_t *rssi_out, bool *secure_out);
