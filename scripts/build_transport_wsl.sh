#!/bin/bash
# Bring the WSL build clone up to the committed state and rebuild the daemon with the
# transport layer, which is what a live latency run needs. The clone at /root/mru is separate
# from the Windows working copy and had been left several commits behind; the frozen
# configuration (4 x 12, window 1, detection on, and the 0-sentinel CLI defaults that stop the
# argument parser shadowing it) postdates that, so a run with the old binary would measure a
# configuration the thesis does not describe.
set -eu
cd /root/mru
git fetch --quiet origin
git checkout --quiet main 2>/dev/null || git checkout --quiet -b main origin/main
git reset --hard --quiet origin/main
echo "clone now at $(git log --oneline -1)"
git submodule update --init --depth 1 third_party/HdrHistogram_c --quiet 2>/dev/null || true
git submodule update --init --recursive --quiet third_party/minknow_api 2>/dev/null || true

cmake -S . -B build-transport -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DMRU_WITH_TRANSPORT=ON -G Ninja > /tmp/cmake_cfg.log 2>&1 \
  || { tail -25 /tmp/cmake_cfg.log; exit 1; }
cmake --build build-transport -j "$(nproc)" --target mru_daemon > /tmp/cmake_build.log 2>&1 \
  || { tail -40 /tmp/cmake_build.log; exit 1; }
ls -la build-transport/mru_daemon
./build-transport/mru_daemon --help 2>&1 | grep -iE 'latency-out|bits|events|detect' | head
