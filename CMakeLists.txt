cmake_minimum_required(VERSION 3.20)
project(microsecond_readuntil VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard / defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Keep frame pointers in every build: without them perf call graphs are useless,
# and perf is how we justify the cache-conscious claims.
if(NOT MSVC)
  add_compile_options(-fno-omit-frame-pointer)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MRU_BUILD_TESTS    "Build the core stress tests"                 ON)
option(MRU_WERROR         "Treat warnings as errors"                    OFF)
option(MRU_NATIVE         "Tune for this host (-march=native)"          OFF)
option(MRU_TSAN           "ThreadSanitizer build"                       OFF)
option(MRU_ASAN           "AddressSanitizer build"                      OFF)
option(MRU_UBSAN          "UndefinedBehaviorSanitizer build"            OFF)
option(MRU_WITH_TRANSPORT "Build the MinKNOW gRPC transport layer"      OFF)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
# -Wconversion is deliberately NOT here: it fires on the calibration maths and
# would push you to sprinkle casts that hide real narrowing bugs. Run it as a
# periodic audit instead:  cmake -DCMAKE_CXX_FLAGS="-Wconversion -Wsign-conversion"
add_library(mru_warnings INTERFACE)
if(MSVC)
  target_compile_options(mru_warnings INTERFACE
    /W4 /permissive- /EHsc /utf-8 /Zc:preprocessor /Zc:__cplusplus)
  if(MRU_WERROR)
    target_compile_options(mru_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(mru_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wcast-align -Wold-style-cast -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough -Wunused
    -Wnon-virtual-dtor -Woverloaded-virtual)
  if(MRU_WERROR)
    target_compile_options(mru_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
add_library(mru_sanitizers INTERFACE)

if(MRU_TSAN AND MRU_ASAN)
  message(FATAL_ERROR "MRU_TSAN and MRU_ASAN cannot be combined: build two trees.")
endif()

if(MRU_TSAN)
  if(MSVC)
    message(FATAL_ERROR
      "MSVC has no ThreadSanitizer. Configure the TSan build under Linux or WSL2 "
      "with gcc or clang. This is not optional for the lock-free code -- the data "
      "plane must be proven race-free somewhere.")
  endif()
  # -O1 keeps the test fast enough to run in CI while leaving stacks readable.
  target_compile_options(mru_sanitizers INTERFACE -fsanitize=thread -g -O1)
  target_link_options(mru_sanitizers INTERFACE -fsanitize=thread)
  message(STATUS "mru: ThreadSanitizer enabled")
endif()

if(MRU_ASAN)
  if(MSVC)
    target_compile_options(mru_sanitizers INTERFACE /fsanitize=address /Zi)
  else()
    target_compile_options(mru_sanitizers INTERFACE
      -fsanitize=address -fsanitize-address-use-after-scope -g -O1)
    target_link_options(mru_sanitizers INTERFACE -fsanitize=address)
  endif()
  message(STATUS "mru: AddressSanitizer enabled")
endif()

if(MRU_UBSAN)
  if(MSVC)
    message(WARNING "MSVC has no UBSan; MRU_UBSAN ignored.")
  else()
    target_compile_options(mru_sanitizers INTERFACE
      -fsanitize=undefined -fno-sanitize-recover=undefined -g)
    target_link_options(mru_sanitizers INTERFACE -fsanitize=undefined)
    message(STATUS "mru: UndefinedBehaviorSanitizer enabled")
  endif()
endif()

# ---------------------------------------------------------------------------
# mru_core -- header-only data plane
# ---------------------------------------------------------------------------
add_library(mru_core INTERFACE)
target_include_directories(mru_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mru_core INTERFACE Threads::Threads)

# CPU_SET and sched_getcpu live behind _GNU_SOURCE, and the macro has to precede
# the first libc header -- hence here rather than in affinity.hpp.
if(UNIX AND NOT APPLE)
  target_compile_definitions(mru_core INTERFACE _GNU_SOURCE)
endif()

if(MRU_NATIVE)
  if(MSVC)
    target_compile_options(mru_core INTERFACE /arch:AVX2)
  else()
    target_compile_options(mru_core INTERFACE -march=native -mtune=native)
  endif()
  message(STATUS "mru: tuning for this host -- binaries are NOT portable")
endif()

# ---------------------------------------------------------------------------
# Optional transport layer (step 3; off by default so step 1-2 needs no deps)
# ---------------------------------------------------------------------------
if(MRU_WITH_TRANSPORT)
  if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "MRU_WITH_TRANSPORT needs CMake >= 3.24 for protobuf_generate")
  endif()

  set(MINKNOW_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minknow_api/proto")
  if(NOT EXISTS "${MINKNOW_PROTO_DIR}")
    message(FATAL_ERROR
      "minknow_api protos not found at ${MINKNOW_PROTO_DIR}\n"
      "  git submodule add https://github.com/nanoporetech/minknow_api third_party/minknow_api\n"
      "  git submodule update --init --recursive")
  endif()

  find_package(protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)

  file(GLOB MINKNOW_PROTOS "${MINKNOW_PROTO_DIR}/minknow_api/*.proto")
  add_library(minknow_proto STATIC ${MINKNOW_PROTOS})
  target_link_libraries(minknow_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
  target_include_directories(minknow_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

  protobuf_generate(TARGET minknow_proto
    IMPORT_DIRS "${MINKNOW_PROTO_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  protobuf_generate(TARGET minknow_proto LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS "${MINKNOW_PROTO_DIR}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

  # Switches on PbArenaPool in arena_pool.hpp.
  target_compile_definitions(mru_core INTERFACE MRU_WITH_PROTOBUF=1)
  target_link_libraries(mru_core INTERFACE minknow_proto)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MRU_BUILD_TESTS)
  enable_testing()

  add_executable(mru_ring_stress test/ring_tsan.cpp)
  target_link_libraries(mru_ring_stress PRIVATE mru_core mru_warnings mru_sanitizers)

  add_test(NAME core.ring_stress COMMAND mru_ring_stress)
  set_tests_properties(core.ring_stress PROPERTIES TIMEOUT 900)

  # A deliberately long soak, excluded from the default run.
  add_test(NAME core.ring_soak COMMAND mru_ring_stress 200000000 8 5000000)
  set_tests_properties(core.ring_soak PROPERTIES TIMEOUT 3600 LABELS soak)
endif()

# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "microsecond-readuntil ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests      : ${MRU_BUILD_TESTS}")
message(STATUS "  transport  : ${MRU_WITH_TRANSPORT}")
message(STATUS "  sanitizers : tsan=${MRU_TSAN} asan=${MRU_ASAN} ubsan=${MRU_UBSAN}")
message(STATUS "")
