#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SCAN_PROFILE_CLIMATE = 0,
    SCAN_PROFILE_COUNTER = 1,
} scan_profile_t;

typedef struct {
    uint32_t ts_ms;
    int8_t rssi;
    uint8_t addr[6];
    uint16_t company_id;
    scan_profile_t profile;
    /* climate profile (nodifyr.climate.v1) */
    uint16_t node_id;
    int16_t temperature;
    uint8_t humidity;
    uint8_t battery;
    uint16_t sequence;
    /* counter profile (nodifyr.counter.v1) — identity is BD_ADDR only */
    uint32_t event_counter;
} scan_record_t;

const char *scan_profile_device_type(scan_profile_t profile);

void scan_store_init(void);
void scan_store_push(const scan_record_t *rec);
size_t scan_store_drain(scan_record_t *out, size_t max_out);
