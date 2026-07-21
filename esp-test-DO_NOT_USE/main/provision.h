#pragma once

#include <stdbool.h>

#include "esp_err.h"

bool provision_boot_button_held(void);
bool provision_serial_requested(void);
void provision_serial_monitor_start(void);
esp_err_t provision_run(void);
