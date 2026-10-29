#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/compatibility/guest_symbols.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/dynarmic_backend.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/platform/android_window.h"
#include "gunbros/platform/network_time.h"
#include "gunbros/filesystem/path_mapper.h"
#include "gunbros/compatibility/jni_textbox.h"
#include "gunbros/compatibility/jni_methods.h"
#include "gunbros/platform/ngs_ext.h"
#include "gunbros/platform/ngs_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

/* An idle server can take longer than one request's timeout to wake (e.g. a free App Service
   plan), and a sign-in sent before it answers fails for good: no friends, no Bro-Ops, and
   Brotherhood shows only the default Bro until the game is restarted. Keep pinging for up to
   ~2 minutes (12 tries, each up to 5 s plus 5 s apart); sign-in waits for this. */
#define NGS_WARMUP_TRIES 12
static int s_warmup_state = 0;  /* 0: not started, 1: waiting for a reply, 2: done */

void game_hooks_ngs_warmup(void) {
    static int s_tries = 0, s_wait_frames = 0;
    char reply[NGS_EXT_REPLY_MAX];
    if (s_warmup_state == 2) return;
    if (!ngs_config_enabled()) { s_warmup_state = 2; return; }
    if (s_warmup_state == 0) {
        if (s_wait_frames > 0) { s_wait_frames--; return; }
        if (ngs_ext_start("/gunbros/ping", "{}")) s_warmup_state = 1;
    } else if (ngs_ext_poll(reply, sizeof(reply))) {
        if (reply[0]) {
            LOGI("[NGS] server is up");
            s_warmup_state = 2;
        } else if (++s_tries >= NGS_WARMUP_TRIES) {
            LOGW("[NGS] server didn't answer the warm-up ping");
            s_warmup_state = 2;
        } else {
            s_warmup_state = 0;
            s_wait_frames = 300;  /* ~5 s, also when the connection is refused right away */
        }
    }
}

