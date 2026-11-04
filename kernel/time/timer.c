/* PIT-based tick source and soft timer queue.
 *
 * M3 keeps the 8254 PIT at 1000 Hz because it works on every x86 machine and
 * in QEMU without ACPI parsing. The APIC timer replaces it once the MADT is
 * parsed; timer_ticks()/timer_add() stay the same.
 */

#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/mm.h>
#include <mcube/timer.h>
#include <mcube/sched.h>

#define PIT_HZ 1193182u
#define MAX_SOFT_TIMERS 128

/* ---- independent, interrupt-free time base ---------------------------
 *
 * PIT channel 2 is a plain down counter: program a count and poll the latched
 * value until it stops at zero. No IRQ involved, so it still works on the
 * machines where IRQ0 never reaches the CPU (UEFI firmware that leaves the
 * 8259 disconnected and expects the OS to drive the IOAPIC). Boot uses it to
 * bound the tick-source acceptance test instead of halting forever. */
#define PIT_CH2_PORT 0x42u
#define PIT_CMD_PORT 0x43u
#define PIT_SPEAKER  0x61u
#define PIT_TICKS_PER_MS 1193u        /* 1193182 / 1000, rounded down */

static void pit_ch2_start(u16 count) {
    /* Gate on (bit 0), speaker data off (bit 1) so nothing beeps. */
    outb(PIT_SPEAKER, (u8)((inb(PIT_SPEAKER) & 0xFCu) | 0x01u));
    outb(PIT_CMD_PORT, 0xB0u);        /* channel 2, lobyte/hibyte, mode 0 */
    outb(PIT_CH2_PORT, (u8)(count & 0xFFu));
    outb(PIT_CH2_PORT, (u8)(count >> 8));
}

static u16 pit_ch2_read(void) {
    u8 lo, hi;
    outb(PIT_CMD_PORT, 0x80u);        /* latch channel 2 */
    lo = inb(PIT_CH2_PORT);
    hi = inb(PIT_CH2_PORT);
    return (u16)((u16)lo | ((u16)hi << 8));
}

/* Wait for `ticks` PIT ticks (0 means a full 65536, i.e. about 54.9 ms). The
 * loop is bounded so a chipset without a counting channel 2 cannot wedge boot;
 * a full 54.9 ms period needs roughly 55k iterations, so 400k is generous
 * headroom while still ending in well under a second even when every port
 * access is trapped by firmware. */
static void pit_wait_ticks(u32 ticks) {
    if (ticks > 0xFFFFu) ticks = 0xFFFFu;
    pit_ch2_start((u16)ticks);
    for (u32 guard = 0; guard < 400000u; guard++) {
        if (pit_ch2_read() == 0 && pit_ch2_read() == 0) return;
    }
}

static void pit_busy_wait_ms(u32 ms) {
    while (ms >= 54u) {
        pit_wait_ticks(0);
        ms -= 54u;
    }
    if (ms) pit_wait_ticks(ms * PIT_TICKS_PER_MS);
}

/* ---- local APIC timer ------------------------------------------------
 *
 * The fallback tick source for firmware that never delivers IRQ0 to the CPU.
 * The local APIC timer is wired straight to the local CPU: once its LVT entry
 * is unmasked the vector fires without any IOAPIC routing, so this works even
 * when the 8259 is disconnected. Its frequency is calibrated against PIT
 * channel 2, which needs no interrupts either. */
#define IRQ_TIMER_VECTOR 32u          /* IRQ_BASE: the vector the PIT handler uses */
#define LAPIC_VA        0xFFFFD00000000000ULL
#define IA32_APIC_BASE  0x1Bu
#define APIC_ENABLE_BIT (1ULL << 11)
#define LAPIC_TPR       0x080u
#define LAPIC_EOI       0x0B0u
#define LAPIC_SVR       0x0F0u
#define LAPIC_TIMER     0x320u
#define LAPIC_TDCR      0x3E0u
#define LAPIC_TICR      0x380u
#define LAPIC_TCCR      0x390u

static volatile u32 *g_lapic;
static int g_lapic_timer;
static u32 g_lapic_period;

static u32 lapic_read(u32 off) { return g_lapic[off >> 2]; }
static void lapic_write(u32 off, u32 v) { g_lapic[off >> 2] = v; }

int timer_uses_lapic(void) { return g_lapic_timer; }

void timer_lapic_eoi(void) {
    if (g_lapic && g_lapic_timer) lapic_write(LAPIC_EOI, 0);
}

