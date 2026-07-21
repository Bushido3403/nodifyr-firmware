#ifndef NODIFYR_LTE_H_
#define NODIFYR_LTE_H_

#include <stdbool.h>
#include <stdint.h>

/** Initialize modem, provision TLS CA offline, attach LTE, wait for PDN/IP. */
int nodifyr_lte_connect(void);

/** True when registered with an active PDN. */
bool nodifyr_lte_is_connected(void);

/** Log IPv4, DNS, truncated IMEI, and wait for date_time sync. */
int nodifyr_lte_log_network_info(void);

/** Block until date_time reports a valid wall clock (or timeout). */
int nodifyr_lte_wait_time(int timeout_ms);

#endif /* NODIFYR_LTE_H_ */
