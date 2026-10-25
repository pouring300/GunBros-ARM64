# System Architecture: GunBros ARM64 Bridge

## 1. Overview and Project Purpose

**GunBros** is a classic third-person action game ("dual-stick shooter") originally developed by **Glu Mobile** for Android 2.x–4.x (32-bit ARMv7 architecture, `armeabi-v7a`).

In recent generations of mobile processors (ARMv9-based ARM Cortex cores, found in devices running Android 14, 15 and 16), manufacturers and the operating system have completely removed hardware execution support for 32-bit instructions (AArch32). When you try to run a 32-bit application on these devices, the operating system fails immediately because the processor only supports the 64-bit instruction set (AArch64).

The goal of **GunBros ARM64 Bridge** is to let GunBros run natively, smoothly (stable 60 FPS) and safely on any modern 64-bit Android device, without needing the game's original source code, using:
1. **Dynarmic**: A high-performance dynamic recompilation (JIT) engine that translates blocks of ARM32 machine code into ARM64 instructions in real time.
2. **HLE (High-Level Emulation) Layer**: Intercepts calls to the C library (Bionic libc/libm), the POSIX filesystem and OpenGL ES 1.1 graphics, executing them at native 64-bit speed on the host device.
3. **Compatibility and Binary Patch Layer**: Fixes historical bugs in the game (hangs caused by shut-down servers, touch issues and visual aspect problems).
4. **Platform and Audio Layer**: Manages the EGL window, visual scaling with black bars (*letterboxing*) and low-latency audio via AAudio.

---

## 2. Subsystem Diagram

The project is designed around the **single responsibility** principle, strictly separating emulation, Android interoperability and the filesystem:

```
+-------------------------------------------------------------------------------+
|                                JNI Entry Points                               |
|                       (src/bridge/jni_exports.c)                              |
|                 Interacts with the Java Activity / Android OS                 |
+-------------------------------------------------------------------------------+
                                        |
+---------------------------------------v---------------------------------------+
|                                  Bridge Core                                  |
|               (src/bridge/bridge_core.c: lifecycle, render loop)              |
+-------------------+-------------------+-------------------+-------------------+
                    |                   |                   |
+-------------------v---+   +-----------v-------+   +-------v-------------------+
|     Emulator Core     |   |  Platform Layer   |   | Compatibility Layer       |
|                       |   |                   |   |                           |
| - dynarmic_backend    |   | - android_window  |   | - guest_symbols (macros)  |
| - elf_loader (SHA256) |   | - android_audio   |   | - game_patches (table)    |
| - guest_memory        |   | - pcm_ring        |   | - game_hooks (touch hack) |
| - hle_table / modules |   | - jni_env_helper  |   | - jni_virtual_machine     |
|   (libc/stdio/net/gl) |   |                   |   | - jni_methods / jni_audio |
+-----------------------+   +-------------------+   +---------------------------+
                    |                   |
+-------------------v-------------------v---------------------------------------+
|                                Filesystem Layer                               |
|                                                                               |
| - path_mapper: Redirects legacy paths to modern Scoped Storage                |
| - file_handles: Virtual descriptor table isolated from the host (1..64)       |
| - file_operations: fopen, fread, fwrite (autoflush), fseek, rename, stat      |
| - directory_operations: opendir, readdir, closedir, mkdir, rmdir              |
| - filesystem_logger: I/O auditing and diagnostics in Logcat (tag GunBrosFS)   |
+-------------------------------------------------------------------------------+
```

---

## 3. Detailed Subsystem Description

### 3.1. Bridge Subsystem (`include/gunbros/bridge/`, `src/bridge/`)
- **`bridge_core`**: Coordinates the application lifecycle (`bridge_init`, `bridge_start`, `bridge_step_frame`, `bridge_pause`, `bridge_resume`, `bridge_destroy`). Manages the global emulator lock (`g_emu_mutex`), dispatches touch events while delegating game-specific actions to `game_hooks`, and runs the guest engine's main render function (`nativeRender`).
- **`jni_exports`**: Defines the native JNI functions required by Glu Mobile's legacy Java classes (`GluPlatformActivityJNI`, `GWallet`, etc.). Its sole responsibility is to receive calls from Android, check data types and forward them to the corresponding native modules without embedding business logic.

### 3.2. Emulation and HLE Subsystem (`include/gunbros/emulator/`, `src/emulator/`, `src/hle/`)
- **`dynarmic_backend`**: C++20 wrapper around the Dynarmic JIT engine. Configures memory page tables, software interrupt (SVC) traps, coprocessor registers and compiled block caches. Executes guest ARM32 code starting from a given memory address.
- **`elf_loader`**: Parses the 32-bit ELF binary `libandroidplatform.so`. Reads the program headers (`PT_LOAD`), maps the segments into the emulated memory space, computes the SHA-256 cryptographic hash of the loaded binary (optionally checking it against `EXPECTED_GUEST_SHA256`) and dynamically resolves import symbols by name via `hle_resolve_import`.
- **`guest_memory`**: Manages the 32-bit virtual memory map, providing safe bidirectional translation functions between 32-bit guest pointers and real 64-bit host pointers.
- **Modular HLE Layer (`src/hle/`)**: Decoupled from the original monolith into specialized modules registered dynamically through declarative `{name, handler}` tables:
  - `libc_string.c`: Memory and string operations, safe `snprintf` and `sscanf` normalized to 32-bit guest widths.
  - `libc_time.c`: Time functions (`time`, `clock_gettime`, `localtime_r`, etc.), restricting writes to 4-byte integers to avoid memory corruption.
  - `libc_stdio.c`: File glue that connects `fopen`, `fread`, `fwrite`, `fseek` (with sign preserved) and `stat` to `gunbros_fs_core`.
  - `net_stubs.c`: Network stubs for obsolete socket APIs and debugging interfaces.
  - `gles1.c`: Interception and forwarding of OpenGL ES 1.1 CM primitives and extensions.
  - `hle_table.c`: Central registry of HLE modules, PLT stub allocation and SVC dispatch from Dynarmic.

