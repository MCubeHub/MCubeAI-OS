/* Per-CPU data and the GS-base setup used by syscall/sysret.
 *
 * Invariant:
 *   in kernel mode: GS.base = &g_percpu, KERNEL_GS_BASE = user GS (0)
 *   in user mode:   GS.base = 0,          KERNEL_GS_BASE = &g_percpu
 *
 * swapgs is executed by syscall_entry, by user_entry_trampoline and by the
 * interrupt stubs when the interrupted CS is a user selector.
 */

#include <mcube/percpu.h>
#include <mcube/io.h>
#include <mcube/kernel.h>

percpu_t g_percpu;

#define MSR_PAT 0x277u

/* Default PAT entry 1 is write-through. Repoint it at write-combining so the
 * framebuffer can be mapped WC instead of UC: its writes then leave the CPU in
 * bursts instead of one bus transaction per pixel. */
static void pat_init(void) {
    u64 pat = read_msr(MSR_PAT);
    pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8);
    write_msr(MSR_PAT, pat);
}

void percpu_init(void) {
    memset(&g_percpu, 0, sizeof(g_percpu));
    g_percpu.cpu_id = 0;

    pat_init();
    write_msr(MSR_GS_BASE, (u64)(uptr)&g_percpu);
    write_msr(MSR_KERNEL_GS_BASE, 0);
    kprintf("[cpu] per-cpu block @ %p, GS base armed\n", (void *)&g_percpu);
}
