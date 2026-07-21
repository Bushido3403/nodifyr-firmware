#include "uploader.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ble_scan.h"
#include "esp_coexist.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "scan_store.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = "uploader";

/* Sized to hold a full batch (64 * ~140B) with headroom so JSON never
 * truncates mid-record. Kept in .bss, not on the task stack. */
#define UPLOAD_BODY_MAX       12288
#define UPLOAD_RESP_MAX       512
#define UPLOAD_HTTP_TIMEOUT_MS 10000

static char s_body[UPLOAD_BODY_MAX];

typedef struct {
    char data[UPLOAD_RESP_MAX];
    size_t len;
} upload_resp_t;

static esp_err_t upload_http_event(esp_http_client_event_t *evt)
{
    upload_resp_t *resp = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp != NULL && evt->data_len > 0) {
        size_t copy = (size_t)evt->data_len;
        if (resp->len + copy >= sizeof(resp->data)) {
            copy = sizeof(resp->data) - resp->len - 1;
        }
        if (copy > 0) {
            memcpy(resp->data + resp->len, evt->data, copy);
            resp->len += copy;
            resp->data[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

static void mac_str(char *out, size_t out_len, const uint8_t *addr)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static bool reading_valid(const scan_record_t *rec)
{
    if (rec->profile == SCAN_PROFILE_COUNTER) {
        return true;
    }
    if (rec->humidity > 100 || rec->battery > 100) {
        return false;
    }
    return true;
}

/* Drop readings that would fail Zod validation; compact batch in place. */
static size_t compact_valid(scan_record_t *records, size_t count)
{
    size_t kept = 0;

    for (size_t i = 0; i < count; i++) {
        if (!reading_valid(&records[i])) {
            ESP_LOGW(TAG, "dropping invalid climate reading seq=%u hum=%u bat=%u",
                     records[i].sequence, records[i].humidity, records[i].battery);
            continue;
        }
        if (kept != i) {
            records[kept] = records[i];
        }
        kept++;
    }
    return kept;
}

/* Serialize one reading; returns bytes written or -1 on overflow. */
static int append_reading_json(char *buf, size_t buf_len, size_t off, bool first,
                               const scan_record_t *rec)
{
    char mac[18];
    mac_str(mac, sizeof(mac), rec->addr);

    if (rec->profile == SCAN_PROFILE_COUNTER) {
        return snprintf(buf + off, buf_len - off,
                        "%s{\"ts\":%" PRIu32 ",\"rssi\":%d,\"mac\":\"%s\","
                        "\"device_type\":\"%s\",\"sequence\":%u,"
                        "\"fields\":{\"event_counter\":%" PRIu32 "}}",
                        first ? "" : ",", rec->ts_ms, rec->rssi, mac,
                        scan_profile_device_type(rec->profile), rec->sequence,
                        rec->event_counter);
    }

    /* Legacy flat climate shape until API accepts unified device_type+fields. */
    return snprintf(buf + off, buf_len - off,
                    "%s{\"ts\":%" PRIu32 ",\"rssi\":%d,\"mac\":\"%s\","
                    "\"node_id\":%u,\"temperature\":%d,\"humidity\":%u,"
                    "\"battery\":%u,\"sequence\":%u}",
                    first ? "" : ",", rec->ts_ms, rec->rssi, mac, rec->node_id,
                    rec->temperature, rec->humidity, rec->battery, rec->sequence);
}

/* Serialize as many records as fit; report how many were included so the
 * caller can re-queue any leftovers. Returns the byte length of the body. */
static size_t build_body(const scan_record_t *records, size_t count, size_t *out_fit)
{
    size_t off = 0;
    size_t fit = 0;

    int n = snprintf(s_body, sizeof(s_body), "{\"readings\":[");
    if (n < 0) {
        *out_fit = 0;
        s_body[0] = '\0';
        return 0;
    }
    off = (size_t)n;

    for (size_t i = 0; i < count; i++) {
        int rn = append_reading_json(s_body, sizeof(s_body), off, fit == 0, &records[i]);
        if (rn < 0) {
            continue;
        }

        /* +2 reserves room for the closing "]}". */
        if ((size_t)rn >= sizeof(s_body) - off || off + (size_t)rn + 2 >= sizeof(s_body)) {
            ESP_LOGW(TAG, "body full at %u/%u records", (unsigned)fit, (unsigned)count);
            break;
        }

        off += (size_t)rn;
        fit++;
    }

    s_body[off++] = ']';
    s_body[off++] = '}';
    s_body[off] = '\0';

    *out_fit = fit;
    return off;
}

static esp_err_t upload_batch(const scan_record_t *records, size_t count, size_t *out_sent,
                              int *out_status)
{
    *out_sent = 0;
    if (out_status != NULL) {
        *out_status = 0;
    }
    if (count == 0) {
        return ESP_OK;
    }

    if (count > CONFIG_NODIFYR_UPLOAD_BATCH_MAX) {
        ESP_LOGE(TAG, "batch size %u exceeds max %d", (unsigned)count,
                 CONFIG_NODIFYR_UPLOAD_BATCH_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    const nodifyr_config_t *cfg = nodifyr_config_get();
    if (!nodifyr_config_valid_api_key(cfg->api_key) || cfg->api_url[0] == '\0') {
        ESP_LOGE(TAG, "no gateway key/url — skipping upload");
        return ESP_ERR_INVALID_STATE;
    }

    size_t fit = 0;
    size_t body_len = build_body(records, count, &fit);
    if (fit == 0 || body_len == 0) {
        return ESP_FAIL;
    }

    upload_resp_t resp = {0};
    esp_http_client_config_t cfg_http = {
        .url = cfg->api_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = UPLOAD_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = upload_http_event,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg_http);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "X-Api-Key", cfg->api_key) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        esp_http_client_set_post_field(client, s_body, body_len);
        err = esp_http_client_perform(client);
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (out_status != NULL) {
        *out_status = status;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status < 200 || status >= 300) {
        if (resp.len > 0) {
            ESP_LOGW(TAG, "upload rejected HTTP %d: %s", status, resp.data);
        } else {
            ESP_LOGW(TAG, "upload rejected HTTP %d (empty response body)", status);
        }
        return ESP_FAIL;
    }

    *out_sent = fit;
    ESP_LOGI(TAG, "uploaded %u/%u records, HTTP %d", (unsigned)fit, (unsigned)count, status);
    return ESP_OK;
}

static void requeue(const scan_record_t *records, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        scan_store_push(&records[i]);
    }
}

static void uploader_task(void *arg)
{
    static scan_record_t batch[CONFIG_NODIFYR_UPLOAD_BATCH_MAX];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NODIFYR_UPLOAD_INTERVAL_SEC * 1000));

        if (!wifi_is_connected()) {
            continue;
        }

        size_t n = scan_store_drain(batch, CONFIG_NODIFYR_UPLOAD_BATCH_MAX);
        if (n == 0) {
            continue;
        }

        n = compact_valid(batch, n);
        if (n == 0) {
            continue;
        }

        ESP_LOGI(TAG, "uploading %u records (seq %u..%u)",
                 (unsigned)n, batch[0].sequence, batch[n - 1].sequence);

        /* Pause BLE only for the upload window; always resume afterwards. */
        esp_err_t pause_err = ble_scan_pause(2000);
        if (pause_err != ESP_OK) {
            ESP_LOGW(TAG, "scan pause failed (%s); uploading anyway", esp_err_to_name(pause_err));
        }

        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        size_t sent = 0;
        int http_status = 0;
        esp_err_t err = upload_batch(batch, n, &sent, &http_status);
        esp_coex_preference_set(ESP_COEX_PREFER_BT);

        if (ble_scan_resume() != ESP_OK) {
            ESP_LOGW(TAG, "scan resume failed; watchdog will recover");
        }

        if (err == ESP_OK) {
            if (sent < n) {
                requeue(batch, sent, n);
            }
        } else if (http_status == 400) {
            /* Schema validation failure — retrying the same batch will not help. */
            ESP_LOGW(TAG, "discarding %u records after HTTP 400 validation error",
                     (unsigned)n);
        } else {
            requeue(batch, 0, n);
        }
    }
}

esp_err_t uploader_start(void)
{
    if (!nodifyr_config_get()->provisioned) {
        ESP_LOGW(TAG, "not provisioned — uploader not started");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(uploader_task, "uploader", 10240, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
