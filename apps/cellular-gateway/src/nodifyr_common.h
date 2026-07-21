#ifndef NODIFYR_COMMON_H_
#define NODIFYR_COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define NODIFYR_HARDWARE_ID_MAX	48
#define NODIFYR_SECRET_MAX	65
#define NODIFYR_API_KEY_MAX	64
#define NODIFYR_API_URL_MAX	128

#define NODIFYR_DEVICE_TYPE_CAR	"nodifyr.radar.car.v1"
#define NODIFYR_DEVICE_TYPE_GAP	"nodifyr.radar.gap.v1"

enum nodifyr_state {
	NODIFYR_STATE_BOOT = 0,
	NODIFYR_STATE_PAIRING,
	NODIFYR_STATE_NORMAL,
	NODIFYR_STATE_ERROR,
	NODIFYR_STATE_FATAL,
};

enum nodifyr_reading_kind {
	NODIFYR_READING_CAR = 0,
	NODIFYR_READING_CAR_SUMMARY,
	NODIFYR_READING_GAP,
};

struct nodifyr_car_fields {
	uint16_t speed_centi_kph;
	uint16_t direction_deg;
	uint16_t length_cm;
	uint8_t confidence_pct;
	bool has_length_cm;
	bool has_confidence_pct;
};

/** Aggregate window for upload_mode "summary" (same device_type). */
struct nodifyr_car_summary_fields {
	uint16_t car_count;
	uint16_t avg_speed_centi_kph;
	uint16_t min_speed_centi_kph;
	uint16_t max_speed_centi_kph;
	uint32_t window_sec;
	bool has_min_max;
	bool has_window_sec;
};

struct nodifyr_gap_fields {
	uint32_t dropped_count;
	int64_t first_ts;
	int64_t last_ts;
};

struct nodifyr_reading {
	enum nodifyr_reading_kind kind;
	int64_t ts;
	int8_t rssi;
	uint16_t sequence;
	union {
		struct nodifyr_car_fields car;
		struct nodifyr_car_summary_fields summary;
		struct nodifyr_gap_fields gap;
	} fields;
};

struct nodifyr_identity {
	char hardware_id[NODIFYR_HARDWARE_ID_MAX];
	char pairing_secret[NODIFYR_SECRET_MAX];
	char api_key[NODIFYR_API_KEY_MAX];
	char api_url[NODIFYR_API_URL_MAX];
	bool has_pairing_secret;
	bool has_api_key;
};

#endif /* NODIFYR_COMMON_H_ */
