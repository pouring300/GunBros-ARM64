# Change Report and Technical Review: GunBros ARM64 Bridge

**Completion date**: October 2026  
**Working branch**: `refactor/review-2026-10`  
**Archive tag**: `legacy-archive`  
**Test status**: 10/10 passed (100% CTest pass rate)  
**NDK build status**: Successful (`build/android/libandroidplatformjni.so` and `GunBros_Android16_Dynarmic.apk` generated)

---

## 1. Summary of Actions by Item

### Phase 1: Persistence Defect Fixes (Highest Priority)

* **1.1 Sign fix in `h_fseek` and return value audit**:
  * **Action**: In `src/hle/libc_stdio.c` (previously `hle_dispatcher.c`), the offset cast in `h_fseek` was fixed. Originally `(long)c->r1` interpreted the `uint32_t` value as a positive unsigned 64-bit integer (~4 GB) for negative offsets with `SEEK_END` or `SEEK_CUR`. It was changed to `(long)(int32_t)c->r1`. `h_ftell` was audited, confirming that it returns `(uint32_t)-1` on errors.
  * **Verification**: Verified with the new unit test `test_fseek_negative_offset` in `tests/test_hle_libc.c`, validating negative offsets from `SEEK_END` and `SEEK_CUR` with exact data reads.
  * **Commit**: `7ceccae fix(hle): preserve sign in h_fseek offset and audit ftell error return (1.1)`

* **1.2 Width overflow fix in `h_time` and time function audit**:
  * **Action**: `h_time` wrote a 64-bit (host) `time_t` into the 32-bit guest's memory if `c->r0 != 0`, overwriting 4 contiguous bytes of adjacent memory. It was fixed by forcing a `uint32_t` write via `guest_ptr(c->r0, 4)`. `gettimeofday`, `clock_gettime`, `difftime`, `localtime`, `gmtime`, `mktime`, `ctime` and `strftime` were audited, ensuring 32-bit limits and pointer validation.
  * **Verification**: Verified with `test_time_4byte_write` in `tests/test_hle_libc.c`, using a contiguous 4-byte control word (*canary*) to confirm there is no memory overflow.
  * **Commit**: `33f091f fix(hle): write 32-bit uint32_t in h_time and bounds-check time handlers (1.2)`

* **1.3 Full population of `struct stat` with the Bionic ARM32 layout**:
  * **Action**: In `src/filesystem/file_operations.c`, `gunbros_stat` only filled in `st_mode` and `st_size`. The Bionic headers in Android NDK r26d (`asm-generic/stat.h`) were cross-checked and the exact 104-byte layout was implemented:
    * `ino` @ 12 (4B)
    * `mode` @ 16 (4B)
    * `nlink` @ 20 (4B)
    * `uid` @ 24 (4B), `gid` @ 28 (4B)
    * `size` @ 48 (8B, uint64_t)
    * `blksize` @ 56 (4B)
    * `blocks` @ 64 (8B, uint64_t)
    * `atime` @ 72 (4B), `atime_nsec` @ 76 (4B)
    * `mtime` @ 80 (4B), `mtime_nsec` @ 84 (4B)
    * `ctime` @ 88 (4B), `ctime_nsec` @ 92 (4B)
    * `ino64` @ 96 (8B)
    * Total size: 104 bytes. Safe writing via `memcpy` with no unaligned accesses.
  * **Verification**: Verified in `tests/filesystem/test_file_operations.c` and in `tests/test_hle_libc.c` (`test_stat_bionic_layout`).
  * **Commit**: `7c76a1e fix(fs): populate full bionic ARM32 struct stat layout using memcpy and add host test (1.3)`

