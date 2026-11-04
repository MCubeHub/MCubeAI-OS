#pragma once
/* Timekeeping, PIT/APIC tick source and soft timers. */

#include <mcube/types.h>

#define HZ 1000u

typedef void (*timer_fn_t)(void *arg);

void timer_init(void);
u64  timer_ticks(void);
u64  timer_uptime_ms(void);
u64  timer_boot_time_unix(void);
/* Current wall-clock epoch: the CMOS anchor advanced by the tick counter, and
 * re-anchored to the RTC once a minute so tick-source drift cannot accumulate. */
u64  timer_now_unix(void);

/* One-shot (period_ms == 0) or periodic soft timer. Returns an id. */
u32  timer_add(u64 delay_ms, u64 period_ms, timer_fn_t fn, void *arg);
void timer_cancel(u32 id);

/* Sleep in the idle loop until the requested number of milliseconds passed.
 * Requires interrupts to be enabled; used by kernel threads before the
 * scheduler exists. */
void timer_sleep_ms(u64 ms);

/* Called from the IRQ0 handler. */
void timer_tick(void);

/* Boot-time acceptance test for the tick source. Measures the window with the
 * interrupt-free PIT channel-2 counter and switches to the local APIC timer
 * when IRQ0 never arrives (UEFI machines that leave the 8259 disconnected).
 * Returns the number of timer ticks observed during the window. */
u32  timer_selftest(u32 ms);

/* Non-zero once the tick is driven by the local APIC timer instead of the PIT.
 * The IRQ path uses this to send the right end-of-interrupt to the right
 * controller. */
int  timer_uses_lapic(void);
void timer_lapic_eoi(void);
