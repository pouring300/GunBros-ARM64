# Automated Testing Guide: GunBros ARM64 Bridge

This document details the unit and integration test suite that validates the robustness, isolation and safety of the different subsystems without needing a physical Android device.

---

## 1. Testing Philosophy

A fundamental principle of this project is that **no refactoring or code change is considered correct until it has been verified by automated tests**.

The tests are designed to:
1. **Run on the host**: They compile and run directly on the development machine (Linux, macOS, WSL2) using any modern compiler (GCC or Clang).
2. **Isolate responsibilities**: They test individual components independently (filesystem, virtual memory, audio buffers).
3. **Simulate real lifecycles**: They include multi-process tests to certify data persistence in storage when the app is force-closed.

---

## 2. Catalog of Available Tests

The test suite consists of 5 executables managed through **CTest**:

```
+-------------------------------------------------------------------------------+
|                                CTest Test Suite                               |
+-------------------------------------------------------------------------------+
  |-- test_path_mapper       : Path normalization, Glu schemes and TOC rules
  |-- test_file_operations   : Virtual handles, autoflush, fseek, mkdir, rmdir
  |-- test_persistence       : Atomic save persistence across two processes
  |-- test_pcm_ring          : Lock-free audio ring buffer and wrap-around
  |-- test_guest_memory      : Virtual allocation, commpage and pointer conversion
```

### 2.1. `test_path_mapper` (`tests/filesystem/test_path_mapper.c`)
Validates the path resolution engine:
- Checks that null pointers or empty strings return valid, safe default paths.
- Verifies automatic redirection of relative paths to the app's private directory.
- Verifies removal of Glu virtual schemes (`data:`, `temp:`, `cache:`, `apk:`) both on their own and when they have been concatenated with directory prefixes.
- Checks restoration of the `p` character in `ackTOC` files (`ackTOC_*.dat` -> `packTOC_*.dat`).
- Checks WVGA-to-XGA texture fallback resolution when the original asset does not exist.

### 2.2. `test_file_operations` (`tests/filesystem/test_file_operations.c`)
Validates the virtual file descriptor table and input/output operations:
- Assigns safe descriptors in the range `1` to `64`, certifying that the operating system's descriptors `0`, `1` and `2` are not altered.
- Verifies that writes perform an immediate `fflush()`.
- Validates positioning with `fseek()` and reading with `ftell()`.
- Checks the guard against double closes (*double-close safety*).
- Validates recursive subdirectory creation via `gunbros_mkdir()`.
- Verifies the directory lifecycle (`opendir`, `closedir`, `rmdir`).

### 2.3. `test_persistence` (`tests/filesystem/test_persistence.c`)
Critical integration test that simulates the real lifecycle of two game sessions:
1. **Process 1 (Game Session)**:
   - Opens a `temp.dat` file in write mode.
   - Writes a simulated save structure with coins, stars and weapons.
   - Calls `fflush()` and closes the file.
   - Deletes `savegame.dat` and renames `temp.dat` to `savegame.dat`.
   - The process exits cleanly.
2. **Process 2 (Cold Restart)**:
   - A new independent process is started from the command line.
   - It tries to open `savegame.dat` in read mode.
   - It validates the integrity of every field of the data structure byte by byte.

### 2.4. `test_pcm_ring` (`tests/platform/test_pcm_ring.c`)
Validates the 64 KB lock-free SPSC ring buffer for audio:
- Writes blocks of simulated audio samples.
- Verifies that reading returns exactly the same bytes in strict FIFO order.
- Tests the pointer continuously crossing the buffer boundary (*ring wrap-around*).

### 2.5. `test_guest_memory` (`tests/emulator/test_guest_memory.c`)
Validates the emulator's virtual memory map:
- Checks anonymous memory allocation (`mmap`) for the code, heap and stack ranges.
- Validates bidirectional conversion between 32-bit guest addresses and real 64-bit host pointers.
- Verifies initialization of the *commpage* stubs (`__kuser_cmpxchg`) at `0xffff0fc0`.

---

## 3. How to Run the Tests

### 3.1. Using the Automated Script
The simplest way to build and run all the tests:
```bash
./scripts/run_tests.sh
```

### 3.2. Using Manual CTest Commands
```bash
# Build the tests
cmake -B build/host -S . -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/host -j$(nproc)

# Run the suite
ctest --test-dir build/host --output-on-failure --verbose
```

### Example of Successful Output:
```text
Test project .../build/host
    Start 1: test_path_mapper
1/5 Test #1: test_path_mapper .................   Passed    0.01 sec
    Start 2: test_file_operations
2/5 Test #2: test_file_operations .............   Passed    0.01 sec
    Start 3: test_persistence
3/5 Test #3: test_persistence .................   Passed    0.01 sec
    Start 4: test_pcm_ring
4/5 Test #4: test_pcm_ring ....................   Passed    0.00 sec
    Start 5: test_guest_memory
5/5 Test #5: test_guest_memory ................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 5
Total Test time (real) = 0.04 sec
```

---

## 4. How to Add New Tests

If you develop a new feature or want to test a specific patch:

1. Create your test file in the appropriate directory under `tests/`, for example:
   ```c
   // tests/compatibility/test_my_feature.c
   #include <assert.h>
   #include <stdio.h>

   int main(void) {
       printf("=== Running My Feature Test ===\n");
       assert(1 == 1);
       printf("  [PASS] Feature works correctly!\n");
       return 0;
   }
   ```

2. Register the new executable in `CMakeLists.txt` inside the `if(BUILD_TESTING)` block:
   ```cmake
   add_executable(test_my_feature tests/compatibility/test_my_feature.c)
   target_link_libraries(test_my_feature PRIVATE gunbros_common)
   add_test(NAME test_my_feature COMMAND test_my_feature)
   ```

3. Run `./scripts/run_tests.sh` again to verify that your test builds and passes automatically along with the others.
