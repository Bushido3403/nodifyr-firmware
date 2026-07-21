#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t ble_scan_start(void);
esp_err_t ble_scan_pause(uint32_t timeout_ms);
esp_err_t ble_scan_resume(void);
