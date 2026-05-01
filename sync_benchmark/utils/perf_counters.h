/**
 * utils/perf_counters.h  —  Hardware performance counter access
 *
 * On Linux, uses perf_event_open() to read:
 *   - CPU migrations
 *   - Context switches
 *   - Cache misses (L1 data)
 *   - Instructions retired
 *   - CPU cycles
 *
 * Falls back to /proc/self/status on non-perf platforms.
 */
#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <iostream>

#ifdef __linux__
#  include <unistd.h>
#  include <sys/syscall.h>
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <cstring>
#  include <cerrno>
#  include <fcntl.h>
#endif

namespace perf {

struct Counters {
    long cpu_migrations  = -1;
    long ctx_switches    = -1;
    long cache_misses    = -1;   // L1d misses
    long instructions    = -1;
    long cycles          = -1;
    long branch_misses   = -1;
};

#ifdef __linux__
inline long perf_event_open(struct perf_event_attr* hw_event,
                             pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

class PerfGroup {
    struct Event { long fd = -1; std::string name; };
    std::array<Event, 6> events_;

    long open_event(uint32_t type, uint64_t config) {
        struct perf_event_attr pe{};
        pe.type           = type;
        pe.size           = sizeof(pe);
        pe.config         = config;
        pe.disabled       = 1;
        pe.exclude_kernel = 0;
        pe.exclude_hv     = 1;
        pe.inherit        = 1;
        return perf_event_open(&pe, 0, -1, -1, 0);
    }

public:
    bool available = false;

    PerfGroup() {
        long fd = open_event(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS);
        available = (fd >= 0);
        if (fd >= 0) close(fd); // just probe
    }

    Counters read_delta(std::function<void()> fn) {
        Counters c;
        if (!available) return c;

        long fds[6];
        fds[0] = open_event(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS);
        fds[1] = open_event(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES);
        fds[2] = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
        fds[3] = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
        fds[4] = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
        fds[5] = open_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);

        for (int i = 0; i < 6; ++i)
            if (fds[i] >= 0) { ioctl(fds[i], PERF_EVENT_IOC_RESET, 0);
                               ioctl(fds[i], PERF_EVENT_IOC_ENABLE, 0); }

        fn();

        for (int i = 0; i < 6; ++i)
            if (fds[i] >= 0) ioctl(fds[i], PERF_EVENT_IOC_DISABLE, 0);

        auto rd = [&](int i, long& out) {
            if (fds[i] < 0) return;
            long val = 0;
            if (::read(fds[i], &val, sizeof(val)) == sizeof(val)) out = val;
            close(fds[i]);
        };
        rd(0, c.cpu_migrations);
        rd(1, c.ctx_switches);
        rd(2, c.cache_misses);
        rd(3, c.instructions);
        rd(4, c.cycles);
        rd(5, c.branch_misses);
        return c;
    }
};
#else
class PerfGroup {
public:
    bool available = false;
    Counters read_delta(std::function<void()> fn) { fn(); return {}; }
};
#endif

/* ── Read voluntary context switches from /proc/self/status ─ */
inline long read_voluntary_ctxsw() {
#ifdef __linux__
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "voluntary_ctxt_switches: %ld", &val) == 1) break;
    }
    fclose(f);
    return val;
#else
    return -1;
#endif
}

} // namespace perf
