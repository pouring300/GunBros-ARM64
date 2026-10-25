# Golden Save Tests

This directory contains the `golden_save_runner` test executable and the documentation for validating real **Gun Bros** save dumps against the HLE file layer.

## Repository Policy (Rule 5)

In accordance with the project's intellectual property rules:
- Proprietary save dumps (`savegame.dat`, `prefs.dat`, `-1_1000..-1_1019`), game assets and game code are **NOT distributed or committed to the repository**.
- When no save file is present in this directory, `golden_save_runner` runs in informational mode and passes.

## How to Contribute a Real Save for Verification

If you have an Android device or emulator running the original game or the bridge:

1. **Locate the save files on the device:**
   ```bash
   adb shell run-as com.glu.android.gunbros_free ls -l /data/data/com.glu.android.gunbros_free/files/
   ```

2. **Extract `savegame.dat` and `prefs.dat`:**
   ```bash
   adb exec-out run-as com.glu.android.gunbros_free cat files/savegame.dat > tests/golden/savegame.dat
   adb exec-out run-as com.glu.android.gunbros_free cat files/prefs.dat > tests/golden/prefs.dat
   ```

3. **Check their SHA-256 hashes:**
   ```bash
   sha256sum tests/golden/savegame.dat tests/golden/prefs.dat
   ```

4. **Run the runner:**
   ```bash
   # Via the full test suite:
   ./scripts/run_tests.sh

   # Or by running the binary directly with any file:
   ./build/host/golden_save_runner tests/golden/savegame.dat
   ./build/host/golden_save_runner tests/golden/prefs.dat
   ```

## Sequence Reproduced by the Runner

The runner replicates exactly the libc/POSIX calls issued by the ARM32 guest code:
1. `h_stat`: Checks that the file exists and gets its exact size using the 104-byte Bionic ARM32 layout.
2. `h_fopen`: Opens it in binary read mode (`"rb"`).
3. `h_fseek`: Moves the cursor to the end (`SEEK_END`) with offset 0.
4. `h_ftell`: Checks that the returned position exactly matches the size obtained from `stat`.
5. `h_fseek`: Rewinds to the start (`SEEK_SET`).
6. `h_fread`: Reads the contents into guest memory and produces a hex dump of the first 64 bytes.
7. `h_fclose`: Closes the descriptor.
8. Safe save flow (`SafeWriteFile`): writes to a temporary file (`"wb"`), validates the write and renames it atomically.