void game_hooks_auto_register(void) {
    static int s_done = 0, s_menu_frames = 0;
    if (s_done) return;

    if (!ngs_config_enabled()) { s_done = 1; return; }

    uint32_t gunbros = get_gunbros(), gstate = 0;
    if (gunbros == 0) return;
    guest_mem_read(gunbros + GUEST_OFF_GB_GSTATE, &gstate, 4);
    if (gstate != 5) { s_menu_frames = 0; return; }
    if (++s_menu_frames < 300) return;  /* let the main menu settle (~5 s) */
    if (s_warmup_state != 2) return;    /* and the server wake up */
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

/* Resuming a saved session (CNGSLocalUser::AttemptToContinueSession) waits on the session's
   network-time check, which never reports back once the time is already valid (the bridge
   seeds it). The local user is then left with a request outstanding for good, and the game
   only polls for messages (Bro-Ops) while it has none (CNGS::HandleUpdate). A while after
   sign-in, close it the game's own way; CompleteReadRequestOutstanding never goes below zero. */
static void settle_local_user(uint32_t profile) {
    uint32_t pending = 0;
    guest_mem_read(profile + GUEST_OFF_SERVER_OBJECT_PENDING, &pending, 4);
    if (pending == 0) return;
    LOGI("[NGS] closing %u stale request(s) left by sign-in", pending);
    for (uint32_t i = 0; i < pending && i < 4; i++) {
        prepare_call_arm(profile);
        run_guest(GUEST_FN_SERVER_OBJECT_COMPLETE_READ, "ngs_settle");
    }
}

static uint32_t s_friends_menu = 0;       /* Brotherhood (CMenuFriends), once it has been shown */

/* The menu currently on screen (as CMenuSystem::BindMenu finds it). */
static uint32_t current_menu(void) {
    uint32_t gunbros = get_gunbros(), menu_sys = 0, branch = 0, menu = 0;
    if (gunbros) guest_mem_read(gunbros + GUEST_OFF_GB_MENUSYS, &menu_sys, 4);
    if (menu_sys == 0) return 0;
    guest_mem_read(menu_sys + GUEST_OFF_MENU_SYS_BRANCH, &branch, 4);
    if (branch == GUEST_MENU_BRANCH_POPUP) guest_mem_read(menu_sys + GUEST_OFF_MENU_SYS_PREV_BRANCH, &branch, 4);
    if (branch > 0x20) return 0;
    guest_mem_read(menu_sys + GUEST_OFF_MENU_SYS_STACKS + branch * GUEST_MENU_STACK_SIZE + GUEST_OFF_MENU_STACK_MENU,
                   &menu, 4);
    return menu;
}

/* The on-duty Bro is saved as his credentials (ActiveCred.dat). At startup
   CFriendDataManager::FetchFriendsManagerInfo restores him only if he is already known as a
   friend, but the friend list arrives later, after sign-in, so the built-in default Bro (no name,
   level 0) takes his place. Once the Brothers list is in (it loads with the Bros tab), put him
   back on duty the way tapping him does (CMenuAction::DoAction 0x65). Returns true when done. */
static bool restore_on_duty_bro(void) {
    uint32_t gunbros = get_gunbros(), mgr = 0, fetch = 0, pending = 0, list = 0, count = 0, items = 0;
    uint32_t active = 0, fallback = 0;
    if (gunbros) guest_mem_read(gunbros + GUEST_OFF_GB_FRIEND_DATA_MGR, &mgr, 4);
    if (mgr == 0) return false;
    guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_FETCH_LIST, &fetch, 4);
    if (fetch) guest_mem_read(fetch + GUEST_OFF_SERVER_OBJECT_PENDING, &pending, 4);
    guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_FRIENDS, &list, 4);
    if (list) guest_mem_read(list + GUEST_OFF_LIST_COUNT, &count, 4);
    if (pending != 0 || count == 0) return false;

    guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_ACTIVE, &active, 4);
    guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_DEFAULT_BRO, &fallback, 4);
    if (active != 0 && active != fallback) return true;  /* a Bro is already on duty */

    /* Friends are told apart by client id; take it from the file (a length-prefixed JSON object). */
    char path[512], json[512];
    int32_t client_id = 0;
    snprintf(path, sizeof(path), "%sActiveCred.dat", path_mapper_get_base_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return true;
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    for (size_t i = 0; i < n; i++) if (json[i] == '\0') json[i] = ' ';
    const char *id = strstr(json, "\"clientid\":");
    if (id) client_id = (int32_t)strtol(id + 11, NULL, 10);
    if (client_id <= 0) return true;

    /* With Brotherhood on screen, CMenuFriends::Refresh(0x65) also shows him there, but only once
       the menu has finished loading. */
    uint32_t menu = 0, options = 0;
    uint8_t bound = 0;
    if (s_friends_menu && current_menu() == s_friends_menu) guest_mem_read(s_friends_menu, &menu, 4);
    if (menu == GUEST_VTABLE_CMENUFRIENDS) {
        guest_mem_read(s_friends_menu + GUEST_OFF_MENU_FRIENDS_OPTIONS, &options, 4);
        guest_mem_read(s_friends_menu + GUEST_OFF_MENU_FRIENDS_LIST_BOUND, &bound, 1);
        if (options == 0 || !bound) return false;
    }

    guest_mem_read(list + GUEST_OFF_LIST_ITEMS, &items, 4);
    for (uint32_t i = 0; i < count && i < 256 && items; i++) {
        uint32_t friend_data = 0;
        int32_t friend_id = 0;
        guest_mem_read(items + i * 4, &friend_data, 4);
        if (friend_data) guest_mem_read(friend_data + GUEST_OFF_FRIEND_CLIENT_ID, &friend_id, 4);
        if (friend_id != client_id) continue;
        prepare_call_arm(gunbros + GUEST_OFF_GB_MENU_PLAYER_CONFIG);
        dynarmic_backend_set_reg(1, friend_data + GUEST_OFF_FRIEND_AVATAR_CONFIG);
        run_guest(GUEST_FN_PLAYER_CONFIG_ASSIGN, "friend_config");
        if (menu == GUEST_VTABLE_CMENUFRIENDS) {
            uint32_t refresh = 0;
            guest_mem_read(menu + GUEST_VT_MENU_REFRESH, &refresh, 4);
            prepare_call_arm(s_friends_menu);
            dynarmic_backend_set_reg(1, GUEST_MENU_ACTION_SELECT_FRIEND);
            dynarmic_backend_set_reg(2, i);
            run_guest(refresh, "friend_select");
        } else {
            prepare_call_arm(mgr);
            dynarmic_backend_set_reg(1, friend_data);
            run_guest(GUEST_FN_FRIEND_MGR_SET_ACTIVE, "friend_set_active");
        }
        LOGI("[NGS] on-duty Bro restored");
        break;
    }
    return true;
}

/* Brotherhood gives a small gift once a day per Bro you use (CFriendDataManager::
   IsFriendMarkedAsUsed: used less than 24 h ago). The time is kept per Bro (CFriendData+0x7c) and
   the game saved it on Glu's server as one of the player's own attributes; ours keeps no player's
   own game state, so it would be forgotten whenever the Bros are reloaded and the gift could be
   collected again. Keep it on the phone instead: save newer times to BroUsage.dat ("<client id>
   <time>" lines) and put saved ones back into the Bros the game loads. */
#define BRO_USAGE_MAX 64
static struct { int32_t client_id; uint32_t used; } s_bro_usage[BRO_USAGE_MAX];
static int s_bro_usage_count = -1;  /* -1: not loaded yet */

static void bro_usage_path(char *out, size_t size) {
    snprintf(out, size, "%sBroUsage.dat", path_mapper_get_base_dir());
}

