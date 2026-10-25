#!/usr/bin/env bash
# ==============================================================================
# GunBros ARM64 Bridge - Common Environment & Path Resolver
# ==============================================================================
# This script is sourced by every automation script to:
# 1. Load .env if it exists at the repository root.
# 2. Resolve relative paths (./...) against the project root.
# 3. Automatically and portably detect the required tools
#    (NDK, SDK build-tools, CMake, CTest, Keystore, Dynarmic, Boost).
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 1. Load variables from .env if it exists
if [[ -f "$PROJECT_ROOT/.env" ]]; then
    set -a
    source "$PROJECT_ROOT/.env"
    set +a
fi

# 2. Helper that resolves relative paths to absolute project paths
resolve_path() {
    local p="${1:-}"
    if [[ -z "$p" ]]; then
        echo ""
    elif [[ "$p" = ./* ]]; then
        echo "$PROJECT_ROOT/${p#./}"
    elif [[ "$p" != /* && -e "$PROJECT_ROOT/$p" ]]; then
        echo "$PROJECT_ROOT/$p"
    else
        echo "$p"
    fi
}

# 3. Resolve Android NDK
NDK="${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
NDK="$(resolve_path "$NDK")"
if [[ -z "$NDK" || ! -d "$NDK" ]]; then
    if [[ -d "$PROJECT_ROOT/tools/android-ndk-r26d" ]]; then
        NDK="$PROJECT_ROOT/tools/android-ndk-r26d"
    elif [[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME/ndk" ]]; then
        LATEST_NDK="$(ls -1d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -n 1 || true)"
        [[ -n "$LATEST_NDK" ]] && NDK="$LATEST_NDK"
    fi
fi

# 4. Resolve CMake and CTest
CMAKE_BIN="$(resolve_path "${CMAKE_BIN:-}")"
if [[ -z "$CMAKE_BIN" || ! -x "$CMAKE_BIN" ]]; then
    if which cmake >/dev/null 2>&1; then
        CMAKE_BIN="$(which cmake)"
    elif [[ -x "$PROJECT_ROOT/tools/cmake/bin/cmake" ]]; then
        CMAKE_BIN="$PROJECT_ROOT/tools/cmake/bin/cmake"
    else
        CMAKE_BIN="cmake"
    fi
fi

CTEST_BIN="$(resolve_path "${CTEST_BIN:-}")"
if [[ -z "$CTEST_BIN" || ! -x "$CTEST_BIN" ]]; then
    if which ctest >/dev/null 2>&1; then
        CTEST_BIN="$(which ctest)"
    elif [[ -x "$PROJECT_ROOT/tools/cmake/bin/ctest" ]]; then
        CTEST_BIN="$PROJECT_ROOT/tools/cmake/bin/ctest"
    else
        CTEST_BIN="ctest"
    fi
fi

# 5. Resolve Android SDK Build-Tools (zipalign, apksigner)
BUILD_TOOLS="$(resolve_path "${BUILD_TOOLS:-}")"
if [[ -z "$BUILD_TOOLS" || ! -d "$BUILD_TOOLS" ]]; then
    if [[ -d "$PROJECT_ROOT/tools/build-tools/34.0.0" ]]; then
        BUILD_TOOLS="$PROJECT_ROOT/tools/build-tools/34.0.0"
    elif [[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME/build-tools" ]]; then
        LATEST_BT="$(ls -1d "$ANDROID_HOME"/build-tools/* 2>/dev/null | sort -V | tail -n 1 || true)"
        [[ -n "$LATEST_BT" ]] && BUILD_TOOLS="$LATEST_BT"
    fi
fi

ZIPALIGN="${ZIPALIGN:-}"
APKSIGNER="${APKSIGNER:-}"
if [[ -n "$BUILD_TOOLS" && -d "$BUILD_TOOLS" ]]; then
    [[ -x "$BUILD_TOOLS/zipalign" ]] && ZIPALIGN="$BUILD_TOOLS/zipalign"
    [[ -x "$BUILD_TOOLS/apksigner" ]] && APKSIGNER="$BUILD_TOOLS/apksigner"
fi
ZIPALIGN="${ZIPALIGN:-$(which zipalign 2>/dev/null || echo "zipalign")}"
APKSIGNER="${APKSIGNER:-$(which apksigner 2>/dev/null || echo "apksigner")}"

# 6. Resolve debug keystore
KEYSTORE="$(resolve_path "${KEYSTORE:-}")"
if [[ -z "$KEYSTORE" || ! -f "$KEYSTORE" ]]; then
    if [[ -f "$PROJECT_ROOT/third_party/keystore/debug.keystore" ]]; then
        KEYSTORE="$PROJECT_ROOT/third_party/keystore/debug.keystore"
    elif [[ -f "$HOME/.android/debug.keystore" ]]; then
        KEYSTORE="$HOME/.android/debug.keystore"
    fi
fi
KEYSTORE_PASS="${KEYSTORE_PASS:-pass:android}"

# 7. Resolve strip tool
STRIP="${STRIP:-}"
if [[ -z "$STRIP" && -n "$NDK" && -d "$NDK" ]]; then
    CANDIDATE_STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
    [[ -x "$CANDIDATE_STRIP" ]] && STRIP="$CANDIDATE_STRIP"
fi
if [[ -z "$STRIP" || ! -x "$STRIP" ]]; then
    STRIP="$(which llvm-strip 2>/dev/null || which strip 2>/dev/null || echo "strip")"
fi

# 8. Resolve Dynarmic and Boost
DYNARMIC_ROOT="$(resolve_path "${DYNARMIC_ROOT:-$PROJECT_ROOT/third_party/dynarmic}")"
BOOST_CANDIDATE="$(resolve_path "${BOOST_ROOT:-}")"
if [[ -n "$BOOST_CANDIDATE" && -d "$BOOST_CANDIDATE" ]]; then
    BOOST_ROOT="$BOOST_CANDIDATE"
elif [[ -d "$PROJECT_ROOT/third_party/boost/usr/include" ]]; then
    BOOST_ROOT="$PROJECT_ROOT/third_party/boost/usr/include"
elif [[ -d "$PROJECT_ROOT/third_party/boost/include" ]]; then
    BOOST_ROOT="$PROJECT_ROOT/third_party/boost/include"
elif [[ -d "/usr/include/boost" ]]; then
    BOOST_ROOT="/usr/include"
else
    BOOST_ROOT="$PROJECT_ROOT/third_party/boost/include"
fi

# 9. ADB install flags for Android 14+
ADB_INSTALL_FLAGS="${ADB_INSTALL_FLAGS:---no-incremental --bypass-low-target-sdk-block -r -d}"
