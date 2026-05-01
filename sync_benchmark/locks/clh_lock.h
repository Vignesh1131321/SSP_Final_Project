/**
 * locks/clh_lock.h  —  Craig / Landin / Hagersten (CLH) Queue Lock
 *
 * CLH is a list-based queue lock where each thread spins on its
 * *predecessor's* node (the implicit linked list traverses backwards).
 *
 * Properties vs MCS:
 *   - Simpler unlock (no pointer-chasing forward)
 *   - Still O(1) coherence traffic per thread
 *   - FIFO fairness
 *   - Requires thread-local storage for the "my_pred" pointer
 *
 * Reference: Travis Craig, U. Washington TR 93-02-02, 1993.
 */
#pragma once
#include <atomic>
#include "spinlock.h"  // pulls in CPU_PAUSE

struct alignas(64) CLHNode {
    std::atomic<bool> locked{true}; // true = I am waiting
    char _pad[64 - sizeof(std::atomic<bool>)];
};

class CLHLock {
    std::atomic<CLHNode*> tail_;
    CLHNode               dummy_;   // sentinel initial node

public:
    CLHLock() : tail_(&dummy_) {
        dummy_.locked.store(false, std::memory_order_relaxed);
    }

    /**
     * lock(me, pred_out)
     * After calling, *pred_out is the predecessor node that will
     * be released by the caller on unlock.
     */
    void lock(CLHNode* me, CLHNode** pred_out) noexcept {
        me->locked.store(true, std::memory_order_relaxed);
        CLHNode* pred = tail_.exchange(me, std::memory_order_acq_rel);
        *pred_out = pred;
        // Spin on predecessor's locked flag
        while (pred->locked.load(std::memory_order_acquire))
            CPU_PAUSE();
    }

    /**
     * unlock(me, pred)
     * After unlock the caller can recycle `pred` as its next `me`.
     */
    void unlock(CLHNode* me) noexcept {
        me->locked.store(false, std::memory_order_release);
    }
};
