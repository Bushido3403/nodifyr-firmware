#pragma once

#include <stdbool.h>
#include "u8g2.h"

/** Play the NODIFYR boot animation. Leaves the final logo on screen when done. */
void anim_boot(u8g2_t *u8g2, bool loop);
