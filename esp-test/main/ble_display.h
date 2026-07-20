#pragma once

#include "esp_err.h"
#include "scan_store.h"

esp_err_t ble_display_start(void);
void ble_display_push(const scan_record_t *rec);
