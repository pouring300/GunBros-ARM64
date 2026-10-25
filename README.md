# GunBros ARM64 Bridge

A high-performance real-time (JIT) compatibility and emulation bridge that makes it possible to run the classic 3D action game **GunBros** (originally developed by Glu Mobile for 32-bit ARM architectures / `armeabi-v7a`) on modern Android devices that only support 64-bit applications (Android 14, 15 and 16 / ARMv9 processors).

> [!IMPORTANT]
> **Intellectual Property Notice**: This repository **does NOT include or distribute the APK file, graphics/audio assets, or the original code of the game Gun Bros** (property of Glu Mobile / EA). Users must supply their own legitimate copy of the original APK to generate the base template with `scripts/make_base_apk.sh`.

---

**GunBros ARM64 Bridge** lets you run Gun Bros on phones with current 64-bit Android versions by combining:
- **Dynarmic**: Real-time JIT dynamic recompilation of ARM32 code to ARM64 at a stable 60 FPS.
- **Modular HLE Layer**: Interception and dispatch of system calls (Bionic libc, libm, libz, POSIX and OpenGL ES 1.1 CM), split into thematic modules (`src/hle/`).
- **Controlled Hot Patches**: A declarative table of binary patches with optional verification of original opcodes and capture under `BRIDGE_DEBUG`, used to bypass old shut-down servers (Glu and GameSpy) and fix weapon and skeleton orientation.
- **Robust Persistence**: A filesystem layer compatible with Bionic ARM32, signed 32-bit I/O and diagnostic tracing.

---

## Repository Architecture

The project is strictly decoupled by responsibility:

```text
gunbros/
├── CMakeLists.txt                # Unified CMake configuration (Android NDK & host testing)
├── build.sh                      # Quick script to build and package the APK
├── LICENSE                       # License terms and copyright notices
│
├── docs/                         # Full technical documentation and guides
│   ├── architecture.md           # Subsystem diagram, virtual memory and concurrency
│   ├── build-guide.md            # Host build guide, make_base_apk and NDK cross-compile
│   ├── compatibility.md          # Binary patches (A-J), HLE and synthetic JNI virtual machine
│   ├── filesystem.md             # I/O mechanics, Glu virtual schemes and Bionic stat layout
│   ├── save-debugging.md         # Step-by-step save persistence diagnostic protocol
│   ├── debugging.md              # Debugging with Logcat, watchdog and register dumps
│   └── testing.md                # Testing methodology and CTest suite catalog
│
├── include/gunbros/              # Typed, modular public C/C++ interfaces
│   ├── bridge/                   # Lifecycle, render loop and JNI exports
│   ├── emulator/                 # Dynarmic JIT backend, virtual memory, ELF loader, dispatcher
│   ├── hle/                      # Public declarations of libc HLE handlers
│   ├── filesystem/               # Path mapping, virtual handles, I/O with autoflush
│   ├── platform/                 # AAudio stream, EGL window, letterboxing and PCM ring buffer
│   └── compatibility/            # Binary patches, game hooks, guest symbols and synthetic JVM
│
├── src/                          # Implementation of each subsystem (C99 & C++20)
│   ├── bridge/                   # bridge_core.c, jni_exports.c
│   ├── emulator/                 # dynarmic_backend.cpp, elf_loader.c, guest_memory.c
│   ├── hle/                      # libc_string.c, libc_time.c, libc_stdio.c, net_stubs.c, gles1.c, hle_table.c
│   ├── filesystem/               # path_mapper.c, file_handles.c, file_operations.c, directory_operations.c, filesystem_logger.c
│   ├── platform/                 # android_window.c, android_audio.c, pcm_ring.c, jni_env_helper.c
│   └── compatibility/            # game_patches.c, game_hooks.c, jni_virtual_machine.c, jni_methods.c, jni_audio.c
│
├── tests/                        # Automated host tests (100% CTest pass rate)
│   ├── filesystem/               # test_path_mapper, test_file_operations, test_host_io_persistence
│   ├── platform/                 # test_pcm_ring
│   ├── emulator/                 # test_guest_memory
│   ├── test_hle_libc.c           # Tests for negative fseek, 4-byte time, Bionic stat and sscanf
│   └── golden/                   # golden_save_runner.c and documentation of real save dumps
│
├── scripts/                      # Portable automation and CI scripts
│   ├── make_base_apk.sh          # Generates base_template.apk from the original APK
│   ├── build.sh                  # Cross-compiles with the Android NDK (outputs build/android/)
│   ├── package.sh                # Injects the .so into the APK, zipalign and apksigner signing
│   ├── deploy.sh                 # Installs on a physical device/emulator via ADB
│   ├── run_tests.sh              # Builds and runs CTest on the host in isolation
│   └── collect-logs.sh           # Continuous, filtered log capture (GunBrosARM64, GunBrosFS)
```

