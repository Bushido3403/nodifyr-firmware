#include "lte.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <modem/modem_info.h>
#include <date_time.h>

#include <string.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_REGISTER(nodifyr_lte, CONFIG_NODIFYR_LOG_LEVEL);

static K_SEM_DEFINE(lte_connected, 0, 1);
static volatile bool connected;

static const char ca_certificate[] = {
#include "isrgrootx1.pem.inc"
};

static int certificate_provision(void)
{
	int err;
	bool exists;

	err = modem_key_mgmt_exists(CONFIG_NODIFYR_TLS_SEC_TAG,
				    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    &exists);
	if (err) {
		LOG_ERR("CA exists check failed: %d", err);
		return err;
	}

	if (exists) {
		err = modem_key_mgmt_cmp(CONFIG_NODIFYR_TLS_SEC_TAG,
					 MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					 ca_certificate,
					 sizeof(ca_certificate));
		if (!err) {
			LOG_INF("TLS CA already provisioned (tag %d)",
				CONFIG_NODIFYR_TLS_SEC_TAG);
			return 0;
		}
		LOG_INF("TLS CA mismatch; rewriting");
	}

	err = modem_key_mgmt_write(CONFIG_NODIFYR_TLS_SEC_TAG,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   ca_certificate,
				   sizeof(ca_certificate));
	if (err) {
		LOG_ERR("CA provision failed: %d", err);
		return err;
	}

	LOG_INF("Provisioned ISRG Root X1 to modem (tag %d)",
		CONFIG_NODIFYR_TLS_SEC_TAG);
	return 0;
}

static void mark_connected(void)
{
	if (!connected) {
		connected = true;
		k_sem_give(&lte_connected);
	}
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			if (connected) {
				LOG_WRN("LTE registration lost (%d)",
					evt->nw_reg_status);
			}
			connected = false;
			break;
		}
		LOG_INF("Network registered (%s)",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
				"home" : "roaming");
		/* PDN usually follows registration; unblock connect wait. */
		mark_connected();
		break;

	case LTE_LC_EVT_RRC_UPDATE:
		LOG_DBG("RRC %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
				"connected" : "idle");
		break;

	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			LOG_INF("PDN activated (cid %d)", evt->pdn.cid);
			mark_connected();
			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
			LOG_WRN("PDN deactivated (cid %d)", evt->pdn.cid);
			connected = false;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}
}

bool nodifyr_lte_is_connected(void)
{
	return connected;
}

int nodifyr_lte_wait_time(int timeout_ms)
{
	int64_t now_ms = 0;
	int64_t deadline = k_uptime_get() + timeout_ms;
	int err;

	while (k_uptime_get() < deadline) {
		err = date_time_now(&now_ms);
		if (!err && now_ms > 0) {
			LOG_INF("Wall clock ready: %lld ms", (long long)now_ms);
			return 0;
		}
		k_sleep(K_SECONDS(2));
	}

	LOG_ERR("Timed out waiting for date_time");
	return -ETIMEDOUT;
}

int nodifyr_lte_log_network_info(void)
{
	int err;
	char buf[128];

	err = modem_info_init();
	if (err) {
		LOG_WRN("modem_info_init: %d", err);
	} else {
		err = modem_info_string_get(MODEM_INFO_IMEI, buf, sizeof(buf));
		if (err >= 0) {
			size_t n = strlen(buf);

			if (n > 4) {
				LOG_INF("IMEI ...%s", &buf[n - 4]);
			} else {
				LOG_INF("IMEI available");
			}
		}

		err = modem_info_string_get(MODEM_INFO_IP_ADDRESS, buf, sizeof(buf));
		if (err >= 0) {
			LOG_INF("IP %s", buf);
		}
	}

	return nodifyr_lte_wait_time(120000);
}

int nodifyr_lte_connect(void)
{
	int err;

	LOG_INF("Initializing modem library");
	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init: %d", err);
		return err;
	}

	LOG_INF("Provisioning TLS CA (modem offline)");
	err = certificate_provision();
	if (err) {
		return err;
	}

	LOG_INF("Connecting LTE (APN hologram, IPv4)");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async: %d", err);
		return err;
	}

	err = k_sem_take(&lte_connected, K_MINUTES(5));
	if (err) {
		LOG_ERR("Timed out waiting for LTE registration/PDN");
		return -ETIMEDOUT;
	}

	LOG_INF("LTE connected");
	return 0;
}
