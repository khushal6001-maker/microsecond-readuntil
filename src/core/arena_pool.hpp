// src/core/arena_pool.hpp
//
// Recycled arena slots with reference counting -- the mechanism that makes the
// ingestion path copy-free after protobuf parse.
//
// Lifetime protocol (GET THIS WRONG AND YOU GET A USE-AFTER-FREE)
// --------------------------------------------------------------
//     ArenaSlot* slot = pool.acquire();        // refcount == 1, held by READER
//     if (slot == nullptr) { ++exhausted; continue; }
//
//     auto* resp = Arena::CreateMessage<GetLiveReadsResponse>(&slot->arena());
//     if (!stream->Read(resp)) { slot->release(); break; }
//
//     for (const auto& [channel, rd] : resp->channels()) {
//       slot->retain();                        // one reference per consumer
//       ChunkRef ref{ ..., .owner = slot, ... };
//       if (!shard(channel)->try_push(ref)) slot->release();   // drop
//     }
//
//     slot->release();                         // reader drops ITS reference
//
// The reader's own reference is not optional. Without it, the first worker to
// finish could drive the count to zero and Reset() the arena while the reader
// is still iterating the response and handing out spans into that same arena.
// acquire() therefore returns the slot with refcount already 1, and the reader
// must release exactly once when it is done dispatching.
//
// Workers call slot->release() once they have finished reading the signal. The
// last release resets the arena and returns the slot to the free list, so no
// allocation happens on the hot path and total memory is bounded by
// n_slots * arena_block_size.
//
// The free list is a Treiber stack with MANY pushers (workers) and exactly ONE
// popper (the reader thread). Single-consumer is what makes it ABA-free: a node
// still on the list cannot be concurrently pushed by anyone, so its `next_` is
// stable while the popper reads it.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "core/cacheline.hpp"

#if defined(MRU_WITH_PROTOBUF)
#include <google/protobuf/arena.h>
#endif

namespace mru {

// Anything with a protobuf-compatible Reset() can back a slot. google::protobuf::Arena
// satisfies this, and so does BumpArena below, which lets the core be tested
// with no protobuf dependency at all.
template <class A>
concept ResettableArena = requires(A& a) { a.Reset(); };

template <ResettableArena ArenaT>
class ArenaPool;

template <ResettableArena ArenaT>
class ArenaSlot {
 public:
  ArenaSlot(const ArenaSlot&) = delete;
  ArenaSlot& operator=(const ArenaSlot&) = delete;

  [[nodiscard]] ArenaT& arena() noexcept { return arena_; }
  [[nodiscard]] const ArenaT& arena() const noexcept { return arena_; }

  // Add references. Called by the reader thread before dispatching each
  // descriptor, so it is always single-threaded in practice; relaxed is enough
  // because the subsequent ring push carries the release.
  void retain(std::uint32_t n = 1) noexcept {
    refs_.fetch_add(n, std::memory_order_relaxed);
  }

  // Drop one reference. The last one resets the arena and recycles the slot.
  // acq_rel so that the thread performing the reset observes every prior write
  // made through this slot by every other participant.
  void release() noexcept;

  [[nodiscard]] std::uint32_t refs() const noexcept {
    return refs_.load(std::memory_order_relaxed);
  }

  // Number of completed acquire/release cycles. Used by tests to prove each
  // acquisition reset the arena exactly once. Only read when quiesced.
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

 private:
  friend class ArenaPool<ArenaT>;

  template <class... Args>
  explicit ArenaSlot(ArenaPool<ArenaT>* pool, Args&&... arena_args)
      : arena_(std::forward<Args>(arena_args)...), pool_(pool) {}

  ArenaT arena_;
  ArenaPool<ArenaT>* pool_;
  ArenaSlot* next_{nullptr};  // free-list link; only touched while free
  std::uint64_t generation_{0};
  alignas(kCacheline) std::atomic<std::uint32_t> refs_{0};
};

template <ResettableArena ArenaT>
class ArenaPool {
 public:
  using Slot = ArenaSlot<ArenaT>;

  // `arena_args` are forwarded (by copy, once per slot) to each arena's
  // constructor -- e.g. a byte capacity for BumpArena, or ArenaOptions for
  // protobuf.
  template <class... Args>
  explicit ArenaPool(std::size_t n_slots, Args... arena_args) : n_slots_(n_slots) {
    assert(n_slots > 0);
    slots_.reserve(n_slots);
    for (std::size_t i = 0; i < n_slots; ++i) {
      slots_.emplace_back(new Slot(this, arena_args...));
      push_free(slots_.back().get());
    }
  }