static void sync_bro_usage(void) {
    char path[512];
    bro_usage_path(path, sizeof(path));
    if (s_bro_usage_count < 0) {
        s_bro_usage_count = 0;
        FILE *f = fopen(path, "r");
        if (f) {
            long id;
            unsigned long used;
            while (s_bro_usage_count < BRO_USAGE_MAX && fscanf(f, "%ld %lu", &id, &used) == 2) {
                s_bro_usage[s_bro_usage_count].client_id = (int32_t)id;
                s_bro_usage[s_bro_usage_count].used = (uint32_t)used;
                s_bro_usage_count++;
            }
            fclose(f);
        }
    }

    uint32_t gunbros = get_gunbros(), mgr = 0, list = 0, count = 0, items = 0;
    if (gunbros) guest_mem_read(gunbros + GUEST_OFF_GB_FRIEND_DATA_MGR, &mgr, 4);
    if (mgr) guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_FRIENDS, &list, 4);
    if (list) {
        guest_mem_read(list + GUEST_OFF_LIST_COUNT, &count, 4);
        guest_mem_read(list + GUEST_OFF_LIST_ITEMS, &items, 4);
    }
    bool changed = false;
    for (uint32_t i = 0; i < count && i < 256 && items; i++) {
        uint32_t friend_data = 0, used = 0;
        int32_t client_id = 0;
        guest_mem_read(items + i * 4, &friend_data, 4);
        if (friend_data == 0) continue;
        guest_mem_read(friend_data + GUEST_OFF_FRIEND_CLIENT_ID, &client_id, 4);
        guest_mem_read(friend_data + GUEST_OFF_FRIEND_LAST_USED, &used, 4);
        if (client_id <= 0) continue;
        int k = 0;
        while (k < s_bro_usage_count && s_bro_usage[k].client_id != client_id) k++;
        if (k < s_bro_usage_count && s_bro_usage[k].used > used) {
            guest_mem_write(friend_data + GUEST_OFF_FRIEND_LAST_USED, &s_bro_usage[k].used, 4);
        } else if (used != 0 && (k == s_bro_usage_count || s_bro_usage[k].used < used)) {
            if (k == s_bro_usage_count) {
                if (k == BRO_USAGE_MAX) continue;
                s_bro_usage[k].client_id = client_id;
                s_bro_usage_count++;
            }
            s_bro_usage[k].used = used;
            changed = true;
        }
    }
    if (!changed) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        LOGW("[NGS] can't write %s", path);
        return;
    }
    for (int k = 0; k < s_bro_usage_count; k++) {
        fprintf(f, "%ld %lu\n", (long)s_bro_usage[k].client_id, (unsigned long)s_bro_usage[k].used);
    }
    fclose(f);
}

void game_hooks_ngs_profile_online(void) {
    static int s_done = 0, s_settle_frames = 0, s_frames = 0;
    static bool s_restore = false;
    static uint32_t s_profile = 0;
    if (s_settle_frames > 0 && --s_settle_frames == 0) settle_local_user(s_profile);
    if (s_done && ++s_frames % 60 == 0) {  /* once a second */
        sync_bro_usage();
        if (s_restore && restore_on_duty_bro()) s_restore = false;
    }
    if (s_done) return;
    if (!ngs_config_enabled()) { s_done = 1; return; }

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
    s_restore = true;
    s_profile = profile;
    s_settle_frames = 600;  /* ~10 s */

    /* The save files are the only copy of the player's progress; the server only keeps what
       friends see. Data stores waiting to be reloaded from the server (status 4) would never be
       uploaded, so mark them as changed locally (1) instead. */
    uint8_t status[GUEST_PROFILE_STORE_COUNT];
    guest_mem_read(profile + GUEST_OFF_PROFILE_STORE_STATUS, status, sizeof(status));
    for (int i = 0; i < GUEST_PROFILE_STORE_COUNT; i++) {
        if (status[i] == GUEST_PROFILE_STORE_LOAD_FROM_SERVER) status[i] = GUEST_PROFILE_STORE_CHANGED;
    }
    guest_mem_write(profile + GUEST_OFF_PROFILE_STORE_STATUS, status, sizeof(status));

    guest_mem_read(profile + GUEST_OFF_PROFILE_ONLINE, &online, 1);
    if (online) return;
    online = 1;
    guest_mem_write(profile + GUEST_OFF_PROFILE_ONLINE, &online, 1);
    LOGI("[NGS] signed in, profile online (profile 0x%x)", profile);
}

/* ---- Add a Bro ---------------------------------------------------------------------------- */

/* Brotherhood's "Invite more friends" banner opens the OpenSpy mod's search popup, reused as
   "Add a Bro": its title shows the player's own friend code and the field takes a friend's. The
   bridge asks the replacement server (ngs_ext.h) to make them friends, reports the result in the
   mod's message popup and reloads the friend list. */

