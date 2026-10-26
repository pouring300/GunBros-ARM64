#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/compatibility/guest_symbols.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/dynarmic_backend.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/platform/android_window.h"
#include "gunbros/platform/network_time.h"
#include <string.h>

static uint32_t get_gunbros(void) {
    uint32_t app_ptr = 0, gunbros = 0;
    guest_mem_read(GUEST_ADDR_APP_PTR, &app_ptr, 4);
    if (app_ptr == 0) return 0;
    guest_mem_read(app_ptr + GUEST_OFF_APP_GUNBROS, &gunbros, 4);
    return gunbros;
}

void game_hooks_seed_network_time(void) {
    uint32_t gunbros = get_gunbros(), profile_mgr = 0, session = 0;
    if (gunbros == 0) return;
    guest_mem_read(gunbros + GUEST_OFF_GB_PROFILE_MGR, &profile_mgr, 4);
    if (profile_mgr == 0) return;
    guest_mem_read(profile_mgr + GUEST_OFF_PROFILE_SESSION, &session, 4);
    if (session == 0) return;

    uint64_t now_ms = 0;
    guest_mem_read(session + GUEST_OFF_SESSION_TIME_NOW_MS, &now_ms, 8);
    if (now_ms != 0) return;

    now_ms = network_time_now_ms();
    if (now_ms == 0) return;  /* NTP query still in flight */
    guest_mem_write(session + GUEST_OFF_SESSION_TIME_START_MS, &now_ms, 8);
    guest_mem_write(session + GUEST_OFF_SESSION_TIME_NOW_MS, &now_ms, 8);
    LOGI("Seeded network time: %llu ms (session=0x%x)", (unsigned long long)now_ms, session);
}

void game_hooks_refinery_unlock_chain(void) {
    static uint32_t s_prev[GUEST_REFINERY_SLOT_COUNT];
    static uint32_t s_prev_rm = 0;
    uint32_t gunbros = get_gunbros(), rm = 0;
    if (gunbros == 0) return;
    guest_mem_read(gunbros + GUEST_OFF_GB_REFINERY, &rm, 4);
    if (rm == 0) return;

    uint32_t state[GUEST_REFINERY_SLOT_COUNT];
    uint8_t gate[GUEST_REFINERY_SLOT_COUNT];
    for (int i = 0; i < GUEST_REFINERY_SLOT_COUNT; i++) {
        guest_mem_read(rm + GUEST_OFF_REFINERY_SLOTS + i * 0x20, &state[i], 4);
    }
    guest_mem_read(rm + GUEST_OFF_REFINERY_GATE, gate, sizeof(gate));

    if (rm != s_prev_rm) {  /* first look at this manager: nothing to compare against */
        s_prev_rm = rm;
        memcpy(s_prev, state, sizeof(state));
        return;
    }

    int changed = 0;
    for (int k = 0; k < GUEST_REFINERY_SLOT_COUNT; k++) {
        /* CRefinementSlot::Collect moves a slot from complete back to ready. */
        if (s_prev[k] != GUEST_REFINERY_STATE_COMPLETE || state[k] != GUEST_REFINERY_STATE_READY) continue;
        for (int n = 0; n < GUEST_REFINERY_SLOT_COUNT; n++) {
            /* gate 0 means "not gated" (see CRefinementManager::IsGated) */
            if (n != k && gate[n] != 0 && gate[n] == k && state[n] == GUEST_REFINERY_STATE_LOCKED) {
                state[n] = GUEST_REFINERY_STATE_READY;
                guest_mem_write(rm + GUEST_OFF_REFINERY_SLOTS + n * 0x20, &state[n], 4);
                LOGI("Refinery: collected slot %d, unlocked slot %d", k, n);
                changed = 1;
            }
        }
    }
    memcpy(s_prev, state, sizeof(state));

    if (changed) {
        prepare_call_arm(rm);
        run_guest(GUEST_FN_REFINERY_SAVE_STATE_CHANGE, "refinery_save");
    }
}

void game_hooks_on_touch_ended(int x, int y) {
    (void)y;
    uint32_t app_ptr = 0;
    guest_mem_read(GUEST_ADDR_APP_PTR, &app_ptr, 4);
    if (app_ptr == 0) return;

    uint32_t gunbros = 0;
    guest_mem_read(app_ptr + GUEST_OFF_APP_GUNBROS, &gunbros, 4);
    if (gunbros == 0) return;

    uint32_t gstate = 0;
    guest_mem_read(gunbros + GUEST_OFF_GB_GSTATE, &gstate, 4);
    if (gstate != 5) return; // PLAYER_SELECT state

    uint32_t menu_sys = 0;
    guest_mem_read(gunbros + GUEST_OFF_GB_MENUSYS, &menu_sys, 4);
    if (menu_sys == 0) return;

    uint32_t top_menu = 0;
    guest_mem_read(menu_sys + GUEST_OFF_MENU_TOP, &top_menu, 4);
    if (top_menu == 0) return;

    int gw = 1024, gh = 640;
    window_get_dimensions(&gw, &gh);
    uint32_t sel = (x < gw / 2) ? 0 : 1;
    DLOG("TouchEnded in PLAYER_SELECT: selecting %s (index %d, top=0x%x)",
         sel == 0 ? "Percy Gun" : "Francis Gun", sel, top_menu);
    prepare_call_arm(top_menu);
    dynarmic_backend_set_reg(1, sel);
    run_guest(GUEST_FN_SELECT_CHAR, "select_char");
}
