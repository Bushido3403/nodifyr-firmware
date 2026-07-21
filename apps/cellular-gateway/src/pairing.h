#ifndef NODIFYR_PAIRING_H_
#define NODIFYR_PAIRING_H_

#include "nodifyr_common.h"

enum nodifyr_pair_result {
	NODIFYR_PAIR_CONTINUE = 0,
	NODIFYR_PAIR_APPROVED,
	NODIFYR_PAIR_ERROR,
};

/**
 * Run pairing announce + status poll until approved, error, or one iteration
 * returns CONTINUE (caller sleeps and retries).
 * On APPROVED, api_key/api_url are already persisted (write-before-continue).
 */
enum nodifyr_pair_result nodifyr_pairing_step(struct nodifyr_identity *id);

/** Suggested sleep before next pairing_step while CONTINUE. */
int nodifyr_pairing_next_poll_s(void);

#endif /* NODIFYR_PAIRING_H_ */
