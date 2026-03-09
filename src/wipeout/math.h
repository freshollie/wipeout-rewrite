#ifndef MATH_H
#define MATH_H

#include <stdint.h>

extern float gSineTable[];
extern float gCosineTable[];

#define sins(x) gSineTable[(uint16_t) (x) >> 4]
#define coss(x) gCosineTable[(uint16_t) (x) >> 4]

#endif // MATH_H