enum {
    HOOK_SVC_MENU_ACTION = 0,           /* CMenuAction::DoAction */
    HOOK_SVC_SEARCH_POPUP_ACTION = 1,   /* CAndroidSearchPopup::RefreshMidMenuPopup */
    HOOK_SVC_MESSAGE_POPUP_ACTION = 2,  /* CAndroidMessagePopup::RefreshMidMenuPopup */
};

enum {
    ADD_BRO_IDLE,
    ADD_BRO_REQUESTED,      /* banner tapped */
    ADD_BRO_FETCHING_CODE,  /* asking the server for the player's own code */
    ADD_BRO_SHOWN,          /* popup open */
    ADD_BRO_COPY,           /* "Copy" pressed: copy the code and show the popup again */
    ADD_BRO_SUBMITTED,      /* friend code entered */
    ADD_BRO_ADDING,         /* asking the server to add the friend */
};

#define HOOK_TRAMPOLINES_OFF 0xf000u   /* in the stubs region, i.e. guest 0x8000f000 */
#define HOOK_TRAMPOLINE_SIZE 0x40u

static int s_add_bro_state = ADD_BRO_IDLE;
static uint32_t s_add_bro_popup = 0;    /* our CAndroidSearchPopup while it is shown */
static bool s_add_bro_hint_set = false; /* its text field shows the "enter a code" hint */
static uint32_t s_add_bro_message = 0;  /* our CAndroidMessagePopup while it is shown */
static char s_add_bro_code[32];
static char s_own_code[32];             /* the player's own friend code, once fetched */

/* Hook a guest function that starts with `push {...}; ldr r4, [pc, #imm]` and takes
   (r0 = object, r1 = action): the bridge sees the call first (svc) and returns the action to
   carry on with, which replaces r1. The two original instructions run in the trampoline (the
   literal load relocated), then execution continues at fn + 8. */
static bool hook_action_function(uint8_t *code_base, uint8_t *stubs_base, uint32_t fn,
                                 uint32_t svc_idx, uint32_t slot) {
    uint32_t *code = (uint32_t *)(code_base + fn);
    if ((code[0] & 0xffff4000u) != 0xe92d4000u || (code[1] & 0xfffff000u) != 0xe59f4000u) {
        LOGE("[HOOK] unexpected code at 0x%08x, not hooked", fn);
        return false;
    }
    uint32_t literal = *(uint32_t *)(code_base + fn + 4 + 8 + (code[1] & 0xfffu));
    const uint32_t tramp[] = {
        0xe92d400du,                            /* push {r0, r2, r3, lr} */
        0xef000000u | (GAME_HOOK_SVC_BASE + svc_idx),  /* svc: r0 = action to use */
        0xe1a01000u,                            /* mov  r1, r0 */
        0xe8bd400du,                            /* pop  {r0, r2, r3, lr} */
        code[0],                                /* push {...}          (original) */
        0xe59f4004u,                            /* ldr  r4, =literal   (original, relocated) */
        0xe51ff004u,                            /* ldr  pc, [pc, #-4] */
        fn + 8,
        literal,
    };
    uint32_t off = HOOK_TRAMPOLINES_OFF + slot * HOOK_TRAMPOLINE_SIZE;
    memcpy(stubs_base + off, tramp, sizeof(tramp));
    code[0] = 0xe51ff004u;                      /* ldr pc, [pc, #-4] */
    code[1] = GUEST_STUBS_BASE + off;
    return true;
}

/* Add a Bro shows "Copy" above the text field, but the popup's layout puts its third button
   below it. Both draw callbacks take their position from the layout; route their y coordinate
   through stubs that add an offset, which the bridge sets while the popup is ours
   (set_add_bro_layout). The button's touch area follows where it is drawn. */
#define LAYOUT_STUB_COPY_OFF  (HOOK_TRAMPOLINES_OFF + 3 * HOOK_TRAMPOLINE_SIZE)
#define LAYOUT_STUB_FIELD_OFF (HOOK_TRAMPOLINES_OFF + 4 * HOOK_TRAMPOLINE_SIZE)
/* Words: button C's y offset, the field's y offset. */
#define LAYOUT_DATA (GUEST_STUBS_BASE + HOOK_TRAMPOLINES_OFF + 5 * HOOK_TRAMPOLINE_SIZE)

