#include "telemetry.h"
#include "https_client.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cJSON.h>
#include <date_time.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

LOG_MODULE_REGISTER(nodifyr_telem, CONFIG_NODIFYR_LOG_LEVEL);

#define QUEUE_SIZE CONFIG_NODIFYR_TELEMETRY_QUEUE_SIZE
#define BATCH_MAX CONFIG_NODIFYR_TELEMETRY_BATCH_MAX

static struct nodifyr_reading queue[QUEUE_SIZE];
static size_t q_head;
static size_t q_count;
static uint32_t overflow_count;
static int64_t last_overflow_log_ms;
static struct k_mutex q_lock;

#if defined(CONFIG_NODIFYR_GAP_TELEMETRY)
static void enqueue_gap_locked(uint32_t dropped, int64_t first_ts, int64_t last_ts,
			       uint16_t sequence)
{
	struct nodifyr_reading *slot;
	size_t idx;

	if (q_count == QUEUE_SIZE) {
		/* Prefer merging into an existing gap at head when possible. */
		slot = &queue[q_head];
		if (slot->kind == NODIFYR_READING_GAP) {
			slot->fields.gap.dropped_count += dropped;
			if (first_ts < slot->fields.gap.first_ts) {
				slot->fields.gap.first_ts = first_ts;
			}
			if (last_ts > slot->fields.gap.last_ts) {
				slot->fields.gap.last_ts = last_ts;
			}
			slot->ts = slot->fields.gap.last_ts;
			return;
		}
		q_head = (q_head + 1) % QUEUE_SIZE;
		q_count--;
	}

	idx = (q_head + q_count) % QUEUE_SIZE;
	slot = &queue[idx];
	memset(slot, 0, sizeof(*slot));
	slot->kind = NODIFYR_READING_GAP;
	slot->ts = last_ts;
	slot->rssi = 0;
	slot->sequence = sequence;
	strncpy(slot->mac, NODIFYR_RADAR_MAC, sizeof(slot->mac) - 1);
	slot->fields.gap.dropped_count = dropped;
	slot->fields.gap.first_ts = first_ts;
	slot->fields.gap.last_ts = last_ts;
	q_count++;
}
#endif

int nodifyr_telemetry_init(void)
{
	k_mutex_init(&q_lock);
	q_head = 0;
	q_count = 0;
	overflow_count = 0;
	return 0;
}

size_t nodifyr_telemetry_queued(void)
{
	size_t n;

	k_mutex_lock(&q_lock, K_FOREVER);
	n = q_count;
	k_mutex_unlock(&q_lock);
	return n;
}

void nodifyr_telemetry_enqueue(const struct nodifyr_reading *reading)
{
	size_t idx;
	int64_t now_ms;

	if (!reading) {
		return;
	}

	k_mutex_lock(&q_lock, K_FOREVER);

	if (q_count == QUEUE_SIZE) {
#if defined(CONFIG_NODIFYR_GAP_TELEMETRY)
		struct nodifyr_reading dropped = queue[q_head];
		int64_t first_ts = dropped.ts;
		int64_t last_ts = reading->ts;
		uint16_t seq = reading->sequence;

		q_head = (q_head + 1) % QUEUE_SIZE;
		q_count--;
		enqueue_gap_locked(1, first_ts, last_ts, seq);
#else
		q_head = (q_head + 1) % QUEUE_SIZE;
		q_count--;
		overflow_count++;
		now_ms = k_uptime_get();
		if (now_ms - last_overflow_log_ms > 60000) {
			LOG_WRN("Telemetry queue full; overwrote oldest (%u total)",
				overflow_count);
			last_overflow_log_ms = now_ms;
		}
#endif
	}

	idx = (q_head + q_count) % QUEUE_SIZE;
	queue[idx] = *reading;
	q_count++;

	k_mutex_unlock(&q_lock);
}

static int json_add_i64(cJSON *obj, const char *key, int64_t value)
{
	char buf[32];
	cJSON *item;

	snprintk(buf, sizeof(buf), "%lld", (long long)value);
	item = cJSON_CreateRaw(buf);
	if (!item) {
		return -ENOMEM;
	}
	cJSON_AddItemToObject(obj, key, item);
	return 0;
}

