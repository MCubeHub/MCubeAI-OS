#pragma once
/* Atomically implemented primitives; no libatomic dependency. */

#include <mcube/types.h>
#include <mcube/io.h>

typedef struct {
    volatile u32 value;
} atomic_t;

static inline u32 atomic_load(const atomic_t *a) {
    return __atomic_load_n(&a->value, __ATOMIC_ACQUIRE);
}

static inline void atomic_store(atomic_t *a, u32 v) {
    __atomic_store_n(&a->value, v, __ATOMIC_RELEASE);
}

static inline u32 atomic_fetch_add(atomic_t *a, u32 v) {
    return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST);
}

static inline u32 atomic_fetch_sub(atomic_t *a, u32 v) {
    return __atomic_fetch_sub(&a->value, v, __ATOMIC_SEQ_CST);
}

static inline int atomic_cas(atomic_t *a, u32 expected, u32 desired) {
    return __atomic_compare_exchange_n(&a->value, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void cpu_relax(void) {
    __asm__ __volatile__("pause");
}

/* Small SMP-ready spinlock. */
typedef struct {
    volatile u32 locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) cpu_relax();
    }
}

static inline int spin_trylock(spinlock_t *l) {
    return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE);
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

static inline u64 spin_lock_irqsave(spinlock_t *l) {
    u64 flags = irq_save();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, u64 flags) {
    spin_unlock(l);
    irq_restore(flags);
}