static bool install_add_bro_layout(uint8_t *code_base, uint8_t *stubs_base) {
    uint32_t *copy = (uint32_t *)(code_base + GUEST_ADDR_SEARCH_POPUP_BUTTON_C_Y);
    uint32_t *field = (uint32_t *)(code_base + GUEST_ADDR_SEARCH_POPUP_FIELD_BOUNDS);
    if (copy[0] != 0xe1d220f4u || copy[1] != 0xe1d310f0u ||
        field[0] != 0xe58dc000u || (field[1] & 0xff000000u) != 0xeb000000u) {
        LOGE("[HOOK] unexpected search popup code, Add a Bro layout unchanged");
        return false;
    }
    const uint32_t copy_stub[] = {
        0xe1d220f4u,  /* ldrsh r2, [r2, #4]    y */
        0xe59fc010u,  /* ldr   ip, =LAYOUT_DATA */
        0xe59c1000u,  /* ldr   r1, [ip] */
        0xe0822001u,  /* add   r2, r2, r1 */
        0xe1d310f0u,  /* ldrsh r1, [r3]        x */
        0xe2800f69u,  /* add   r0, r0, #0x1a4  button C */
        0xe59ff000u,  /* ldr   pc, =CMenuMovieButton::Draw(x, y) */
        LAYOUT_DATA,
        GUEST_FN_MENU_MOVIE_BUTTON_DRAW_AT,
    };
    const uint32_t field_stub[] = {
        0xe58dc000u,  /* str   ip, [sp]        h */
        0xe59fc00cu,  /* ldr   ip, =LAYOUT_DATA */
        0xe59ce004u,  /* ldr   lr, [ip, #4] */
        0xe082200eu,  /* add   r2, r2, lr      y */
        0xe59fe004u,  /* ldr   lr, =return address */
        0xe59ff004u,  /* ldr   pc, =CAndroidUiTextField::SetBounds(x, y, w, h) */
        LAYOUT_DATA,
        GUEST_ADDR_SEARCH_POPUP_FIELD_RET,
        GUEST_FN_UI_TEXT_FIELD_SET_BOUNDS,
    };
    memcpy(stubs_base + LAYOUT_STUB_COPY_OFF, copy_stub, sizeof(copy_stub));
    memcpy(stubs_base + LAYOUT_STUB_FIELD_OFF, field_stub, sizeof(field_stub));
    memset(stubs_base + (LAYOUT_DATA - GUEST_STUBS_BASE), 0, 8);
    copy[0] = field[0] = 0xe51ff004u;  /* ldr pc, [pc, #-4] */
    copy[1] = GUEST_STUBS_BASE + LAYOUT_STUB_COPY_OFF;
    field[1] = GUEST_STUBS_BASE + LAYOUT_STUB_FIELD_OFF;
    return true;
}

/* Swap the field and the Copy button in our popup (install_add_bro_layout); other uses of the
   popup keep its layout. The field's Android view takes its bounds once, on its first draw
   (GluTextBox.SetBounds adds the view), so this must be set before the popup is drawn. */
static void set_add_bro_layout(bool ours) {
    int32_t offsets[2] = { 0, 0 };
    if (ours) {
        offsets[0] = -GUEST_SEARCH_POPUP_FIELD_TO_BUTTON_C;
        offsets[1] = GUEST_SEARCH_POPUP_FIELD_TO_BUTTON_C;
    }
    guest_mem_write(LAYOUT_DATA, offsets, sizeof(offsets));
}

bool game_hooks_install(uint8_t *code_base, uint8_t *stubs_base) {
    bool ok = hook_action_function(code_base, stubs_base, GUEST_FN_MENU_DO_ACTION,
                                   HOOK_SVC_MENU_ACTION, 0)
           && hook_action_function(code_base, stubs_base, GUEST_FN_SEARCH_POPUP_REFRESH,
                                   HOOK_SVC_SEARCH_POPUP_ACTION, 1)
           && hook_action_function(code_base, stubs_base, GUEST_FN_MESSAGE_POPUP_REFRESH,
                                   HOOK_SVC_MESSAGE_POPUP_ACTION, 2)
           && install_add_bro_layout(code_base, stubs_base);
    LOGI("[HOOK] menu action hooks %s", ok ? "installed" : "NOT installed");
    return ok;
}

static void read_text_field(uint32_t field, char *out, size_t out_size) {
    uint32_t handle = 0;
    out[0] = '\0';
    if (field) guest_mem_read(field + GUEST_OFF_TEXT_FIELD_OBJECT, &handle, 4);
    if (handle) jni_textbox_read_text(handle, out, out_size);
}

