#include "ble_scan.h"

#include <string.h>

#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "ble_display.h"
#include "scan_store.h"
#include "sensor_packet.h"

static const char *TAG = "ble_scan";

static uint8_t s_own_addr_type;
static volatile bool s_scanning;
static volatile bool s_pause_req;
static volatile bool s_started;
static volatile bool s_synced;
static SemaphoreHandle_t s_stopped_sem;

void ble_store_config_init(void);

static int gap_event(struct ble_gap_event *event, void *arg);

static esp_err_t scan_begin(void)
{
    struct ble_gap_disc_params params = {0};

    params.filter_duplicates = 0;
    params.passive = 1;
    params.itvl = BLE_GAP_SCAN_ITVL_MS(11);
    params.window = BLE_GAP_SCAN_WIN_MS(11);

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
        return ESP_FAIL;
    }

    s_scanning = true;
    return ESP_OK;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr failed: %d", rc);
        return;
    }

    s_synced = true;
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    if (scan_begin() != ESP_OK) {
        ESP_LOGE(TAG, "initial scan failed; watchdog will retry");
        return;
    }
    ESP_LOGI(TAG, "continuous scan running");
}

/* Self-heal: if scanning ever stops while it shouldn't, restart it. */
static void scan_watchdog_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_synced && s_started && !s_pause_req && !s_scanning) {
            ESP_LOGW(TAG, "scan stalled; restarting");
            esp_coex_preference_set(ESP_COEX_PREFER_BT);
            if (scan_begin() != ESP_OK) {
                ESP_LOGE(TAG, "scan restart failed; retrying");
            }
        }
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset: %d", reason);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        scan_record_t rec;
        if (!nodifyr_parse_adv(event->disc.data, event->disc.length_data, event->disc.rssi,
                               event->disc.addr.val, &rec)) {
            return 0;
        }
        scan_store_push(&rec);
        ble_display_push(&rec);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        xSemaphoreGive(s_stopped_sem);
        if (!s_pause_req && s_started) {
            scan_begin();
        }
        return 0;
    default:
        return 0;
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_scan_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_stopped_sem = xSemaphoreCreateBinary();
    if (s_stopped_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    s_started = true;
    nimble_port_freertos_init(host_task);

    if (xTaskCreate(scan_watchdog_task, "scan_wd", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "scan watchdog not started (low memory)");
    }
    return ESP_OK;
}

esp_err_t ble_scan_pause(uint32_t timeout_ms)
{
    if (!s_started || s_stopped_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_scanning) {
        return ESP_OK;
    }

    /* Clear any stale completion signal so we wait for *this* cancel. */
    xSemaphoreTake(s_stopped_sem, 0);

    s_pause_req = true;
    int rc = ble_gap_disc_cancel();
    if (rc != 0) {
        /* BLE_HS_EALREADY: already stopped — treat as paused. */
        s_scanning = false;
        ESP_LOGW(TAG, "disc_cancel rc=%d", rc);
        return ESP_OK;
    }

    if (xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* Cancel was issued; treat scan as stopped even if DISC_COMPLETE is late.
         * Keep s_pause_req set so a tardy completion won't auto-restart mid-upload. */
        s_scanning = false;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t ble_scan_resume(void)
{
    s_pause_req = false;
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    if (s_scanning) {
        /* Pause timed out with stale s_scanning — cancel before restarting. */
        xSemaphoreTake(s_stopped_sem, 0);
        if (ble_gap_disc_cancel() == 0) {
            xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(2000));
        }
        s_scanning = false;
    }

    return scan_begin();
}