* **1.4 Width normalization in `h_sscanf` and support for stack arguments**:
  * **Action**: `h_sscanf` only accepted 2 register arguments (`r2`, `r3`) and passed pointers to the native `sscanf` with `%ld`/`%lu` specifiers, causing 8-byte writes into the guest's 4-byte integers. A format normalization function (`normalize_scanf_format`) was implemented that downgrades `%ld`/`%lu`/`%lx` to `%d`/`%u`/`%x`, and the variadic dispatch was extended up to `MAX_SSCANF_ARGS` (16 arguments), reading the remaining arguments from `sp` in guest memory. A warning log was added if the number of conversions exceeds the supported arguments.
  * **Verification**: Verified with `test_sscanf_multi_args` in `tests/test_hle_libc.c` with 5 conversions (%s, %d, %ld, %u, %f), reading parameters from registers and the stack with integrity canaries intact.
  * **Commit**: `a2fa1a2 fix(hle): normalize sscanf format to guest integer width and support stack arguments (1.4)`

* **1.5 Informative stubs logging LR for unimplemented calls**:
  * **Action**: Informative handlers with a one-time warning per function (`stub_warn_once`) were created for `fprintf`, `fdopen`, `mkstemp`, `close`, `dup`, `ioctl`, `puts`. For `fprintf` and `fdopen` a critical alert was added if the output stream is not `stdout`/`stderr`, preventing silent, uncaptured writes of data to disk.
  * **Verification**: Build and inspection of debug output on host and Android.
  * **Commit**: `90c4fb2 feat(hle): add informative stub logging for fprintf, fdopen, mkstemp, close, dup, ioctl, puts (1.5)`

* **1.6 `BRIDGE_DEBUG` traces for saves and APK resources**:
  * **Action**: Traces under the `BRIDGE_DEBUG` flag were added in `gunbros_fopen` to intercept opens of key files (`savegame.dat`, `prefs.dat`, `-1_10*`), logging the mode and origin. In `OpenAPKFile`, informative logging of the request was added before returning NULL.
  * **Verification**: Build with `-DBRIDGE_DEBUG` verified with `scripts/build.sh`.
  * **Commit**: `101c18d feat(debug): add BRIDGE_DEBUG traces for savegame fopen and OpenAPKFile (1.6)`

---

### Phase 2: Diagnostics and Logging

* **2.1 Unification of Logcat tags (`GunBrosARM64` and `GunBrosFS`)**:
  * **Action**: The logging tags were unified: `GunBrosARM64` for the bridge core and emulator, and `GunBrosFS` for the filesystem subsystem (`filesystem_logger.c`). `scripts/collect-logs.sh` was updated to capture both tags (`GunBrosARM64:V GunBrosFS:V *:S`) and the examples and tags in `docs/filesystem.md` and `docs/architecture.md` were corrected.
  * **Verification**: Syntax check and execution of `scripts/collect-logs.sh`.
  * **Commit**: `5fceb1d fix(logging): unify GunBrosFS tag in filesystem_logger, collect-logs.sh and docs (2.1)`

* **2.2 Full tracing of save I/O and hexadecimal dump**:
  * **Action**: `file_operations.c` was instrumented to log, under `BRIDGE_DEBUG`, all operations on `savegame.dat` and `prefs.dat` (`stat`, `fopen`, `fread` requested vs. obtained, `fseek` with offset and whence, `ftell`, `rewind`, `fclose`). When closing a descriptor opened with mode `"wb"`, the first 64 bytes are dumped to logcat in hexadecimal format.
  * **Verification**: Verified by running `test_file_operations` and `test_hle_libc`.
  * **Commit**: `4ebdd85 feat(fs): trace savegame file operations and dump wb hexdump under BRIDGE_DEBUG (2.2)`

* **2.3 Creation of a persistence diagnostic protocol**:
  * **Action**: `docs/save-debugging.md` was written, describing a methodical 5-step procedure to distinguish between on-disk persistence failures vs. profile rejection by the engine's internal logic.
  * **Commit**: `38c695a docs: create save-debugging.md persistence diagnostic protocol (2.3)`

---

### Phase 3: Structural Refactoring

