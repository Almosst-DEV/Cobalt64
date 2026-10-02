//  tests/dalshim/IOKit/IOLib.h - the kernel KPI bits smu_dal.cpp / smu_v14_0.cpp / amdgpu_regs.h use, for a HOST build. IOSleep does not sleep: it
//  hands the milliseconds to the test's world model (dalshim_sleep), which advances a fake clock and lets the simulated PMFW answer.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "IOReturn.h"
#include "IOLocks.h"
extern "C" void dalshim_sleep(unsigned ms);
static inline void IOLog(const char *format, ...) { va_list ap; va_start(ap, format); va_end(ap); (void)format; }
static inline void IOSleep(unsigned ms) { dalshim_sleep(ms); }
static inline void *IOMalloc(size_t size) { return malloc(size); }
static inline void IOFree(void *address, size_t size) { (void)size; free(address); }
