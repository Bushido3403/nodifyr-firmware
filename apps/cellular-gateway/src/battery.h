#ifndef NODIFYR_BATTERY_H_
#define NODIFYR_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Read gateway battery state-of-charge estimate (0–100).
 * Thingy:91 X: nPM1300 charger voltage mapped to a LiPo curve.
 * Other boards: returns false (omit battery_pct from uplink).
 */
bool nodifyr_battery_read_pct(uint8_t *pct_out);

#endif /* NODIFYR_BATTERY_H_ */
