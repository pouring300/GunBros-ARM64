#!/usr/bin/env bash
# ==============================================================================
# GunBros ARM64 Bridge - Dependency Checker and Installer
# ==============================================================================
# This script plays a role similar to 'npm install' or 'pip install -r requirements.txt':
# 1. Creates the .env file from .env.example if it does not exist.
# 2. Checks system dependencies (CMake, Ninja/Make, compiler, Zip, ADB).
# 3. Checks and validates the Android NDK and the Android SDK Build-Tools.
# 4. Checks the prebuilt Dynarmic libraries and the Boost headers.
# 5. Automatically generates a debug keystore if none exists.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# If --download or --install is passed, run the installer first
if [[ "${1:-}" == "--download" || "${1:-}" == "--install" || "${1:-}" == "-i" ]]; then
    "$SCRIPT_DIR/download_deps.sh"
fi

echo -e "${BLUE}=== [GunBros Setup] Checking Project Dependencies ===${NC}"

# 1. Ensure .env file exists
if [[ ! -f "$PROJECT_ROOT/.env" ]]; then
    echo -e "${YELLOW}[!] No .env found. Creating .env from .env.example...${NC}"
    cp "$PROJECT_ROOT/.env.example" "$PROJECT_ROOT/.env"
    echo -e "${GREEN}[✓] .env file created successfully.${NC}"
else
    echo -e "${GREEN}[✓] .env file detected.${NC}"
fi

# Load common environment
source "$SCRIPT_DIR/common_env.sh"

ALL_OK=true

# 2. Check basic system tools
check_tool() {
    local cmd="$1"
    local name="$2"
    if which "$cmd" >/dev/null 2>&1; then
        echo -e "  ${GREEN}[✓]${NC} $name ($cmd): $(which "$cmd")"
    else
        echo -e "  ${RED}[✗]${NC} $name ($cmd) not found in PATH."
        ALL_OK=false
    fi
}

echo -e "\n${BLUE}1. Host System Tools:${NC}"
if which "$CMAKE_BIN" >/dev/null 2>&1 || [[ -x "$CMAKE_BIN" ]]; then
    echo -e "  ${GREEN}[✓]${NC} CMake: $CMAKE_BIN"
else
    echo -e "  ${RED}[✗]${NC} CMake ($CMAKE_BIN) not found. Set CMAKE_BIN in .env."
    ALL_OK=false
fi
check_tool "make" "Make (or Ninja)" || check_tool "ninja" "Ninja" || true
check_tool "zip" "Zip"
check_tool "unzip" "Unzip"
check_tool "adb" "Android Debug Bridge (ADB)" || echo -e "      ${YELLOW}(Optional for host tests, required for on-device deploy)${NC}"

# 3. Check C/C++ compiler
echo -e "\n${BLUE}2. C/C++ Compiler (for local CTest tests):${NC}"
if which clang >/dev/null 2>&1; then
    echo -e "  ${GREEN}[✓]${NC} Clang: $(clang --version | head -n 1)"
elif which gcc >/dev/null 2>&1; then
    echo -e "  ${GREEN}[✓]${NC} GCC: $(gcc --version | head -n 1)"
else
    echo -e "  ${RED}[✗]${NC} No C/C++ compiler detected (install clang or gcc)."
    ALL_OK=false
fi