> **Archive note**: The previous historical code has been purged from the working tree and is preserved in full under the Git tag `legacy-archive`.

---

## Quick Start

### 1. Set Up the Environment and Dependencies
Check your system's tools and automatically download/configure missing dependencies:
```bash
./setup.sh
# Or, for unattended automatic download of the NDK, Build-Tools, Boost and Dynarmic:
./setup.sh --download
```

### 2. Generate the Base Template (`base_template.apk`)
Supply your legitimate copy of the original Gun Bros v1.1.9 APK (ARM32):
```bash
./scripts/make_base_apk.sh /path/to/GunBros_original.apk
```

### 3. Build the Complete Android APK
Compiles the native library to `build/android/libandroidplatformjni.so`, injects it into the base APK, performs 4-byte alignment (`zipalign`) and cryptographic signing:
```bash
./build.sh
```
The final generated file is: `GunBros_Android16_Dynarmic.apk`.

### 4. Run the Automated Test Suite (Host)
Builds and runs the 7 test suites through **CTest** on your computer (no Android phone required):
```bash
./scripts/run_tests.sh
```

### 5. Install and Run on Your Android Device
Connect your Android device over USB with ADB debugging enabled:
```bash
./scripts/deploy.sh
```
*Note*: The script automatically applies the `--bypass-low-target-sdk-block` flag required by Android 14, 15 and 16 for applications with a legacy target SDK.

### 6. View Logs and Live Diagnostics
```bash
./scripts/collect-logs.sh
```

---

## Filesystem and Persistence Status

Following the audit and fixes in the October 2026 review, the following critical defects in the HLE file and memory layer were fixed:

1. **Sign fix in `h_fseek`**: Fixed the cast that turned negative guest offsets (commonly used with `SEEK_END` or `SEEK_CUR`) into positive 64-bit integers (~4 GB), which caused subsequent reads to come back empty.
2. **Width isolation in `h_time`**: Fixed the write of the host's 64-bit `time_t`, which overflowed into adjacent 32-bit guest memory. Now exactly one `uint32_t` is written.
3. **`struct stat` layout identical to Bionic ARM32**: Implemented the exact 104-byte Bionic mapping (`st_ino@12`, `st_mode@16`, `st_nlink@20`, `st_uid@24`, `st_gid@28`, `st_size@48` [8 bytes], `st_blksize@56`, `st_blocks@64` [8 bytes], `st_atime@72`, `st_mtime@80`, `st_ctime@88`), avoiding misalignments with `memcpy`.
4. **Width normalization in `h_sscanf`**: The `%ld`/`%lu`/`%lx` specifiers are normalized to 4-byte types so that the 64-bit host `sscanf` does not overwrite adjacent guest integers, and an unlimited number of stack arguments is supported.
5. **Informative stubs**: Removed the silent failure of unimplemented functions (`fprintf`, `fdopen`, `mkstemp`, `close`, `dup`, `ioctl`, `puts`); they now log a warning with the guest's `LR` value and alert if streams other than `stdout`/`stderr` suffer lost writes.
6. **Save tracing under `BRIDGE_DEBUG`**: All opens, reads, seeks and closes of save files (`savegame.dat`, `prefs.dat`, etc.) are traced, and a hex dump of the first 64 bytes is emitted when files opened in write mode are closed.
7. **Diagnostic protocol**: To check whether a game session keeps its progress or is reset by the internal logic, follow the protocol detailed in [docs/save-debugging.md](docs/save-debugging.md).

---

## Technical Documentation Index

For an in-depth understanding of each aspect of the project, see the dedicated guides in the `docs/` folder:

1. [**System Architecture** (`docs/architecture.md`)](docs/architecture.md): Subsystem diagram, HLE modularization, virtual memory map and multithreaded synchronization model.
2. [**Filesystem and Persistence** (`docs/filesystem.md`)](docs/filesystem.md): Scoped Storage, Glu virtual schemes (`data:`, `temp:`), atomic saving and Bionic ARM32 `stat` layout.
3. [**Save Diagnostic Protocol** (`docs/save-debugging.md`)](docs/save-debugging.md): Step-by-step protocol for diagnosing reads, overwrites and hexdumps across sessions.
4. [**Compatibility and HLE Layer** (`docs/compatibility.md`)](docs/compatibility.md): Table-driven binary patches, character hooks, synthetic JVM and decoupled JNI methods.
5. [**Build Guide** (`docs/build-guide.md`)](docs/build-guide.md): Prerequisites, generating `base_template.apk`, NDK build and packaging.
6. [**Testing Guide** (`docs/testing.md`)](docs/testing.md): Unit testing methodology, HLE libc test suite and real-save runner.
7. [**Debugging Guide** (`docs/debugging.md`)](docs/debugging.md): Unified Logcat tags (`GunBrosARM64`, `GunBrosFS`), Watchdog dump and troubleshooting.
