#ifndef NODIFYR_TELEMETRY_H_
#define NODIFYR_TELEMETRY_H_

#include "nodifyr_common.h"

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
 * Build and POST up to 64 readings (cloud hard limit).
 * Requires valid wall-clock and api_key.
 */
enum nodifyr_upload_result nodifyr_telemetry_upload(const struct nodifyr_identity *id);

#endif /* NODIFYR_TELEMETRY_H_ */
