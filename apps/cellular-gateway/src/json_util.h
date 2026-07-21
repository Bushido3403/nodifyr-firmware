#ifndef NODIFYR_JSON_UTIL_H_
#define NODIFYR_JSON_UTIL_H_

#include <cJSON.h>
#include <stdbool.h>

const char *nodifyr_json_get_string(const cJSON *obj, const char *key);
bool nodifyr_json_get_bool(const cJSON *obj, const char *key, bool *out);

#endif /* NODIFYR_JSON_UTIL_H_ */
