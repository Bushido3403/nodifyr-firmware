#include "scan_store.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SCAN_STORE_CAP 512

/* Short, bounded waits so neither the BLE host callback (push) nor the
 * uploader (drain) can ever block indefinitely on lock contention. */
#define SCAN_PUSH_WAIT_MS  20
#define SCAN_DRAIN_WAIT_MS 200

static const char *TAG = "scan_store";

const char *scan_profile_device_type(scan_profile_t profile)
{
    switch (profile) {
    case SCAN_PROFILE_COUNTER:
        return "nodifyr.counter.v1";
    case SCAN_PROFILE_CLIMATE:
    default:
        return "nodifyr.climate.v1";
    }
}

static scan_record_t s_buf[SCAN_STORE_CAP];
static size_t s_head;
static size_t s_count;
static uint32_t s_dropped;
static SemaphoreHandle_t s_lock;

void scan_store_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void scan_store_push(const scan_record_t *rec)
{
    if (s_lock == NULL || rec == NULL) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(SCAN_PUSH_WAIT_MS)) != pdTRUE) {
        /* Never stall the radio callback; drop instead. */
        s_dropped++;
        return;
    }

    s_buf[s_head] = *rec;
    s_head = (s_head + 1) % SCAN_STORE_CAP;
    if (s_count < SCAN_STORE_CAP) {
        s_count++;
    }
    xSemaphoreGive(s_lock);
}

size_t scan_store_drain(scan_record_t *out, size_t max_out)
{
    if (s_lock == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(SCAN_DRAIN_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "drain skipped: lock busy");
        return 0;
    }

    size_t n = s_count < max_out ? s_count : max_out;
    size_t start = (s_head + SCAN_STORE_CAP - s_count) % SCAN_STORE_CAP;

    for (size_t i = 0; i < n; i++) {
        out[i] = s_buf[(start + i) % SCAN_STORE_CAP];
    }

    s_count -= n;
    uint32_t dropped = s_dropped;
    s_dropped = 0;
    xSemaphoreGive(s_lock);

    if (dropped > 0) {
        ESP_LOGW(TAG, "%u records dropped under load since last drain", (unsigned)dropped);
    }
    return n;
}
