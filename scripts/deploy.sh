#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Load environment and variables from .env
source "$SCRIPT_DIR/common_env.sh"

APK_PATH="$PROJECT_ROOT/GunBros_Android16_Dynarmic.apk"
PKG_NAME="com.glu.android.gunbros_free"
ACTIVITY_NAME="com.glu.android.gunbros_free.GunBros"

# Configure the ADB command with a specific serial if one is set in .env
ADB_CMD="adb"
if [[ -n "${ADB_SERIAL:-}" ]]; then
    ADB_CMD="adb -s $ADB_SERIAL"
fi

echo "=== [GunBros Deploy] Checking ADB Device Connectivity ==="

DEVICE_COUNT=$($ADB_CMD devices | grep -v "List of devices" | grep "device$" | wc -l || true)

if [[ "$DEVICE_COUNT" -eq 0 ]]; then
    echo "=========================================================================="
    echo "WARNING: No active Android device or emulator detected via ADB."
    echo ""
    echo "To install manually once you connect a device:"
    echo "  1. $ADB_CMD install $ADB_INSTALL_FLAGS $APK_PATH"
    echo "  2. $ADB_CMD shell pm grant $PKG_NAME android.permission.READ_EXTERNAL_STORAGE || true"
    echo "  3. $ADB_CMD shell pm grant $PKG_NAME android.permission.WRITE_EXTERNAL_STORAGE || true"
    echo "  4. $ADB_CMD shell am start -n $PKG_NAME/$ACTIVITY_NAME"
    echo "  5. ./scripts/collect-logs.sh"
    echo "=========================================================================="
    exit 0
fi

echo "[GunBros Deploy] Device detected. Installing $APK_PATH..."
$ADB_CMD install $ADB_INSTALL_FLAGS "$APK_PATH"

echo "[GunBros Deploy] Setting up storage permissions..."
$ADB_CMD shell pm grant "$PKG_NAME" android.permission.READ_EXTERNAL_STORAGE || true
$ADB_CMD shell pm grant "$PKG_NAME" android.permission.WRITE_EXTERNAL_STORAGE || true
$ADB_CMD shell appops set "$PKG_NAME" MANAGE_EXTERNAL_STORAGE allow || true

echo "[GunBros Deploy] Starting activity $ACTIVITY_NAME..."
$ADB_CMD shell am force-stop "$PKG_NAME" || true
$ADB_CMD shell am start -n "$PKG_NAME/$ACTIVITY_NAME"

echo "[GunBros Deploy] App started. Streaming logs (Ctrl+C to stop)..."
"$SCRIPT_DIR/collect-logs.sh"
