#pragma once
#include <stdint.h>
extern "C" void clock_get_uptime(uint64_t *);
extern "C" void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
