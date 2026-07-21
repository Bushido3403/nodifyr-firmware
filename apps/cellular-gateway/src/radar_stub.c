#include "radar_stub.h"
#include "telemetry.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <date_time.h>

#include <string.h>

LOG_MODULE_REGISTER(nodifyr_radar, CONFIG_NODIFYR_LOG_LEVEL);

/* Fill-in car.v1 samples until a real radar module is wired up. Values are
 * pseudo-random within the cloud schema ranges (not sensor measurements).
 */

static uint16_t sequence;
static bool started;

static uint32_t prng_u32(void)
{
	return sys_rand32_get();
}

static uint16_t prng_range_u16(uint16_t lo, uint16_t hi_inclusive)
{
	uint32_t span;

	if (hi_inclusive <= lo) {
		return lo;
	}

	span = (uint32_t)hi_inclusive - (uint32_t)lo + 1U;
	return (uint16_t)(lo + (prng_u32() % span));
}

static void emit_one(void)
{
	struct nodifyr_reading r;
	int64_t now_ms = 0;
	int err;

	err = date_time_now(&now_ms);
	if (err || now_ms <= 0) {
		LOG_DBG("Stub skip: no wall clock yet");
		return;
	}

	memset(&r, 0, sizeof(r));
	r.kind = NODIFYR_READING_CAR;
	r.ts = now_ms;
	r.rssi = (int8_t)prng_range_u16(0, 20) - 10; /* -10 .. +10 fill-in */
	r.sequence = sequence++;
	memcpy(r.mac, NODIFYR_RADAR_MAC, sizeof(r.mac));
	r.mac[sizeof(r.mac) - 1] = '\0';

	/* Plausible road traffic fill-in (schema: speed 0..65535 centi-kph,
	 * direction 0..359, optional length/confidence).
	 */
	r.fields.car.speed_centi_kph = prng_range_u16(800, 11000); /* 8–110 km/h */
	r.fields.car.direction_deg = prng_range_u16(0, 359);
	r.fields.car.length_cm = prng_range_u16(350, 550);
	r.fields.car.confidence_pct = (uint8_t)prng_range_u16(70, 99);
	r.fields.car.has_length_cm = true;
	r.fields.car.has_confidence_pct = true;

	nodifyr_telemetry_observe_car(&r);
	LOG_INF("Stub car seq=%u speed=%u dir=%u len=%u conf=%u", r.sequence,
		r.fields.car.speed_centi_kph, r.fields.car.direction_deg,
		r.fields.car.length_cm, r.fields.car.confidence_pct);
}

static void stub_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("Radar stub: pseudo-random fill-in (interval %d s)",
		CONFIG_NODIFYR_RADAR_STUB_INTERVAL_S);

	while (1) {
		emit_one();
		k_sleep(K_SECONDS(CONFIG_NODIFYR_RADAR_STUB_INTERVAL_S));
	}
}

static K_THREAD_STACK_DEFINE(stub_stack, 2048);
static struct k_thread stub_thread_data;

int nodifyr_radar_stub_start(void)
{
	if (started) {
		return 0;
	}

	started = true;
	k_thread_create(&stub_thread_data, stub_stack,
			K_THREAD_STACK_SIZEOF(stub_stack), stub_thread, NULL,
			NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&stub_thread_data, "radar_stub");
	return 0;
}
