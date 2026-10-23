#pragma once
/* Timekeeping, PIT/APIC tick source and soft timers. */

#include <mcube/types.h>

#define HZ 1000u

typedef void (*timer_fn_t)(void *arg);

void timer_init(void);
u64  timer_ticks(void);
u64  timer_uptime_ms(void);
u64  timer_boot_time_unix(void);

/* One-shot (period_ms == 0) or periodic soft timer. Returns an id. */
u32  timer_add(u64 delay_ms, u64 period_ms, timer_fn_t fn, void *arg);
void timer_cancel(u32 id);

/* Sleep in the idle loop until the requested number of milliseconds passed.
 * Requires interrupts to be enabled; used by kernel threads before the
 * scheduler exists. */
void timer_sleep_ms(u64 ms);

/* Called from the IRQ0 handler. */
void timer_tick(void);
