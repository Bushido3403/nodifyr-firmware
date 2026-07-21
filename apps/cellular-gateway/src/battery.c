#include "battery.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(nodifyr_batt, CONFIG_NODIFYR_LOG_LEVEL);

#if DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_charger), okay)

/* Rough LiPo OCV → SoC for Thingy:91 X pack (LP803448-class, 4.2 V full). */
struct ocv_point {
	int mv;
	uint8_t pct;
};

static const struct ocv_point ocv_table[] = {
	{ 4200, 100 },
	{ 4050, 90 },
	{ 3900, 75 },
	{ 3800, 60 },
	{ 3700, 45 },
	{ 3600, 30 },
	{ 3500, 18 },
	{ 3400, 10 },
	{ 3300, 5 },
	{ 3000, 0 },
};

static uint8_t voltage_mv_to_pct(int mv)
{
	size_t i;

	if (mv >= ocv_table[0].mv) {
		return 100;
	}
	if (mv <= ocv_table[ARRAY_SIZE(ocv_table) - 1].mv) {
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(ocv_table) - 1; i++) {
		if (mv <= ocv_table[i].mv && mv >= ocv_table[i + 1].mv) {
			int mv_hi = ocv_table[i].mv;
			int mv_lo = ocv_table[i + 1].mv;
			int pct_hi = ocv_table[i].pct;
			int pct_lo = ocv_table[i + 1].pct;
			int span = mv_hi - mv_lo;

			if (span <= 0) {
				return (uint8_t)pct_lo;
			}
			return (uint8_t)(pct_lo +
					 ((pct_hi - pct_lo) * (mv - mv_lo)) /
						 span);
		}
	}

	return 0;
}

bool nodifyr_battery_read_pct(uint8_t *pct_out)
{
	const struct device *charger =
		DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
	struct sensor_value volt;
	int err;
	int mv;

	if (!pct_out) {
		return false;
	}

	if (!device_is_ready(charger)) {
		LOG_WRN("nPM1300 charger not ready");
		return false;
	}

	err = sensor_sample_fetch(charger);
	if (err) {
		LOG_WRN("Battery sample fetch failed: %d", err);
		return false;
	}

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &volt);
	if (err) {
		LOG_WRN("Battery voltage get failed: %d", err);
		return false;
	}

	/* sensor_value: val1 = volts, val2 = microvolts */
	mv = volt.val1 * 1000 + volt.val2 / 1000;
	*pct_out = voltage_mv_to_pct(mv);
	LOG_INF("Battery ~%u%% (%d mV)", *pct_out, mv);
	return true;
}

#else /* !npm1300_charger */

bool nodifyr_battery_read_pct(uint8_t *pct_out)
{
	ARG_UNUSED(pct_out);
	return false;
}

#endif