static int json_add_i32(cJSON *obj, const char *key, int32_t value)
{
	char buf[16];
	cJSON *item;

	snprintk(buf, sizeof(buf), "%d", value);
	item = cJSON_CreateRaw(buf);
	if (!item) {
		return -ENOMEM;
	}
	cJSON_AddItemToObject(obj, key, item);
	return 0;
}

static cJSON *reading_to_json(const struct nodifyr_reading *r)
{
	cJSON *obj;
	cJSON *fields;
	const char *dtype;
	uint16_t direction;

	obj = cJSON_CreateObject();
	if (!obj) {
		return NULL;
	}

	dtype = (r->kind == NODIFYR_READING_GAP) ? NODIFYR_DEVICE_TYPE_GAP :
						   NODIFYR_DEVICE_TYPE_CAR;

	/* Integer-only wire format (nodifyr-app normalize.ts Number.isInteger). */
	if (json_add_i64(obj, "ts", r->ts) ||
	    json_add_i32(obj, "rssi", r->rssi) ||
	    !cJSON_AddStringToObject(obj, "mac", r->mac) ||
	    !cJSON_AddStringToObject(obj, "device_type", dtype) ||
	    json_add_i32(obj, "sequence", r->sequence)) {
		cJSON_Delete(obj);
		return NULL;
	}

	fields = cJSON_CreateObject();
	if (!fields) {
		cJSON_Delete(obj);
		return NULL;
	}

	if (r->kind == NODIFYR_READING_CAR) {
		/* App allowlist: direction_deg max 359 (360 rejected). */
		direction = r->fields.car.direction_deg % 360;
		if (json_add_i32(fields, "speed_centi_kph",
				 r->fields.car.speed_centi_kph) ||
		    json_add_i32(fields, "direction_deg", direction)) {
			cJSON_Delete(fields);
			cJSON_Delete(obj);
			return NULL;
		}
		if (r->fields.car.has_length_cm) {
			(void)json_add_i32(fields, "length_cm",
					   r->fields.car.length_cm);
		}
		if (r->fields.car.has_confidence_pct) {
			(void)json_add_i32(fields, "confidence_pct",
					   r->fields.car.confidence_pct);
		}
	} else {
		if (json_add_i32(fields, "dropped_count",
				 (int32_t)r->fields.gap.dropped_count) ||
		    json_add_i64(fields, "first_ts", r->fields.gap.first_ts) ||
		    json_add_i64(fields, "last_ts", r->fields.gap.last_ts)) {
			cJSON_Delete(fields);
			cJSON_Delete(obj);
			return NULL;
		}
	}

	cJSON_AddItemToObject(obj, "fields", fields);
	return obj;
}

/**
 * Parse api_url from cloud (full https://host/path) into host + path.
 * Matches nodifyr-app getTelemetryApiUrl(): {origin}/api/v1/telemetry.
 */
static int parse_telemetry_url(const char *url, char *host, size_t host_len,
			       char *path, size_t path_len)
{
	const char *p;
	const char *slash;
	size_t host_bytes;

	if (!url || !host || !path) {
		return -EINVAL;
	}

	if (strncmp(url, "https://", 8) == 0) {
		p = url + 8;
		slash = strchr(p, '/');
		if (!slash) {
			if (strlen(p) >= host_len) {
				return -ENOMEM;
			}
			strcpy(host, p);
			strncpy(path, "/", path_len - 1);
			path[path_len - 1] = '\0';
			return 0;
		}

		host_bytes = (size_t)(slash - p);
		if (host_bytes == 0 || host_bytes >= host_len) {
			return -ENOMEM;
		}
		memcpy(host, p, host_bytes);
		host[host_bytes] = '\0';

		strncpy(path, slash, path_len - 1);
		path[path_len - 1] = '\0';
		return 0;
	}

	/* Relative path only — use default cloud host. */
	host[0] = '\0';
	strncpy(path, url, path_len - 1);
	path[path_len - 1] = '\0';
	return 0;
}

