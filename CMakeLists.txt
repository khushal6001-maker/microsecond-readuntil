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

# MinGW only: link the C++ and GCC runtimes statically.
#
# Not a style preference. On a typical Windows dev box several libstdc++-6.dll
# copies sit on PATH (Git for Windows ships one, Strawberry Perl ships two), and
# whichever appears first wins at load time. If that one predates the compiler,
# any binary referencing newer libstdc++ symbols dies at startup with
# STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139) and no diagnostic. Linking statically
# makes the dev build independent of PATH ordering. CI on Linux is unaffected.
if(MINGW)
  add_link_options(-static-libstdc++ -static-libgcc)
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
  set(MINKNOW_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minknow_api/proto")
  if(NOT EXISTS "${MINKNOW_PROTO_ROOT}/minknow_api/data.proto")
    message(FATAL_ERROR
      "minknow_api protos not found under ${MINKNOW_PROTO_ROOT}\n"
      "  git submodule update --init --recursive\n"
      "The submodule is pinned to the tag matching the target MinKNOW Core minor "
      "version (6.10.3). See src/transport/proto_compat.hpp.")
  endif()

  # Two supported toolchains, because they package gRPC completely differently:
  #   * vcpkg / upstream build -> CMake package config, imported targets
  #   * Debian/Ubuntu apt      -> pkg-config only, no gRPCConfig.cmake
  # Deliberately NOT using protobuf_generate(): it only exists on the CMake-config
  # path, so an explicit protoc command is the only thing that works on both.
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    message(STATUS "mru: gRPC via CMake package config")
    set(MRU_GRPC_LINK gRPC::grpc++ protobuf::libprotobuf)
    set(MRU_PROTOC "$<TARGET_FILE:protobuf::protoc>")
    set(MRU_GRPC_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
  else()
    message(STATUS "mru: gRPC via pkg-config")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MRU_PB REQUIRED IMPORTED_TARGET protobuf)
    pkg_check_modules(MRU_GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    find_program(MRU_PROTOC NAMES protoc REQUIRED)
    find_program(MRU_GRPC_PLUGIN NAMES grpc_cpp_plugin REQUIRED)
    set(MRU_GRPC_LINK PkgConfig::MRU_GRPCPP PkgConfig::MRU_PB)
  endif()

  file(GLOB MINKNOW_PROTOS "${MINKNOW_PROTO_ROOT}/minknow_api/*.proto")
  list(LENGTH MINKNOW_PROTOS MINKNOW_PROTO_COUNT)

  set(MRU_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${MRU_GEN_DIR}/minknow_api")

  # Derive the output list from the protos themselves: grpc_cpp_plugin emits a
  # .grpc.pb.cc only for files that declare a service, and listing an output that
  # protoc never produces is a hard build failure. Reading the files keeps this
  # correct when ONT adds or removes a service.
  set(MRU_GEN_SRCS "")
  foreach(P IN LISTS MINKNOW_PROTOS)
    get_filename_component(STEM "${P}" NAME_WE)
    list(APPEND MRU_GEN_SRCS "${MRU_GEN_DIR}/minknow_api/${STEM}.pb.cc")
    file(STRINGS "${P}" MRU_HAS_SERVICE REGEX "^[ \t]*service[ \t]+")
    if(MRU_HAS_SERVICE)
      list(APPEND MRU_GEN_SRCS "${MRU_GEN_DIR}/minknow_api/${STEM}.grpc.pb.cc")
    endif()
  endforeach()

  add_custom_command(
    OUTPUT ${MRU_GEN_SRCS}
    COMMAND "${MRU_PROTOC}"
            -I "${MINKNOW_PROTO_ROOT}"
            --cpp_out "${MRU_GEN_DIR}"
            --grpc_out "${MRU_GEN_DIR}"
            "--plugin=protoc-gen-grpc=${MRU_GRPC_PLUGIN}"
            ${MINKNOW_PROTOS}
    DEPENDS ${MINKNOW_PROTOS}
    COMMENT "protoc: generating minknow_api bindings (${MINKNOW_PROTO_COUNT} protos)"
    VERBATIM)

  add_library(minknow_proto STATIC ${MRU_GEN_SRCS})
  target_link_libraries(minknow_proto PUBLIC ${MRU_GRPC_LINK})
  # SYSTEM: generated protobuf/gRPC code is not warning-clean, and consumers of
  # these headers are built with -Werror.
  target_include_directories(minknow_proto SYSTEM PUBLIC "${MRU_GEN_DIR}")
  if(NOT MSVC)
    target_compile_options(minknow_proto PRIVATE -w)
  endif()

  # Switches on PbArenaPool in arena_pool.hpp and ChunkRef in chunk_ref.hpp.
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

  add_executable(mru_read_state_test test/read_state_test.cpp)
  target_link_libraries(mru_read_state_test PRIVATE mru_core mru_warnings mru_sanitizers)

  add_test(NAME decide.read_state COMMAND mru_read_state_test)
  set_tests_properties(decide.read_state PROPERTIES TIMEOUT 300)

  # Credential discovery has no gRPC dependency, so it is tested in every
  # configuration rather than only in the transport build.
  add_executable(mru_auth_test test/auth_test.cpp src/transport/auth.cpp)
  target_link_libraries(mru_auth_test PRIVATE mru_core mru_warnings mru_sanitizers)

  add_test(NAME transport.auth COMMAND mru_auth_test)
  set_tests_properties(transport.auth PROPERTIES TIMEOUT 120)

  # Compiling this exercises every field-number assertion in proto_compat.hpp,
  # and running it exercises the zero-copy path against real protobuf arenas.
  # Needs the generated headers, so it only exists in the transport build.
  if(MRU_WITH_TRANSPORT)
    add_executable(mru_proto_compat_test test/proto_compat_test.cpp)
    target_link_libraries(mru_proto_compat_test
                          PRIVATE mru_core mru_warnings mru_sanitizers)

    add_test(NAME transport.proto_compat COMMAND mru_proto_compat_test)
    set_tests_properties(transport.proto_compat PROPERTIES TIMEOUT 300)

    # Compiling this type-checks all of live_reads_stream.hpp. The runtime part
    # only covers what works without a MinKNOW server; the rest needs Icarust.
    add_executable(mru_live_reads_test
                   test/live_reads_stream_test.cpp src/transport/auth.cpp)
    target_link_libraries(mru_live_reads_test
                          PRIVATE mru_core mru_warnings mru_sanitizers)

    add_test(NAME transport.live_reads COMMAND mru_live_reads_test)
    set_tests_properties(transport.live_reads PROPERTIES TIMEOUT 300)
  endif()

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