### 3.3. Filesystem Subsystem (`include/gunbros/filesystem/`, `src/filesystem/`)
- **`path_mapper`**: Resolves and normalizes every file path requested by the game. Redirects obsolete hardcoded paths to the application's secure private storage (Scoped Storage). Fixes Glu's truncation (`ackTOC*` -> `packTOC*`) and strips virtual prefixes (`data:`, `temp:`, `cache:`).
- **`file_handles`**: Virtual file descriptor table with indices from `1` to `64`. Protects the host's standard descriptors (`stdin`, `stdout`, `stderr`, sockets and pipes), preventing emulated code from closing or altering them.
- **`file_operations`**: Implements `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `stat`, `remove` and `rename`. Applies immediate buffer flushing (`fflush`) and provides a `struct stat` layout identical to Bionic ARM32's (104 bytes).
- **`directory_operations`**: Emulates POSIX directory operations (`opendir`, `readdir`, `closedir`, `mkdir`, `rmdir`).
- **`filesystem_logger`**: I/O auditing and diagnostics in Logcat under the unified `GunBrosFS` tag.

### 3.4. Platform Subsystem (`include/gunbros/platform/`, `src/platform/`)
- **`android_window`**: Interface to Android's `ANativeWindow` and the EGL context (OpenGL ES 1.1 CM) with letterboxing support.
- **`android_audio`**: Low-latency AAudio-based audio engine fed from a ring buffer.
- **`pcm_ring`**: Lock-free single-producer, single-consumer (SPSC) byte ring buffer with 64 KB capacity.
- **`jni_env_helper`**: Safe management of `JNIEnv*` pointers and global references across threads.

### 3.5. Compatibility Subsystem (`include/gunbros/compatibility/`, `src/compatibility/`)
- **`guest_symbols.h`**: Centralizes all magic addresses, numeric constants and structure offsets of the ARM32 guest binary (avoiding values scattered throughout the code).
- **`game_patches`**: Declarative table of binary patches `{addr, expected_old, new_val, name}`:
  - Optionally validates the original opcodes before hot-writing.
  - Supports a capture mode under `BRIDGE_DEBUG` to log the original opcodes of unverified patches.
  - Aborts applying the patch and logs an error when the expected bytes do not match.
- **`game_hooks`**: Hosts hooks for game-specific behavior, such as touch detection for character selection in `TouchEnded`.
- **`jni_virtual_machine`**: Manages the emulated synthetic `JavaVM` and `JNIEnv` structures.
- **`jni_methods` and `jni_audio`**: JNI method dispatch through an associative `{name, handler}` table decoupled from `strcmp` chains, isolating the audio subsystem from system queries.

---

## 4. Emulated Virtual Memory Map

Dynarmic requires a contiguous, predictable 32-bit address space. Virtual memory is organized as follows:

| Guest Address Range | Size | Purpose |
| :--- | :--- | :--- |
| `0x00000000 - 0x00600000` | 6 MB | Code (.text) and data (.data, .rodata, .bss) segments of `libandroidplatform.so`. |
| `0x60000000 - 0x70000000` | 256 MB | Emulated heap for dynamic allocations (`malloc`, `calloc`, `realloc`, `operator new`). |
| `0x70000000 - 0x70800000` | 8 MB | Guest execution stack (Guest Stack), growing downward from `0x707ffff0`. |
| `0x80000000 - 0x80020000` | 128 KB | HLE SVC interrupt stub table and injected helper code. |
| `0xffff0000 - 0x100000000` | 64 KB | 32-bit Linux commpage (support for kernel helper calls such as `__kuser_cmpxchg`). |

---

## 5. Concurrency and Synchronization Model

1. **Main Emulator Lock (`g_emu_mutex`)**:
   Serializes access to the Dynarmic instance. Ensures that only one thread at a time executes emulated guest CPU code, while still allowing reentrant calls from HLE routines.
2. **Lock-Free Audio Ring Buffer**:
   The emulator thread writes PCM audio samples while the AAudio callback thread consumes them. Because it uses an SPSC ring buffer design with atomic memory barriers (`__ATOMIC_ACQUIRE` and `__ATOMIC_RELEASE`), no locks or mutexes are needed, eliminating clicks or pauses in the sound.
3. **File Descriptor Isolation**:
   The virtual handle table is synchronized with `s_handles_mutex`. Opens, reads and closes are safe even if the game engine uses background loading threads for resources.
4. **Decoupled Touch Input Queue**:
   Touch events coming from the Android user interface are queued in a mutex-protected list (`s_touch_mutex`). At the start of each frame, the render thread drains this queue and hands it to the emulated engine, avoiding frame drops caused by touch event congestion.
