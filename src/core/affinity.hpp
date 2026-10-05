// src/core/affinity.hpp
//
// Thread-to-core pinning, naming, and scheduling-class helpers.
//
// Pinning is what turns the shard-per-worker model from an idea into a
// measurement: without it the scheduler migrates workers between cores, every
// migration cold-starts the L1/L2 working set, and your p99.9 is dominated by
// the kernel rather than by your code.
//
// Pinning alone is NOT sufficient for microsecond measurement. The host also
// needs, and the paper must state:
//
//     isolcpus=<cores> nohz_full=<cores> rcu_nocbs=<cores>   (kernel cmdline)
//     cpupower frequency-set -g performance
//     IRQ affinity moved off the isolated cores
//     turbo either pinned or disabled, SMT siblings accounted for
//
// Every function here returns bool and never throws. Failure to pin is not
// fatal -- the daemon should log it and mark the run as UNPINNED so the numbers
// are never silently mixed with pinned ones.
//
// NOTE: on Linux this header needs _GNU_SOURCE for CPU_SET and sched_getcpu.
// That is set by the build (target_compile_definitions), not here, because the
// macro must precede the first libc header in the translation unit.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace mru {

[[nodiscard]] inline unsigned hardware_cores() noexcept {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1u : n;
}

// Spin-wait hint. Cheaper than yielding and avoids hammering the memory
// subsystem while waiting on a ring.
inline void cpu_relax() noexcept {
#if defined(_M_X64) || defined(_M_IX86)
  YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

// ---------------------------------------------------------------------------
// Pinning
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool pin_this_thread_to_core(unsigned core) noexcept {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(core), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
  // Group 0 only: cores >= 64 need SetThreadGroupAffinity. Fine for now;
  // revisit if the benchmark host is a dual-socket machine with >64 threads.
  if (core >= 64) return false;
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << core;
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
  // macOS exposes only affinity *hints* (THREAD_AFFINITY_POLICY) and will not
  // honour a hard pin. Report failure rather than pretending.
  (void)core;
  return false;
#endif
}

// Which core are we actually on? Returns -1 when unknown. Use this to VERIFY a
// pin took effect instead of trusting the setter's return value.
[[nodiscard]] inline int current_core() noexcept {
#if defined(__linux__)
  return sched_getcpu();
#elif defined(_WIN32)
  return static_cast<int>(GetCurrentProcessorNumber());
#else
  return -1;
#endif
}

[[nodiscard]] inline bool verify_pinned_to(unsigned core) noexcept {
  const int c = current_core();
  return c >= 0 && static_cast<unsigned>(c) == core;
}

// ---------------------------------------------------------------------------
// Naming -- makes perf, top and gdb output readable
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool set_thread_name(const char* name) noexcept {
#if defined(__linux__)
  // Linux caps thread names at 15 chars + NUL and fails the call if longer.
  char buf[16];
  std::strncpy(buf, name, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return pthread_setname_np(pthread_self(), buf) == 0;
#elif defined(_WIN32)
  // Resolved dynamically: SetThreadDescription needs Windows 10+ and is not
  // declared by every MinGW header set, but the symbol is always in kernel32.
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  // Double cast through a generic function pointer: GetProcAddress returns
  // FARPROC, and going straight to the real signature trips -Wcast-function-type.
  static const auto fn = reinterpret_cast<SetThreadDescriptionFn>(
      reinterpret_cast<void (*)()>(
          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (fn == nullptr) return false;

  wchar_t wide[64];
  std::size_t i = 0;
  for (; name[i] != '\0' && i < (sizeof(wide) / sizeof(wide[0])) - 1; ++i) {
    wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
  }
  wide[i] = L'\0';
  return SUCCEEDED(fn(GetCurrentThread(), wide));
#elif defined(__APPLE__)
  return pthread_setname_np(name) == 0;
#else
  (void)name;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Scheduling class
// ---------------------------------------------------------------------------

// SCHED_FIFO for the reader/writer threads removes scheduler-induced tail
// latency. Requires CAP_SYS_NICE or an rtprio limit in
// /etc/security/limits.conf -- it will simply fail otherwise, which is why the
// caller must handle false rather than assume success.
//
// Do not run a spin loop at FIFO priority on a non-isolated core: it can starve
// the kernel threads on that CPU. Pin first, isolate first, then raise priority.
[[nodiscard]] inline bool try_set_fifo_priority(int priority = 50) noexcept {
#if defined(__linux__)
  sched_param p{};
  p.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
#elif defined(_WIN32)
  (void)priority;
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
  (void)priority;
  return false;
#endif
}

// One place to describe how the daemon lays threads out, so the config, the
// telemetry and the paper's methods section cannot drift apart.
// Logical CPUs grouped by the physical core they share. Empty if the topology could
// not be read, which the caller must treat as "unknown", never as "no SMT".
//
// WHY THIS IS NOT first_core + i, WHICH IS WHAT THIS FILE USED TO DO.
//
// Linux numbers logical CPUs in whatever order firmware presents them, and the two
// common orders disagree about what "the next core" means:
//
//   siblings adjacent   cpu0+cpu1 are one physical core  (this project's WSL2 VM,
//                                                         and VMs generally)
//   siblings split      cpu0+cpu4 are one physical core  (common on bare-metal
//                                                         Intel desktops/servers)
//
// So NO fixed stride is portable -- not +1, not +n/2. The old contiguous plan put
// the reader on cpu0 and the writer on cpu1, which on this machine is a SINGLE
// physical core: two of the hottest threads in the system sharing execution units,
// L1 and L2, while a fourth physical core sat completely idle. That is both a
// performance bug and a measurement confound, and unlike the WSL2 scheduling noise
// it would have followed us onto bare metal.
[[nodiscard]] inline std::vector<std::vector<unsigned>> cpu_sibling_groups() {
  std::vector<std::vector<unsigned>> groups;
#if defined(__linux__)
  const unsigned n = hardware_cores();
  for (unsigned cpu = 0; cpu < n; ++cpu) {
    char path[160];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return {};  // unknown: say so rather than guess
    // The list is comma- and/or dash-separated, e.g. "0-1" or "0,4". Read the
    // leading id; it is the lowest-numbered sibling and so names the group.
    unsigned first = 0;
    const int got = std::fscanf(f, "%u", &first);
    std::fclose(f);
    if (got != 1) return {};
    if (first == cpu) {
      groups.push_back({cpu});
    } else {
      bool placed = false;
      for (auto& g : groups) {
        if (!g.empty() && g.front() == first) {
          g.push_back(cpu);
          placed = true;
          break;
        }
      }
      if (!placed) return {};  // siblings before their leader: not a layout we model
    }
  }
#endif
  return groups;
}

// CPUs ordered so that consecutive entries are on DIFFERENT physical cores for as
// long as possible: every core's first thread, then every core's second thread, and
// so on. Allocating roles in this order means the first `groups.size()` roles each
// get a whole physical core, and only after that do roles start riding siblings --
// and even then each gets its own logical CPU rather than timesharing one.
[[nodiscard]] inline std::vector<unsigned> cpus_by_core_then_sibling() {
  const auto groups = cpu_sibling_groups();
  std::vector<unsigned> order;
  std::size_t widest = 0;
  for (const auto& g : groups) widest = g.size() > widest ? g.size() : widest;
  for (std::size_t depth = 0; depth < widest; ++depth) {
    for (const auto& g : groups) {
      if (depth < g.size()) order.push_back(g[depth]);
    }
  }
  return order;
}

struct CorePlan {
  unsigned reader_core{0};
  unsigned writer_core{1};
  unsigned first_worker_core{2};
  unsigned worker_count{4};

  // When non-empty this overrides the first_worker_core + i arithmetic, which is
  // necessary because the CPUs belonging to distinct physical cores are not
  // contiguous on every host. See cpu_sibling_groups() above.
  std::vector<unsigned> worker_cores{};

  [[nodiscard]] unsigned worker_core(unsigned worker_index) const noexcept {
    if (worker_index < worker_cores.size()) return worker_cores[worker_index];
    return first_worker_core + worker_index;
  }
  [[nodiscard]] bool fits_on_host() const noexcept {
    return first_worker_core + worker_count <= hardware_cores();
  }
  [[nodiscard]] std::string describe() const {
    std::string d = "reader=" + std::to_string(reader_core) + " writer=" +
                    std::to_string(writer_core) + " workers=";
    if (worker_cores.empty()) {
      d += std::to_string(first_worker_core) + ".." +
           std::to_string(first_worker_core + worker_count - 1);
    } else {
      for (std::size_t i = 0; i < worker_cores.size(); ++i) {
        if (i != 0) d += ",";
        d += std::to_string(worker_cores[i]);
      }
    }
    return d;
  }
};

// Assign reader, writer and workers to distinct physical cores for as long as the
// host has them, then report honestly what had to be given up.
//
// Role order is deliberate: WORKERS FIRST. A worker runs the entire decision path
// -- normalise, quantise, probe, vote, decide -- so it is what the latency figure
// actually measures. The reader and the writer are comparatively cheap. When there
// are not enough physical cores for every role (six data-plane threads on a
// four-core laptop, for instance) the workers keep whole cores to themselves and it
// is the reader and writer that end up on SMT siblings.
//
// shared_siblings is true when any role had to ride a sibling of an already-used
// core. A run with it set must not be compared against one without it, and no tail
// percentile from such a run can be attributed to the daemon rather than to SMT
// contention.
struct CoreAssignment {
  CorePlan plan{};
  bool shared_siblings = false;
  bool topology_known = false;
  std::string note;
};

[[nodiscard]] inline CoreAssignment assign_distinct_cores(unsigned worker_count) {
  CoreAssignment a;
  a.plan.worker_count = worker_count;

  const auto groups = cpu_sibling_groups();
  const auto order = cpus_by_core_then_sibling();
  a.topology_known = !order.empty();

  if (!a.topology_known) {
    // Topology unreadable: keep the old contiguous layout but SAY SO, because on an
    // SMT host it may be stacking the hottest threads onto shared cores.
    const unsigned n = hardware_cores();
    a.plan.reader_core = 0;
    a.plan.writer_core = n > 1 ? 1u : 0u;
    a.plan.first_worker_core = n > 2 ? 2u : 0u;
    a.shared_siblings = true;  // unknown, so assume the worse case
    a.note = "CPU topology unreadable; using contiguous CPUs, which may share SMT "
             "siblings. Tail latency from this run is unattributable.";
    return a;
  }

  const std::size_t cores = groups.size();
  const std::size_t roles = static_cast<std::size_t>(worker_count) + 2;
  std::size_t next = 0;
  const auto take = [&]() -> unsigned {
    const unsigned cpu = order[next % order.size()];
    if (next >= cores) a.shared_siblings = true;
    ++next;
    return cpu;
  };

  a.plan.worker_cores.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) a.plan.worker_cores.push_back(take());
  a.plan.reader_core = take();
  a.plan.writer_core = take();
  a.plan.first_worker_core =
      a.plan.worker_cores.empty() ? 0u : a.plan.worker_cores.front();

  a.note = std::to_string(cores) + " physical cores / " +
           std::to_string(order.size()) + " logical for " + std::to_string(roles) +
           " data-plane threads";
  if (roles > order.size()) {
    a.note += " -- MORE THREADS THAN LOGICAL CPUS, so threads timeshare. Reduce "
              "--shards.";
  } else if (a.shared_siblings) {
    a.note += " -- workers have their own cores; reader/writer ride SMT siblings. "
              "Reduce --shards to " + std::to_string(cores >= 2 ? cores - 2 : 1) +
              " for a layout with no sibling sharing at all.";
  } else {
    a.note += " -- every role on its own physical core.";
  }
  return a;
}

}  // namespace mru
