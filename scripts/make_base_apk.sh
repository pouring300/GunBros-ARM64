#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OUT_TEMPLATE="$PROJECT_ROOT/base_template.apk"

usage() {
    echo "Usage: $0 <path-to-original-GunBros-APK>"
    echo ""
    echo "Generates 'base_template.apk' from the original Gun Bros APK (ARM32)."
    echo "The resulting file contains the original assets (including the 32-bit"
    echo "ELF 'assets/libandroidplatformjni.so') and strips the stale signatures"
    echo "so the ARM64 bridge can be injected and the APK signed with apksigner."
    echo ""
    echo "Example:"
    echo "  $0 ~/Downloads/GunBros_v1.1.9.apk"
    exit 1
}

if [[ $# -lt 1 || -z "$1" ]]; then
    usage
fi

INPUT_APK="$1"

if [[ ! -f "$INPUT_APK" ]]; then
    echo "ERROR: The specified file does not exist: $INPUT_APK" >&2
    exit 1
fi

echo "=== [GunBros Base APK] Preparing base_template.apk ==="
echo "  Source APK:  $INPUT_APK"
echo "  Destination: $OUT_TEMPLATE"

# Validate that it is a valid zip/APK file
if ! unzip -tq "$INPUT_APK" >/dev/null 2>&1; then
    echo "ERROR: The provided file is not a valid APK/ZIP." >&2
    exit 1
fi

# Copy to a temporary location
TMP_APK="/tmp/gunbros_base_template_$$.apk"
TMP_EXTRACT="/tmp/gunbros_extract_$$"
trap 'rm -rf "$TMP_APK" "$TMP_EXTRACT"' EXIT

cp -f "$INPUT_APK" "$TMP_APK"

# Validate and prepare the original ARM32 binary in assets/
if ! unzip -l "$TMP_APK" | grep -q "assets/libandroidplatformjni.so"; then
    if unzip -l "$TMP_APK" | grep -q "lib/armeabi/libandroidplatformjni.so"; then
        echo "  [INFO] Extracting lib/armeabi/libandroidplatformjni.so to assets/libandroidplatformjni.so..."
        mkdir -p "$TMP_EXTRACT/assets"
        unzip -q -j "$TMP_APK" "lib/armeabi/libandroidplatformjni.so" -d "$TMP_EXTRACT/assets"
        (cd "$TMP_EXTRACT" && zip -0 -q -u "$TMP_APK" "assets/libandroidplatformjni.so")
    elif unzip -l "$TMP_APK" | grep -q "lib/armeabi-v7a/libandroidplatformjni.so"; then
        echo "  [INFO] Extracting lib/armeabi-v7a/libandroidplatformjni.so to assets/libandroidplatformjni.so..."
        mkdir -p "$TMP_EXTRACT/assets"
        unzip -q -j "$TMP_APK" "lib/armeabi-v7a/libandroidplatformjni.so" -d "$TMP_EXTRACT/assets"
        (cd "$TMP_EXTRACT" && zip -0 -q -u "$TMP_APK" "assets/libandroidplatformjni.so")
    else
        echo "ERROR: 'libandroidplatformjni.so' was not found in the APK's assets/ or lib/armeabi/." >&2
        exit 1
    fi
fi

# Remove previous signatures (META-INF/*.SF, *.RSA, *.MF) to avoid conflicts
echo "  [1/2] Removing old signatures from META-INF..."
zip -q -d "$TMP_APK" "META-INF/*" 2>/dev/null || true

# Remove existing 32-bit armeabi and arm64 libraries to leave a clean slot for the bridge
zip -q -d "$TMP_APK" "lib/armeabi/*" "lib/armeabi-v7a/*" "lib/arm64-v8a/*" 2>/dev/null || true

# Move to the project root
echo "  [2/2] Creating base_template.apk..."
cp -f "$TMP_APK" "$OUT_TEMPLATE"

echo "=== [GunBros Base APK] base_template.apk generated successfully ($(ls -lh "$OUT_TEMPLATE" | awk '{print $5}')) ==="
echo "You can now build and package the game by running:"
echo "  ./scripts/build.sh"
echo "or package it directly with:"
echo "  ./scripts/package.sh"
