#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_BACKOFF_MIN_MS  1000
#define WIFI_BACKOFF_MAX_MS  30000

static EventGroupHandle_t s_events;
static bool s_common_init;
static bool s_sta_netif_created;
static bool s_ap_netif_created;
static TaskHandle_t s_reconnect_task;
static volatile uint32_t s_backoff_ms = WIFI_BACKOFF_MIN_MS;
static volatile bool s_sta_enabled;

#define WIFI_SCAN_MAX_RESULTS 16
#define WIFI_SCAN_RAW_MAX     32

enum { SCAN_IDLE = 0, SCAN_RUNNING, SCAN_DONE };

typedef struct {
    char ssid[NODIFYR_WIFI_SSID_MAX];
    int8_t rssi;
    bool secure;
} wifi_scan_ap_t;

static wifi_scan_ap_t s_scan_results[WIFI_SCAN_MAX_RESULTS];
static volatile size_t s_scan_result_count;
static volatile int s_scan_state = SCAN_IDLE;

/* Dedicated reconnect with capped exponential backoff. The event handler only
 * signals this task; it never calls esp_wifi_connect() in a tight loop, so a
 * missing/wrong AP can never spin the CPU or flood the log. */
static void wifi_reconnect_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_sta_enabled) {
            continue;
        }

        uint32_t delay = s_backoff_ms;
        vTaskDelay(pdMS_TO_TICKS(delay));
        if (!s_sta_enabled) {
            continue;
        }

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "reconnecting (backoff %ums)", (unsigned)delay);
        }

        uint32_t next = delay * 2;
        if (next > WIFI_BACKOFF_MAX_MS) {
            next = WIFI_BACKOFF_MAX_MS;
        }
        s_backoff_ms = next;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* The STA interface also starts during a background scan, when we have
         * no creds and don't want to associate. Only auto-connect once a STA
         * session has been explicitly enabled. */
        if (!s_sta_enabled) {
            return;
        }
        s_backoff_ms = WIFI_BACKOFF_MIN_MS;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "initial connect failed: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_events != NULL) {
            xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        }
        if (s_reconnect_task != NULL) {
            xTaskNotifyGive(s_reconnect_task);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_backoff_ms = WIFI_BACKOFF_MIN_MS;
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "connected (got IP)");
    }
}

esp_err_t wifi_init_common(void)
{
    if (s_common_init) {
        return ESP_OK;
    }

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi evt reg failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip evt reg failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_reconnect_task == NULL &&
        xTaskCreate(wifi_reconnect_task, "wifi_recon", 3072, NULL, 4, &s_reconnect_task) != pdPASS) {
        ESP_LOGE(TAG, "reconnect task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_common_init = true;
    return ESP_OK;
}

static esp_err_t ensure_sta_netif(void)
{
    if (!s_sta_netif_created) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            return ESP_FAIL;
        }
        s_sta_netif_created = true;
    }
    return ESP_OK;
}

static esp_err_t ensure_ap_netif(void)
{
    if (!s_ap_netif_created) {
        if (esp_netif_create_default_wifi_ap() == NULL) {
            return ESP_FAIL;
        }
        s_ap_netif_created = true;
    }
    return ESP_OK;
}

esp_err_t wifi_start_sta(void)
{
    const nodifyr_config_t *cfg = nodifyr_config_get();
    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGE(TAG, "no WiFi SSID configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_init_common();
    if (err != ESP_OK) {
        return err;
    }

    err = ensure_sta_netif();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA netif create failed");
        return err;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, cfg->wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cfg->wifi_pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode =
        (cfg->wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sta_enabled = true;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        s_sta_enabled = false;
        return err;
    }

    ESP_LOGI(TAG, "STA started, SSID=%s", cfg->wifi_ssid);
    return ESP_OK;
}

esp_err_t wifi_start_ap(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_init_common();
    if (err != ESP_OK) {
        return err;
    }

    err = ensure_ap_netif();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP netif create failed");
        return err;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;

    if (password != NULL && password[0] != '\0') {
        strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "AP started, SSID=%s (%s)", ssid,
             (password != NULL && password[0] != '\0') ? "secured" : "open");
    return ESP_OK;
}

static int scan_rssi_cmp(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = a;
    const wifi_ap_record_t *rb = b;
    return rb->rssi - ra->rssi;  /* strongest first */
}

static void wifi_scan_store(wifi_ap_record_t *recs, uint16_t num)
{
    size_t count = 0;
    for (uint16_t i = 0; i < num && count < WIFI_SCAN_MAX_RESULTS; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;  /* hidden / no SSID */
        }

        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strncmp(s_scan_results[j].ssid, ssid, sizeof(s_scan_results[j].ssid)) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;  /* records are RSSI-sorted, so the first wins (strongest) */
        }

        strncpy(s_scan_results[count].ssid, ssid, sizeof(s_scan_results[count].ssid) - 1);
        s_scan_results[count].ssid[sizeof(s_scan_results[count].ssid) - 1] = '\0';
        s_scan_results[count].rssi = recs[i].rssi;
        s_scan_results[count].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
        count++;
    }
    s_scan_result_count = count;
}

static void wifi_scan_task(void *arg)
{
    (void)arg;

    esp_err_t err = wifi_init_common();
    if (err == ESP_OK) {
        err = ensure_sta_netif();
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();  /* scanning needs the STA radio up */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan setup failed: %s", esp_err_to_name(err));
        s_scan_state = SCAN_DONE;
        vTaskDelete(NULL);
        return;
    }

    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    err = esp_wifi_scan_start(&scan_cfg, true);  /* blocking */
    if (err == ESP_OK) {
        uint16_t found = 0;
        esp_wifi_scan_get_ap_num(&found);
        if (found > WIFI_SCAN_RAW_MAX) {
            found = WIFI_SCAN_RAW_MAX;
        }
        if (found > 0) {
            wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
            if (recs != NULL) {
                esp_wifi_scan_get_ap_records(&found, recs);
                qsort(recs, found, sizeof(recs[0]), scan_rssi_cmp);
                wifi_scan_store(recs, found);
                free(recs);
            }
        }
        ESP_LOGI(TAG, "scan complete, %u networks", (unsigned)s_scan_result_count);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }

    esp_wifi_stop();  /* release the radio so the AP/STA can start cleanly after */
    s_scan_state = SCAN_DONE;
    vTaskDelete(NULL);
}

void wifi_scan_start(void)
{
    if (s_scan_state != SCAN_IDLE) {
        return;  /* already running or finished */
    }
    s_scan_state = SCAN_RUNNING;
    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "scan task create failed");
        s_scan_state = SCAN_DONE;  /* unblock waiters with an empty list */
    }
}

bool wifi_scan_wait(TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while (s_scan_state != SCAN_DONE) {
        if (xTaskGetTickCount() - start >= timeout_ticks) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

size_t wifi_scan_count(void)
{
    return s_scan_result_count;
}

bool wifi_scan_get(size_t index, char *ssid_out, size_t ssid_len,
                   int8_t *rssi_out, bool *secure_out)
{
    if (index >= s_scan_result_count) {
        return false;
    }
    if (ssid_out != NULL && ssid_len > 0) {
        strncpy(ssid_out, s_scan_results[index].ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
    }
    if (rssi_out != NULL) {
        *rssi_out = s_scan_results[index].rssi;
    }
    if (secure_out != NULL) {
        *secure_out = s_scan_results[index].secure;
    }
    return true;
}

esp_err_t wifi_stop_ap(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi stopped");
    }
    return err;
}

bool wifi_is_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_wait_connected(TickType_t timeout_ticks)
{
    if (s_events == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
