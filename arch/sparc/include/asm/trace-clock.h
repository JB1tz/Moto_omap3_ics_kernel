/*
 * Copyright (C) 2008, Mathieu Desnoyers
 *
 * Trace clock definitions for Sparc64.
 */

#ifndef _ASM_SPARC_TRACE_CLOCK_H
#define _ASM_SPARC_TRACE_CLOCK_H

#include <linux/timex.h>

static inline u32 trace_clock_read32(void)
{
	return get_cycles();
}

static inline u64 trace_clock_read64(void)
{
	return get_cycles();
}

static inline unsigned int trace_clock_frequency(void)
{
	return get_cycles_rate();
}

static inline u32 trace_clock_freq_scale(void)
{
	return 1;
}

static inline void get_trace_clock(void)
{
}

static inline void put_trace_clock(void)
{
}

static inline void set_trace_clock_is_sync(int state)
{
}
#endif /* _ASM_SPARC_TRACE_CLOCK_H */
