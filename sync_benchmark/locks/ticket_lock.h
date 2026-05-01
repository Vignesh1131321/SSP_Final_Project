/**
 * locks/ticket_lock.h  —  Ticket (Bakery) Lock
 *
 * Guarantees strict FIFO ordering.
 * Each thread draws a ticket; the thread whose ticket equals
 * `now_serving` enters the critical section.
 *
 * Properties:
 *   - Strong fairness (FIFO)
 *   - O(1) space, O(P) coherence traffic per unlock
 *   - Scales poorly under NUMA due to shared `now_serving`
 */
#pragma once
#include <atomic>
#include "spinlock.h"  // pulls in CPU_PAUSE

class TicketLock {
    alignas(64) std::atomic<uint64_t> next_ticket_{0};
    alignas(64) std::atomic<uint64_t> now_serving_{0};
public:
    void lock() noexcept {
        uint64_t my_ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);
        while (now_serving_.load(std::memory_order_acquire) != my_ticket)
            CPU_PAUSE();
    }
    void unlock() noexcept {
        now_serving_.fetch_add(1, std::memory_order_release);
    }

    /** Return the current queue depth (for monitoring). */
    uint64_t queue_depth() const noexcept {
        uint64_t issued  = next_ticket_.load(std::memory_order_relaxed);
        uint64_t serving = now_serving_.load(std::memory_order_relaxed);
        return (issued > serving) ? (issued - serving) : 0;
    }
};