static int lapic_present(void) {
    u32 a = 1, b, c = 0, d;
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    (void)b;
    return (int)((d >> 9) & 1u);
}

/* Map the APIC page uncached and put it into software-enable mode. */
static int lapic_map(void) {
    u64 msr = read_msr(IA32_APIC_BASE);
    u64 base = msr & 0x000FFFFFFFFFF000ULL;
    if (!base) base = 0xFEE00000ULL;
    if (!(msr & APIC_ENABLE_BIT)) write_msr(IA32_APIC_BASE, msr | APIC_ENABLE_BIT);

    vmm_map_page(LAPIC_VA, base,
                 PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT | PTE_NX);
    g_lapic = (volatile u32 *)(uptr)LAPIC_VA;

    lapic_write(LAPIC_SVR, 0x100u | 0xFFu);   /* software enable, spurious 0xFF */
    lapic_write(LAPIC_TPR, 0);                /* accept everything */
    return 1;
}

/* Program the periodic tick. Returns 1 when the counter is running. */
static int lapic_start_timer(void) {
    if (!lapic_present() || !lapic_map()) return 0;

    /* One PIT channel-2 period (65536 ticks = 54.9 ms) is the yardstick. */
    lapic_write(LAPIC_TDCR, 0x3u);            /* divide by 16 */
    lapic_write(LAPIC_TIMER, (1u << 16) | (u32)IRQ_TIMER_VECTOR); /* masked */
    lapic_write(LAPIC_TICR, 0xFFFFFFFFu);
    u32 first = lapic_read(LAPIC_TCCR);
    pit_wait_ticks(0);
    u32 last = lapic_read(LAPIC_TCCR);

    u32 delta = first - last;
    if (delta < 1000u || last == 0) {
        /* Channel 2 is unusable, so the divider cannot be measured. Fall back
         * to the nominal APIC timer rate (100 MHz bus / 16) rather than leaving
         * a machine with no tick source at all: a slightly wrong tick is far
         * better than a scheduler that never preempts. */
        g_lapic_period = 6250u;              /* 100 MHz / 16 / 1000 Hz */
        kprintf("[time] APIC timer uncalibrated, using nominal %u counts\n",
                (u32)g_lapic_period);
        lapic_write(LAPIC_TIMER, (1u << 17) | (u32)IRQ_TIMER_VECTOR);
        lapic_write(LAPIC_TICR, g_lapic_period);
        g_lapic_timer = 1;
        return 1;
    }

    u64 per_second = (u64)delta * (u64)PIT_HZ / 65536ULL;
    if (per_second < HZ) return 0;
    u32 period = (u32)(per_second / HZ);
    if (!period) return 0;

    g_lapic_period = period;
    lapic_write(LAPIC_TIMER, (1u << 17) | (u32)IRQ_TIMER_VECTOR); /* periodic */
    lapic_write(LAPIC_TICR, period);
    g_lapic_timer = 1;
    return 1;
}

struct soft_timer {
    u32 id;
    u64 expire_ms;
    u64 period_ms;
    timer_fn_t fn;
    void *arg;
    int active;
};

static volatile u64 g_ticks;
static u64 g_boot_unix;
static struct soft_timer g_timers[MAX_SOFT_TIMERS];
static u32 g_next_timer_id = 1;

static void pit_init(void) {
    u32 divisor = PIT_HZ / HZ;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    outb(0x43, 0x36);                     /* channel 0, lobyte/hibyte, mode 3 */
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)(divisor >> 8));
}

static void timer_irq(void *frame, void *ctx) {
    UNUSED(frame);
    UNUSED(ctx);
    timer_tick();
    sched_tick();
    if (sched_need_resched()) {
        schedule();
    }
}

