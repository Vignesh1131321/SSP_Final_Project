/**
 * locks/mcs_lock.h  —  Mellor-Crummey & Scott (MCS) Queue Lock
 *
 * Each thread spins on its OWN node, so:
 *   - Cache-coherence traffic is O(1) per thread on unlock
 *   - No thundering-herd on release
 *   - FIFO fairness
 *   - Requires O(P) space (one node per thread)
 *
 * Reference: Mellor-Crummey & Scott, TOCS 1991.
 */
#pragma once
#include <atomic>
#include <cstddef>
#include <thread>
#include "spinlock.h"  // pulls in CPU_PAUSE

struct alignas(64) MCSNode {
    std::atomic<MCSNode*> next{nullptr};
    std::atomic<bool>     locked{false};
    char _pad[64 - sizeof(std::atomic<MCSNode*>) - sizeof(std::atomic<bool>)];
};

class MCSLock {
    std::atomic<MCSNode*> tail_{nullptr};

public:
    void lock(MCSNode* me) noexcept {
        me->next.store(nullptr, std::memory_order_relaxed);
        MCSNode* prev = tail_.exchange(me, std::memory_order_acq_rel);
        if (prev != nullptr) {
            // Mark ourselves waiting before publishing to predecessor.
            me->locked.store(true, std::memory_order_seq_cst);
            // Enqueue behind predecessor
            prev->next.store(me, std::memory_order_release);
            // Spin on OWN node
            uint32_t spins = 0;
            while (me->locked.load(std::memory_order_acquire)) {
                CPU_PAUSE();
                if ((++spins & 0x3FFu) == 0) std::this_thread::yield();
            }
        }
    }

    void unlock(MCSNode* me) noexcept {
        MCSNode* succ = me->next.load(std::memory_order_acquire);
        if (succ == nullptr) {
            // Try to make tail null (no successor)
            MCSNode* expected = me;
            if (tail_.compare_exchange_strong(expected, nullptr,
                    std::memory_order_release, std::memory_order_relaxed))
                return;
            // A new thread is enqueueing; wait for it to set next
            uint32_t spins = 0;
            while ((succ = me->next.load(std::memory_order_acquire)) == nullptr) {
                CPU_PAUSE();
                if ((++spins & 0x3FFu) == 0) std::this_thread::yield();
            }
        }
        succ->locked.store(false, std::memory_order_release);
        me->next.store(nullptr, std::memory_order_relaxed);
    }
};