# 4. Check Android NDK
echo -e "\n${BLUE}3. Android NDK (for ARM64 builds):${NC}"
if [[ -n "$NDK" && -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
    echo -e "  ${GREEN}[✓]${NC} Valid NDK detected at: $NDK"
else
    echo -e "  ${RED}[✗]${NC} NDK not found at: '$NDK'"
    echo -e "      ${YELLOW}Fix: Install the Android NDK (r23c through r26d) via Android Studio"
    echo -e "      or download it and set ANDROID_NDK_HOME in your .env file.${NC}"
    ALL_OK=false
fi

# 5. Check Android Build Tools (zipalign, apksigner)
echo -e "\n${BLUE}4. Android SDK Build-Tools (APK packaging and signing):${NC}"
if which "$ZIPALIGN" >/dev/null 2>&1 || [[ -x "$ZIPALIGN" ]]; then
    echo -e "  ${GREEN}[✓]${NC} zipalign: $ZIPALIGN"
else
    echo -e "  ${RED}[✗]${NC} zipalign not found. Set BUILD_TOOLS in your .env file."
    ALL_OK=false
fi

if which "$APKSIGNER" >/dev/null 2>&1 || [[ -x "$APKSIGNER" ]]; then
    echo -e "  ${GREEN}[✓]${NC} apksigner: $APKSIGNER"
else
    echo -e "  ${RED}[✗]${NC} apksigner not found. Set BUILD_TOOLS in your .env file."
    ALL_OK=false
fi

# 6. Check debug keystore (or generate it if missing)
echo -e "\n${BLUE}5. Debug Keystore:${NC}"
if [[ -f "$KEYSTORE" ]]; then
    echo -e "  ${GREEN}[✓]${NC} Existing keystore: $KEYSTORE"
else
    echo -e "  ${YELLOW}[!] Keystore not found at '$KEYSTORE'. Trying to generate a new one...${NC}"
    mkdir -p "$(dirname "$KEYSTORE")"
    if which keytool >/dev/null 2>&1; then
        keytool -genkey -v -keystore "$KEYSTORE" \
          -storepass android -alias androiddebugkey -keypass android \
          -keyalg RSA -keysize 2048 -validity 10000 \
          -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
        echo -e "  ${GREEN}[✓]${NC} Keystore generated successfully at: $KEYSTORE"
    else
        echo -e "  ${RED}[✗]${NC} keytool not found. Install OpenJDK/Java to generate the keystore."
        ALL_OK=false
    fi
fi

# 7. Check emulation dependencies (Dynarmic and Boost)
echo -e "\n${BLUE}6. Emulation Dependencies (Dynarmic and Boost):${NC}"
LIBDYNARMIC="$DYNARMIC_ROOT/build-arm64/src/dynarmic/libdynarmic.a"
if [[ -f "$LIBDYNARMIC" ]]; then
    echo -e "  ${GREEN}[✓]${NC} Dynarmic ARM64 library: $LIBDYNARMIC"
else
    echo -e "  ${RED}[✗]${NC} libdynarmic.a not found at $LIBDYNARMIC"
    ALL_OK=false
fi

if [[ -f "$BOOST_ROOT/boost/version.hpp" || -f "$BOOST_ROOT/version.hpp" ]]; then
    echo -e "  ${GREEN}[✓]${NC} Boost headers: $BOOST_ROOT"
else
    echo -e "  ${RED}[✗]${NC} Boost headers not found at $BOOST_ROOT"
    ALL_OK=false
fi

# Final summary
echo -e "\n${BLUE}==============================================================================${NC}"
if [ "$ALL_OK" = true ]; then
    echo -e "${GREEN}Excellent! All dependencies are ready and verified.${NC}"
    echo -e "To build the project and generate the APK:"
    echo -e "  ${YELLOW}./build.sh${NC}"
    echo -e "To run the tests on your machine:"
    echo -e "  ${YELLOW}./scripts/run_tests.sh${NC}"
else
    echo -e "${YELLOW}Missing dependencies were detected.${NC}"
    echo -e "You can download and set them up automatically by running:"
    echo -e "  ${YELLOW}./setup.sh --download${NC}"
    
    # If the session is interactive, offer to download right away
    if [[ -t 0 ]]; then
        echo -e ""
        read -r -p "Download and build the missing dependencies automatically now? [y/N]: " REPLY || true
        if [[ "$REPLY" =~ ^[sSyY]$ ]]; then
            "$SCRIPT_DIR/download_deps.sh"
            echo -e "\n${BLUE}Re-checking dependencies after installation...${NC}"
            exec "$0" "$@"
        fi
    fi
fi
echo -e "${BLUE}==============================================================================${NC}"
