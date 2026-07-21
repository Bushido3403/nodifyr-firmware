#include "json_util.h"

#include <string.h>

const char *nodifyr_json_get_string(const cJSON *obj, const char *key)
{
	const cJSON *item;

	if (!obj || !key) {
		return NULL;
	}

	item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!cJSON_IsString(item) || item->valuestring == NULL) {
		return NULL;
	}

	return item->valuestring;
}

bool nodifyr_json_get_bool(const cJSON *obj, const char *key, bool *out)
{
	const cJSON *item;

	if (!obj || !key || !out) {
		return false;
	}

	item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!cJSON_IsBool(item)) {
		return false;
	}

	*out = cJSON_IsTrue(item);
	return true;
}
