#include "pairing.h"
#include "https_client.h"
#include "identity.h"
#include "json_util.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(nodifyr_pair, CONFIG_NODIFYR_LOG_LEVEL);

static int64_t pairing_started_ms;
static bool announced;

static int backoff_s_for_status(int http_status, int retry_after)
{
	if (retry_after > 0) {
		return retry_after;
	}
	if (http_status == 429) {
		return 60;
	}
	return 15;
}

int nodifyr_pairing_next_poll_s(void)
{
	int64_t elapsed_s;

	if (pairing_started_ms == 0) {
		return CONFIG_NODIFYR_PAIR_POLL_INTERVAL_S;
	}

	elapsed_s = (k_uptime_get() - pairing_started_ms) / 1000;
	if (elapsed_s >= CONFIG_NODIFYR_PAIR_POLL_BACKOFF_AFTER_S) {
		return CONFIG_NODIFYR_PAIR_POLL_BACKOFF_S;
	}

	return CONFIG_NODIFYR_PAIR_POLL_INTERVAL_S;
}

static int post_pair(struct nodifyr_identity *id)
{
	int err;
	cJSON *root;
	char *payload;
	struct nodifyr_http_response resp;
	const char *status;

	root = cJSON_CreateObject();
	if (!root) {
		return -ENOMEM;
	}

	cJSON_AddStringToObject(root, "hardware_id", id->hardware_id);
	cJSON_AddStringToObject(root, "pairing_secret", id->pairing_secret);
	payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!payload) {
		return -ENOMEM;
	}

	LOG_INF("POST /pair hardware_id=%s", id->hardware_id);
	nodifyr_log_trunc_secret("pairing_secret", id->pairing_secret);

	err = nodifyr_https_request(NULL, "POST", CONFIG_NODIFYR_PAIR_PATH, NULL,
				    payload, NULL, &resp);
	cJSON_free(payload);
	if (err) {
		return err;
	}

	if (resp.status_code == 403) {
		LOG_ERR("Pair rejected (403 revoked)");
		return -EPERM;
	}
	if (resp.status_code == 401) {
		LOG_ERR("Pair unauthorized (401)");
		return -EACCES;
	}
	if (resp.status_code == 409) {
		LOG_ERR("Pair conflict (409)");
		return -EEXIST;
	}
	if (resp.status_code == 429) {
		LOG_WRN("Pair rate limited; retry in %d s",
			backoff_s_for_status(429, resp.retry_after_s));
		return -EAGAIN;
	}
	if (resp.status_code < 200 || resp.status_code >= 300) {
		LOG_ERR("Pair HTTP %d", resp.status_code);
		return -EIO;
	}

	root = cJSON_Parse(resp.body);
	if (!root) {
		LOG_ERR("Pair response JSON parse failed");
		return -EINVAL;
	}

	status = nodifyr_json_get_string(root, "status");
	LOG_INF("Pair announce status=%s", status ? status : "?");
	cJSON_Delete(root);
	return 0;
}

static enum nodifyr_pair_result get_status(struct nodifyr_identity *id)
{
	int err;
	char query[192];
	struct nodifyr_http_response resp;
	cJSON *root;
	const char *status;
	const char *api_key;
	const char *api_url;

	err = snprintk(query, sizeof(query),
		       "hardware_id=%s&pairing_secret=%s",
		       id->hardware_id, id->pairing_secret);
	if (err < 0 || err >= (int)sizeof(query)) {
		return NODIFYR_PAIR_ERROR;
	}

	err = nodifyr_https_request(NULL, "GET", CONFIG_NODIFYR_STATUS_PATH, query,
				    NULL, NULL, &resp);
	if (err) {
		LOG_WRN("Status request failed: %d", err);
		return NODIFYR_PAIR_CONTINUE;
	}

	if (resp.status_code == 403) {
		LOG_ERR("Status 403 revoked — stopping");
		return NODIFYR_PAIR_ERROR;
	}
	if (resp.status_code == 401) {
		LOG_ERR("Status 401");
		return NODIFYR_PAIR_ERROR;
	}
	if (resp.status_code == 429) {
		LOG_WRN("Status 429; backing off %d s",
			backoff_s_for_status(429, resp.retry_after_s));
		return NODIFYR_PAIR_CONTINUE;
	}
	if (resp.status_code < 200 || resp.status_code >= 300) {
		LOG_WRN("Status HTTP %d", resp.status_code);
		return NODIFYR_PAIR_CONTINUE;
	}

	root = cJSON_Parse(resp.body);
	if (!root) {
		LOG_WRN("Status JSON parse failed");
		return NODIFYR_PAIR_CONTINUE;
	}

	status = nodifyr_json_get_string(root, "status");
	if (!status) {
		cJSON_Delete(root);
		return NODIFYR_PAIR_CONTINUE;
	}

	LOG_INF("Pair status=%s", status);

	if (strcmp(status, "pending") == 0) {
		cJSON_Delete(root);
		return NODIFYR_PAIR_CONTINUE;
	}

	if (strcmp(status, "approved") == 0) {
		api_key = nodifyr_json_get_string(root, "api_key");
		api_url = nodifyr_json_get_string(root, "api_url");
		if (!api_key) {
			LOG_ERR("approved without api_key");
			cJSON_Delete(root);
			return NODIFYR_PAIR_ERROR;
		}

		err = nodifyr_identity_save_credentials(id, api_key, api_url);
		cJSON_Delete(root);
		if (err) {
			LOG_ERR("Persist credentials failed: %d", err);
			return NODIFYR_PAIR_ERROR;
		}

		return NODIFYR_PAIR_APPROVED;
	}

	if (strcmp(status, "active") == 0) {
		cJSON_Delete(root);
		if (!id->has_api_key) {
			LOG_ERR("active without local api_key — support re-pair");
			return NODIFYR_PAIR_ERROR;
		}
		return NODIFYR_PAIR_APPROVED;
	}

	cJSON_Delete(root);
	return NODIFYR_PAIR_CONTINUE;
}

enum nodifyr_pair_result nodifyr_pairing_step(struct nodifyr_identity *id)
{
	int err;

	if (!id->has_pairing_secret) {
		return NODIFYR_PAIR_ERROR;
	}

	if (pairing_started_ms == 0) {
		pairing_started_ms = k_uptime_get();
	}

	if (!announced) {
		err = post_pair(id);
		if (err == -EPERM || err == -EACCES || err == -EEXIST) {
			return NODIFYR_PAIR_ERROR;
		}
		if (err == -EAGAIN) {
			return NODIFYR_PAIR_CONTINUE;
		}
		if (err) {
			LOG_WRN("Pair announce failed (%d); will retry", err);
			return NODIFYR_PAIR_CONTINUE;
		}
		announced = true;
	}

	return get_status(id);
}
