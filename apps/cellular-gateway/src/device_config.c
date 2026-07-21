#include "device_config.h"
#include "https_client.h"
#include "json_util.h"
#include "identity.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <cJSON.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(nodifyr_cfg, CONFIG_NODIFYR_LOG_LEVEL);

/* Match nodifyr-app DEFAULT_DEVICE_CONFIG (mode + interval only). */
#define DEF_UPLOAD_INTERVAL_S 60

static struct nodifyr_device_config cfg;
static struct k_mutex cfg_lock;

static void apply_defaults(struct nodifyr_device_config *c)
{
	c->upload_mode = NODIFYR_UPLOAD_DETAILED;
	c->upload_interval_sec = DEF_UPLOAD_INTERVAL_S;
	c->version = 0;
	c->have_version = false;
}

static int clamp_int(int v, int lo, int hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb,
			void *cb_arg)
{
	const char *next;
	char buf[16];
	int err;

	if (settings_name_steq(name, "ver", &next) && !next) {
		if (len >= sizeof(buf)) {
			return -ENOMEM;
		}
		memset(buf, 0, sizeof(buf));
		err = read_cb(cb_arg, buf, len);
		if (err < 0) {
			return err;
		}
		buf[len] = '\0';
		cfg.version = (int)strtol(buf, NULL, 10);
		cfg.have_version = true;
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(nodifyr_cfg, "nodifyr_cfg", NULL, settings_set,
			       NULL, NULL);

static int persist_version(int version)
{
	char buf[16];
	int n;
	int err;

	n = snprintk(buf, sizeof(buf), "%d", version);
	if (n <= 0 || n >= (int)sizeof(buf)) {
		return -EINVAL;
	}

	err = settings_save_one("nodifyr_cfg/ver", buf, (size_t)n);
	if (err) {
		LOG_ERR("Failed to persist cfg_ver: %d", err);
	}
	return err;
}

static int parse_and_apply(const cJSON *root, int version)
{
	const cJSON *config;
	const char *mode;
	struct nodifyr_device_config next;
	int v;

	config = cJSON_GetObjectItemCaseSensitive(root, "config");
	if (!cJSON_IsObject(config)) {
		LOG_ERR("Config response missing config object");
		return -EINVAL;
	}

	next = cfg;
	next.version = version;
	next.have_version = true;

	mode = nodifyr_json_get_string(config, "upload_mode");
	if (mode) {
		if (strcmp(mode, "summary") == 0) {
			next.upload_mode = NODIFYR_UPLOAD_SUMMARY;
		} else {
			next.upload_mode = NODIFYR_UPLOAD_DETAILED;
		}
	}

	if (nodifyr_json_get_int(config, "upload_interval_sec", &v)) {
		next.upload_interval_sec = clamp_int(v, 10, 86400);
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	cfg = next;
	k_mutex_unlock(&cfg_lock);

	(void)persist_version(version);

	LOG_INF("Config v%d applied: mode=%s interval=%ds", version,
		next.upload_mode == NODIFYR_UPLOAD_SUMMARY ? "summary" :
							     "detailed",
		next.upload_interval_sec);

	return 0;
}

void nodifyr_device_config_init(void)
{
	k_mutex_init(&cfg_lock);
	apply_defaults(&cfg);
	(void)settings_load_subtree("nodifyr_cfg");
	LOG_INF("Runtime config defaults ready (version %s%d)",
		cfg.have_version ? "" : "unset/",
		cfg.have_version ? cfg.version : 0);
}

void nodifyr_device_config_get(struct nodifyr_device_config *out)
{
	if (!out) {
		return;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	*out = cfg;
	k_mutex_unlock(&cfg_lock);
}

int nodifyr_device_config_poll(const struct nodifyr_identity *id)
{
	char query[48];
	char api_hdr[96];
	const char *headers[3];
	struct nodifyr_http_response resp;
	cJSON *root;
	const cJSON *unchanged;
	const cJSON *ver_item;
	int version;
	int err;
	struct nodifyr_device_config snap;

	if (!id || !id->has_api_key) {
		return -EINVAL;
	}

	nodifyr_device_config_get(&snap);

	if (snap.have_version) {
		snprintk(query, sizeof(query), "since=%d", snap.version);
	} else {
		query[0] = '\0';
	}

	snprintk(api_hdr, sizeof(api_hdr), "X-Api-Key: %s\r\n", id->api_key);
	headers[0] = api_hdr;
	headers[1] = NULL;

	err = nodifyr_https_request(NULL, "GET", "/api/v1/config",
				    query[0] ? query : NULL, NULL, headers,
				    &resp);
	if (err) {
		return err;
	}

	if (resp.status_code == 401 || resp.status_code == 403) {
		LOG_ERR("Config poll auth HTTP %d", resp.status_code);
		return -EACCES;
	}

	if (resp.status_code == 429 || resp.status_code >= 500) {
		LOG_WRN("Config poll HTTP %d", resp.status_code);
		return -EAGAIN;
	}

	if (resp.status_code < 200 || resp.status_code >= 300 || !resp.body) {
		LOG_WRN("Config poll HTTP %d", resp.status_code);
		return -EIO;
	}

	root = cJSON_ParseWithLength(resp.body, resp.body_len);
	if (!root) {
		LOG_ERR("Config JSON parse failed");
		return -EINVAL;
	}

	unchanged = cJSON_GetObjectItemCaseSensitive(root, "unchanged");
	if (cJSON_IsTrue(unchanged)) {
		LOG_DBG("Config unchanged (v%d)", snap.version);
		cJSON_Delete(root);
		return 0;
	}

	ver_item = cJSON_GetObjectItemCaseSensitive(root, "version");
	if (!cJSON_IsNumber(ver_item)) {
		LOG_ERR("Config response missing version");
		cJSON_Delete(root);
		return -EINVAL;
	}
	version = ver_item->valueint;

	err = parse_and_apply(root, version);
	cJSON_Delete(root);
	return err;
}