void game_hooks_on_svc(uint32_t idx) {
    uint32_t object = dynarmic_backend_get_reg(0), action = dynarmic_backend_get_reg(1);
    switch (idx) {
    case HOOK_SVC_MENU_ACTION: {
        /* Brotherhood's "Invite more friends" banner: action 0x67 with argument 0 on CMenuFriends,
           which showed the Facebook "How to get Bros" help. */
        uint32_t vtable = 0;
        if (object) guest_mem_read(object, &vtable, 4);
        /* The Facebook buttons (social action with network 2 = Facebook) open the community's
           Discord instead. */
        if (action == GUEST_MENU_ACTION_SOCIAL && dynarmic_backend_get_reg(2) == GUEST_SOCIAL_NETWORK_FACEBOOK) {
            jni_launch_url(GUNBROS_COMMUNITY_URL, 0);
            action = GUEST_MENU_ACTION_NONE;
            break;
        }
        /* Remember Brotherhood for sync_friends_menu: it sends 0x61 whenever the tab resets. */
        if (vtable == GUEST_VTABLE_CMENUFRIENDS && action == GUEST_MENU_ACTION_FRIENDS_RESET) {
            s_friends_menu = object;
        }
        if (action == GUEST_MENU_ACTION_MENU_SPECIFIC && dynarmic_backend_get_reg(2) == 0 &&
            vtable == GUEST_VTABLE_CMENUFRIENDS) {
            if (s_add_bro_state == ADD_BRO_IDLE) s_add_bro_state = ADD_BRO_REQUESTED;
            s_friends_menu = object;
            action = GUEST_MENU_ACTION_NONE;
        }
        break;
    }
    case HOOK_SVC_SEARCH_POPUP_ACTION:
        if (object != 0 && object == s_add_bro_popup) {
            uint32_t field = 0;
            guest_mem_read(object + GUEST_OFF_SEARCH_POPUP_FIELD, &field, 4);
            read_text_field(field, s_add_bro_code, sizeof(s_add_bro_code));
            s_add_bro_popup = 0;
            set_add_bro_layout(false);
            if (action == GUEST_POPUP_ACTION_COPY) {
                s_add_bro_state = ADD_BRO_COPY;
            } else {
                s_add_bro_state = (action == GUEST_POPUP_ACTION_ADD_BUDDY && s_add_bro_code[0])
                                  ? ADD_BRO_SUBMITTED : ADD_BRO_IDLE;
            }
            action = GUEST_SEARCH_POPUP_ACTION_CLOSE;  /* not an OpenSpy search */
        }
        break;
    case HOOK_SVC_MESSAGE_POPUP_ACTION:
        if (object != 0 && object == s_add_bro_message) {
            s_add_bro_message = 0;
            action = GUEST_POPUP_ACTION_CANCEL;  /* with message type 2: just closes */
        }
        break;
    default:
        LOGW("[HOOK] unknown hook svc %u", idx);
        break;
    }
    dynarmic_backend_set_reg(0, action);
}

/* A guest CStrWChar holding `s`. Small and short-lived; never freed. */
static uint32_t guest_cstrwchar(const char *s) {
    uint32_t obj = guest_alloc(0x10), words[4] = { GUEST_VTABLE_CSTRWCHAR, GUEST_CSTRWCHAR_MAGIC, 0, 0 };
    guest_mem_write(obj, words, sizeof(words));
    prepare_call_arm(obj);
    dynarmic_backend_set_reg(1, guest_str(s));
    run_guest(GUEST_FN_CSTRWCHAR_CONCATENATE_CHAR, "cstrwchar");
    return obj;
}

/* Call a guest function with r0-r3 and further arguments on the stack. */
static void call_guest_args(uint32_t fn, const char *tag, const uint32_t *args, int count) {
    prepare_call_arm(args[0]);
    for (int i = 1; i < count && i < 4; i++) dynarmic_backend_set_reg(i, args[i]);
    if (count > 4) {
        uint32_t sp = GUEST_STACK_TOP - (uint32_t)((count - 4 + 1) & ~1) * 4;
        guest_mem_write(sp, args + 4, (uint32_t)(count - 4) * 4);
        dynarmic_backend_set_reg(13, sp);
    }
    run_guest(fn, tag);
}

static uint32_t current_mid_popup(void) {
    uint32_t gunbros = get_gunbros(), menu_sys = 0, popup = 0;
    if (gunbros) guest_mem_read(gunbros + GUEST_OFF_GB_MENUSYS, &menu_sys, 4);
    if (menu_sys) guest_mem_read(menu_sys + GUEST_OFF_MENU_MID_POPUP, &popup, 4);
    return popup;
}

static void show_add_bro_popup(const char *title) {
    const uint32_t args[] = { guest_cstrwchar(title), GUEST_POPUP_BUTTON_CANCEL,
                              GUEST_POPUP_BUTTON_ADD_BUDDY, GUEST_POPUP_BUTTON_COPY, (uint32_t)-1 };
    set_add_bro_layout(true);
    call_guest_args(GUEST_FN_SHOW_SEARCH_POPUP, "add_bro_popup", args, 5);
    s_add_bro_popup = current_mid_popup();
    s_add_bro_hint_set = false;
    if (s_add_bro_popup) {
        /* No popup button label says "Copy": relabel the third one. A button's label is a
           CStrWChar it owns (+0x3c); give it one allocated by the game (np_malloc), as
           CMenuMovieButton::SetLabel would, so the game can free it. The old one is left alone. */
        prepare_call_arm(0x10);
        run_guest(GUEST_FN_NP_MALLOC, "np_malloc");
        uint32_t label = dynarmic_backend_get_reg(0);
        if (label) {
            uint32_t words[4] = { GUEST_VTABLE_CSTRWCHAR, GUEST_CSTRWCHAR_MAGIC, 0, 0 };
            guest_mem_write(label, words, sizeof(words));
            prepare_call_arm(label);
            dynarmic_backend_set_reg(1, guest_str("Copy"));
            run_guest(GUEST_FN_CSTRWCHAR_CONCATENATE_CHAR, "cstrwchar");
            guest_mem_write(s_add_bro_popup + GUEST_OFF_SEARCH_POPUP_BUTTON_C + GUEST_OFF_MENU_BUTTON_LABEL,
                            &label, 4);
        }
    }
}