* **3.1 Decomposition of the monolithic `hle_dispatcher.c` dispatcher**:
  * **Action**: The 945-line file was split into specialized components inside `src/hle/`:
    * `src/hle/libc_string.c`: memory, strings, printf/scanf.
    * `src/hle/libc_time.c`: time and calendar.
    * `src/hle/libc_stdio.c`: files and directories.
    * `src/hle/net_stubs.c`: network and debugging stubs.
    * `src/hle/gles1.c`: OpenGL ES 1.1 CM and OES extensions.
    * `src/hle/hle_table.c`: central registry and SVC dispatch.
    * `src/hle/hle_internal.h`: common internal header.
  * **Verification**: `scripts/run_tests.sh` and `scripts/build.sh` passed.
  * **Commit**: `6841aff refactor(hle): split monolithic hle_dispatcher.c into modular src/hle/ components (3.1)`

* **3.2 Dynamic resolution of ELF imports by name**:
  * **Action**: The static array `s_plt_names[N_PLT]` was replaced with a dynamic mechanism (`hle_resolve_import`). Each `src/hle/` module exposes a `{name, handler}` table that is registered in `hle_init()`. When the ELF is loaded, `elf_loader.c` looks up the symbol by name and assigns it a dynamic PLT stub slot. If a guest symbol has no handler, an explicit error is logged to logcat.
  * **Verification**: Dynamic linking and execution tests in CTest and NDK.
  * **Commit**: `39f6a6f refactor(hle): resolve guest imports dynamically by name and register handlers per module (3.2)`

* **3.3 Centralization of magic addresses in `guest_symbols.h`**:
  * **Action**: `include/gunbros/compatibility/guest_symbols.h` was created, centralizing all fixed virtual addresses, constants and structure offsets of the 32-bit binary (`GUEST_ADDR_APP_PTR`, `GUEST_FN_SELECT_CHAR`, `GunBros` offsets, etc.). Scattered magic numbers were removed from `bridge_core.c`, `game_patches.c` and `elf_loader.c`.
  * **Verification**: Build verification with no changes to binary values.
  * **Commit**: `16f02d4 refactor(compat): centralize magic guest addresses and offsets in guest_symbols.h (3.3)`

* **3.4 Declarative patch table with opcode verification**:
  * **Action**: `game_patches.c` was reorganized around a table of `game_patch_entry_t { addr, expected_old, new_value, name, verified }` structures. If `expected_old` is defined and does not match the memory read, the patch is aborted and the error is reported. Under `BRIDGE_DEBUG`, the original opcodes found are logged to logcat (`[PATCH:CAPTURE]`) to make it easy to incorporate them offline.
  * **Verification**: Build with conditional validation and patch logging.
  * **Commit**: `38a16fe refactor(compat): convert game_patches.c to table-driven with expected_old verification and BRIDGE_DEBUG capture (3.4)`

* **3.5 SHA-256 verification of the ARM32 guest binary**:
  * **Action**: A SHA-256 algorithm in pure C99 (with no external dependencies or heavy cryptographic libraries) was implemented in `src/emulator/elf_loader.c`. When the `libandroidplatform.so` ELF is loaded, its SHA-256 is computed and logged. The `EXPECTED_GUEST_SHA256` macro was declared in `guest_symbols.h`: if empty, it only warns; if filled in and mismatched, it refuses to apply the binary patches.
  * **Verification**: Validation of the computed hash against `sha256sum` of the guest binary in host tests.
  * **Commit**: `a2fa988 feat(loader): verify loaded guest ARM32 ELF SHA-256 against EXPECTED_GUEST_SHA256 (3.5)`

* **3.6 Decoupling of game hooks and JNI method table with isolated audio**:
  * **Action**:
    1. The character selection logic in `TouchEnded` was extracted from `bridge_core.c` to `src/compatibility/game_hooks.c` (`game_hooks_on_touch_ended`).
    2. The sequential `strcmp` chains in `jni_virtual_machine.c` were replaced with a declarative table `s_jni_method_table[]` in `src/compatibility/jni_methods.c`.
    3. The JNI audio routines (`InitialiseSoundEngine`, `StartAudioStream`, etc.) were split out into `src/compatibility/jni_audio.c`.
  * **Verification**: Verification on host and NDK build.
  * **Commit**: `242092c refactor(compat): extract character select to game_hooks and table-drive JNI methods with isolated audio (3.6)`

