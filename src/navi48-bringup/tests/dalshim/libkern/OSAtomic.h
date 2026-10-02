#pragma once
#include <stdint.h>
typedef uint32_t UInt32;
extern "C" bool OSCompareAndSwap(UInt32, UInt32, volatile UInt32 *);
extern "C" bool OSCompareAndSwapPtr(void *, void *, void *volatile *);
