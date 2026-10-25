#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Load environment and resolve dependencies from .env
source "$SCRIPT_DIR/common_env.sh"

BUILD_DIR="$PROJECT_ROOT/build/host"

echo "=== [GunBros Tests] Building & Executing Host Test Suite ==="
echo "  CMake: $CMAKE_BIN"
echo "  CTest: $CTEST_BIN"

"$CMAKE_BIN" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -S "$PROJECT_ROOT"

"$CMAKE_BIN" --build "$BUILD_DIR" -j"$(nproc)"

# Clean up any leftover temporary directories from previous runs
rm -rf /tmp/gunbros_fs_test /tmp/gunbros_persist_test /tmp/gunbros_hle_test

echo "=== [GunBros Tests] Running CTest ==="
"$CTEST_BIN" --test-dir "$BUILD_DIR" --output-on-failure --verbose

# Post-test cleanup
rm -rf /tmp/gunbros_fs_test /tmp/gunbros_persist_test /tmp/gunbros_hle_test

echo "=== [GunBros Tests] All tests completed successfully! ==="
