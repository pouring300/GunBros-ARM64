/**
 * @file golden_save_runner.c
 * @brief Test runner that reproduces the exact sequence of guest libc calls
 *        on real Gun Bros save files (savegame.dat, prefs.dat).
 *
 * In line with project Rule 5, real save dumps are NOT included
 * in the repository. If 'tests/golden/savegame.dat' does not exist, the test explains how to provide it
 * and exits successfully.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "gunbros/common/common.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/hle/hle_libc.h"
#include "gunbros/filesystem/path_mapper.h"
#include "gunbros/filesystem/file_operations.h"

#define TEST_PASS(name) printf("  [PASS] %s\n", name)
#define TEST_FAIL(name, msg) do { printf("  [FAIL] %s: %s\n", name, msg); exit(1); } while(0)
#define ASSERT_TRUE(cond, name, msg) do { if (!(cond)) TEST_FAIL(name, msg); } while(0)
#define ASSERT_EQ(a, b, name, msg) do { if ((a) != (b)) { printf("Expected %ld got %ld\n", (long)(b), (long)(a)); TEST_FAIL(name, msg); } } while(0)

static uint32_t write_guest_str(const char *s) {
    size_t len = strlen(s) + 1;
    uint32_t ga = guest_alloc((uint32_t)len);
    char *p = (char*)guest_to_host(ga);
    if (p) memcpy(p, s, len);
    return ga;
}

static bool run_guest_save_sequence(const char *filename) {
    printf("[GOLDEN] Running guest file sequence on '%s'...\n", filename);

    hle_t c = {0};

    // 1. stat(filename, &st)
    uint32_t st_ga = guest_alloc(104);
    c.r0 = write_guest_str(filename);
    c.r1 = st_ga;
    h_stat(&c);
    if (c.r0 != 0) {
        printf("[GOLDEN] File '%s' does not exist or stat failed.\n", filename);
        return false;
    }

    uint64_t file_size = 0;
    memcpy(&file_size, guest_to_host(st_ga + 48), 8);
    printf("[GOLDEN] stat reported size: %lu bytes\n", (unsigned long)file_size);

    // 2. fopen(filename, "rb")
    memset(&c, 0, sizeof(c));
    c.r0 = write_guest_str(filename);
    c.r1 = write_guest_str("rb");
    h_fopen(&c);
    uint32_t fp = c.r0;
    ASSERT_TRUE(fp > 0, "golden_seq", "fopen 'rb' failed on existing file");

    // 3. fseek(fp, 0, SEEK_END)
    memset(&c, 0, sizeof(c));
    c.r0 = fp;
    c.r1 = 0;
    c.r2 = 2; // SEEK_END
    h_fseek(&c);
    ASSERT_EQ(c.r0, 0, "golden_seq", "fseek SEEK_END failed");

    // 4. ftell(fp)
    memset(&c, 0, sizeof(c));
    c.r0 = fp;
    h_ftell(&c);
    uint32_t end_pos = c.r0;
    ASSERT_EQ(end_pos, (uint32_t)file_size, "golden_seq", "ftell mismatch with stat size");

    // 5. fseek(fp, 0, SEEK_SET)
    memset(&c, 0, sizeof(c));
    c.r0 = fp;
    c.r1 = 0;
    c.r2 = 0; // SEEK_SET
    h_fseek(&c);
    ASSERT_EQ(c.r0, 0, "golden_seq", "fseek SEEK_SET failed");

    // 6. fread(buf, 1, file_size, fp)
    uint32_t buf_ga = guest_alloc((uint32_t)file_size);
    memset(&c, 0, sizeof(c));
    c.r0 = buf_ga;
    c.r1 = 1;
    c.r2 = (uint32_t)file_size;
    c.r3 = fp;
    h_fread(&c);
    ASSERT_EQ(c.r0, (uint32_t)file_size, "golden_seq", "fread failed to read full file");

    // Hexdump preview of first 64 bytes
    const uint8_t *data = (const uint8_t*)guest_to_host(buf_ga);
    size_t preview_len = file_size < 64 ? (size_t)file_size : 64;
    printf("[GOLDEN] Hexdump preview (%zu bytes):\n  ", preview_len);
    for (size_t i = 0; i < preview_len; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < preview_len) printf("\n  ");
    }
    printf("\n");

    // 7. fclose(fp)
    memset(&c, 0, sizeof(c));
    c.r0 = fp;
    h_fclose(&c);

    // 8. Test SafeWriteFile round-trip
    memset(&c, 0, sizeof(c));
    c.r0 = write_guest_str("temp_golden.dat");
    c.r1 = write_guest_str("wb");
    h_fopen(&c);
    uint32_t fp_out = c.r0;
    ASSERT_TRUE(fp_out > 0, "golden_seq", "fopen 'wb' failed for temp_golden.dat");

    memset(&c, 0, sizeof(c));
    c.r0 = buf_ga;
    c.r1 = 1;
    c.r2 = (uint32_t)file_size;
    c.r3 = fp_out;
    h_fwrite(&c);
    ASSERT_EQ(c.r0, (uint32_t)file_size, "golden_seq", "fwrite failed to write golden save data");

    memset(&c, 0, sizeof(c));
    c.r0 = fp_out;
    h_fclose(&c);

    // Rename temp_golden.dat -> golden_verify.dat
    memset(&c, 0, sizeof(c));
    c.r0 = write_guest_str("temp_golden.dat");
    c.r1 = write_guest_str("golden_verify.dat");
    h_rename(&c);
    ASSERT_EQ(c.r0, 0, "golden_seq", "rename failed");

    // Cleanup verify file
    memset(&c, 0, sizeof(c));
    c.r0 = write_guest_str("golden_verify.dat");
    h_remove(&c);

    return true;
}

static void create_synthetic_fixture(const char *path, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t *dummy = (uint8_t*)malloc(size);
    if (dummy) {
        memset(dummy, 0, size);
        memcpy(dummy, "GBRS\x01\x00\x00\x00", 8);
        for (size_t i = 8; i < size; i++) {
            dummy[i] = (uint8_t)(i ^ 0xAA);
        }
        fwrite(dummy, 1, size, f);
        free(dummy);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    printf("=== Running Golden Save Runner ===\n");

    const char *golden_file = "tests/golden/savegame.dat";
    if (argc > 1) {
        golden_file = argv[1];
    }

    // Initialize subsystems
    guest_memory_init();
    path_mapper_set_base_dir(".");

    // 1. Always execute full sequence on a structured synthetic fixture
    const char *synth_path = "/tmp/gunbros_golden_synthetic.dat";
    create_synthetic_fixture(synth_path, 1024);
    bool synth_ok = run_guest_save_sequence(synth_path);
    remove(synth_path);
    ASSERT_TRUE(synth_ok, "golden_save_runner", "failed to run guest save sequence on synthetic fixture");
    TEST_PASS("golden_save_runner (synthetic save fixture full I/O cycle verified)");

    // 2. If real golden save dump is present, test it as well
    struct stat st;
    if (stat(golden_file, &st) == 0) {
        bool ok = run_guest_save_sequence(golden_file);
        ASSERT_TRUE(ok, "golden_save_runner", "failed to run guest save sequence on real save dump");
        TEST_PASS("golden_save_runner with real save dump verified");
    } else {
        printf("[GOLDEN] Real dump '%s' not present.\n", golden_file);
        printf("[GOLDEN] (As per project rules, proprietary save files are not committed to git).\n");
        printf("[GOLDEN] To test real game saves, see tests/golden/README.md.\n");
    }

    guest_memory_release();
    return 0;
}
