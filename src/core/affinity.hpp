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
#include <cstring>
#include <string>
#include <thread>

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
struct CorePlan {
  unsigned reader_core{0};
  unsigned writer_core{1};
  unsigned first_worker_core{2};
  unsigned worker_count{4};

  [[nodiscard]] unsigned worker_core(unsigned worker_index) const noexcept {
    return first_worker_core + worker_index;
  }
  [[nodiscard]] bool fits_on_host() const noexcept {
    return first_worker_core + worker_count <= hardware_cores();
  }
  [[nodiscard]] std::string describe() const {
    return "reader=" + std::to_string(reader_core) +
           " writer=" + std::to_string(writer_core) + " workers=" +
           std::to_string(first_worker_core) + ".." +
           std::to_string(first_worker_core + worker_count - 1);
  }
};

}  // namespace mru
