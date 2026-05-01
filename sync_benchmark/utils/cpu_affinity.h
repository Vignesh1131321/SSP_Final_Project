/**
 * utils/cpu_affinity.h  —  CPU affinity helpers (cross-platform)
 *
 * Linux:   pthread_setaffinity_np  + sched_getcpu
 * Windows: SetThreadAffinityMask   + GetCurrentProcessor (via GetCurrentProcessorNumber)
 * macOS:   stub (macOS does not expose per-thread affinity publicly)
 */
#pragma once
#include <vector>
#include <thread>
#include <iostream>

namespace cpu {

inline int hardware_threads() {
    return static_cast<int>(std::thread::hardware_concurrency());
}

/* ── Pin calling thread to core_id ─────────────────────── */
inline bool pin_to_core(int core_id) {
#if defined(__linux__)
    #include <sched.h>
    #include <pthread.h>
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id % hardware_threads(), &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;

#elif defined(_WIN32)
    #include <windows.h>
    DWORD_PTR mask = (DWORD_PTR)1 << (core_id % hardware_threads());
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;

#else
    (void)core_id;
    return false; // macOS / unknown
#endif
}

/* ── Which core is this thread running on? ─────────────── */
inline int current_core() {
#if defined(__linux__)
    return sched_getcpu();
#elif defined(_WIN32)
    return (int)GetCurrentProcessorNumber();
#else
    return -1;
#endif
}

/* ── Build affinity map for N threads ───────────────────── */
inline std::vector<int> make_affinity_map(int n_threads,
                                          bool avoid_ht = false) {
    int ncores = hardware_threads();
    std::vector<int> map(n_threads);
    if (ncores <= 0) ncores = 1;

    // Only use stride=2 when it still guarantees unique core IDs.
    // Duplicate core pinning can severely distort lock behavior,
    // especially for queue locks such as MCS.
    int stride = 1;
    if (avoid_ht && n_threads <= (ncores / 2)) {
        stride = 2;
    }

    for (int i = 0; i < n_threads; ++i)
        map[i] = (i * stride) % ncores;
    return map;
}

inline void print_affinity_map(const std::vector<int>& m) {
    std::cout << "  CPU affinity: [";
    for (size_t i = 0; i < m.size(); ++i) {
        std::cout << m[i];
        if (i + 1 < m.size()) std::cout << ',';
    }
    std::cout << "]\n";
}

} // namespace cpu
