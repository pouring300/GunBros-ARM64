#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/compatibility/guest_symbols.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/dynarmic_backend.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/platform/android_window.h"
#include "gunbros/platform/network_time.h"
#include "gunbros/filesystem/path_mapper.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

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

static uint32_t guest_str(const char *s) {
    uint32_t g = guest_alloc((uint32_t)strlen(s) + 1);
    guest_mem_write(g, s, (uint32_t)strlen(s) + 1);
    return g;
}

static uint32_t guest_wstr(const char *s) {
    size_t n = strlen(s);
    uint32_t g = guest_alloc((uint32_t)(n + 1) * 2);
    for (size_t i = 0; i <= n; i++) {
        uint16_t ch = (uint8_t)s[i];
        guest_mem_write(g + (uint32_t)i * 2, &ch, 2);
    }
    return g;
}

static void random_hex(char *out, size_t bytes) {
    uint8_t buf[16] = {0};
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(buf, 1, bytes, f);
        fclose(f);
    }
    for (size_t i = 0; i < bytes; i++) sprintf(out + i * 2, "%02x", buf[i]);
}

/* com::glu::platform::components::CHash::Find on CApplet::m_App's singleton table. */
static uint32_t guest_singleton(uint32_t key) {
    uint32_t app_ptr = 0, hash = 0, buckets = 0, table = 0, node = 0;
    guest_mem_read(GUEST_ADDR_APP_PTR, &app_ptr, 4);
    if (app_ptr == 0) return 0;
    guest_mem_read(app_ptr + GUEST_OFF_APP_SINGLETONS, &hash, 4);
    if (hash == 0) return 0;
    guest_mem_read(hash + 0xc, &buckets, 4);
    guest_mem_read(hash + 0x10, &table, 4);
    if (buckets == 0 || table == 0) return 0;
    guest_mem_read(table + (key % buckets) * 4, &node, 4);
    while (node != 0) {
        uint32_t k = 0, v = 0;
        guest_mem_read(node + 8, &k, 4);
        if (k == key) {
            guest_mem_read(node + 0xc, &v, 4);
            return v;
        }
        guest_mem_read(node + 0x10, &node, 4);
    }
    return 0;
}

static void call_dialog_handler(uint32_t fn, const char *tag, const char *a, const char *b, const char *c) {
    prepare_call_arm(0);
    dynarmic_backend_set_reg(1, guest_wstr(a));
    dynarmic_backend_set_reg(2, guest_wstr(b));
    if (c) dynarmic_backend_set_reg(3, guest_wstr(c));
    run_guest(fn, tag);
}

