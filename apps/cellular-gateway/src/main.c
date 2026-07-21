#include "nodifyr_common.h"
#include "lte.h"
#include "identity.h"
#include "pairing.h"
#include "telemetry.h"
#include "radar_stub.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(nodifyr_main, CONFIG_NODIFYR_LOG_LEVEL);

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

static void run_normal(void)
{
	enum nodifyr_upload_result up;
	int upload_backoff_s = CONFIG_NODIFYR_TELEMETRY_UPLOAD_INTERVAL_S;

	LOG_INF("NORMAL: radar stub + telemetry upload");
	(void)nodifyr_radar_stub_start();

	while (state == NODIFYR_STATE_NORMAL) {
		if (nodifyr_telemetry_queued() > 0) {
			up = nodifyr_telemetry_upload(&identity);
			switch (up) {
			case NODIFYR_UPLOAD_OK:
			case NODIFYR_UPLOAD_DROP:
				upload_backoff_s =
					CONFIG_NODIFYR_TELEMETRY_UPLOAD_INTERVAL_S;
				break;
			case NODIFYR_UPLOAD_AUTH_ERROR:
				enter_error("telemetry 401/403");
				continue;
			case NODIFYR_UPLOAD_RETRY:
			default:
				upload_backoff_s =
					MIN(upload_backoff_s * 2, 300);
				LOG_WRN("Upload retry backoff %d s",
					upload_backoff_s);
				break;
			}
		}

		k_sleep(K_SECONDS(upload_backoff_s));
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