/* Read the CMOS RTC once to anchor wall-clock time. */
static u64 rtc_read_unix(void) {
    static const int days_before_month[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    outb(0x70, 0x0B);
    u8 reg_b = inb(0x71);
    int bcd = !(reg_b & 0x04);

    /* Two consistent samples: a read that straddles the seconds rollover can
     * mix the old minute with the new second, which shows up as a clock that
     * starts one unit off. The second sample is kept only when the seconds
     * register did not change while the rest was being read. */
    u8 sec = 0, min = 0, hour = 0, day = 0, month = 0, year = 0;
    for (int tries = 0; tries < 4; tries++) {
        for (int guard = 0; guard < 1000; guard++) {
            outb(0x70, 0x0A);
            if (!(inb(0x71) & 0x80)) break; /* UIP clear */
        }
        outb(0x70, 0x00); sec = inb(0x71);
        outb(0x70, 0x02); min = inb(0x71);
        outb(0x70, 0x04); hour = inb(0x71);
        outb(0x70, 0x07); day = inb(0x71);
        outb(0x70, 0x08); month = inb(0x71);
        outb(0x70, 0x09); year = inb(0x71);
        outb(0x70, 0x00);
        if (inb(0x71) == sec) break;
    }

    int h12 = !(reg_b & 0x02);
    int pm = h12 && (hour & 0x80) != 0;
    if (bcd) {
        sec = (u8)((sec & 0x0F) + (sec >> 4) * 10);
        min = (u8)((min & 0x0F) + (min >> 4) * 10);
        hour = (u8)((hour & 0x0F) + ((hour >> 4) & 0x07) * 10);
        day = (u8)((day & 0x0F) + (day >> 4) * 10);
        month = (u8)((month & 0x0F) + (month >> 4) * 10);
        year = (u8)((year & 0x0F) + (year >> 4) * 10);
    }
    hour = (u8)(hour & 0x7F);
    if (h12) {
        if (hour == 12) hour = 0;          /* 12 AM */
        if (pm) hour = (u8)(hour + 12);
    }

    int y = 2000 + year;
    if (year < 70) y = 2000 + year;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    u64 days = 0;
    for (int yy = 1970; yy < y; yy++) {
        days += (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
    }
    days += (u64)days_before_month[(month - 1) % 12];
    if (month > 2 && leap) days++;
    days += (u64)day - 1;
    return days * 86400 + (u64)hour * 3600 + (u64)min * 60 + sec;
}

/* Wall-clock anchor. The RTC is read once at boot, then the tick counter
 * advances the epoch; sys_time used to return the frozen boot value, which is
 * why the taskbar clock never moved. Re-anchoring once a minute also hides the
 * drift of a miscalibrated APIC timer. */
static u64 g_rtc_base_epoch;
static u64 g_rtc_base_ticks;
static u64 g_rtc_last_read;

u64 timer_now_unix(void) {
    u64 now = g_rtc_base_epoch + (g_ticks - g_rtc_base_ticks) / HZ;

    if (g_ticks - g_rtc_last_read >= 60u * HZ) {
        u64 rtc = rtc_read_unix();
        /* Ignore an implausible reading (dead RTC, year 2000, etc.) and keep
         * counting from the previous anchor in that case. */
        u64 diff = rtc > now ? rtc - now : now - rtc;
        if (rtc > 1577836800ULL && diff < 86400ULL) {
            g_rtc_base_epoch = rtc;
            g_rtc_base_ticks = g_ticks;
            now = rtc;
        }
        g_rtc_last_read = g_ticks;
    }
    return now;
}

void timer_init(void) {
    memset(g_timers, 0, sizeof(g_timers));
    g_ticks = 0;
    g_boot_unix = rtc_read_unix();
    g_rtc_base_epoch = g_boot_unix;
    g_rtc_base_ticks = 0;
    g_rtc_last_read = 0;
    pit_init();
    irq_register(32, timer_irq, NULL);
    irq_unmask(0);
    kprintf("[time] PIT %u Hz, RTC epoch %u (CMOS)\n", HZ, (u32)g_boot_unix);
}

u64 timer_ticks(void) { return g_ticks; }
u64 timer_uptime_ms(void) { return g_ticks; }
u64 timer_boot_time_unix(void) { return g_boot_unix; }

u32 timer_add(u64 delay_ms, u64 period_ms, timer_fn_t fn, void *arg) {
    u64 flags = irq_save();
    for (int i = 0; i < MAX_SOFT_TIMERS; i++) {
        if (!g_timers[i].active) {
            g_timers[i].active = 1;
            g_timers[i].id = g_next_timer_id++;
            if (g_next_timer_id == 0) g_next_timer_id = 1;
            g_timers[i].expire_ms = g_ticks + delay_ms;
            g_timers[i].period_ms = period_ms;
            g_timers[i].fn = fn;
            g_timers[i].arg = arg;
            irq_restore(flags);
            return g_timers[i].id;
        }
    }
    irq_restore(flags);
    return 0;
}

void timer_cancel(u32 id) {
    u64 flags = irq_save();
    for (int i = 0; i < MAX_SOFT_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].id == id) {
            g_timers[i].active = 0;
            break;
        }
    }
    irq_restore(flags);
}

