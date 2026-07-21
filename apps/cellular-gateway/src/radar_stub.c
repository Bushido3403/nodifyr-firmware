#include "radar_stub.h"
#include "telemetry.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <date_time.h>

#include <string.h>

LOG_MODULE_REGISTER(nodifyr_radar, CONFIG_NODIFYR_LOG_LEVEL);

static uint16_t sequence;
static uint16_t speed_centi = 4520;
static uint16_t direction = 180;
static bool started;

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
	r.rssi = 0;
	r.sequence = sequence++;
	strncpy(r.mac, NODIFYR_RADAR_MAC, sizeof(r.mac) - 1);
	r.fields.car.speed_centi_kph = speed_centi;
	r.fields.car.direction_deg = direction;
	r.fields.car.length_cm = 420;
	r.fields.car.confidence_pct = 91;
	r.fields.car.has_length_cm = true;
	r.fields.car.has_confidence_pct = true;

	speed_centi = (uint16_t)(3500 + ((sequence * 37) % 2500));
	direction = (uint16_t)((direction + 15) % 360); /* app max 359 */

	nodifyr_telemetry_enqueue(&r);
	LOG_INF("Stub car seq=%u speed=%u dir=%u", r.sequence,
		r.fields.car.speed_centi_kph, r.fields.car.direction_deg);
}

static void stub_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("Radar stub running (interval %d s)",
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
