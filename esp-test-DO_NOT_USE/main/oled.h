#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "u8g2.h"

esp_err_t oled_init(void);
bool oled_is_ready(void);
u8g2_t *oled_get(void);
void oled_lock(void);
void oled_unlock(void);