/* The popup's labels are a single line each (no wrapping): keep texts short. */
static void show_add_bro_message(const char *title, const char *message) {
    const uint32_t args[] = { GUEST_MESSAGE_POPUP_TYPE_PLAIN, guest_cstrwchar(title),
                              guest_cstrwchar(message), (uint32_t)-1, (uint32_t)-1,
                              (uint32_t)-1, GUEST_POPUP_BUTTON_OK, (uint32_t)-1 };
    call_guest_args(GUEST_FN_SHOW_MESSAGE_POPUP, "add_bro_message", args, 8);
    s_add_bro_message = current_mid_popup();
}

/* The signed-in player's NGS session id (CNGSSession config, UTF-16), as ASCII. */
static bool read_session_id(char *out, size_t out_size) {
    uint32_t gunbros = get_gunbros(), profile = 0, session = 0, wstr = 0;
    out[0] = '\0';
    if (gunbros) guest_mem_read(gunbros + GUEST_OFF_GB_PROFILE_MGR, &profile, 4);
    if (profile) guest_mem_read(profile + GUEST_OFF_PROFILE_SESSION, &session, 4);
    if (session) guest_mem_read(session + GUEST_OFF_SESSION_ID, &wstr, 4);
    if (!wstr) return false;
    size_t i = 0;
    for (; i + 1 < out_size; i++) {
        uint16_t ch = 0;
        guest_mem_read(wstr + (uint32_t)i * 2, &ch, 2);
        if (ch == 0) break;
        if (ch < 0x21 || ch > 0x7e || ch == '"' || ch == '\\') return false;
        out[i] = (char)ch;
    }
    out[i] = '\0';
    return i > 0;
}

/* Brotherhood reloads its friends whenever it is shown again or the friend list changes
   (CMenuFriends::Update -> CMenuDataProvider::LoadData). The menu system binds the list as soon
   as the loader has *started* the fetch (CFriendDataManager::FriendListFetchLoader), so with a
   server that takes a moment to answer it binds an empty list, and nothing binds it again once
   the friends arrive: the Brothers section stays empty, and the menu counts as busy (ignores
   taps) while its list isn't bound. When Brotherhood is on screen, its fetch is done and its list
   shows a different number of friends than the game has, bind the list again, as
   CMenuFriends::Update does before a reload. */
static void sync_friends_menu(void) {
    static int s_frames = 0;
    if (s_friends_menu == 0 || ++s_frames % 10) return;
    uint32_t vtable = 0, gunbros = get_gunbros(), mgr = 0, fetch = 0, pending = 0, list = 0, count = 0, rows = 0;
    uint8_t reload = 0, bound = 0;
    guest_mem_read(s_friends_menu, &vtable, 4);
    if (vtable != GUEST_VTABLE_CMENUFRIENDS || current_menu() != s_friends_menu) return;
    guest_mem_read(s_friends_menu + GUEST_OFF_MENU_FRIENDS_RELOAD, &reload, 1);
    guest_mem_read(s_friends_menu + GUEST_OFF_MENU_FRIENDS_LIST_BOUND, &bound, 1);
    if (reload || !bound) return;  /* the game is (re)loading it itself */
    if (gunbros) guest_mem_read(gunbros + GUEST_OFF_GB_FRIEND_DATA_MGR, &mgr, 4);
    if (mgr == 0) return;
    guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_FETCH_LIST, &fetch, 4);
    if (fetch) guest_mem_read(fetch + GUEST_OFF_SERVER_OBJECT_PENDING, &pending, 4);
    guest_mem_read(mgr + GUEST_OFF_FRIEND_MGR_FRIENDS, &list, 4);
    if (list) guest_mem_read(list + GUEST_OFF_LIST_COUNT, &count, 4);
    guest_mem_read(s_friends_menu + GUEST_OFF_MENU_FRIENDS_ROWS, &rows, 4);
    if (pending != 0 || rows == count) return;

    uint32_t refresh = 0;
    uint8_t unbound = 0;
    LOGI("[NGS] Brotherhood shows %u of %u Bros, binding its list again", rows, count);
    guest_mem_write(s_friends_menu + GUEST_OFF_MENU_FRIENDS_LIST_BOUND, &unbound, 1);
    guest_mem_read(vtable + GUEST_VT_MENU_REFRESH, &refresh, 4);
    prepare_call_arm(s_friends_menu);
    dynarmic_backend_set_reg(1, GUEST_MENU_REFRESH_BIND);
    dynarmic_backend_set_reg(2, (uint32_t)-1);
    run_guest(refresh, "friends_bind");
}