void timer_tick(void) {
    g_ticks++;

    for (int i = 0; i < MAX_SOFT_TIMERS; i++) {
        struct soft_timer *t = &g_timers[i];
        if (!t->active || t->expire_ms > g_ticks) continue;
        timer_fn_t fn = t->fn;
        void *arg = t->arg;
        if (t->period_ms) {
            t->expire_ms = g_ticks + t->period_ms;
        } else {
            t->active = 0;
        }
        if (fn) fn(arg);
    }
}

/* Set once a tick source has actually been observed to advance the counter.
 * Until then a hlt would be a one-way trip on firmware that never delivers an
 * interrupt to this CPU, so early boot sleeps poll the PIT channel-2 counter
 * instead and cannot wedge the machine. */
static int g_tick_live;

void timer_sleep_ms(u64 ms) {
    if (!g_tick_live) {
        pit_busy_wait_ms((u32)ms);
        return;
    }

    u64 deadline = g_ticks + ms;
    u64 wakes = 0;
    while (g_ticks < deadline) {
        cpu_hlt();
        /* hlt returns on *any* interrupt; if the tick source stopped while
         * other IRQs still arrive this loop must not spin forever. */
        if (++wakes > ms * 8 + 4096) break;
    }
}

/* Bounded, port-free wait used by the boot tick test.
 *
 * The window used to be measured with PIT channel 2, i.e. with port I/O. On
 * firmware that traps accesses to the 8254 (SMM emulation on several laptop
 * chipsets) one read costs tens of microseconds, so the ~440k port accesses
 * of a 50 ms window became a multi-minute stall: the boot log stopped right
 * after VFS_TEST_OK on a 2024 ThinkPad, before the "[time] PIT IRQ0" line was
 * ever printed. A PAUSE spin has no such dependency, the budget scales with
 * the requested window and the loop also exits as soon as enough ticks have
 * arrived. Returns the ticks observed (0 = the source never fired). */
static u32 spin_ticks(u32 ms) {
    u64 t0 = g_ticks;
    u64 budget = (u64)ms * 400000ull;      /* keeps a dead source bounded */

    while ((u32)(g_ticks - t0) < ms) {
        if (budget == 0) break;
        budget--;
        __asm__ __volatile__("pause" ::: "memory");
    }
    return (u32)(g_ticks - t0);
}

/* Boot-time tick-source acceptance test.
 *
 * The old code halted for 50 ms and trusted the PIT to wake the CPU. On the
 * laptops where firmware leaves the 8259 disconnected (ACPI/UEFI machines that
 * expect the OS to drive the IOAPIC) IRQ0 never arrives, so the very first
 * hlt wedged the boot with the log stopping right before the "uptime after
 * sleeping" line. Measure with a bounded spin instead, and bring up the local
 * APIC timer when no tick shows up. Returns the number of ticks observed. */
u32 timer_selftest(u32 ms) {
    kprintf("[time] probing the tick source (up to %u ms)\n", (u32)ms);

    u32 elapsed = spin_ticks(ms);
    kprintf("[time] PIT IRQ0 %s: %u ticks in %u ms\n",
            elapsed ? "delivered" : "MISSING", (u32)elapsed, (u32)ms);

    /* IRQ0 flapping much faster than the programmed rate means the controller
     * is re-raising the line instead of time passing; the counter would drift
     * wildly, so treat it as broken and fall through to the local APIC. */
    int storm = elapsed > ms * 4u;
    if (storm) {
        kprintf("[time] IRQ0 is storming (%u ticks), masking it\n", (u32)elapsed);
    } else if (elapsed >= ms / 2u) {
        g_tick_live = 1;
        return elapsed;
    }

    /* No usable IRQ0. Mask it, then drive the tick from the local APIC timer,
     * which needs no interrupt-controller routing at all. */
    irq_mask(0);
    if (!lapic_start_timer()) {
        kprintf("[time] no local APIC timer either, tick source is dead\n");
        return elapsed;
    }
    kprintf("[time] switched to the local APIC timer (%u counts/tick)\n",
            (u32)g_lapic_period);

    elapsed = spin_ticks(ms);
    kprintf("[time] APIC timer %s: %u ticks in %u ms\n",
            elapsed ? "delivered" : "MISSING", (u32)elapsed, (u32)ms);
    if (elapsed) g_tick_live = 1;
    return elapsed;
}
