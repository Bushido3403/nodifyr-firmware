#ifndef NODIFYR_IDENTITY_H_
#define NODIFYR_IDENTITY_H_

#include "nodifyr_common.h"

int nodifyr_identity_init(struct nodifyr_identity *id);
int nodifyr_identity_load(struct nodifyr_identity *id);
int nodifyr_identity_set_imei(struct nodifyr_identity *id);
int nodifyr_identity_save_credentials(struct nodifyr_identity *id,
				      const char *api_key,
				      const char *api_url);

void nodifyr_log_trunc_secret(const char *label, const char *secret);
void nodifyr_log_trunc_api_key(const char *api_key);

#endif /* NODIFYR_IDENTITY_H_ */
