# Filesystem and Persistence: GunBros ARM64 Bridge

## 1. The Storage Challenge on Modern Android

In the Android 2.x to 4.x era, apps had practically unrestricted read and write access to external storage (`/sdcard/` or `/mnt/sdcard/`) and internal paths (`/data/data/<package>/`). The GunBros engine was programmed assuming these fixed paths.

Starting with Android 10, and strictly in Android 11 through 16, the operating system enforces **Scoped Storage** and strict **SELinux** policies:
- Apps cannot create or modify arbitrary files in the root of `/sdcard/`.
- Any attempt to directly access obsolete paths without going through the compatibility layer results in permission denied (`EACCES`) or path not found (`ENOENT`).
- Background processes are aggressively frozen by the operating system (*Cached App Freezer*), which discards any user-space memory buffer (`stdio`) that has not been explicitly flushed to disk via system calls.

To solve this without modifying the game's original source code, the filesystem subsystem (`src/filesystem/`) emulates a complete POSIX layer with transparent path redirection.

---

## 2. Path Normalization and Mapping (`path_mapper`)

All input calls involving file paths (`fopen`, `stat`, `remove`, `rename`, `mkdir`, `opendir`, `rmdir`) are processed by the `path_mapper_normalize()` function.

### Transformation Rules Applied

1. **Redirection to the App's Private Storage**:
   Any path referring to `/sdcard/glu/gunbros/`, `/data/data/com.glu.android.gunbros_free/files/` or relative file names is redirected to the app's private files directory:
   ```
   /sdcard/glu/gunbros/savegame.dat -> <APP_EXTERNAL_FILES_DIR>/files/savegame.dat
   ```
   If external storage is not available, the internal directory (`<APP_INTERNAL_FILES_DIR>/files/`) is used as a fallback.

2. **Detection and Removal of Glu Virtual Schemes**:
   The Glu Mobile engine uses virtual protocol schemes to refer to different locations:
   - `data:` -> Game data directory.
   - `temp:` -> Temporary files directory.
   - `cache:` -> Engine cache.
   - `apk:` -> Files packaged in the APK.

   In the game's C++ code, path strings are often concatenated with the base directory before calling operating system functions, resulting in hybrid paths such as:
   ```
   <APP_DIR>/files/data:/prefs.dat
   <APP_DIR>/files/temp:/temp.dat
   ```
   The Linux kernel would interpret `data:` as a subfolder that does not exist on the filesystem, causing an `ENOENT` failure (Error 2: No such file or directory).
   The normalizer detects the presence of these schemes and extracts the real file name, ensuring the path points directly to:
   ```
   <APP_DIR>/files/prefs.dat
   ```

3. **Prefix Correction for Resource Files (`ackTOC`)**:
   In the Glu engine, a long-standing string manipulation bug truncated the first character of package table-of-contents files (`packTOC_*.dat`). If the game requests a file whose name starts with `ackTOC`, the normalizer automatically prepends the missing letter `p` (`packTOC`), allowing textures and sounds to load smoothly.

4. **Automatic Resolution Substitution (WVGA to XGA)**:
   If the game requests a texture atlas for WVGA resolution (`_wvga.png`) that is not present in the installed data, the mapper checks for the existence of the high-definition XGA variant (`_xga.png`) and redirects the open without the engine failing.

---

## 3. Atomic Save Mechanics in GunBros

Through reverse engineering and disassembly of the `libandroidplatform.so` library, the main persistence routine was identified in the functions `CSaveGameMgr::Write`, `COptionsMgr::Write` and `CFileUtil::SafeWriteApplicationSaveGameFile`.

The game uses an atomic write pattern to prevent a power failure or unexpected shutdown from corrupting the original files:

```
[Step 1] fopen("temp.dat", "wb")
               |
               v
[Step 2] fwrite(updated_data) -> fflush() -> fclose()
               |
               v
[Step 3] remove("data:/savegame.dat")    (or prefs.dat)
               |
               v
[Step 4] rename("temp.dat", "data:/savegame.dat")
```

If in Step 3 or 4 the path were not normalized or permissions failed, the `temp.dat` file would be left orphaned and the final `savegame.dat` file would be deleted or not updated.

---

## 4. Virtual Descriptor Table (`file_handles`)

The emulated 32-bit code expects to work with numeric file identifiers or `FILE*` pointers. Returning direct native pointers or raw file descriptors from the host operating system can cause serious failures if the guest code performs improper operations or assumes 32-bit values.

