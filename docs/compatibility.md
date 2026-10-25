# Compatibility Layer and HLE Emulation: GunBros ARM64 Bridge

## 1. Overview

The compatibility layer (`include/gunbros/compatibility/`, `src/compatibility/` and `src/hle/`) makes it possible to run the original 32-bit binary `libandroidplatform.so` on modern Android without access to the game's source code. 

It combines four core mechanisms:
1. **Centralized Symbols and Addresses (`guest_symbols.h`)**: Centralization of all the guest's magic addresses, constants and offsets in a single typed header.
2. **Declarative Binary Patch Table (`game_patches.c`)**: Controlled hot modification with validation of original opcodes (`expected_old`) and capture under `BRIDGE_DEBUG`.
3. **Synthetic, Modular JNI Environment (`jni_virtual_machine.c`, `jni_methods.c`, `jni_audio.c`)**: JNI method dispatch through a decoupled associative table and an isolated audio subsystem.
4. **Modular HLE Dispatch (`src/hle/`)**: Interception of 178 system and graphics functions organized by module (`libc_string`, `libc_time`, `libc_stdio`, `net_stubs`, `gles1`, `hle_table`) with dynamic resolution by name.

---

## 2. In-Memory Binary Patches (`game_patches.c`)

Immediately after the ELF loader (`elf_loader.c`) maps the segments into memory, it computes the binary's SHA-256 hash and optionally validates `EXPECTED_GUEST_SHA256`. Before static constructors run, the `game_patches_apply_all()` function applies the `s_patches[]` patch table:

```c
typedef struct {
    uint32_t addr;          // Guest virtual address
    uint32_t expected_old;  // Expected original opcode (0 if unverified)
    uint32_t new_value;     // New opcode to write
    const char *name;       // Descriptive name
    bool verified;          // true if expected_old is verified
} game_patch_entry_t;
```

### Patch Behavior and Verification
- **Verified Patches**: The original opcode in memory is checked before writing. If it matches, the patch is applied; if it differs, application is aborted and an error is logged to Logcat.
- **Unverified Patches (`expected_old == 0`)**: Applied with a warning in the log.
- **Capture Mode (`BRIDGE_DEBUG`)**: On startup, the emulator logs `[PATCH:CAPTURE]` to Logcat with the original hex value of each patch so that the author can fill in the expected opcodes.

### Catalog of Declared Patches

| ID | Virtual Address | Original Code | Patched Code | Component / Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **A** | `GUEST_PATCH_RESLOADER` (`0x00366878`) | `0x0affffc8` (`beq`) | `0xe1a00000` (`nop`) | **Resource Server Bypass**: Prevents an infinite loop in `CResourceLoader::LoadNext` that waited for a response from defunct Glu servers. |
| **B** | `GUEST_PATCH_DUALSTICK` (`0x001fd450`) | Capture pending | Multi-pointer routine | **Dual-Stick Multitouch**: Adapts touch control to handle independent pointers for moving and shooting simultaneously. |
| **C** | `GUEST_PATCH_SKELETON` (`0x0021be00`) | Capture pending | Range guard | **3D Skeleton Stability**: `CMesh::GetNodeAt` adds a bounds check to prevent segmentation faults from undefined bones. |
| **D** | `GUEST_PATCH_TIMING` (`0x0021b330`) | Capture pending | Time clamping | **High Refresh Rate Displays (90Hz / 120Hz)**: Prevents division by zero or negative time jumps in `CMesh::GetInterpolationValues`. |
| **E** | `GUEST_PATCH_MUZZLE` (`0x002c918c`) | Capture pending | Transposed matrix | **Weapon Muzzle Flash Orientation**: Fixes the orientation of the 3D muzzle flash model in `CGun` so that it lines up with the barrel. |
| **J** | `GUEST_PATCH_GAMESPY` (`0x0037a4e8`) | `0xe3a00000` / `0xe12fff1e` | `mov r0, #0` + `bx lr` | **GameSpy Network Bypass**: Skips the mandatory connection to defunct GameSpy servers (`peerchat.gamespy.com`), allowing single-player play. |

---

## 3. Game Behavior Hooks (`game_hooks.c`)

