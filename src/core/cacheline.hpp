// src/core/cacheline.hpp
//
// Cacheline constants and a CPU relax primitive, shared by the data-plane
// containers. Kept in its own header so spsc_ring.hpp and arena_pool.hpp do
// not have to include each other.
//
// We hardcode the value rather than using std::hardware_destructive_interference_size
// because GCC emits -Winterference-size when that constant is used in a context
// that affects layout (which is exactly what we are doing), and because the
// standard value is conservatively 64 on x86-64 while Apple silicon needs 128.
#pragma once

#include <cstddef>

namespace mru {

#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheline = 128;
#else
inline constexpr std::size_t kCacheline = 64;
#endif

// Pads a type out to a whole number of cachelines so that independent
// instances never share a line. Use for per-worker counters.
template <class T>
struct alignas(kCacheline) Padded {
  T value{};
};

}  // namespace mru
