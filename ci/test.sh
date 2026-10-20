#!/bin/sh
# Builds and runs the test suite as CI does. Design: docs/design/009-ci.md
#
# Usage: ci/test.sh release|tsan|asan
# Runs from the repository root. Quiet-machine checks (label `quiet`) are left
# out: a shared runner cannot promise them. Every skipped check is listed.
set -eu

kind=$1
build=build-ci-$kind
case $kind in
  release) flags="" baselines=ON type=Release ;;
  tsan)    flags="-fsanitize=thread" baselines=OFF type=RelWithDebInfo ;;
  asan)    flags="-fsanitize=address,undefined -fno-sanitize-recover=all" baselines=OFF type=RelWithDebInfo ;;
  *) echo "usage: $0 release|tsan|asan" >&2; exit 2 ;;
esac

echo "== $(g++ --version | head -1); $(cmake --version | head -1); $(nproc) CPUs; kernel $(uname -r) $(uname -m)"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=$type -DLFIPC_BASELINES=$baselines \
      -DCMAKE_CXX_FLAGS="$flags ${flags:+-fno-omit-frame-pointer}"
cmake --build "$build"

status=0
(cd "$build" && ctest -LE quiet --output-on-failure) || status=$?

echo "== skipped checks"
grep -h '^skip ' "$build/Testing/Temporary/LastTest.log" || echo "none"
exit $status
