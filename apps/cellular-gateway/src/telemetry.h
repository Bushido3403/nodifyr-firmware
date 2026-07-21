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

size_t nodifyr_telemetry_queued(void);

/**
 * Build and POST up to CONFIG_NODIFYR_TELEMETRY_BATCH_MAX readings.
 * Requires valid wall-clock and api_key.
 */
enum nodifyr_upload_result nodifyr_telemetry_upload(const struct nodifyr_identity *id);

#endif /* NODIFYR_TELEMETRY_H_ */
