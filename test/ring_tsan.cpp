// test/ring_tsan.cpp
//
// Stress test for the core data plane. Designed to be run three ways:
//
//   1. plain RelWithDebInfo  -> throughput sanity, correctness at speed
//   2. -DMRU_TSAN=ON         -> race detection (the important one)
//   3. -DMRU_ASAN=ON         -> bounds / lifetime errors in the arena protocol
//
// Deliberately framework-free so it builds and runs before vcpkg installs
// anything. All assertions execute on the main thread AFTER joins; worker
// threads only touch atomic counters, so the test itself does not manufacture
// the races it is supposed to detect.
//
// Usage: mru_ring_stress [ops] [shards] [arena_iters]

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "core/affinity.hpp"
#include "core/arena_pool.hpp"
#include "core/spsc_ring.hpp"
#include "core/tsc.hpp"

#if defined(__SANITIZE_THREAD__)
#define MRU_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MRU_TSAN_BUILD 1
#endif
#endif
#ifndef MRU_TSAN_BUILD
#define MRU_TSAN_BUILD 0
#endif

namespace {

int g_failures = 0;

void fail(const char* file, int line, const char* what, const char* expr) {
  std::printf("  FAIL  %s:%d  %s  [%s]\n", file, line, what, expr);
  ++g_failures;
}

#define CHECK(cond, what)                                 \
  do {                                                    \
    if (!(cond)) fail(__FILE__, __LINE__, (what), #cond); \
  } while (0)

#define CHECK_EQ(a, b, what)                                                      \
  do {                                                                            \
    const std::uint64_t lhs_ = static_cast<std::uint64_t>(a);                      \
    const std::uint64_t rhs_ = static_cast<std::uint64_t>(b);                      \
    if (lhs_ != rhs_) {                                                            \
      std::printf("  FAIL  %s:%d  %s  (%llu != %llu)\n", __FILE__, __LINE__,       \
                  (what), static_cast<unsigned long long>(lhs_),                   \
                  static_cast<unsigned long long>(rhs_));                          \
      ++g_failures;                                                                \
    }                                                                              \
  } while (0)

void banner(const char* name) { std::printf("[ RUN ] %s\n", name); }

// ---------------------------------------------------------------------------
// 1. SPSC: nothing lost, nothing duplicated, nothing reordered
// ---------------------------------------------------------------------------

struct Item {
  std::uint64_t seq;
  std::uint32_t shard;
  std::uint32_t pad;
};

constexpr std::size_t kRingCap = 1024;
using ItemRing = mru::SpscRing<Item, kRingCap>;

void test_spsc_sequence(std::uint64_t ops) {
  banner("spsc_sequence");
  auto ring = std::make_unique<ItemRing>();
  std::atomic<std::uint64_t> out_of_order{0};
  std::atomic<std::uint64_t> consumed{0};

  const std::uint64_t t0 = mru::rdtscp();
  {
    std::jthread producer([&] {
      (void)mru::set_thread_name("mru-prod");
      for (std::uint64_t i = 0; i < ops; ++i) {
        const Item it{i, 0, 0};
        while (!ring->try_push(it)) mru::cpu_relax();
      }
    });
    std::jthread consumer([&] {
      (void)mru::set_thread_name("mru-cons");
      std::uint64_t expect = 0;
      Item it{};
      while (expect < ops) {
        if (ring->try_pop(it)) {
          if (it.seq != expect) out_of_order.fetch_add(1, std::memory_order_relaxed);
          ++expect;
        } else {
          mru::cpu_relax();
        }
      }
      consumed.store(expect, std::memory_order_relaxed);
    });
  }
  const std::uint64_t cycles = mru::rdtscp() - t0;

  CHECK_EQ(out_of_order.load(), 0u, "sequence violations in SPSC ring");
  CHECK_EQ(consumed.load(), ops, "items consumed");
  CHECK_EQ(ring->pushed_total(), ops, "producer counter");
  CHECK_EQ(ring->popped_total(), ops, "consumer counter");
  CHECK(ring->empty_approx(), "ring drained at end");

  const double ns = mru::TscClock::instance().to_ns(cycles);
  // ops / microseconds == millions of ops per second
  std::printf("        %llu ops in %.2f ms -> %.1f M ops/s, %.1f ns/op\n",
              static_cast<unsigned long long>(ops), ns / 1e6,
              static_cast<double>(ops) / (ns / 1e3), ns / static_cast<double>(ops));
}

// ---------------------------------------------------------------------------
// 2. Sharded fan-out: the actual data-plane topology
//    one producer -> N rings chosen by (key & mask) -> N pinned consumers
// ---------------------------------------------------------------------------

void test_sharded_fanout(std::uint64_t ops, unsigned shards) {
  banner("sharded_fanout");
  const std::uint64_t mask = shards - 1;

  std::vector<std::unique_ptr<ItemRing>> rings;
  rings.reserve(shards);
  for (unsigned s = 0; s < shards; ++s) rings.emplace_back(std::make_unique<ItemRing>());

  std::vector<std::atomic<std::uint64_t>> bad(shards);
  std::vector<std::atomic<std::uint64_t>> got(shards);
  for (unsigned s = 0; s < shards; ++s) {
    bad[s].store(0);
    got[s].store(0);
  }

  // Each shard receives the subsequence seq = s, s+shards, s+2*shards, ...
  const std::uint64_t per_shard = ops / shards;

  {
    std::vector<std::jthread> consumers;
    consumers.reserve(shards);
    for (unsigned s = 0; s < shards; ++s) {
      consumers.emplace_back([&, s] {
        (void)mru::pin_this_thread_to_core(s % mru::hardware_cores());
        std::uint64_t expect = s;
        std::uint64_t n = 0;
        Item it{};
        while (n < per_shard) {
          if (rings[s]->try_pop(it)) {
            if (it.seq != expect || it.shard != s) {
              bad[s].fetch_add(1, std::memory_order_relaxed);
            }
            expect += shards;
            ++n;
          } else {
            mru::cpu_relax();
          }
        }
        got[s].store(n, std::memory_order_relaxed);
      });
    }

    std::jthread producer([&] {
      (void)mru::set_thread_name("mru-reader");
      for (std::uint64_t i = 0; i < per_shard * shards; ++i) {
        const auto s = static_cast<std::uint32_t>(i & mask);
        const Item it{i, s, 0};
        while (!rings[s]->try_push(it)) mru::cpu_relax();
      }
    });
  }

  for (unsigned s = 0; s < shards; ++s) {
    CHECK_EQ(bad[s].load(), 0u, "per-shard ordering violation");
    CHECK_EQ(got[s].load(), per_shard, "per-shard delivery count");
  }
  std::printf("        %u shards x %llu items, all in order\n", shards,
              static_cast<unsigned long long>(per_shard));
}

// ---------------------------------------------------------------------------
// 3. Drop mode: the real backpressure policy. Producer never retries, so we
//    assert conservation (pushed + dropped == offered) and monotonicity rather
//    than contiguity.
// ---------------------------------------------------------------------------

void test_drop_conservation(std::uint64_t ops) {
  banner("drop_conservation");
  auto ring = std::make_unique<ItemRing>();
  std::atomic<std::uint64_t> pushed{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> popped{0};
  std::atomic<std::uint64_t> regressions{0};
  std::atomic<bool> done{false};

  {
    std::jthread consumer([&] {
      std::uint64_t last = 0;
      bool first = true;
      std::uint64_t n = 0;
      Item it{};
      for (;;) {
        if (ring->try_pop(it)) {
          if (!first && it.seq <= last) regressions.fetch_add(1, std::memory_order_relaxed);
          last = it.seq;
          first = false;
          ++n;
          continue;
        }
        if (done.load(std::memory_order_acquire) && ring->size_approx() == 0) break;
        mru::cpu_relax();
      }
      popped.store(n, std::memory_order_relaxed);
    });

    std::jthread producer([&] {
      std::uint64_t ok = 0, drop = 0;
      for (std::uint64_t i = 0; i < ops; ++i) {
        const Item it{i, 0, 0};
        if (ring->try_push(it)) {
          ++ok;
        } else {
          ++drop;  // this is what the daemon does: drop the stale chunk, count it
        }
      }
      pushed.store(ok, std::memory_order_relaxed);
      dropped.store(drop, std::memory_order_relaxed);
      done.store(true, std::memory_order_release);
    });
  }

  CHECK_EQ(pushed.load() + dropped.load(), ops, "offered == pushed + dropped");
  CHECK_EQ(popped.load(), pushed.load(), "every pushed item was consumed");
  CHECK_EQ(regressions.load(), 0u, "monotonicity under drop");
  std::printf("        offered %llu, pushed %llu, dropped %llu (%.2f%%)\n",
              static_cast<unsigned long long>(ops),
              static_cast<unsigned long long>(pushed.load()),
              static_cast<unsigned long long>(dropped.load()),
              100.0 * static_cast<double>(dropped.load()) / static_cast<double>(ops));
}

// ---------------------------------------------------------------------------
// 4. Arena pool: refcount protocol, no premature reset, no slot leak
//
//    This is the test that matters most, because the failure it catches is a
//    use-after-free that only shows up under load. The reader writes a payload
//    into arena memory and hands out spans; if a worker could drive the
//    refcount to zero while the reader is still dispatching, the arena would be
//    reset and the magic value clobbered. Deliberately starved of slots so the
//    handoff runs hot.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kMagic = 0xA5C3F10Du;
constexpr std::size_t kPoolSlots = 8;
constexpr std::size_t kArenaBytes = 4096;

struct Payload {
  std::uint64_t seq;
  std::uint32_t shard;
  std::uint32_t magic;
};

using Pool = mru::BumpArenaPool;
using Slot = Pool::Slot;

struct Ref {
  const Payload* payload;
  Slot* owner;
  std::uint64_t seq;
  std::uint32_t shard;
  std::uint32_t pad;
};

using RefRing = mru::SpscRing<Ref, 256>;

void test_arena_pool_refcount(std::uint64_t iters, unsigned shards) {
  banner("arena_pool_refcount");
  Pool pool(kPoolSlots, kArenaBytes);

  std::vector<std::unique_ptr<RefRing>> rings;
  rings.reserve(shards);
  for (unsigned s = 0; s < shards; ++s) rings.emplace_back(std::make_unique<RefRing>());

  std::vector<std::atomic<std::uint64_t>> corrupt(shards);
  std::vector<std::atomic<std::uint64_t>> seen(shards);
  for (unsigned s = 0; s < shards; ++s) {
    corrupt[s].store(0);
    seen[s].store(0);
  }
  std::atomic<std::uint64_t> alloc_failures{0};
  std::atomic<std::uint64_t> acquire_stalls{0};

  {
    std::vector<std::jthread> workers;
    workers.reserve(shards);
    for (unsigned s = 0; s < shards; ++s) {
      workers.emplace_back([&, s] {
        std::uint64_t expect = 0;
        Ref r{};
        while (expect < iters) {
          if (!rings[s]->try_pop(r)) {
            mru::cpu_relax();
            continue;
          }
          // Read through the span while holding our reference -- exactly what a
          // decision worker does with the raw signal.
          const Payload p = *r.payload;
          if (p.magic != kMagic || p.seq != expect || p.shard != s) {
            corrupt[s].fetch_add(1, std::memory_order_relaxed);
          }
          r.owner->release();  // done with the arena
          ++expect;
        }
        seen[s].store(expect, std::memory_order_relaxed);
      });
    }

    std::jthread reader([&] {
      (void)mru::set_thread_name("mru-reader");
      for (std::uint64_t i = 0; i < iters; ++i) {
        Slot* slot = nullptr;
        while ((slot = pool.acquire()) == nullptr) {
          acquire_stalls.fetch_add(1, std::memory_order_relaxed);
          mru::cpu_relax();
        }
        // refcount is 1 here and it belongs to US until dispatch completes.
        for (unsigned s = 0; s < shards; ++s) {
          auto* pl = static_cast<Payload*>(
              slot->arena().Allocate(sizeof(Payload), alignof(Payload)));
          if (pl == nullptr) {
            alloc_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          pl->seq = i;
          pl->shard = s;
          pl->magic = kMagic;

          slot->retain();
          const Ref r{pl, slot, i, s, 0};
          while (!rings[s]->try_push(r)) mru::cpu_relax();
        }
        slot->release();  // drop the reader's own reference
      }
    });
  }

  for (unsigned s = 0; s < shards; ++s) {
    CHECK_EQ(corrupt[s].load(), 0u, "arena payload corrupted (premature Reset?)");
    CHECK_EQ(seen[s].load(), iters, "worker delivery count");
  }
  CHECK_EQ(alloc_failures.load(), 0u, "arena ran out of space");
  CHECK_EQ(pool.recycled(), iters, "one full recycle per acquisition");
  CHECK_EQ(pool.free_count_quiesced(), kPoolSlots, "slot leak: free list short");

  std::uint64_t resets = 0;
  for (std::size_t i = 0; i < pool.slot_count(); ++i) {
    resets += pool.slot_at(i)->arena().resets();
    CHECK_EQ(pool.slot_at(i)->refs(), 0u, "slot left with live references");
  }
  CHECK_EQ(resets, iters, "arena Reset() called exactly once per acquisition");

  std::printf("        %llu acquisitions over %zu slots, %llu acquire stalls, "
              "%llu pool exhaustions\n",
              static_cast<unsigned long long>(iters), kPoolSlots,
              static_cast<unsigned long long>(acquire_stalls.load()),
              static_cast<unsigned long long>(pool.exhausted()));
}

// ---------------------------------------------------------------------------
// 5. Timing and pinning smoke test
// ---------------------------------------------------------------------------

void test_tsc_and_affinity() {
  banner("tsc_and_affinity");
  const auto& clk = mru::TscClock::instance();

  std::printf("        TSC %.1f MHz, invariant=%s, calibrated=%s\n", clk.mhz(),
              clk.invariant() ? "yes" : "NO", clk.calibrated() ? "yes" : "no");
  CHECK(clk.calibrated(), "TSC calibration produced no usable sample");
  if (!clk.invariant()) {
    std::printf("        WARNING: TSC is not invariant on this host. Functional "
                "testing is fine; do NOT publish latency numbers from here.\n");
  }

  // A 50 ms sleep must measure as 50 ms, give or take scheduler overshoot.
  const std::uint64_t t0 = mru::rdtscp_serialized();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const double ms = clk.to_ns(mru::rdtscp_serialized() - t0) / 1e6;
  std::printf("        measured 50 ms sleep as %.2f ms\n", ms);
  CHECK(ms > 40.0 && ms < 150.0, "calibration is wildly off");

  mru::CycleHistogram h;
  for (std::uint64_t v = 1; v <= 1000; ++v) h.record(v);
  CHECK_EQ(h.count(), 1000u, "histogram count");
  CHECK(h.min() == 1 && h.max() == 1000, "histogram min/max");
  CHECK(h.percentile(0.50) >= 500 && h.percentile(0.50) <= 1023,
        "p50 within one power-of-two bucket");
  CHECK(h.percentile(0.999) >= 1000, "p99.9 not below max");

  // Pinning is informational: CI containers and restricted cpusets legitimately
  // refuse it, and that must not fail the build.
  const bool pinned = mru::pin_this_thread_to_core(0);
  std::printf("        pin_to_core(0)=%s current_core=%d  plan: %s\n",
              pinned ? "ok" : "refused", mru::current_core(),
              mru::CorePlan{}.describe().c_str());
  if (pinned && !mru::verify_pinned_to(0)) {
    std::printf("        WARNING: pin reported success but we are on core %d\n",
                mru::current_core());
  }
}

std::uint64_t arg_or(int argc, char** argv, int idx, std::uint64_t fallback) {
  if (argc <= idx) return fallback;
  const long long v = std::atoll(argv[idx]);
  return v > 0 ? static_cast<std::uint64_t>(v) : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  // TSan instruments every memory access, so a full-speed op count would take
  // minutes. Keep the default brisk and raise it explicitly for a soak run.
  const std::uint64_t default_ops = MRU_TSAN_BUILD ? 2'000'000ull : 20'000'000ull;
  const std::uint64_t default_iters = MRU_TSAN_BUILD ? 50'000ull : 500'000ull;

  const std::uint64_t ops = arg_or(argc, argv, 1, default_ops);
  auto shards = static_cast<unsigned>(arg_or(argc, argv, 2, 4));
  const std::uint64_t iters = arg_or(argc, argv, 3, default_iters);

  if ((shards & (shards - 1)) != 0 || shards == 0) {
    std::printf("shards must be a power of two\n");
    return 2;
  }

  std::printf("microsecond-readuntil core stress test\n");
  std::printf("  build: %s, cores: %u, ops: %llu, shards: %u, arena iters: %llu\n\n",
              MRU_TSAN_BUILD ? "ThreadSanitizer" : "plain", mru::hardware_cores(),
              static_cast<unsigned long long>(ops), shards,
              static_cast<unsigned long long>(iters));

  test_tsc_and_affinity();
  test_spsc_sequence(ops);
  test_sharded_fanout(ops, shards);
  test_drop_conservation(ops);
  test_arena_pool_refcount(iters, shards);

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
