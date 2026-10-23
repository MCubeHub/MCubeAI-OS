/* AI-native subsystem kernel hooks.
 *
 * The inference engine, context/memory managers and agent scheduler run in the
 * `aiosd` user-space daemon (M6). The kernel side only owns:
 *   - the reserved ai_* syscall range
 *   - a shared-memory arena descriptor used for token streaming
 *   - audit hooks for tool invocations
 *
 * M1 only reports the reserved ABI so the boot log shows the subsystem is
 * wired in from day one.
 */

#include <mcube/kernel.h>

#define AI_SYSCALL_BASE 0x400u
#define AI_SYSCALL_COUNT 24u

void ai_subsys_init(void) {
    kprintf("[ai] native subsystem hooks registered: syscalls 0x%x..0x%x (daemon: aiosd)\n",
            AI_SYSCALL_BASE, AI_SYSCALL_BASE + AI_SYSCALL_COUNT - 1);
}
