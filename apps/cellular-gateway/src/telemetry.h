#ifndef NODIFYR_TELEMETRY_H_
#define NODIFYR_TELEMETRY_H_

#include "nodifyr_common.h"

#include <zephyr/kernel.h>

enum nodifyr_upload_result {
	NODIFYR_UPLOAD_OK = 0,
	NODIFYR_UPLOAD_RETRY,
	NODIFYR_UPLOAD_DROP,
	NODIFYR_UPLOAD_AUTH_ERROR,
};

int nodifyr_telemetry_init(void);

/** Enqueue a reading. Never blocks the producer. May overwrite oldest. */
void nodifyr_telemetry_enqueue(const struct nodifyr_reading *reading);

/**
 * Route a car detection through the active upload_mode:
 * detailed → enqueue one reading; summary → accumulate into the window.
 */
void nodifyr_telemetry_observe_car(const struct nodifyr_reading *car);

/** Close any open summary window whose upload_interval has elapsed. */
void nodifyr_telemetry_summary_flush(void);

/** Force-close the current summary window (call at upload time). */
void nodifyr_telemetry_summary_flush_force(void);

size_t nodifyr_telemetry_queued(void);

/**
 * Blocks until the telemetry queue reaches threshold readings, or timeout.
 * Used so NORMAL can sleep until the next upload without polling every few
 * seconds, while still reacting promptly when the detailed batch fills.
 */
int nodifyr_telemetry_wait_queued(size_t threshold, k_timeout_t timeout);

/**
 * Build and POST telemetry. Includes device_ts + battery_pct when available.
 * May send an empty readings array for a battery/heartbeat-only check-in.
 * Requires valid wall-clock and api_key.
 */
enum nodifyr_upload_result nodifyr_telemetry_upload(const struct nodifyr_identity *id);

#endif /* NODIFYR_TELEMETRY_H_ */
