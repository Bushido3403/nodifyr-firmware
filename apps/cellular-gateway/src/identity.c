#include "identity.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <modem/modem_info.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(nodifyr_id, CONFIG_NODIFYR_LOG_LEVEL);

static struct nodifyr_identity *load_target;

static int settings_set(const char *name, size_t len, settings_read_cb read_cb,
			void *cb_arg)
{
	int err;
	const char *next;
	char *dest = NULL;
	size_t dest_size = 0;

	if (!load_target) {
		return -EINVAL;
	}

	if (settings_name_steq(name, "pairing_secret", &next) && !next) {
		dest = load_target->pairing_secret;
		dest_size = sizeof(load_target->pairing_secret);
	} else if (settings_name_steq(name, "api_key", &next) && !next) {
		dest = load_target->api_key;
		dest_size = sizeof(load_target->api_key);
	} else if (settings_name_steq(name, "api_url", &next) && !next) {
		dest = load_target->api_url;
		dest_size = sizeof(load_target->api_url);
	} else {
		return -ENOENT;
	}

	if (len >= dest_size) {
		return -ENOMEM;
	}

	memset(dest, 0, dest_size);
	err = read_cb(cb_arg, dest, len);
	if (err < 0) {
		return err;
	}
	dest[len] = '\0';

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(nodifyr, "nodifyr", NULL, settings_set, NULL,
			       NULL);

void nodifyr_log_trunc_secret(const char *label, const char *secret)
{
	size_t n;

	if (!secret || secret[0] == '\0') {
		LOG_INF("%s: <empty>", label);
		return;
	}

	n = strlen(secret);
	if (n <= 4) {
		LOG_INF("%s: ****", label);
		return;
	}

	LOG_INF("%s: ...%s", label, &secret[n - 4]);
}

void nodifyr_log_trunc_api_key(const char *api_key)
{
	size_t n;

	if (!api_key || api_key[0] == '\0') {
		LOG_INF("api_key: <empty>");
		return;
	}

	n = strlen(api_key);
	if (n <= 7) {
		LOG_INF("api_key: ngw_...");
		return;
	}

	LOG_INF("api_key: ngw_...%s", &api_key[n - 3]);
}

static int seed_pairing_secret_if_needed(struct nodifyr_identity *id)
{
	const char *seed = CONFIG_NODIFYR_PAIRING_SECRET;
	int err;

	if (id->pairing_secret[0] != '\0') {
		return 0;
	}

	if (seed[0] == '\0') {
		return 0;
	}

	if (strlen(seed) >= sizeof(id->pairing_secret)) {
		LOG_ERR("CONFIG_NODIFYR_PAIRING_SECRET too long");
		return -EINVAL;
	}

	LOG_WRN("Seeding pairing_secret from Kconfig (dev only)");
	strncpy(id->pairing_secret, seed, sizeof(id->pairing_secret) - 1);
	id->pairing_secret[sizeof(id->pairing_secret) - 1] = '\0';

	err = settings_save_one("nodifyr/pairing_secret", id->pairing_secret,
				strlen(id->pairing_secret));
	if (err) {
		LOG_ERR("Failed to persist seeded pairing_secret: %d", err);
		return err;
	}

	return 0;
}

int nodifyr_identity_init(struct nodifyr_identity *id)
{
	int err;

	memset(id, 0, sizeof(*id));

	err = settings_subsys_init();
	if (err) {
		LOG_ERR("settings_subsys_init: %d", err);
		return err;
	}

	return nodifyr_identity_load(id);
}

int nodifyr_identity_load(struct nodifyr_identity *id)
{
	int err;

	load_target = id;
	err = settings_load_subtree("nodifyr");
	load_target = NULL;
	if (err) {
		LOG_ERR("settings_load_subtree: %d", err);
		return err;
	}

	err = seed_pairing_secret_if_needed(id);
	if (err) {
		return err;
	}

	id->has_pairing_secret = id->pairing_secret[0] != '\0';
	id->has_api_key = id->api_key[0] != '\0';

	if (!id->has_api_key && id->api_url[0] == '\0') {
		strncpy(id->api_url, CONFIG_NODIFYR_DEFAULT_TELEMETRY_URL,
			sizeof(id->api_url) - 1);
	}

	nodifyr_log_trunc_secret("pairing_secret", id->pairing_secret);
	if (id->has_api_key) {
		nodifyr_log_trunc_api_key(id->api_key);
		LOG_INF("api_url: %s", id->api_url);
	} else {
		LOG_INF("api_key: <none>");
	}

	return 0;
}

int nodifyr_identity_set_imei(struct nodifyr_identity *id)
{
	int err;
	char imei[32];

	err = modem_info_init();
	if (err) {
		LOG_ERR("modem_info_init: %d", err);
		return err;
	}

	err = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
	if (err < 0) {
		LOG_ERR("IMEI read failed: %d", err);
		return err;
	}

	err = snprintk(id->hardware_id, sizeof(id->hardware_id), "nrf-%s", imei);
	if (err < 0 || err >= (int)sizeof(id->hardware_id)) {
		return -ENOMEM;
	}

	LOG_INF("hardware_id: %s", id->hardware_id);
	return 0;
}

int nodifyr_identity_save_credentials(struct nodifyr_identity *id,
				      const char *api_key,
				      const char *api_url)
{
	int err;

	if (!api_key || api_key[0] == '\0') {
		return -EINVAL;
	}

	if (strlen(api_key) >= sizeof(id->api_key)) {
		return -ENOMEM;
	}

	strncpy(id->api_key, api_key, sizeof(id->api_key) - 1);
	id->api_key[sizeof(id->api_key) - 1] = '\0';

	if (api_url && api_url[0] != '\0') {
		if (strlen(api_url) >= sizeof(id->api_url)) {
			return -ENOMEM;
		}
		strncpy(id->api_url, api_url, sizeof(id->api_url) - 1);
		id->api_url[sizeof(id->api_url) - 1] = '\0';
	} else if (id->api_url[0] == '\0') {
		strncpy(id->api_url, CONFIG_NODIFYR_DEFAULT_TELEMETRY_URL,
			sizeof(id->api_url) - 1);
	}

	/* Write-before-continue: persist before returning to NORMAL. */
	err = settings_save_one("nodifyr/api_key", id->api_key,
				strlen(id->api_key));
	if (err) {
		LOG_ERR("Failed to save api_key: %d", err);
		return err;
	}

	err = settings_save_one("nodifyr/api_url", id->api_url,
				strlen(id->api_url));
	if (err) {
		LOG_ERR("Failed to save api_url: %d", err);
		return err;
	}

	id->has_api_key = true;
	LOG_INF("Persisted API credentials");
	nodifyr_log_trunc_api_key(id->api_key);
	LOG_INF("api_url: %s", id->api_url);
	return 0;
}
