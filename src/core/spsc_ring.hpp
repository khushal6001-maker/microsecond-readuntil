// src/core/spsc_ring.hpp
//
// Bounded single-producer / single-consumer queue for the read-until data plane.
//
// One instance per shard. The gRPC reader thread is the sole producer; one
// pinned decision worker is the sole consumer. Because channels are mapped to
// shards by `channel & (nshards - 1)`, every chunk for a given channel reaches
// the same worker, so per-channel read state needs no synchronisation at all.
//
// Design notes
// ------------
// * Free-running 64-bit counters, power-of-two capacity, mask on index. The
//   full usable capacity is `Capacity` (not Capacity - 1) because head and tail
//   are absolute, not wrapped.
// * Each side caches its view of the opposite counter. Without this, every push
//   reads a line the consumer is writing and pays a coherence miss per element;
//   with it, the hot path touches only producer-private state until the ring is
//   actually near-full or near-empty. This is the single most important
//   optimisation in the file -- `perf c2c` will show the difference.
// * head_/tail_/cached_* each occupy their own cacheline to prevent false
//   sharing between the two threads.
// * Memory ordering: producer writes the slot, then release-stores tail_;
//   consumer acquire-loads tail_, then reads the slot. The reverse pair
//   (consumer release-stores head_, producer acquire-loads head_) is what stops
//   the producer overwriting a slot the consumer has not yet read. Both
//   directions are required; dropping either is a real race, not a theoretical
//   one.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/cacheline.hpp"

namespace mru {

template <class T, std::size_t Capacity>
  requires(std::has_single_bit(Capacity))
class SpscRing {
 public:
  // The data plane only ever carries POD descriptors (channel ids, spans into
  // arena memory, timestamps). Enforcing that here keeps the queue free of
  // destructor and exception concerns on the hot path.
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing carries POD descriptors only");
  static_assert(std::is_default_constructible_v<T>,
                "SpscRing preallocates its slots");

  using value_type = T;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  // ---- producer side (exactly one thread) --------------------------------

  // Returns false if the ring is full. Callers in the data plane must DROP on
  // false and increment a counter -- never block, because blocking the reader
  // thread stalls every channel, and a stale signal chunk is worthless anyway.
  [[nodiscard]] bool try_push(const T& v) noexcept {
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t + 1 - cached_head_ > Capacity) {
      // Cached value may simply be stale; refresh and retest before failing.
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t + 1 - cached_head_ > Capacity) return false;  // genuinely full
    }
    slots_[t & kMask] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // ---- consumer side (exactly one thread) --------------------------------

  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return false;  // genuinely empty
    }
    out = slots_[h & kMask];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Drains up to `max` items with a single acquire load and a single release
  // store, instead of one pair per item. Worth using in the worker loop: it
  // amortises the coherence traffic and feeds the batched index prefetch.
  [[nodiscard]] std::size_t try_pop_bulk(T* out, std::size_t max) noexcept {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return 0;
    }
    const std::size_t avail = static_cast<std::size_t>(cached_tail_ - h);
    const std::size_t n = avail < max ? avail : max;
    for (std::size_t i = 0; i < n; ++i) out[i] = slots_[(h + i) & kMask];
    head_.store(h + n, std::memory_order_release);
    return n;
  }

  // ---- telemetry (approximate; safe to call from any thread) -------------

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint64_t t = tail_.load(std::memory_order_acquire);
    const std::uint64_t h = head_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(t - h);
  }

  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

  [[nodiscard]] std::uint64_t pushed_total() const noexcept {
    return tail_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t popped_total() const noexcept {
    return head_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  alignas(kCacheline) std::atomic<std::uint64_t> head_{0};  // written: consumer
  alignas(kCacheline) std::uint64_t cached_tail_{0};        // consumer-private
  alignas(kCacheline) std::atomic<std::uint64_t> tail_{0};  // written: producer
  alignas(kCacheline) std::uint64_t cached_head_{0};        // producer-private
  alignas(kCacheline) T slots_[Capacity]{};
};

}  // namespace mru