The file layer implements a **virtual descriptor table**:
- It assigns integer identifiers from `1` to `64` (`GUNBROS_MAX_OPEN_FILES`).
- Each slot stores the native `FILE*` pointer, the open mode (`"rb"`, `"wb"`, etc.), the original path and the normalized path.
- The host's slots `0` (stdin), `1` (stdout) and `2` (stderr) are fully protected; no game operation can close or overwrite the system's standard outputs.
- Safety guards against double closes are implemented (*double-close protection*).

---

## 5. Forced Flush to Disk (`file_operations`)

The C standard library (`stdio`) uses user-space memory buffers to optimize writes. However, on mobile devices, the user can switch apps or close the game from the recent apps menu at any time.

To avoid losing blocks that have not yet been physically sent to flash storage:
1. `gunbros_fwrite()` automatically invokes `fflush()` after every write operation.
2. `gunbros_fclose()` performs a final `fflush()` before invoking `fclose()` on the host operating system.

---

## 6. Current State of Progress Persistence

### What Works Correctly
- Operating system and kernel level operations (`fopen`, `fwrite`, `fflush`, `remove`, `rename`) are fully verified and work without errors on real devices running modern Android.
- Configuration files (`prefs.dat`), temporary files and user profile containers (`-1_1000` to `-1_1019`, `-1_PDST`) are created, written and atomically renamed in the app folder with success confirmation:
  ```
  [FS:rename] SUCCESS '/storage/.../files/temp.dat' -> '/storage/.../files/prefs.dat'
  [FS:fopen] SUCCESS slot=1 mode='wb' path='/storage/.../files/-1_1000'
  ```
- The cross-process persistence test suite (`tests/filesystem/test_persistence.c`) passes 100% of its checks.

### Open Issue to Investigate (Internal Game Logic)
Even though the files are physically saved to device storage, when the app is fully restarted, the game engine resets the player's visible progress (coins, purchased weapons and campaign level revert to their default values).

### Technical Hypotheses for Contributors and Researchers

If you want to help fix progress persistence, these are the key areas identified during reverse engineering:

1. **Device Identity Dependency / Save Encryption**:
   - Many Glu Mobile titles of this generation derived an XOR or AES encryption key for `savegame.dat` from device data returned by JNI calls:
     - `TelephonyManager.getDeviceId()` (IMEI)
     - `WifiInfo.getMacAddress()`
     - `Settings.Secure.ANDROID_ID`
   - In our emulation layer (`jni_virtual_machine.c`), these values return predefined synthetic responses. If the returned value is not consistent, or if the engine detects a change in some machine parameter, it considers the save to belong to another device and discards it.

2. **Invalid File Detection and Forced Reset in `CSaveGameMgr::Read`**:
   - The save file read routine is located at virtual address `0x00252b90`.
   - If the read fails, if the file size does not match the expected header, or if the internal checksum does not match, the engine immediately branches to `0x00252cc4`, where it executes:
     1. `CSaveGameMgr::Reset()` (clears all in-memory values).
     2. `CSaveGameMgr::Write()` (overwrites the file on disk with the reset save).
   - It is recommended to disassemble and place logging points (*trace logging*) at `0x00252b90` through `0x00252cf0` to observe exactly which condition triggers the branch to the reset.

3. **File Metadata Verification (`stat`)**:
   - The game calls `stat` to check the modification time (`st_mtime`) or the size of `savegame.dat`. Ensure that the fields of the 32-bit `stat` structure returned by `hle_dispatcher.c` exactly match the Android 2.x Bionic structure.

---

## 7. Real-Time Diagnostics and Auditing

All filesystem operations emit detailed logs to Logcat under the `GunBrosFS` tag (and general emulator events under `GunBrosARM64`):

```bash
adb logcat -s GunBrosFS GunBrosARM64
```

Example audit output during execution:
```text
I GunBrosFS: [FS:fopen] SUCCESS slot=1 mode='wb' path='/storage/.../files/temp.dat' (orig='data:/temp.dat')
I GunBrosFS: [FS:fwrite] slot=1 req=1024 got=1024 errno=0
I GunBrosFS: [FS:fclose] slot=1 path='/storage/.../files/temp.dat' OK
I GunBrosFS: [FS:remove] SUCCESS '/storage/.../files/prefs.dat' (orig='data:/prefs.dat')
I GunBrosFS: [FS:rename] SUCCESS '/storage/.../files/temp.dat' -> '/storage/.../files/prefs.dat'
```
