#ifndef NODIFYR_DEVICE_CONFIG_H_
#define NODIFYR_DEVICE_CONFIG_H_

#include "nodifyr_common.h"

#include <stdbool.h>
#include <stdint.h>

enum nodifyr_upload_mode {
	NODIFYR_UPLOAD_DETAILED = 0,
	NODIFYR_UPLOAD_SUMMARY,
};

/**
 * Runtime config from GET /api/v1/config (dashboard → cellular).
 * Fetched when the device uploads telemetry (same cadence).
 */
struct nodifyr_device_config {
	enum nodifyr_upload_mode upload_mode;
	int upload_interval_sec;
	int version;
	bool have_version;
};

void nodifyr_device_config_init(void);

/** Snapshot of the currently applied config (safe to call from any thread). */
void nodifyr_device_config_get(struct nodifyr_device_config *out);

/**
 * GET /api/v1/config?since=<version>. Persists + applies when version changes.
 * Returns 0 on success (including unchanged), negative on transport error,
 * -EACCES on 401/403.
 */
int nodifyr_device_config_poll(const struct nodifyr_identity *id);

#endif /* NODIFYR_DEVICE_CONFIG_H_ */
