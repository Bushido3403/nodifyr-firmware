#include "ble_display.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "oled.h"
#include "scan_store.h"

static const char *TAG = "ble_display";
static QueueHandle_t s_queue;

static void mac_str(char *out, size_t out_len, const uint8_t *addr)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static void draw_packet(const scan_record_t *rec, uint32_t count)
{
    char line1[24];
    char line2[24];
    char line3[24];

    if (rec->profile == SCAN_PROFILE_COUNTER) {
        char mac[18];
        mac_str(mac, sizeof(mac), rec->addr);
        snprintf(line1, sizeof(line1), "counter");
        snprintf(line2, sizeof(line2), "%s", mac);
        snprintf(line3, sizeof(line3), "evt %" PRIu32 "  #%lu",
                 rec->event_counter, (unsigned long)count);
        ESP_LOGI(TAG, "%s | %s | rssi %d", line1, line2, rec->rssi);
        ESP_LOGI(TAG, "  event_counter=%" PRIu32, rec->event_counter);
    } else {
        snprintf(line1, sizeof(line1), "node %u  seq %u", rec->node_id, rec->sequence);
        snprintf(line2, sizeof(line2), "%.2fC %u%% bat %u%%",
                 rec->temperature / 100.0f, rec->humidity, rec->battery);
        snprintf(line3, sizeof(line3), "#%lu  rssi %d", (unsigned long)count, rec->rssi);
        ESP_LOGI(TAG, "%s | %s | %s", line1, line2, line3);
    }

    if (!oled_is_ready()) {
        return;
    }

    u8g2_t *display = oled_get();
    oled_lock();
    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_helvB08_tr);
    u8g2_DrawStr(display, 0, 10, "NODIFYR");
    u8g2_SetFont(display, u8g2_font_helvR08_tr);
    u8g2_DrawStr(display, 0, 22, line1);
    u8g2_DrawStr(display, 0, 34, line2);
    u8g2_DrawStr(display, 0, 46, line3);
    u8g2_SendBuffer(display);
    oled_unlock();
}

static void display_task(void *arg)
{
    scan_record_t rec;
    uint32_t count = 0;

    for (;;) {
        if (xQueueReceive(s_queue, &rec, portMAX_DELAY) == pdTRUE) {
            count++;
            draw_packet(&rec, count);
        }
    }
}

void ble_display_push(const scan_record_t *rec)
{
    if (s_queue == NULL || rec == NULL) {
        return;
    }

    xQueueOverwrite(s_queue, rec);
}

esp_err_t ble_display_start(void)
{
    s_queue = xQueueCreate(1, sizeof(scan_record_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(display_task, "ble_display", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (oled_is_ready()) {
        u8g2_t *display = oled_get();
        oled_lock();
        u8g2_ClearBuffer(display);
        u8g2_SetFont(display, u8g2_font_helvR08_tr);
        u8g2_DrawStr(display, 0, 32, "Scanning...");
        u8g2_SendBuffer(display);
        oled_unlock();
    }

    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}
