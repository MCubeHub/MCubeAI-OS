/* PIT-based tick source and soft timer queue.
 *
 * M3 keeps the 8254 PIT at 1000 Hz because it works on every x86 machine and
 * in QEMU without ACPI parsing. The APIC timer replaces it once the MADT is
 * parsed; timer_ticks()/timer_add() stay the same.
 */

#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/timer.h>
#include <mcube/sched.h>

#define PIT_HZ 1193182u
#define MAX_SOFT_TIMERS 128

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

    for (int guard = 0; guard < 100; guard++) {
        outb(0x70, 0x0A);
        if (!(inb(0x71) & 0x80)) break; /* UIP clear */
    }

    u8 sec, min, hour, day, month, year;
    outb(0x70, 0x00); sec = inb(0x71);
    outb(0x70, 0x02); min = inb(0x71);
    outb(0x70, 0x04); hour = inb(0x71);
    outb(0x70, 0x07); day = inb(0x71);
    outb(0x70, 0x08); month = inb(0x71);
    outb(0x70, 0x09); year = inb(0x71);

    if (bcd) {
        sec = (u8)((sec & 0x0F) + (sec >> 4) * 10);
        min = (u8)((min & 0x0F) + (min >> 4) * 10);
        hour = (u8)((hour & 0x0F) + ((hour >> 4) & 0x07) * 10);
        day = (u8)((day & 0x0F) + (day >> 4) * 10);
        month = (u8)((month & 0x0F) + (month >> 4) * 10);
        year = (u8)((year & 0x0F) + (year >> 4) * 10);
    }
    if (!(reg_b & 0x02) && hour & 0x80) hour = (u8)((hour & 0x7F) + 12);
    hour = (u8)(hour & 0x7F);

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

void timer_init(void) {
    memset(g_timers, 0, sizeof(g_timers));
    g_ticks = 0;
    g_boot_unix = rtc_read_unix();
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

void timer_sleep_ms(u64 ms) {
    u64 deadline = g_ticks + ms;
    while (g_ticks < deadline) {
        cpu_hlt();
    }
}
