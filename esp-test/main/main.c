#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "anim.h"
#include "ble_display.h"
#include "ble_scan.h"
#include "nvs_config.h"
#include "oled.h"
#include "provision.h"
#include "scan_store.h"
#include "uploader.h"
#include "wifi.h"

static const char *TAG = "main";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static void run_normal_mode(bool upload_enabled)
{
    scan_store_init();

    if (ble_display_start() != ESP_OK) {
        ESP_LOGW(TAG, "display task not started; continuing headless");
    }

    if (ble_scan_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE scan failed to start; device idle until reboot");
    }

    if (!upload_enabled) {
        ESP_LOGW(TAG, "normal mode — BLE only (not provisioned)");
        return;
    }

    esp_err_t err = wifi_start_sta();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi start failed (%s); BLE continues, uploads disabled",
                 esp_err_to_name(err));
        return;
    }

    err = uploader_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uploader start failed (%s); BLE continues", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "normal mode — BLE scan + upload");
}

void app_main(void)
{
    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s); using volatile defaults", esp_err_to_name(err));
    }

    if (oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed; running without display");
    }

    bool needs_provisioning = false;
    nodifyr_config_init(&needs_provisioning);
    bool auto_setup = nodifyr_config_auto_setup_enabled();
    bool will_auto_provision = needs_provisioning && auto_setup;

    /* Scan for nearby networks in the background so the provisioning portal's
     * dropdown is ready by the time the user opens it. Overlaps the boot
     * animation below; only worth it when we expect to show the portal. */
    if (will_auto_provision) {
        wifi_scan_start();
    }

    if (oled_is_ready()) {
        anim_boot(oled_get(), false);
    }

    provision_serial_monitor_start();

    bool force_provision = provision_boot_button_held() || provision_serial_requested();
    if (force_provision || will_auto_provision) {
        ESP_LOGI(TAG, "entering provisioning mode");
        /* Returns only if setup couldn't start; otherwise it reboots itself. */
        provision_run();
    }

    run_normal_mode(nodifyr_config_get()->provisioned);
}
