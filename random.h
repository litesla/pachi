#ifndef PACHI_RANDOM_H
#define PACHI_RANDOM_H

#include <stdint.h>

#include "util.h"

void fast_srandom(unsigned long seed);
unsigned long fast_getseed(void);

/* Note that only 16bit numbers can be returned. */
/*请注意，只能返回16位数字。*/
uint16_t fast_random(unsigned int max);
/* Use this one if you want larger numbers. */
/*如果你想要更大的数字，就用这个。*/
static uint32_t fast_irandom(unsigned int max);

/* Get random number in [0..1] range. */
float fast_frandom();


static inline uint32_t
fast_irandom(unsigned int max)
{
	if (max <= 65536)
		return fast_random(max);
	int himax = (max - 1) / 65536;
	uint16_t hi = fast_random(himax + 1);
	return ((uint32_t)hi << 16) | fast_random(hi < himax ? 65536 : max % 65536);
}

#endif
