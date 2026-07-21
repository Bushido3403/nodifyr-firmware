#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define NODIFYR_API_KEY_MAX   64
#define NODIFYR_API_URL_MAX   128
#define NODIFYR_WIFI_SSID_MAX 33
#define NODIFYR_WIFI_PASS_MAX 65
#define NODIFYR_HW_ID_MAX     32

typedef struct {
    char api_key[NODIFYR_API_KEY_MAX];
    char api_url[NODIFYR_API_URL_MAX];
    char wifi_ssid[NODIFYR_WIFI_SSID_MAX];
    char wifi_pass[NODIFYR_WIFI_PASS_MAX];
    char hardware_id[NODIFYR_HW_ID_MAX];
    bool provisioned;
} nodifyr_config_t;

esp_err_t nodifyr_config_init(bool *needs_provisioning);
const nodifyr_config_t *nodifyr_config_get(void);
esp_err_t nodifyr_config_save_gateway(const char *api_key, const char *api_url);
esp_err_t nodifyr_config_save_wifi(const char *ssid, const char *pass);
esp_err_t nodifyr_config_disable_auto_setup(void);
bool nodifyr_config_auto_setup_enabled(void);
void nodifyr_config_key_preview(const char *key, char *out, size_t out_len);
bool nodifyr_config_valid_api_key(const char *key);
