/**
 * platform/platform_info.h
 * ─────────────────────────
 * RQ9 — How do synchronisation mechanisms behave across different OSes?
 *
 * Detects and reports:
 *   • OS name and version
 *   • Scheduler type (CFS / EEVDF on Linux; QoS on macOS; NT on Windows)
 *   • Kernel synchronisation backend (futex / mach_semaphore / SRWL)
 *   • CPU topology (logical cores, physical cores estimate, NUMA nodes)
 *   • Cache line size
 *   • Timer resolution
 *
 * Also provides platform-appropriate wrappers used by the harness.
 */
#pragma once
#include <string>
#include <vector>
#include <thread>
#include <cstdio>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#  include <sysinfoapi.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <sys/utsname.h>
#  include <sys/sysinfo.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <mach/mach.h>
#endif

namespace platform {

struct SystemInfo {
    std::string os_name;
    std::string os_version;
    std::string kernel_sync_backend;  // futex / mach_semaphore / SRWLOCK
    std::string scheduler;
    int         logical_cpus    = 0;
    int         physical_cores  = 0;  // estimate
    int         numa_nodes      = 1;
    uint64_t    cache_line_bytes = 64;
    uint64_t    timer_resolution_ns = 0;
    uint64_t    total_ram_mb    = 0;
};

inline SystemInfo query() {
    SystemInfo si;
    si.logical_cpus = static_cast<int>(std::thread::hardware_concurrency());

#if defined(_WIN32)
    si.os_name  = "Windows";
    si.scheduler = "NT Dispatcher (priority-based preemptive)";
    si.kernel_sync_backend = "SRWLock / CRITICAL_SECTION / WaitOnAddress";

    OSVERSIONINFOEXW ovi{};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    // RtlGetVersion is more reliable than GetVersionEx
    typedef NTSTATUS(WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
    auto fn = (RtlGetVersionFn)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn && fn(&ovi) == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                 ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber);
        si.os_version = buf;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        si.total_ram_mb = ms.ullTotalPhys / (1024*1024);

    // Cache line: use GetLogicalProcessorInformation
    DWORD sz = 0;
    GetLogicalProcessorInformation(nullptr, &sz);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf2(sz / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (GetLogicalProcessorInformation(buf2.data(), &sz)) {
        for (auto& e : buf2) {
            if (e.Relationship == RelationCache && e.Cache.Level == 1)
                si.cache_line_bytes = e.Cache.LineSize;
        }
    }
    si.physical_cores = si.logical_cpus / 2;  // heuristic

#elif defined(__linux__)
    si.os_name = "Linux";
    si.scheduler = "CFS / EEVDF (fair-share preemptive)";
    si.kernel_sync_backend = "futex (fast userspace mutex)";

    struct utsname u{};
    if (uname(&u) == 0) si.os_version = u.release;

    struct sysinfo info{};
    if (sysinfo(&info) == 0)
        si.total_ram_mb = (uint64_t)info.totalram * info.mem_unit / (1024*1024);

    // Physical cores from /sys
    FILE* f = popen("grep -c ^processor /proc/cpuinfo 2>/dev/null", "r");
    if (f) { int n = 0; fscanf(f, "%d", &n); si.physical_cores = n; pclose(f); }
    if (si.physical_cores == 0) si.physical_cores = si.logical_cpus / 2;

    // NUMA nodes
    FILE* fn = popen("ls /sys/devices/system/node/node* 2>/dev/null | wc -l", "r");
    if (fn) { int nn = 0; fscanf(fn, "%d", &nn); if (nn > 0) si.numa_nodes = nn; pclose(fn); }

    // Cache line
    FILE* fc = fopen("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", "r");
    if (fc) { int cl = 0; fscanf(fc, "%d", &cl); if (cl > 0) si.cache_line_bytes = cl; fclose(fc); }

    // Timer resolution
    struct timespec ts{};
    clock_getres(CLOCK_MONOTONIC_RAW, &ts);
    si.timer_resolution_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;

#elif defined(__APPLE__)
    si.os_name = "macOS";
    si.scheduler = "XNU Grand Central Dispatch + mach thread scheduler";
    si.kernel_sync_backend = "mach_semaphore / os_unfair_lock / libpthread";

    char ver[64] = {};
    size_t vlen = sizeof(ver);
    sysctlbyname("kern.osrelease", ver, &vlen, nullptr, 0);
    si.os_version = ver;

    int nc = 0; size_t ncl = sizeof(nc);
    sysctlbyname("hw.physicalcpu", &nc, &ncl, nullptr, 0);
    si.physical_cores = nc;

    int64_t mem = 0; size_t ml = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &ml, nullptr, 0);
    si.total_ram_mb = static_cast<uint64_t>(mem) / (1024*1024);

    int cl = 64; size_t cll = sizeof(cl);
    sysctlbyname("hw.cachelinesize", &cl, &cll, nullptr, 0);
    si.cache_line_bytes = cl;
    si.numa_nodes = 1;  // macOS abstracts NUMA
#else
    si.os_name = "Unknown";
    si.physical_cores = si.logical_cpus / 2;
#endif

    return si;
}

inline void print(const SystemInfo& si) {
    printf("── Platform Information ───────────────────────────────────────\n");
    printf("  OS                  : %s %s\n",
           si.os_name.c_str(), si.os_version.c_str());
    printf("  Kernel sync backend : %s\n", si.kernel_sync_backend.c_str());
    printf("  Scheduler           : %s\n", si.scheduler.c_str());
    printf("  Logical CPUs        : %d\n", si.logical_cpus);
    printf("  Physical cores (est): %d\n", si.physical_cores);
    printf("  NUMA nodes          : %d\n", si.numa_nodes);
    printf("  Cache line          : %llu bytes\n",
           (unsigned long long)si.cache_line_bytes);
    printf("  RAM                 : %llu MB\n",
           (unsigned long long)si.total_ram_mb);
    if (si.timer_resolution_ns > 0)
        printf("  Timer resolution    : %llu ns\n",
               (unsigned long long)si.timer_resolution_ns);
    printf("───────────────────────────────────────────────────────────────\n\n");
}

/* Write platform info row to CSV for cross-OS comparison */
inline void write_platform_row(const SystemInfo& si, const std::string& csv_path) {
    FILE* f = fopen(csv_path.c_str(), "a");
    if (!f) return;
    fprintf(f, "platform_info,system,os=%s,version=%s,"
               "logical_cpus=%d,physical_cores=%d,"
               "numa_nodes=%d,cache_line=%llu,"
               "scheduler=%s,sync_backend=%s\n",
            si.os_name.c_str(), si.os_version.c_str(),
            si.logical_cpus, si.physical_cores,
            si.numa_nodes,
            (unsigned long long)si.cache_line_bytes,
            si.scheduler.c_str(),
            si.kernel_sync_backend.c_str());
    fclose(f);
}

} // namespace platform
