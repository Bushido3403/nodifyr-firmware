/*
 * Heltec WiFi LoRa 32 V3 onboard OLED (SSD1306 128x64).
 * SDA=17, SCL=18, RST=21, Vext=36 (LOW = power on)
 */

#include "oled.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PIN_SDA   17
#define PIN_SCL   18
#define PIN_RST   21
#define PIN_VEXT  36
#define OLED_ADDR 0x3C

/* Bounded so a disconnected panel can never stall a task for long. */
#define OLED_I2C_TIMEOUT_MS  100
#define OLED_FAIL_LIMIT      20

static const char *TAG = "oled";

static u8g2_t s_display;
static SemaphoreHandle_t s_lock;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_dev;
static bool s_ready;
static uint32_t s_consec_fail;

static uint8_t u8x8_byte_i2c_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    static uint8_t buf[132];
    static uint8_t len;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT: {
        if (i2c_bus == NULL) {
            return 0;
        }
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_ADDR,
            .scl_speed_hz = 400000,
        };
        return i2c_master_bus_add_device(i2c_bus, &dev, &i2c_dev) == ESP_OK;
    }
    case U8X8_MSG_BYTE_START_TRANSFER:
        len = 0;
        break;
    case U8X8_MSG_BYTE_SEND:
        for (int i = 0; i < arg_int; i++) {
            if (len < sizeof(buf)) {
                buf[len++] = ((uint8_t *)arg_ptr)[i];
            }
        }
        break;
    case U8X8_MSG_BYTE_END_TRANSFER: {
        if (i2c_dev == NULL) {
            return 0;
        }
        /* If the panel has been failing, stop blocking on it. Pretend success
         * so u8g2 doesn't retry; periodically allow one probe to recover. */
        if (s_consec_fail >= OLED_FAIL_LIMIT && (s_consec_fail % 256) != 0) {
            s_consec_fail++;
            return 1;
        }
        esp_err_t err = i2c_master_transmit(i2c_dev, buf, len, OLED_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            s_consec_fail++;
            return 0;
        }
        s_consec_fail = 0;
        break;
    }
    default:
        return 0;
    }
    return 1;
}

static uint8_t u8x8_gpio_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        break;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(arg_int * 10);
        break;
    default:
        return 0;
    }
    return 1;
}

u8g2_t *oled_get(void)
{
    return &s_display;
}

bool oled_is_ready(void)
{
    return s_ready;
}

void oled_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void oled_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t oled_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = gpio_config(&(gpio_config_t){
        .pin_bit_mask = (1ULL << PIN_VEXT) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    });
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(PIN_VEXT, 0);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    err = i2c_new_master_bus(&(i2c_master_bus_config_t){
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    }, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_display, U8G2_R0, u8x8_byte_i2c_cb, u8x8_gpio_delay_cb);
    u8g2_InitDisplay(&s_display);
    u8g2_SetPowerSave(&s_display, 0);

    s_ready = true;
    ESP_LOGI(TAG, "OLED ready");
    return ESP_OK;
}
