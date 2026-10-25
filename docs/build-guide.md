# Build Guide: GunBros ARM64 Bridge

This guide describes how to build the project both in local mode (to run the automated tests on your computer) and in cross-compilation mode for Android (producing the final 64-bit APK that can be installed on any device).

---

## 1. Prerequisites

### 1.1. Operating System
- **Linux**: Ubuntu 20.04+, Debian 11+, Fedora 36+, Arch Linux or equivalent distributions.
- **Windows**: Supported via **WSL2** (Windows Subsystem for Linux with Ubuntu 22.04+).
- **macOS**: Supported with the command-line developer tools.

### 1.2. Host System Tools
- **C/C++ Compiler**: GCC 10+ or Clang 12+ (C99 and C++20 compatible).
- **CMake**: Version 3.20 or later.
- **Ninja or Make**: Batch build system.
- **Zip / Unzip**: For manipulating APK packages.

### 1.3. Android Development Tools
- **Android NDK**: NDK version `r23c` to `r26d` (`r26d` recommended).
- **Android SDK Build-Tools**: Version `30.0.0` or later (includes `zipalign` and `apksigner`).
- **External Dependencies**:
  - **Dynarmic** library built for the `arm64-v8a` architecture (`libdynarmic.a`, `libfmt.a`, `libmcl.a`).
  - **Boost** headers (required by the Dynarmic interface).
- **Debug Keystore**: Standard Android keystore (`~/.android/debug.keystore`).

---

## 2. Setting Up the Working Environment

The project uses a `.env`-based configuration system (similar to tools like Node.js or Python) so that anyone can run it without modifying internal files.

### 2.1. Automatic Verification and Installation with `./setup.sh`
The fastest way to prepare the environment is to run:
```bash
./setup.sh
```
If missing dependencies are detected (portable CMake, Android NDK, Android Build-Tools, Boost or Dynarmic), the script will interactively ask whether you want to download and build them automatically.

You can also force an unattended download and setup directly with:
```bash
./setup.sh --download
```
This automated process:
1. Creates your `.env` file automatically if it does not exist.
2. Downloads portable CMake (if it is not on the system).
3. Downloads Android SDK Build-Tools 34.0.0 (`zipalign`, `apksigner`, `aapt`).
4. Downloads the official Android NDK r26d from Google.
5. Downloads the Boost C++ headers.
6. Clones Dynarmic from GitHub and builds its static libraries for the `arm64-v8a` architecture.
7. Generates a debug keystore (`debug.keystore`) if you do not have one.
8. Configures the relative paths in `.env` so that everything is ready to build.

### 2.2. Manual Customization via `.env`
If you want to customize specific paths for your machine, edit `.env`:
```bash
cp .env.example .env
```
Example of available variables:
```bash
# Android NDK and SDK
ANDROID_NDK_HOME="./tools/android-ndk-r26d" # or $HOME/Android/Sdk/ndk/26.1.10909125
BUILD_TOOLS="./tools/build-tools/34.0.0"     # or $ANDROID_HOME/build-tools/34.0.0

# Tools
CMAKE_BIN="cmake"
CTEST_BIN="ctest"

# Dependencies (resolved automatically in third_party/)
DYNARMIC_ROOT="./third_party/dynarmic"
BOOST_ROOT="./third_party/boost/usr/include"
```
> **Note**: Relative paths starting with `./` are automatically resolved relative to the project root.


---

## 3. Quick Start (Automated Scripts)

### 3.0. Preparing the Base APK (`base_template.apk`)
In accordance with copyright rules and repository hygiene, the original APK files and game assets **are not included** in the repository.

Before packaging for the first time, generate `base_template.apk` from a user-supplied original Gun Bros v1.1.9 APK (ARM32):
```bash
./scripts/make_base_apk.sh /path/to/GunBros_original.apk
```
This script:
1. Checks that the essential assets are present (`assets/libandroidplatformjni.so`, etc.).
2. Removes obsolete cryptographic signatures from `META-INF/` to allow a clean signature with `apksigner`.
3. Prepares the structure for the subsequent injection of the ARM64 library.

### 3.1. Build the Complete Android APK
From the repository root directory:
```bash
./build.sh
```
This command automatically runs:
1. CMake configuration for the `arm64-v8a` architecture targeting Android API 30+.
2. Compilation of all C/C++ modules in `src/`.
3. Linking against the Dynarmic library and Android system libraries (`libEGL`, `libGLESv1_CM`, `libaaudio`, `liblog`).
4. Binary size reduction via `llvm-strip`.
5. Injection of `libandroidplatformjni.so` into `base_template.apk`.
6. 4-byte alignment (`zipalign`) and cryptographic signing with `apksigner`.

The resulting file will be:
```
GunBros_Android16_Dynarmic.apk
```

### 3.2. Run the Test Suite on the Host
To verify that the filesystem, memory and audio buffers work correctly without connecting a phone:
```bash
./scripts/run_tests.sh
```

---

## 4. Manual Build with CMake

If you prefer fine-grained control over each step of the process:

### 4.1. Host Unit Tests (Debug Mode)
```bash
# 1. Create the build directory
mkdir -p build/host

# 2. Configure the project with testing enabled
cmake -B build/host -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON

# 3. Build the test executables
cmake --build build/host -j$(nproc)

# 4. Run all tests via CTest
ctest --test-dir build/host --output-on-failure --verbose
```

### 4.2. Android Shared Library (`libandroidplatformjni.so`)
```bash
# 1. Create the build directory
mkdir -p build/android

# 2. Configure cross-compilation with the NDK
cmake -B build/android -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DDYNARMIC_ROOT="$DYNARMIC_ROOT" \
  -DBOOST_ROOT="$BOOST_ROOT"

# 3. Build the shared target
cmake --build build/android --target androidplatformjni -j$(nproc)

# 4. Strip unneeded symbols
$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip \
  --strip-unneeded build/android/libandroidplatformjni.so
```

---

## 5. Manual APK Packaging and Signing

If you have only rebuilt the `.so` file and want to package it into the APK:
```bash
./scripts/package.sh build/android/libandroidplatformjni.so
```

---

## 6. Deployment and Installation on a Physical Device or Emulator

To install and run the application on a device connected over USB with ADB debugging enabled:
```bash
./scripts/deploy.sh
```

### Important Notes for Devices Running Android 14, 15 and 16:
By default, Android 14+ blocks the installation of applications whose `targetSdkVersion` is lower than 23. To allow installation on these modern operating systems, the script uses the special ADB parameter:
```bash
adb install --no-incremental --bypass-low-target-sdk-block -r -d GunBros_Android16_Dynarmic.apk
```
After installation, the necessary storage permissions are granted automatically so that the compatibility layer can access the game's resources.
