#include "nvs_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "nvs_cfg";
static const char *NS = "nodifyr";

static nodifyr_config_t s_cfg;

static bool key_valid(const char *key)
{
    if (key == NULL) {
        return false;
    }

    size_t len = strlen(key);
    return len >= 8 && strncmp(key, "ngw_", 4) == 0;
}

static esp_err_t load_string(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static void build_hardware_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed (%s); using fallback id", esp_err_to_name(err));
    }

    snprintf(out, out_len, "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool nodifyr_config_valid_api_key(const char *key)
{
    return key_valid(key);
}

void nodifyr_config_key_preview(const char *key, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    if (key == NULL || key[0] == '\0') {
        snprintf(out, out_len, "(none)");
        return;
    }

    size_t len = strlen(key);
    if (len <= 7) {
        snprintf(out, out_len, "ngw_...");
        return;
    }

    snprintf(out, out_len, "ngw_...%s", key + len - 3);
}

const nodifyr_config_t *nodifyr_config_get(void)
{
    return &s_cfg;
}

esp_err_t nodifyr_config_save_gateway(const char *api_key, const char *api_url)
{
    if (!key_valid(api_key) || api_url == NULL || api_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, "api_key", api_key);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "api_url", api_url);
    }
    if (err == ESP_OK) {
        uint8_t prov = 1;
        err = nvs_set_u8(h, "provisioned", prov);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }

    strncpy(s_cfg.api_key, api_key, sizeof(s_cfg.api_key) - 1);
    strncpy(s_cfg.api_url, api_url, sizeof(s_cfg.api_url) - 1);
    s_cfg.provisioned = true;

    char preview[NODIFYR_API_KEY_MAX];
    nodifyr_config_key_preview(api_key, preview, sizeof(preview));
    ESP_LOGI(TAG, "gateway saved, key=%s url=%s", preview, api_url);
    return ESP_OK;
}

esp_err_t nodifyr_config_save_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, "wifi_ssid", ssid);
    if (err == ESP_OK && pass != NULL) {
        err = nvs_set_str(h, "wifi_pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }

    strncpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid) - 1);
    if (pass != NULL) {
        strncpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass) - 1);
    } else {
        s_cfg.wifi_pass[0] = '\0';
    }

    ESP_LOGI(TAG, "WiFi creds saved, ssid=%s", ssid);
    return ESP_OK;
}

esp_err_t nodifyr_config_disable_auto_setup(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(h, "auto_setup", 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "auto setup disabled until forced");
    return err;
}

bool nodifyr_config_auto_setup_enabled(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return true;
    }

    uint8_t enabled = 1;
    nvs_get_u8(h, "auto_setup", &enabled);
    nvs_close(h);
    return enabled != 0;
}

esp_err_t nodifyr_config_init(bool *needs_provisioning)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    build_hardware_id(s_cfg.hardware_id, sizeof(s_cfg.hardware_id));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace missing, using defaults");
    } else {
        uint8_t prov = 0;
        if (nvs_get_u8(h, "provisioned", &prov) == ESP_OK && prov == 1) {
            s_cfg.provisioned = true;
        }

        load_string(h, "api_key", s_cfg.api_key, sizeof(s_cfg.api_key));
        load_string(h, "api_url", s_cfg.api_url, sizeof(s_cfg.api_url));
        load_string(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
        load_string(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
        nvs_close(h);
    }

    if (s_cfg.api_url[0] == '\0') {
        strncpy(s_cfg.api_url, CONFIG_NODIFYR_API_URL, sizeof(s_cfg.api_url) - 1);
    }

    if (key_valid(s_cfg.api_key)) {
        s_cfg.provisioned = true;
        char preview[NODIFYR_API_KEY_MAX];
        nodifyr_config_key_preview(s_cfg.api_key, preview, sizeof(preview));
        ESP_LOGI(TAG, "using NVS gateway key=%s", preview);
    } else if (key_valid(CONFIG_NODIFYR_GATEWAY_API_KEY)) {
        strncpy(s_cfg.api_key, CONFIG_NODIFYR_GATEWAY_API_KEY, sizeof(s_cfg.api_key) - 1);
        s_cfg.provisioned = true;
        err = nodifyr_config_save_gateway(s_cfg.api_key, s_cfg.api_url);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not persist Kconfig key to NVS: %s", esp_err_to_name(err));
        }
        char preview[NODIFYR_API_KEY_MAX];
        nodifyr_config_key_preview(s_cfg.api_key, preview, sizeof(preview));
        ESP_LOGI(TAG, "using Kconfig gateway key=%s (saved to NVS)", preview);
    } else {
        s_cfg.provisioned = false;
        s_cfg.api_key[0] = '\0';
        ESP_LOGW(TAG, "no gateway API key — uploads disabled until provisioned");
    }

    if (s_cfg.wifi_ssid[0] == '\0' && CONFIG_NODIFYR_WIFI_SSID[0] != '\0') {
        strncpy(s_cfg.wifi_ssid, CONFIG_NODIFYR_WIFI_SSID, sizeof(s_cfg.wifi_ssid) - 1);
        strncpy(s_cfg.wifi_pass, CONFIG_NODIFYR_WIFI_PASSWORD, sizeof(s_cfg.wifi_pass) - 1);
    }

    if (needs_provisioning != NULL) {
        *needs_provisioning = !s_cfg.provisioned;
    }

    ESP_LOGI(TAG, "hardware_id=%s provisioned=%d", s_cfg.hardware_id, s_cfg.provisioned);
    return ESP_OK;
}