* **3.7 Documentation of the provenance of the `guest_helpers_bin.h` blob**:
  * **Action**: A header comment was added in `guest_helpers_bin.h` and a detailed section in `docs/compatibility.md`, documenting that the 460-byte binary array contains the injected ARM32 machine code for `qsort`/`heapsort` with guest callbacks, whose original source is not in the repository and is pending contribution by the author.
  * **Commit**: `f3dc1ad docs(compat): document provenance of guest_helpers_bin blob (3.7)`

---

### Phase 4: Test Suite

* **4.1 Host unit tests for HLE libc (`test_hle_libc`)**:
  * **Action**: The `libc_string.c`, `libc_time.c` and `libc_stdio.c` handlers were decoupled from any graphics (OpenGL/EGL) and Dynarmic dependency via the `include/gunbros/hle/hle_libc.h` header. `tests/test_hle_libc.c` was created to verify:
    * `h_fseek` with negative offsets (`SEEK_END`, `SEEK_CUR`) and `h_ftell`.
    * `h_time` writing only 4 bytes (no overflow into contiguous guest memory).
    * `h_stat` verifying the 104-byte Bionic ARM32 layout and trailing canary.
    * `h_sscanf` with multiple register and stack arguments, normalizing `%ld`.
  * **Verification**: Run with CTest (100% pass).
  * **Commit**: `288dc61 test(hle): make libc handlers testable on host and add test_hle_libc (4.1)`

* **4.2 Renaming of the host persistence test and runner for real saves (`golden_save_runner`)**:
  * **Action**: `test_persistence.c` was renamed to `tests/filesystem/test_host_io_persistence.c`, with a header clarifying that it validates the host's atomic I/O semantics with synthetic structs. `tests/golden/golden_save_runner.c` was implemented, reproducing the guest's exact call sequence (`stat` -> `fopen "rb"` -> `fseek SEEK_END` -> `ftell` -> `rewind` -> `fread` -> `fclose` -> `SafeWriteFile`). `tests/golden/README.md` was created, explaining how to contribute a real dump (`savegame.dat`).
  * **Verification**: Both binaries run within the CTest suite.
  * **Commit**: `63bf1be test(fs): rename persistence test to test_host_io_persistence and add golden save runner with docs (4.2)`

* **4.3 Test integration in CMake/CTest and isolated execution**:
  * **Action**: The base tests were registered in `CMakeLists.txt`, with isolation and cleanup of temporary files (`/tmp/gunbros_*`) in `scripts/run_tests.sh`.
  * **Verification**: `./scripts/run_tests.sh` runs the full suite successfully.
  * **Commit**: `43f571b test(ci): register new test targets in CMake and ensure run_tests.sh executes suite with isolation (4.3)`

* **4.4 Cross-Verification and Strict Equivalence Suites (Technical Review)**:
  * **Action**: In response to the technical audit, to harden the highest-risk changes without depending on a physical device:
    1. **`test_patch_equivalence`**: The patch table (`game_patches_apply_all`) was compared on host against the original `legacy-archive` logic over 5 MB buffers, certifying byte-for-byte equivalence (0 bytes of difference across 5,242,880 bytes analyzed).
    2. **`test_hle_resolution`**: Dynamic name-based resolution of the 178 symbols imported by the original ELF (`s_plt_names`) was verified, guaranteeing that each symbol resolves to a non-null function pointer and that repeated lookups are idempotent.
    3. **`test_sha256`**: The C99 SHA-256 implementation was extracted to `src/common/sha256.c` and verified against the official NIST / RFC 6234 vectors (empty string, "abc", 56-byte block-boundary vector and chunked streaming).
    4. **`golden_save_runner`**: It was updated to generate a synthetic binary fixture and always validate the full cycle of 8 guest I/O operations (`stat`, `fopen`, `fseek`, `ftell`, `fread`, `fwrite`, `rename`, `remove`) on every CTest run, additionally running against `savegame.dat` if present.
  * **Verification**: CTest runs 10 of 10 tests with 100% success.