void game_hooks_add_bro(void) {
    char session[64], body[160], reply[NGS_EXT_REPLY_MAX], text[128];

    sync_friends_menu();

    switch (s_add_bro_state) {
    case ADD_BRO_REQUESTED:
        if (!read_session_id(session, sizeof(session))) {
            show_add_bro_message("Add a Bro", "Not signed in yet. Try again soon.");
            s_add_bro_state = ADD_BRO_IDLE;
            break;
        }
        snprintf(body, sizeof(body), "{\"session\": \"%s\"}", session);
        if (ngs_ext_start("/gunbros/friendcode", body)) s_add_bro_state = ADD_BRO_FETCHING_CODE;
        break;

    case ADD_BRO_FETCHING_CODE: {
        if (!ngs_ext_poll(reply, sizeof(reply))) break;
        char code[32];
        if (ngs_ext_json_string(reply, "code", code, sizeof(code))) {
            /* The popup's title is the only label it has (one line, no wrapping), so the field's
               hint explains what to type. */
            snprintf(s_own_code, sizeof(s_own_code), "%s", code);
            snprintf(text, sizeof(text), "Your friend code: %s", code);
            show_add_bro_popup(text);
            s_add_bro_state = ADD_BRO_SHOWN;
        } else {
            show_add_bro_message("Add a Bro", "Can't reach the Gun Bros server.");
            s_add_bro_state = ADD_BRO_IDLE;
        }
        break;
    }

    case ADD_BRO_SHOWN:
        if (!s_add_bro_hint_set && s_add_bro_popup) {
            uint32_t field = 0, handle = 0;
            guest_mem_read(s_add_bro_popup + GUEST_OFF_SEARCH_POPUP_FIELD, &field, 4);
            if (field) guest_mem_read(field + GUEST_OFF_TEXT_FIELD_OBJECT, &handle, 4);
            if (handle) s_add_bro_hint_set = jni_textbox_set_hint(handle, "Enter a friend's code");
        }
        /* Closed some other way (e.g. back key): forget it. */
        if (s_add_bro_popup != 0 && current_mid_popup() != s_add_bro_popup) {
            s_add_bro_popup = 0;
            set_add_bro_layout(false);
            s_add_bro_state = ADD_BRO_IDLE;
        }
        break;

    case ADD_BRO_COPY:
        jni_copy_to_clipboard("Gun Bros friend code", s_own_code);
        snprintf(text, sizeof(text), "Copied! Code: %s", s_own_code);
        show_add_bro_popup(text);
        s_add_bro_state = ADD_BRO_SHOWN;
        break;

    case ADD_BRO_SUBMITTED:
        if (!read_session_id(session, sizeof(session))) {
            show_add_bro_message("Add a Bro", "Not signed in.");
            s_add_bro_state = ADD_BRO_IDLE;
            break;
        }
        for (char *c = s_add_bro_code; *c; c++) {  /* keep the JSON simple: code characters only */
            if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = ' ';
        }
        snprintf(body, sizeof(body), "{\"session\": \"%s\", \"code\": \"%s\"}", session, s_add_bro_code);
        if (ngs_ext_start("/gunbros/addfriend", body)) s_add_bro_state = ADD_BRO_ADDING;
        break;

    case ADD_BRO_ADDING: {
        if (!ngs_ext_poll(reply, sizeof(reply))) break;
        char name[64], error[96];
        if (ngs_ext_json_string(reply, "gamertag", name, sizeof(name))) {
            snprintf(text, sizeof(text), "%s is now your Bro!", name);
            show_add_bro_message("Add a Bro", text);
            /* Reload the friend list, as CGluSocialManager::LoadFriends does. */
            uint32_t ngs = guest_singleton(GUEST_SINGLETON_NGS), local_user = 0;
            if (ngs) guest_mem_read(ngs + GUEST_OFF_NGS_LOCAL_USER, &local_user, 4);
            if (local_user) {
                prepare_call_arm(local_user);
                run_guest(GUEST_FN_NGS_UPDATE_FRIEND_LIST, "ngs_friends");
            }
        } else if (ngs_ext_json_string(reply, "error", error, sizeof(error))) {
            show_add_bro_message("Add a Bro", error);
        } else {
            show_add_bro_message("Add a Bro", "Can't reach the Gun Bros server.");
        }
        s_add_bro_state = ADD_BRO_IDLE;
        break;
    }

    default:
        break;
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
