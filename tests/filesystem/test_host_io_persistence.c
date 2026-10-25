/**
 * @file test_host_io_persistence.c
 * @brief Host-side file I/O persistence and atomic rename test suite.
 *
 * DESIGN NOTE (Phase 4.2):
 * This test checks the atomic I/O persistence semantics on the host
 * (fopen/fwrite/fclose/rename) by simulating two independent sequential processes.
 * It uses a simplified synthetic structure ('GunBrosSaveData') for unit-testing purposes,
 * NOT the proprietary binary format of the Gun Bros game.
 * To replay the exact sequence of guest calls against real save dumps,
 * see tests/golden/golden_save_runner.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include "gunbros/filesystem/path_mapper.h"
#include "gunbros/filesystem/file_handles.h"
#include "gunbros/filesystem/file_operations.h"

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

#define SAVE_MAGIC 0x47554e42u // 'GUNB'

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t coins;
    uint32_t war_bucks;
    uint32_t equipped_weapon;
    uint32_t checksum;
} GunBrosSaveData;

static uint32_t calculate_checksum(const GunBrosSaveData *data) {
    return data->magic ^ data->version ^ data->coins ^ data->war_bucks ^ data->equipped_weapon;
}

static void run_game_session_save(const char *base_dir) {
    path_mapper_set_base_dir(base_dir);
    file_handles_init();

    // 1. Prepare save data
    GunBrosSaveData save;
    save.magic = SAVE_MAGIC;
    save.version = 1;
    save.coins = 500000;
    save.war_bucks = 2500;
    save.equipped_weapon = 101; // e.g. Kraken Rocket Launcher
    save.checksum = calculate_checksum(&save);

    // 2. GunBros SafeWriteFile flow:
    // a. Write temp.dat
    uint32_t h_temp = gunbros_fopen("temp.dat", "wb");
    assert(h_temp != 0);

    size_t written = gunbros_fwrite(&save, sizeof(save), 1, h_temp);
    assert(written == 1);

    int close_temp = gunbros_fclose(h_temp);
    assert(close_temp == 0);

    // b. Remove existing savegame.dat (if present)
    gunbros_remove("savegame.dat");

    // c. Rename temp.dat -> savegame.dat
    int ren_res = gunbros_rename("temp.dat", "savegame.dat");
    assert(ren_res == 0);

    printf("  [Session 1] Save written and flushed successfully. Terminating session...\n");
}

static void run_game_session_load(const char *base_dir) {
    // New process session: freshly initialized handles and path mapper
    path_mapper_set_base_dir(base_dir);
    file_handles_init();

    printf("  [Session 2] Starting fresh session. Attempting to load savegame.dat...\n");

    uint32_t h_save = gunbros_fopen("savegame.dat", "rb");
    assert(h_save != 0 && "savegame.dat must exist and open cleanly in new session");

    GunBrosSaveData loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t read_items = gunbros_fread(&loaded, sizeof(loaded), 1, h_save);
    assert(read_items == 1);
    gunbros_fclose(h_save);

    // Verify integrity
    assert(loaded.magic == SAVE_MAGIC);
    assert(loaded.version == 1);
    assert(loaded.coins == 500000);
    assert(loaded.war_bucks == 2500);
    assert(loaded.equipped_weapon == 101);
    assert(loaded.checksum == calculate_checksum(&loaded));

    printf("  [Session 2] Save integrity verified: coins=%u, war_bucks=%u, weapon=%u\n",
           loaded.coins, loaded.war_bucks, loaded.equipped_weapon);
}

int main(void) {
    printf("=== Running Cross-Session Persistence Test Suite ===\n");

    const char *test_base_dir = "/tmp/gunbros_persist_test/files/";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf /tmp/gunbros_persist_test && mkdir -p %s", test_base_dir);
    system(cmd);

    // Process 1: Save
    pid_t pid = fork();
    if (pid == 0) {
        run_game_session_save(test_base_dir);
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    TEST_PASS("Process 1 save write and exit cleanly");

    // Process 2: Load and verify in a completely new process
    pid_t pid2 = fork();
    if (pid2 == 0) {
        run_game_session_load(test_base_dir);
        exit(0);
    }

    waitpid(pid2, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    TEST_PASS("Process 2 fresh boot successfully loaded and validated persistent savegame");

    // Cleanup
    snprintf(cmd, sizeof(cmd), "rm -rf /tmp/gunbros_persist_test");
    system(cmd);

    printf("=== All Persistence tests PASSED! ===\n\n");
    return 0;
}