---

### Phase 5: Repository Hygiene

* **5.1 Preservation with the `legacy-archive` tag and removal of `legacy/`**:
  * **Action**: The existence of the immutable `legacy-archive` git tag preserving the previous code was confirmed. The `legacy/` folder was removed from the working tree and `README.md` was updated.
  * **Commit**: `d88a0bf chore: purge legacy/ directory after preserving under legacy-archive git tag (5.1)`

* **5.2 Removal of `libandroidplatformjni.so` from the repository root**:
  * **Action**: The prebuilt binary file in the root was removed with `git rm` and the `cp "$SO_PATH" "$PROJECT_ROOT/libandroidplatformjni.so"` directive was dropped from `scripts/build.sh`. The build output is now confined to `build/android/`.
  * **Commit**: `4ea4800 build: remove root libandroidplatformjni.so and stop copying from build.sh (5.2)`

* **5.3 `make_base_apk.sh` generator script and validation in `package.sh`**:
  * **Action**: `scripts/make_base_apk.sh` was created; it takes the path to an original Gun Bros v1.1.9 APK, verifies the presence of `assets/libandroidplatformjni.so`, cleans the stale `META-INF/` signatures and generates `base_template.apk`. `package.sh` was updated to emit a clear, explanatory error message if the base template does not exist.
  * **Commit**: `0e508f3 feat(build): add make_base_apk.sh script and explicit missing template error in package.sh (5.3)`

* **5.4 Creation of `LICENSE` and non-distribution notice in `README.md`**:
  * **Action**: The `LICENSE` file was created, stating that the formal choice of license for the bridge code is pending from the author, and including the Glu Mobile / EA copyright notice. A prominent notice was added to `README.md` stating that the repository neither contains nor distributes APKs or assets.
  * **Commit**: `ea4af9d docs(legal): add LICENSE disclaimer and README non-distribution notice (5.4)`

* **5.5 Update and hardening of `.gitignore`**:
  * **Action**: `.gitignore` was reviewed and updated to strictly cover: `build/`, `logs/`, `*.apk`, `*.so`, `*.idsig`, `tools/`, `third_party/dynarmic` and `third_party/boost`, while preserving the versioned keystore in `third_party/keystore/debug.keystore`.
  * **Commit**: `640c458 chore: update .gitignore to cover logs, build, binaries, tools and third_party (5.5)`

---

### Phase 6: Documentation and Delivery

* **6.1 Comprehensive update of `README.md`**:
  * **Action**: Directory tree synchronized with the actual repository structure; persistence status updated with the fixed defects; technical links checked; build workflow documented with `make_base_apk.sh`.
  * **Commit**: `ee4bf25 docs(readme): update structure, persistence status, doc links and build workflow (6.1)`

* **6.2 Update of `docs/architecture.md` and `docs/compatibility.md`**:
  * **Action**: The architectural changes were documented: HLE modularization, declarative patch table with capture, `game_hooks.c` and decoupling of JNI methods and audio.
  * **Commit**: `edfe813 docs(arch,compat): document HLE modularization, table-driven patches and decoupled JNI (6.2)`

---

## 2. Discrepancies Found Relative to the Previous Report

During analysis of the actual code, the following discrepancies with the previous review document were identified:

1. **Location of `safe_guest_snprintf`**: The report assumed that all string formatting lived in `hle_dispatcher.c`. In the actual code, `safe_guest_snprintf` was implemented at the end of `src/compatibility/jni_virtual_machine.c`. To make the libc HLE layer self-contained and testable on host without pulling in Android dependencies, `safe_guest_snprintf` was cleanly relocated to `src/hle/libc_string.c`.
2. **Local presence of `base_template.apk`**: The report stated that `base_template.apk` did not exist in the repository. In the user's local workspace there was a 223 MB file not versioned in Git. It was kept out of Git tracking via `.gitignore`, and `scripts/make_base_apk.sh` was implemented to guarantee that any developer can generate it reproducibly without violating Rule 5.
3. **Non-variadic vs. variadic JNI dispatch**: It was observed that the JNI method index for `CallNonvirtual*Method` (`idx 64..93`) receives `clazz` in `r2` and `methodID` in `r3`, unlike the standard `CallObjectMethod`, which receives `methodID` in `r2`. The normalization in `jni_methods_dispatch` unified this calculation, fixing dispatch discrepancies in non-virtual methods.
4. **Declaration of `strptime` on Host**: On Linux glibc, `strptime` requires explicitly defining `_GNU_SOURCE` or `_XOPEN_SOURCE` before `<time.h>`. `#define _GNU_SOURCE` was added to `libc_time.c` to allow the host test to build cleanly without altering Bionic behavior.

---

## 3. Items Pending from the Author ("Pending from the Author")

The following items have been technically prepared but require direct contribution or a decision from the project author:

1. **`EXPECTED_GUEST_SHA256` Cryptographic Hash (`include/gunbros/compatibility/guest_symbols.h`)**:
   * *Current status*: Defined as an empty string `""`. The `elf_loader.c` loader computes the SHA-256 of the loaded binary and prints it to Logcat (`Loaded guest ARM32 ELF SHA-256: <hash>`).
   * *Required action*: Copy the hash obtained in a run session of the original APK and assign it to `EXPECTED_GUEST_SHA256` to enable strict blocking of corrupt binaries or incompatible versions.

2. **Original Opcodes for `expected_old` in `game_patches.c`**:
   * *Current status*: Patches A (`0x00366878`) and J (`0x0037a4e8`) are marked as verified (`verified = true`). Patches B, C, D and E have `expected_old = 0` and `verified = false`.
   * *Required action*: Boot the game on an emulator or device with `BRIDGE_DEBUG` enabled. The emulator will emit lines with the tag `[PATCH:CAPTURE] Patch '<name>' original opcode at 0x... is 0x...`. Copy those values into the `expected_old` field of each entry in `s_patches[]` and set `verified = true`.

3. **Source Code of the ARM32 Helper (`guest_helpers_bin.h`)**:
   * *Current status*: The precompiled 460-byte blob injected at `0x8000d000` is retained. In accordance with Rule 7, no code was invented or blindly decompiled.
   * *Required action*: If the author still has the original assembly or C file (for example `guest_qsort.c` or `guest_helpers.s`), add it in a `src/guest_helpers/` folder along with an assembly script using the NDK's 32-bit toolchain.

4. **Formal License Choice (`LICENSE`)**:
   * *Current status*: `LICENSE` file with legal reservation text and a copyright notice.
   * *Required action*: The author must choose the desired open source license (MIT, Apache 2.0, GPLv2/v3). Note that the `Dynarmic` dependency is licensed under BSD-3-Clause.

5. **Contribution of a Real Save Dump (`tests/golden/`)**:
   * *Current status*: `golden_save_runner.c` runs and validates the full I/O cycle with a synthetic fixture in every test, and will additionally run the validation against `tests/golden/savegame.dat` when it is available.
   * *Required action*: Extract a `savegame.dat` from a real playthrough and place it at `tests/golden/savegame.dat` to run the validation with real game data.

6. **Publishing on GitHub and History of Past Binaries**:
   * *Current status*: On the current `refactor/review-2026-10` branch, the working tree and Git index are 100% clean of `.so` and `.apk` files (`git ls-files` does not track any binary). However, in historical commits prior to the start of this refactor (`f0243d9`, `a0f6da3`, `6148f65` on `main`) there were tracked binaries.
   * *Required action*: Before making the repository public on GitHub, if a history fully purged of past heavy blobs is desired, the author can run `git filter-repo --invert-paths --path-glob '*.so' --path-glob '*.apk'` or publish a new repository initialized from this clean branch.
