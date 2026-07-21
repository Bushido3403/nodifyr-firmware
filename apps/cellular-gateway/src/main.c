#include "nodifyr_common.h"
#include "lte.h"
#include "identity.h"
#include "pairing.h"
#include "telemetry.h"
#include "radar_stub.h"
#include "device_config.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(nodifyr_main, CONFIG_NODIFYR_LOG_LEVEL);

/** Cloud hard limit — detailed mode uploads as soon as the queue hits this. */
#define DETAILED_UPLOAD_THRESHOLD 64

static struct nodifyr_identity identity;
static enum nodifyr_state state = NODIFYR_STATE_BOOT;

static void enter_error(const char *reason)
{
	LOG_ERR("ERROR state: %s", reason);
	state = NODIFYR_STATE_ERROR;
}

static void enter_fatal(const char *reason)
{
	LOG_ERR("FATAL: %s", reason);
	state = NODIFYR_STATE_FATAL;
}

static void run_pairing(void)
{
	enum nodifyr_pair_result result;

	LOG_INF("PAIRING: announce + poll status");
	state = NODIFYR_STATE_PAIRING;

	while (state == NODIFYR_STATE_PAIRING) {
		result = nodifyr_pairing_step(&identity);
		switch (result) {
		case NODIFYR_PAIR_APPROVED:
			LOG_INF("Pairing complete — entering NORMAL");
			state = NODIFYR_STATE_NORMAL;
			break;
		case NODIFYR_PAIR_ERROR:
			enter_error("pairing terminal failure");
			break;
		case NODIFYR_PAIR_CONTINUE:
		default:
			LOG_INF("Pairing pending; next poll in %d s",
				nodifyr_pairing_next_poll_s());
			k_sleep(K_SECONDS(nodifyr_pairing_next_poll_s()));
			break;
		}
	}
}

/**
 * One cellular session: flush summary if needed, POST telemetry when queued,
 * then GET /api/v1/config on the same cadence (not a separate timer).
 */
static int upload_and_refresh_config(int *upload_backoff_s)
{
	enum nodifyr_upload_result up;
	struct nodifyr_device_config cfg;
	int err;

	nodifyr_device_config_get(&cfg);

	if (cfg.upload_mode == NODIFYR_UPLOAD_SUMMARY) {
		nodifyr_telemetry_summary_flush_force();
	}

	if (nodifyr_telemetry_queued() > 0) {
		up = nodifyr_telemetry_upload(&identity);
		switch (up) {
		case NODIFYR_UPLOAD_OK:
		case NODIFYR_UPLOAD_DROP:
			*upload_backoff_s = cfg.upload_interval_sec;
			break;
		case NODIFYR_UPLOAD_AUTH_ERROR:
			enter_error("telemetry 401/403");
			return -EACCES;
		case NODIFYR_UPLOAD_RETRY:
		default:
			*upload_backoff_s = MIN(*upload_backoff_s * 2, 300);
			LOG_WRN("Upload retry backoff %d s", *upload_backoff_s);
			break;
		}
	} else {
		*upload_backoff_s = cfg.upload_interval_sec;
	}

	err = nodifyr_device_config_poll(&identity);
	if (err == -EACCES) {
		enter_error("config poll 401/403");
		return err;
	}

	nodifyr_device_config_get(&cfg);
	if (*upload_backoff_s <= cfg.upload_interval_sec) {
		*upload_backoff_s = cfg.upload_interval_sec;
	}

	return 0;
}

static void run_normal(void)
{
	struct nodifyr_device_config cfg;
	int64_t now_ms;
	int64_t last_cycle_ms = 0;
	int upload_backoff_s;
	int err;
	int sleep_s;
	bool due;
	bool full_batch;

	LOG_INF("NORMAL: radar stub + upload/config cycle");
	nodifyr_device_config_init();
	nodifyr_device_config_get(&cfg);
	upload_backoff_s = cfg.upload_interval_sec;

	(void)nodifyr_radar_stub_start();

	/* First cycle ASAP so dashboard config applies. */
	err = upload_and_refresh_config(&upload_backoff_s);
	if (err == -EACCES) {
		return;
	}
	last_cycle_ms = k_uptime_get();
	nodifyr_device_config_get(&cfg);
	upload_backoff_s = cfg.upload_interval_sec;

	while (state == NODIFYR_STATE_NORMAL) {
		now_ms = k_uptime_get();
		nodifyr_device_config_get(&cfg);

		due = (now_ms - last_cycle_ms) >=
		      ((int64_t)upload_backoff_s * 1000);
		full_batch = (cfg.upload_mode == NODIFYR_UPLOAD_DETAILED) &&
			     (nodifyr_telemetry_queued() >=
			      DETAILED_UPLOAD_THRESHOLD);

		if (due || full_batch) {
			if (full_batch && !due) {
				LOG_INF("Detailed queue at %u — uploading now",
					(unsigned)nodifyr_telemetry_queued());
			}
			err = upload_and_refresh_config(&upload_backoff_s);
			if (err == -EACCES) {
				continue;
			}
			last_cycle_ms = k_uptime_get();
			nodifyr_device_config_get(&cfg);
			/* Reset interval after a successful non-retry cycle */
			if (upload_backoff_s > cfg.upload_interval_sec) {
				/* still in retry backoff */
			} else {
				upload_backoff_s = cfg.upload_interval_sec;
			}
		}

		/* Wake often so a full detailed batch is not delayed. */
		sleep_s = MIN(5, cfg.upload_interval_sec);
		if (sleep_s < 1) {
			sleep_s = 1;
		}
		k_sleep(K_SECONDS(sleep_s));
	}
}

int main(void)
{
	int err;

	LOG_INF("Nodifyr cellular gateway starting");
#if IS_ENABLED(CONFIG_NODIFYR_GAP_TELEMETRY)
	LOG_WRN("Gap telemetry ENABLED — requires app nodifyr.radar.gap.v1");
#else
	LOG_INF("Gap telemetry disabled (overwrite-oldest on overflow)");
#endif

	err = nodifyr_identity_init(&identity);
	if (err) {
		enter_fatal("settings init failed");
		goto halt;
	}

	err = nodifyr_lte_connect();
	if (err) {
		enter_fatal("LTE connect failed");
		goto halt;
	}

	err = nodifyr_lte_log_network_info();
	if (err) {
		LOG_WRN("Network info / time sync incomplete: %d", err);
	}

	err = nodifyr_identity_set_imei(&identity);
	if (err) {
		enter_fatal("IMEI unavailable");
		goto halt;
	}

	err = nodifyr_telemetry_init();
	if (err) {
		enter_fatal("telemetry init failed");
		goto halt;
	}

	if (identity.has_api_key) {
		LOG_INF("Boot: api_key present — NORMAL");
		state = NODIFYR_STATE_NORMAL;
		run_normal();
	} else if (!identity.has_pairing_secret) {
		enter_fatal("no pairing_secret (factory issue)");
	} else {
		run_pairing();
		if (state == NODIFYR_STATE_NORMAL) {
			run_normal();
		}
	}

halt:
	while (1) {
		if (state == NODIFYR_STATE_ERROR || state == NODIFYR_STATE_FATAL) {
			LOG_ERR("Halted in state %d", state);
		}
		k_sleep(K_SECONDS(60));
	}

	return 0;
}