enum nodifyr_upload_result nodifyr_telemetry_upload(const struct nodifyr_identity *id)
{
	size_t batch_n;
	size_t i;
	struct nodifyr_reading batch[BATCH_MAX];
	cJSON *root;
	cJSON *readings;
	char *payload;
	char host[64];
	char path[128];
	char api_hdr[96];
	const char *headers[3];
	struct nodifyr_http_response resp;
	int err;
	int64_t now_ms = 0;

	if (!id || !id->has_api_key) {
		return NODIFYR_UPLOAD_AUTH_ERROR;
	}

	err = date_time_now(&now_ms);
	if (err || now_ms <= 0) {
		LOG_WRN("No wall clock; skipping upload");
		return NODIFYR_UPLOAD_RETRY;
	}

	k_mutex_lock(&q_lock, K_FOREVER);
	batch_n = q_count < BATCH_MAX ? q_count : BATCH_MAX;
	for (i = 0; i < batch_n; i++) {
		batch[i] = queue[(q_head + i) % QUEUE_SIZE];
	}
	k_mutex_unlock(&q_lock);

	if (batch_n == 0) {
		return NODIFYR_UPLOAD_OK;
	}

	root = cJSON_CreateObject();
	readings = cJSON_CreateArray();
	if (!root || !readings) {
		cJSON_Delete(root);
		cJSON_Delete(readings);
		return NODIFYR_UPLOAD_RETRY;
	}

	for (i = 0; i < batch_n; i++) {
		cJSON *item = reading_to_json(&batch[i]);

		if (!item) {
			cJSON_Delete(root);
			cJSON_Delete(readings);
			return NODIFYR_UPLOAD_RETRY;
		}
		cJSON_AddItemToArray(readings, item);
	}

	cJSON_AddItemToObject(root, "readings", readings);
	payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!payload) {
		return NODIFYR_UPLOAD_RETRY;
	}

	err = parse_telemetry_url(id->api_url, host, sizeof(host), path,
				  sizeof(path));
	if (err) {
		cJSON_free(payload);
		return NODIFYR_UPLOAD_RETRY;
	}

	snprintk(api_hdr, sizeof(api_hdr), "X-Api-Key: %s\r\n", id->api_key);
	headers[0] = "Content-Type: application/json\r\n";
	headers[1] = api_hdr;
	headers[2] = NULL;

	LOG_INF("Uploading %u readings", (unsigned)batch_n);
	err = nodifyr_https_request(host[0] ? host : NULL, "POST", path, NULL,
				    payload, headers, &resp);
	cJSON_free(payload);
	if (err) {
		return NODIFYR_UPLOAD_RETRY;
	}

	if (resp.status_code == 401 || resp.status_code == 403) {
		LOG_ERR("Telemetry auth error HTTP %d — entering ERROR",
			resp.status_code);
		return NODIFYR_UPLOAD_AUTH_ERROR;
	}

	if (resp.status_code == 400) {
		LOG_ERR("Telemetry schema rejected (400); dropping batch");
		k_mutex_lock(&q_lock, K_FOREVER);
		q_head = (q_head + batch_n) % QUEUE_SIZE;
		q_count -= batch_n;
		k_mutex_unlock(&q_lock);
		return NODIFYR_UPLOAD_DROP;
	}

	if (resp.status_code == 429 || resp.status_code >= 500) {
		LOG_WRN("Telemetry HTTP %d; will retry", resp.status_code);
		return NODIFYR_UPLOAD_RETRY;
	}

	if (resp.status_code < 200 || resp.status_code >= 300) {
		LOG_WRN("Telemetry HTTP %d; will retry", resp.status_code);
		return NODIFYR_UPLOAD_RETRY;
	}

	/* 2xx: dequeue; not idempotent — do not retry this batch. */
	k_mutex_lock(&q_lock, K_FOREVER);
	q_head = (q_head + batch_n) % QUEUE_SIZE;
	q_count -= batch_n;
	k_mutex_unlock(&q_lock);

	LOG_INF("Telemetry accepted (%u)", (unsigned)batch_n);
	return NODIFYR_UPLOAD_OK;
}