  ArenaPool(const ArenaPool&) = delete;
  ArenaPool& operator=(const ArenaPool&) = delete;

  // SINGLE-CONSUMER: only the gRPC reader thread may call this.
  // Returns nullptr when the pool is exhausted, which means consumers are not
  // keeping up. Treat it as backpressure: drop the response, count it, move on.
  [[nodiscard]] Slot* acquire() noexcept {
    Slot* s = pop_free();
    if (s == nullptr) {
      exhausted_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    // The caller holds reference #1 until it finishes dispatching.
    s->refs_.store(1, std::memory_order_relaxed);
    return s;
  }

  [[nodiscard]] std::size_t slot_count() const noexcept { return n_slots_; }
  [[nodiscard]] std::uint64_t exhausted() const noexcept {
    return exhausted_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t recycled() const noexcept {
    return recycled_.load(std::memory_order_relaxed);
  }

  // Walks the free list. ONLY valid when the pool is quiesced (all threads
  // joined) -- it is a leak check for tests, not a runtime gauge.
  [[nodiscard]] std::size_t free_count_quiesced() const noexcept {
    std::size_t n = 0;
    for (Slot* s = free_.load(std::memory_order_acquire); s != nullptr; s = s->next_) ++n;
    return n;
  }

  [[nodiscard]] Slot* slot_at(std::size_t i) const noexcept { return slots_[i].get(); }

 private:
  friend class ArenaSlot<ArenaT>;

  // Many pushers (workers releasing the last reference).
  void push_free(Slot* s) noexcept {
    Slot* head = free_.load(std::memory_order_relaxed);
    do {
      s->next_ = head;
    } while (!free_.compare_exchange_weak(head, s, std::memory_order_release,
                                          std::memory_order_relaxed));
  }

  // Exactly one popper. See the header comment on why this is ABA-free.
  [[nodiscard]] Slot* pop_free() noexcept {
    Slot* head = free_.load(std::memory_order_acquire);
    while (head != nullptr) {
      Slot* next = head->next_;
      if (free_.compare_exchange_weak(head, next, std::memory_order_acquire,
                                      std::memory_order_acquire)) {
        head->next_ = nullptr;
        return head;
      }
    }
    return nullptr;
  }

  std::size_t n_slots_;
  std::vector<std::unique_ptr<Slot>> slots_;
  alignas(kCacheline) std::atomic<Slot*> free_{nullptr};
  alignas(kCacheline) std::atomic<std::uint64_t> exhausted_{0};
  alignas(kCacheline) std::atomic<std::uint64_t> recycled_{0};
};

template <ResettableArena ArenaT>
inline void ArenaSlot<ArenaT>::release() noexcept {
  if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    arena_.Reset();
    ++generation_;  // ordered by the acq_rel above and the free-list handoff
    pool_->recycled_.fetch_add(1, std::memory_order_relaxed);
    pool_->push_free(this);
  }
}

// ---------------------------------------------------------------------------
// A dependency-free arena so core/ and test/ build and run under TSan before
// protobuf or gRPC are in the picture. Deliberately mirrors the parts of
// google::protobuf::Arena that the pool touches.
// ---------------------------------------------------------------------------
class BumpArena {
 public:
  explicit BumpArena(std::size_t capacity_bytes)
      : buf_(std::make_unique<std::byte[]>(capacity_bytes)), cap_(capacity_bytes) {}

  [[nodiscard]] void* Allocate(std::size_t n,
                               std::size_t align = alignof(std::max_align_t)) noexcept {
    assert(align > 0 && (align & (align - 1)) == 0 && "alignment must be a power of two");
    const auto base = reinterpret_cast<std::uintptr_t>(buf_.get());
    const std::uintptr_t aligned =
        (base + used_ + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
    const std::size_t need = static_cast<std::size_t>(aligned - base) + n;
    if (need > cap_) return nullptr;
    used_ = need;
    return reinterpret_cast<void*>(aligned);
  }

  // Name and return value match google::protobuf::Arena::Reset().
  std::uint64_t Reset() noexcept {
    const std::uint64_t had = used_;
    used_ = 0;
    ++resets_;
    return had;
  }

  [[nodiscard]] std::size_t used() const noexcept { return used_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
  [[nodiscard]] std::uint64_t resets() const noexcept { return resets_; }

 private:
  std::unique_ptr<std::byte[]> buf_;
  std::size_t cap_;
  std::size_t used_{0};
  std::uint64_t resets_{0};
};

using BumpArenaPool = ArenaPool<BumpArena>;

#if defined(MRU_WITH_PROTOBUF)
using PbArenaPool = ArenaPool<google::protobuf::Arena>;
#endif

}  // namespace mru