void game_hooks_auto_register(void) {
    static int s_done = 0, s_menu_frames = 0;
    if (s_done) return;

#ifdef __ANDROID__
    char host[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.gunbros.ngs_host", host);
    if (host[0] == '\0') { s_done = 1; return; }
#else
    s_done = 1;
    return;
#endif

    uint32_t gunbros = get_gunbros(), gstate = 0;
    if (gunbros == 0) return;
    guest_mem_read(gunbros + GUEST_OFF_GB_GSTATE, &gstate, 4);
    if (gstate != 5) { s_menu_frames = 0; return; }
    if (++s_menu_frames < 300) return;  /* let the main menu settle (~5 s) */
    s_done = 1;

    char creds[512], login[512];
    struct stat st;
    snprintf(creds, sizeof(creds), "%sCredentials.dat", path_mapper_get_base_dir());
    snprintf(login, sizeof(login), "%sNgsLogin.dat", path_mapper_get_base_dir());

    if (stat(creds, &st) == 0) {
        /* CGluSocialManager::ValidateUser only sends the gamertag when the password is empty, so
           the Glu login dialog can't sign in. Run the device-level login instead, as the game does
           after a Glu login (CNGSSocialInterface::handlePostUserLogin, network 0 = Glu). With a
           saved session (Session.dat) it just resumes it. */
        uint32_t ngs = guest_singleton(GUEST_SINGLETON_NGS), local_user = 0;
        if (ngs) guest_mem_read(ngs + GUEST_OFF_NGS_LOCAL_USER, &local_user, 4);
        if (local_user == 0) {
            LOGW("[NGS] no local NGS user yet, can't sign in");
            return;
        }
        /* That login sends the Glu player id (CGluSocialInterface::getPlayerID = the account's
           e-mail), which the game only knows after a Glu login. Restore it from NgsLogin.dat. */
        uint32_t glu = guest_singleton(GUEST_SINGLETON_GLU_SOCIAL), email_len = 0;
        if (glu == 0) {
            /* getPlayerID creates the manager if needed; it returns a CStrWChar through r0. */
            uint32_t ret = guest_alloc(0x10), magic = GUEST_CSTRWCHAR_MAGIC;
            guest_mem_write(ret + 4, &magic, 4);
            prepare_call_arm(ret);
            run_guest(GUEST_FN_GLU_SOCIAL_GET_PLAYER_ID, "glu_player_id");
            glu = guest_singleton(GUEST_SINGLETON_GLU_SOCIAL);
        }
        char user[64] = "", email[96];
        FILE *f = fopen(login, "r");
        if (f) {
            if (fscanf(f, "%63s", user) != 1) user[0] = '\0';
            fclose(f);
        }
        if (glu) guest_mem_read(glu + GUEST_OFF_GLU_SOCIAL_EMAIL + 0xc, &email_len, 4);
        if (glu && email_len == 0 && user[0]) {
            snprintf(email, sizeof(email), "%s@gunbros.invalid", user);
            prepare_call_arm(glu + GUEST_OFF_GLU_SOCIAL_EMAIL);
            dynarmic_backend_set_reg(1, guest_str(email));
            run_guest(GUEST_FN_CSTRWCHAR_CONCATENATE_CHAR, "glu_email");
        }
        LOGI("[NGS] signing in as '%s' (local user 0x%x, Glu manager 0x%x)", user, local_user, glu);
        prepare_call_arm(local_user);
        dynarmic_backend_set_reg(1, 0);
        run_guest(GUEST_FN_NGS_LOCAL_USER_VALIDATE, "ngs_validate");
        return;
    }

    char user[64], pass[64], email[96];
    strcpy(user, "bro");
    random_hex(user + 3, 6);
    random_hex(pass, 16);
    snprintf(email, sizeof(email), "%s@gunbros.invalid", user);
    FILE *f = fopen(login, "w");
    if (!f) {
        LOGE("[NGS] can't write %s, not registering", login);
        return;
    }
    fprintf(f, "%s %s\n", user, pass);
    fclose(f);
    LOGI("[NGS] no Glu account yet, registering '%s'", user);
    call_dialog_handler(GUEST_FN_NGS_REGISTER_SUBMITTED, "ngs_register", user, pass, email);
}

void game_hooks_ngs_profile_online(void) {
    static int s_done = 0;
    if (s_done) return;
#ifdef __ANDROID__
    char host[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.gunbros.ngs_host", host);
    if (host[0] == '\0') { s_done = 1; return; }
#else
    s_done = 1;
    return;
#endif

    uint32_t gunbros = get_gunbros(), profile = 0, other = 0;
    uint8_t valid = 0, other_valid = 0, online = 0;
    if (gunbros == 0) return;
    guest_mem_read(gunbros + GUEST_OFF_GB_PROFILE_MGR, &profile, 4);
    if (profile == 0) return;
    guest_mem_read(profile + GUEST_OFF_SERVER_OBJECT_VALID, &valid, 1);
    guest_mem_read(profile + GUEST_OFF_PROFILE_SERVER_OBJ, &other, 4);
    if (other) guest_mem_read(other + GUEST_OFF_SERVER_OBJECT_VALID, &other_valid, 1);
    if (!valid || !other_valid) return;

    /* CProfileManager::LoginInitResourceLoaderCallback sets this flag at startup when the account
       is already signed in; ours signs in later, so set it the same way once it is. */
    s_done = 1;
    guest_mem_read(profile + GUEST_OFF_PROFILE_ONLINE, &online, 1);
    if (online) return;
    online = 1;
    guest_mem_write(profile + GUEST_OFF_PROFILE_ONLINE, &online, 1);
    LOGI("[NGS] signed in, profile online (profile 0x%x)", profile);
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
