#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Load environment and resolve dependencies from .env
source "$SCRIPT_DIR/common_env.sh"

if [[ -z "$NDK" || ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
    echo "ERROR: Invalid Android NDK at '$NDK'." >&2
    echo "Set ANDROID_NDK_HOME in your .env file or define the environment variable." >&2
    exit 1
fi

BUILD_DIR="$PROJECT_ROOT/build/android"

BRIDGE_DEBUG_FLAG="-DBRIDGE_DEBUG=OFF"
if [[ "${BRIDGE_DEBUG:-0}" == "1" ]] || [[ "${1:-}" == "--debug" ]]; then
    BRIDGE_DEBUG_FLAG="-DBRIDGE_DEBUG=ON"
    echo "  Bridge Debug: ENABLED"
else
    echo "  Bridge Debug: DISABLED (Release)"
fi

echo "=== [GunBros Build] Starting ARM64 NDK CMake Build ==="
echo "  NDK:      $NDK"
echo "  CMake:    $CMAKE_BIN"
echo "  Dynarmic: $DYNARMIC_ROOT"
echo "  Boost:    $BOOST_ROOT"

"$CMAKE_BIN" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  $BRIDGE_DEBUG_FLAG \
  -DDYNARMIC_ROOT="$DYNARMIC_ROOT" \
  -DBOOST_ROOT="$BOOST_ROOT" \
  -S "$PROJECT_ROOT"

"$CMAKE_BIN" --build "$BUILD_DIR" --target androidplatformjni -j"$(nproc)"

SO_PATH="$BUILD_DIR/libandroidplatformjni.so"
"$STRIP" --strip-unneeded "$SO_PATH"
echo "[GunBros Build] Stripped $SO_PATH ($(ls -lh "$SO_PATH" | awk '{print $5}'))"

# Package and sign APK
"$SCRIPT_DIR/package.sh" "$SO_PATH"

echo "=== [GunBros Build] Build and APK packaging complete! ==="
