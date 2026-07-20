#include "anim.h"

#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define W           128
#define H           64
#define FRAME_MS    45
#define BRAND_LEN   7
#define HOLD_FRAMES 35

static const char BRAND[] = "NODIFYR";
static const char TAGLINE[] = "Built for the field.";

typedef struct {
    uint8_t order[BRAND_LEN];
    int appear[BRAND_LEN];
    int letter_x[BRAND_LEN];
    int jitter_x[BRAND_LEN];
    int jitter_y[BRAND_LEN];
    int brand_y;
    int tag_x;
    int tag_y;
    int tag_len;
    int tag_start;
    int end_frame;
    uint32_t rng;
} boot_t;

static int rnd(boot_t *s, int lo, int hi)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)((s->rng >> 16) % (uint32_t)(hi - lo + 1));
}

static void shuffle(boot_t *s, uint8_t *arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rnd(s, 0, i);
        uint8_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static void boot_init(boot_t *s, u8g2_t *u8g2)
{
    memset(s, 0, sizeof(*s));
    s->rng = esp_random() ?: 0xC0FFEE01u;

    for (int i = 0; i < BRAND_LEN; i++) {
        s->order[i] = i;
    }
    shuffle(s, s->order, BRAND_LEN);

    u8g2_SetFont(u8g2, u8g2_font_helvB18_tr);
    int brand_w = u8g2_GetStrWidth(u8g2, BRAND);
    int brand_x = (W - brand_w) / 2;
    s->brand_y = 30;

    int x = brand_x;
    for (int i = 0; i < BRAND_LEN; i++) {
        char ch[2] = { BRAND[i], '\0' };
        s->letter_x[i] = x;
        x += u8g2_GetStrWidth(u8g2, ch);
        s->jitter_x[i] = rnd(s, -6, 6);
        s->jitter_y[i] = rnd(s, -5, 5);
    }

    int t = rnd(s, 8, 16);
    for (int i = 0; i < BRAND_LEN; i++) {
        int idx = s->order[i];
        s->appear[idx] = t;
        t += rnd(s, 3, 9);
    }

    u8g2_SetFont(u8g2, u8g2_font_helvR08_tr);
    s->tag_x = (W - u8g2_GetStrWidth(u8g2, TAGLINE)) / 2;
    s->tag_y = s->brand_y + 18;
    s->tag_len = (int)strlen(TAGLINE);

    int brand_done = 0;
    for (int i = 0; i < BRAND_LEN; i++) {
        if (s->appear[i] > brand_done) {
            brand_done = s->appear[i];
        }
    }

    s->tag_start = brand_done + rnd(s, 4, 10);
    s->end_frame = s->tag_start + s->tag_len * 2 + HOLD_FRAMES;
}

static void draw_sparks(u8g2_t *u8g2, boot_t *s, int lx, int ly, int age)
{
    if (age > 5) {
        return;
    }

    for (int i = 0; i < 10; i++) {
        int px = lx + rnd(s, -8, 14);
        int py = ly + rnd(s, -12, 6);
        if (px >= 0 && px < W && py >= 0 && py < H) {
            u8g2_DrawPixel(u8g2, px, py);
        }
    }
}

static void draw_brand(u8g2_t *u8g2, boot_t *s, int frame)
{
    u8g2_SetFont(u8g2, u8g2_font_helvB18_tr);

    for (int i = 0; i < BRAND_LEN; i++) {
        int age = frame - s->appear[i];
        if (age < 0) {
            continue;
        }

        char ch[2] = { BRAND[i], '\0' };
        int settle = age > 8 ? 8 : age;
        int lx = s->letter_x[i] + (s->jitter_x[i] * (8 - settle)) / 8;
        int ly = s->brand_y + (s->jitter_y[i] * (8 - settle)) / 8;

        draw_sparks(u8g2, s, lx, ly, age);
        if (age >= 2) {
            u8g2_DrawStr(u8g2, lx, ly, ch);
        }
    }
}

static void draw_tagline(u8g2_t *u8g2, boot_t *s, int frame)
{
    if (frame < s->tag_start) {
        return;
    }

    int chars = (frame - s->tag_start) / 2;
    if (chars > s->tag_len) {
        chars = s->tag_len;
    }

    char buf[sizeof(TAGLINE)];
    memcpy(buf, TAGLINE, chars);
    buf[chars] = '\0';

    u8g2_SetFont(u8g2, u8g2_font_helvR08_tr);
    u8g2_DrawStr(u8g2, s->tag_x, s->tag_y, buf);
}

void anim_boot(u8g2_t *u8g2, bool loop)
{
    do {
        boot_t boot;
        boot_init(&boot, u8g2);

        for (int frame = 0; frame <= boot.end_frame; frame++) {
            u8g2_ClearBuffer(u8g2);
            draw_brand(u8g2, &boot, frame);
            draw_tagline(u8g2, &boot, frame);
            u8g2_SendBuffer(u8g2);
            vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        }
    } while (loop);
}
