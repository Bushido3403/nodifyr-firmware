#include "sensor_packet.h"

#include <string.h>

#include "esp_timer.h"

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t read_le16s(const uint8_t *p)
{
    return (int16_t)read_le16(p);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void fill_common(scan_record_t *out, int8_t rssi, const uint8_t addr[6], uint16_t company_id)
{
    memset(out, 0, sizeof(*out));
    out->ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    out->rssi = rssi;
    memcpy(out->addr, addr, 6);
    out->company_id = company_id;
}

static bool parse_climate_mfg(const uint8_t *mfg, uint8_t mfg_len, int8_t rssi, const uint8_t addr[6],
                              scan_record_t *out)
{
    if (mfg_len < NODIFYR_MFG_CLIMATE_LEN) {
        return false;
    }

    uint16_t company_id = read_le16(&mfg[0]);
    if (company_id != NODIFYR_COMPANY_ID) {
        return false;
    }

    fill_common(out, rssi, addr, company_id);
    out->profile = SCAN_PROFILE_CLIMATE;
    out->node_id = read_le16(&mfg[2]);
    out->temperature = read_le16s(&mfg[4]);
    out->humidity = mfg[6];
    out->battery = mfg[7];
    out->sequence = read_le16(&mfg[8]);
    return true;
}

static bool parse_counter_mfg(const uint8_t *mfg, uint8_t mfg_len, int8_t rssi, const uint8_t addr[6],
                              scan_record_t *out)
{
    if (mfg_len != NODIFYR_MFG_COUNTER_LEN) {
        return false;
    }

    uint16_t company_id = read_le16(&mfg[0]);
    if (company_id != NODIFYR_COMPANY_ID) {
        return false;
    }

    fill_common(out, rssi, addr, company_id);
    out->profile = SCAN_PROFILE_COUNTER;
    out->event_counter = read_le32(&mfg[2]);
    /* Lower 16 bits for ordering until API accepts full uint32 sequence. */
    out->sequence = (uint16_t)(out->event_counter & 0xFFFF);
    return true;
}

bool nodifyr_parse_adv(const uint8_t *adv, uint8_t adv_len, int8_t rssi, const uint8_t addr[6],
                       scan_record_t *out)
{
    if (adv == NULL || out == NULL || adv_len == 0) {
        return false;
    }

    uint8_t i = 0;
    while (i < adv_len) {
        uint8_t seg_len = adv[i];
        if (seg_len == 0 || i + seg_len >= adv_len) {
            break;
        }

        uint8_t ad_type = adv[i + 1];
        const uint8_t *data = &adv[i + 2];
        uint8_t data_len = seg_len - 1;

        if (ad_type == NODIFYR_AD_TYPE_MFG) {
            /* Exact length disambiguates profiles sharing company ID 0xFFFF. */
            if (data_len == NODIFYR_MFG_COUNTER_LEN &&
                parse_counter_mfg(data, data_len, rssi, addr, out)) {
                return true;
            }
            if (data_len >= NODIFYR_MFG_CLIMATE_LEN &&
                parse_climate_mfg(data, data_len, rssi, addr, out)) {
                return true;
            }
        }

        i += seg_len + 1;
    }

    return false;
}