Game-specific logic decoupled from `bridge_core.c` lives in `src/compatibility/game_hooks.c`:
- **`game_hooks_on_touch_ended(int x, int y)`**: Runs on touch release events (`TouchEnded`). If the engine is in the `PLAYER_SELECT` menu state (state 5), it detects whether the user tapped the left half (Percy Gun) or right half (Francis Gun) of the screen and dispatches the character selection call (`select_char`).

---

## 4. Synthetic JNI Virtual Machine and Modular Dispatch (`jni_methods.c`, `jni_audio.c`)

Emulated JNI method dispatch no longer uses sequential `strcmp` chains in the general dispatcher. It is organized as follows:
1. **`s_jni_method_table[]` Dispatch Table**: Maps each method name to its specialized `jni_method_handler_t` handler.
2. **Audio Isolation (`jni_audio.c`)**: Independently handles `InitialiseSoundEngine`, `StartAudioStream`, `StopAudioStream`, `GetAndroidMinBufferSize`, `mp3Event` and `SetVolume`.
3. **System and Graphics Queries**: Methods such as `eglGetError`, `GetDeviceOrientation`, `OpenAPKFile`, `resdlStringEvent` and memory queries are resolved in pure, independent functions.

---

## 5. Modular HLE Dispatch (`src/hle/`)

The monolithic HLE dispatcher has been restructured into 6 cohesive modules:
- `libc_string.c`: String formatting (safe `snprintf`), memory functions and `sscanf` normalized to 32-bit widths with support for stack arguments.
- `libc_time.c`: Clock and calendar time handling with strict 4-byte writes to avoid corrupting adjacent guest memory.
- `libc_stdio.c`: Glue for file and directory operations, with sign preservation in `fseek` and the Bionic ARM32 layout in `stat`.
- `net_stubs.c`: Stubs for obsolete network primitives.
- `gles1.c`: Interception and forwarding of OpenGL ES 1.1 CM primitives and OES matrices.
- `hle_table.c`: Declarative module registration (`hle_register_module`) and dynamic resolution of ELF symbols by name (`hle_resolve_import`), logging a warning to Logcat for any imported symbol that lacks a handler.

### 5.1. File Operations
- `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `stat`, `remove`, `rename`, `opendir`, `readdir`, `closedir`, `mkdir`, `rmdir` are all routed uniformly to the `gunbros/filesystem` subsystem.

### 5.2. OpenGL ES 1.1 Graphics and Extensions
- **Base Calls**: `glViewport`, `glClear`, `glBindTexture`, `glDrawArrays`, `glDrawElements`, `glEnable`, `glDisable`, etc., are forwarded to the native OpenGL ES 1.1 CM context.
- **OES Extensions for 3D Models**: GunBros uses bone matrix palette extensions for character animations. Handlers are implemented for:
  - `glCurrentPaletteMatrixOES`
  - `glMatrixIndexPointerOES`
  - `glWeightPointerOES`
  - `glClipPlanex` and fixed-point variants (`x`)

### 5.3. Ultra-Wide Screen Adaptation (Letterboxing)
On contemporary devices with 20:9 or 21:9 screens (for example, 2400x1080 or 2772x1240 resolutions), the original game (designed for 800x480) would suffer from distortion or elements falling outside the frame.

The dispatcher intercepts `glViewport` and `glScissor` and applies an affine transformation:
$$\text{host\_x} = \text{vp\_x} + \left( \frac{\text{guest\_x} \times \text{vp\_w}}{\text{guest\_screen\_w}} \right)$$
$$\text{host\_y} = \text{vp\_y} + \left( \frac{\text{guest\_y} \times \text{vp\_h}}{\text{guest\_screen\_h}} \right)$$

The black side bars are cleared with `glClear(GL_COLOR_BUFFER_BIT)` over the full physical surface before drawing of each frame begins.

---

## 6. Watchdog Thread

To diagnose possible hangs in the emulated engine:
- A background watchdog thread monitors the execution time of each frame.
- If a frame takes more than 2.0 seconds to complete (a symptom of a possible deadlock or infinite loop in the 32-bit code), the watchdog logs to Logcat:
  - The current program counter (`PC`).
  - The stack pointer (`SP`).
  - The state of the guest CPU registers (`r0`–`r15`).
  - The last HLE call processed.
