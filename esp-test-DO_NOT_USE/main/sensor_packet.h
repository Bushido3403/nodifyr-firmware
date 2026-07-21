#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "scan_store.h"

#define NODIFYR_AD_TYPE_MFG          0xFF
#define NODIFYR_COMPANY_ID             0xFFFF

/* nodifyr.climate.v1 — Company ID (2) + node_id (2) + temp (2) + hum (1) + bat (1) + seq (2) */
#define NODIFYR_MFG_CLIMATE_LEN        10

/* nodifyr.counter.v1 — Company ID (2) + event counter (4); no Local Name AD */
#define NODIFYR_MFG_COUNTER_LEN        6

/** Parse adv data; returns true for known 0xFF / company 0xFFFF payloads. */
bool nodifyr_parse_adv(const uint8_t *adv, uint8_t adv_len, int8_t rssi, const uint8_t addr[6],
                       scan_record_t *out);
