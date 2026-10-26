#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/compatibility/guest_symbols.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/dynarmic_backend.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/platform/android_window.h"
#include "gunbros/platform/network_time.h"
#include "gunbros/filesystem/path_mapper.h"
#include "gunbros/compatibility/jni_textbox.h"
#include "gunbros/platform/ngs_ext.h"
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
   bridge asks the replacement server (ngs_ext.h) to make them friends and reports the result in
   the mod's message popup. */

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
    ADD_BRO_SUBMITTED,      /* friend code entered */
    ADD_BRO_ADDING,         /* asking the server to add the friend */
};

#define HOOK_TRAMPOLINES_OFF 0xf000u   /* in the stubs region, i.e. guest 0x8000f000 */
#define HOOK_TRAMPOLINE_SIZE 0x40u

static int s_add_bro_state = ADD_BRO_IDLE;
static uint32_t s_add_bro_popup = 0;    /* our CAndroidSearchPopup while it is shown */
static uint32_t s_add_bro_message = 0;  /* our CAndroidMessagePopup while it is shown */
static char s_add_bro_code[32];

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

bool game_hooks_install(uint8_t *code_base, uint8_t *stubs_base) {
    bool ok = hook_action_function(code_base, stubs_base, GUEST_FN_MENU_DO_ACTION,
                                   HOOK_SVC_MENU_ACTION, 0)
           && hook_action_function(code_base, stubs_base, GUEST_FN_SEARCH_POPUP_REFRESH,
                                   HOOK_SVC_SEARCH_POPUP_ACTION, 1)
           && hook_action_function(code_base, stubs_base, GUEST_FN_MESSAGE_POPUP_REFRESH,
                                   HOOK_SVC_MESSAGE_POPUP_ACTION, 2);
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
        if (action == GUEST_MENU_ACTION_MENU_SPECIFIC && dynarmic_backend_get_reg(2) == 0 &&
            vtable == GUEST_VTABLE_CMENUFRIENDS) {
            if (s_add_bro_state == ADD_BRO_IDLE) s_add_bro_state = ADD_BRO_REQUESTED;
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
            s_add_bro_state = (action == GUEST_POPUP_ACTION_ADD_BUDDY && s_add_bro_code[0])
                              ? ADD_BRO_SUBMITTED : ADD_BRO_IDLE;
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
                              GUEST_POPUP_BUTTON_ADD_BUDDY, (uint32_t)-1, (uint32_t)-1 };
    call_guest_args(GUEST_FN_SHOW_SEARCH_POPUP, "add_bro_popup", args, 5);
    s_add_bro_popup = current_mid_popup();
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

void game_hooks_add_bro(void) {
    char session[64], body[160], reply[NGS_EXT_REPLY_MAX], text[128];

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
            snprintf(text, sizeof(text), "Your code: %s", code);
            show_add_bro_popup(text);
            s_add_bro_state = ADD_BRO_SHOWN;
        } else {
            show_add_bro_message("Add a Bro", "Can't reach the Gun Bros server.");
            s_add_bro_state = ADD_BRO_IDLE;
        }
        break;
    }

    case ADD_BRO_SHOWN:
        /* Closed some other way (e.g. back key): forget it. */
        if (s_add_bro_popup != 0 && current_mid_popup() != s_add_bro_popup) {
            s_add_bro_popup = 0;
            s_add_bro_state = ADD_BRO_IDLE;
        }
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
            /* The game only loads its friend list at startup; reloading it mid-session
               (CNGSLocalUser::UpdateFriendListFromServer) leaves Brotherhood empty. */
            snprintf(text, sizeof(text), "%s is now your Bro!", name);
            show_add_bro_message("Restart to see new Bros", text);
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
