#include "gunbros/compatibility/jni_methods.h"
#include "gunbros/compatibility/jni_audio.h"
#include "gunbros/compatibility/jni_textbox.h"
#include "gunbros/compatibility/jni_virtual_machine.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/platform/jni_env_helper.h"
#include <string.h>

uint32_t get_jni_method_arg(int idx, uint32_t r3, uint32_t sp, int arg_idx) {
    int is_va = (idx == 35 || idx == 36 || idx == 38 || idx == 39 ||
                 idx == 41 || idx == 42 || idx == 44 || idx == 45 ||
                 idx == 47 || idx == 48 || idx == 50 || idx == 51 ||
                 idx == 53 || idx == 54 || idx == 56 || idx == 57 ||
                 idx == 59 || idx == 60 || idx == 62 || idx == 63 ||
                 idx == 68 || idx == 69 || idx == 80 || idx == 81 ||
                 idx == 92 || idx == 93 || idx == 118 || idx == 119 ||
                 idx == 130 || idx == 131 || idx == 142 || idx == 143);

    if (is_va || (r3 >= GUEST_STACK_BASE && r3 < GUEST_STACK_BASE + GUEST_STACK_SIZE)) {
        uint32_t *args = (uint32_t*)guest_to_host(r3);
        if (args) return args[arg_idx];
        return 0;
    }

    if (arg_idx == 0) return r3;
    uint32_t *sp_args = (uint32_t*)guest_to_host(sp);
    if (sp_args) return sp_args[arg_idx - 1];
    return 0;
}

static bool jni_method_resdl_string_event(const jni_call_context_t *ctx) {
    *ctx->r0 = GUEST_RESDL_STRING;
    return true;
}

static bool jni_method_open_apk_file(const jni_call_context_t *ctx) {
#ifdef BRIDGE_DEBUG
    uint32_t fname_arg = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);
    const char *fname = (const char*)guest_to_host(fname_arg);
    LOGI("[BRIDGE_DEBUG:JNI] OpenAPKFile called for '%s' - returning NULL", fname ? fname : "NULL");
#else
    DLOG("JNI OpenAPKFile: returning NULL (resources exist on disk)");
#endif
    *ctx->r0 = 0;
    return true;
}

static bool jni_method_egl_get_error(const jni_call_context_t *ctx) {
    *ctx->r0 = 12288; // 0x3000 = EGL_SUCCESS
    return true;
}

static bool jni_method_get_device_orientation(const jni_call_context_t *ctx) {
    *ctx->r0 = 0; // Landscape
    return true;
}

static const char* get_path_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    if (slash) path = slash + 1;
    const char *colon = strrchr(path, ':');
    if (colon) path = colon + 1;
    return path;
}

static bool is_special_resource_file(const char *name) {
    if (!name || name[0] == '\0') return false;
    static const char * const s_special_res[] = {
        "events.dat",
        "packTOC_wvga.dat",
        "packTOC_xga.dat",
        "Gman_intro.3gp",
        "1.mp3", "2.mp3", "3.mp3", "4.mp3", "5.mp3", "6.mp3", "game_0.mp3"
    };
    for (size_t i = 0; i < sizeof(s_special_res)/sizeof(s_special_res[0]); i++) {
        if (strcmp(name, s_special_res[i]) == 0) return true;
    }
    return false;
}

static bool jni_method_resdl_event(const jni_call_context_t *ctx) {
    uint32_t event = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);
    uint32_t arg1 = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 1);
    uint32_t barr_obj = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 2);

    const char *fname = (const char*)guest_to_host(barr_obj);
    if (!fname || fname[0] == '\0') {
        fname = jvm_get_last_byte_array();
    }

    bool is_resource = false;
    if (event == 2 && fname && fname[0] != '\0') {
        const char *base = get_path_basename(fname);
        size_t blen = strlen(base);
        if (blen >= 4 && strcmp(base + blen - 4, ".big") == 0) {
            is_resource = true;
        } else if (is_special_resource_file(base)) {
            is_resource = true;
        }
    }

    LOGI("[JNI:resdlEvent] event=%u, arg1=%u, file='%s' -> %s",
         event, arg1, (fname && fname[0]) ? fname : "(null)",
         is_resource ? "RESOURCE (1)" : "LOCAL/SAVE (0)");

    *ctx->r0 = is_resource ? 1 : 0;
    return true;
}

static bool jni_method_get_device_available_memory(const jni_call_context_t *ctx) {
    *ctx->r0 = 64 * 1024 * 1024;
    if (ctx->r1) *ctx->r1 = 0;
    return true;
}

static bool jni_method_enable_multitouch(const jni_call_context_t *ctx) {
    uint32_t enable = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);
    JNIEnv *real_env = platform_get_env();
    jobject real_callback = platform_get_callback();
    if (real_env && real_callback) {
        jclass cls = (*real_env)->GetObjectClass(real_env, real_callback);
        jmethodID mid = (*real_env)->GetMethodID(real_env, cls, "EnableMultipleTouch", "(Z)V");
        if (mid) (*real_env)->CallVoidMethod(real_env, real_callback, mid, (jboolean)(enable ? JNI_TRUE : JNI_FALSE));
    }
    *ctx->r0 = 0;
    return true;
}

static bool jni_method_set_tick_rate(const jni_call_context_t *ctx) {
    uint32_t rate = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);
    LOGI("[JNI] SetTickRate called with rate=%u ms", rate);
    JNIEnv *real_env = platform_get_env();
    jobject real_callback = platform_get_callback();
    if (real_env && real_callback) {
        jclass cls = (*real_env)->GetObjectClass(real_env, real_callback);
        jmethodID mid = (*real_env)->GetMethodID(real_env, cls, "SetTickRate", "(I)V");
        if (mid) {
            (*real_env)->CallVoidMethod(real_env, real_callback, mid, (jint)rate);
            LOGI("[JNI] Forwarded SetTickRate(%u) to Java GluCallback", rate);
        }
    }
    *ctx->r0 = 0;
    return true;
}

static bool jni_method_start_movie_activity(const jni_call_context_t *ctx) {
    LOGI("[JNI] StartMovieActivity called - returning false to skip intro video and enter main menu shell");
    *ctx->r0 = 0;
    return true;
}

static bool jni_method_return_false(const jni_call_context_t *ctx) {
    *ctx->r0 = 0;
    return true;
}

static bool jni_method_is_user_on_wifi(const jni_call_context_t *ctx) {
    static bool logged = false;
    if (!logged) {
        LOGI("[JNI] GluCallback.IsUserOnWiFi -> returning true (1)");
        logged = true;
    }
    *ctx->r0 = 1;
    return true;
}

static bool jni_method_get_type(const jni_call_context_t *ctx) {
    LOGI("[JNI] NetworkInfo.getType -> returning 1 (TYPE_WIFI)");
    *ctx->r0 = 1; // ConnectivityManager.TYPE_WIFI
    return true;
}

bool jni_launch_url(const char *url, int type) {
    bool ok = false;
    JNIEnv *real_env = platform_get_env();
    jobject real_callback = platform_get_callback();
    if (real_env && real_callback && url && url[0] != '\0') {
        jstring j_url = (*real_env)->NewStringUTF(real_env, url);
        if (j_url) {
            jclass cls = (*real_env)->GetObjectClass(real_env, real_callback);
            jmethodID mid = (*real_env)->GetMethodID(real_env, cls, "LaunchURL", "(Ljava/lang/String;I)Z");
            if (mid) {
                jboolean res = (*real_env)->CallBooleanMethod(real_env, real_callback, mid, j_url, (jint)type);
                LOGI("[JNI] LaunchURL(String, int) returned %d from Java", res);
                ok = res;
            } else {
                (*real_env)->ExceptionClear(real_env);
                jmethodID mid_fallback = (*real_env)->GetMethodID(real_env, cls, "LaunchURL", "(Ljava/lang/String;)Z");
                if (mid_fallback) {
                    jboolean res = (*real_env)->CallBooleanMethod(real_env, real_callback, mid_fallback, j_url);
                    LOGI("[JNI] LaunchURL(String) fallback returned %d from Java", res);
                    ok = res;
                } else {
                    (*real_env)->ExceptionClear(real_env);
                    LOGE("[JNI] Could not find LaunchURL method in Java callback");
                }
            }
            (*real_env)->DeleteLocalRef(real_env, j_url);
        }
    }
    return ok;
}

bool jni_copy_to_clipboard(const char *label, const char *text) {
    JNIEnv *env = platform_get_env();
    jobject context = platform_get_callback();
    if (!env || !context || !text) return false;
    bool ok = false;
    jclass ctx_cls = (*env)->GetObjectClass(env, context);
    jmethodID get_service = (*env)->GetMethodID(env, ctx_cls, "getSystemService",
                                                "(Ljava/lang/String;)Ljava/lang/Object;");
    jclass clip_cls = (*env)->FindClass(env, "android/content/ClipData");
    jmethodID new_plain = clip_cls ? (*env)->GetStaticMethodID(env, clip_cls, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;") : NULL;
    if (get_service && new_plain) {
        jstring j_service = (*env)->NewStringUTF(env, "clipboard");
        jobject manager = (*env)->CallObjectMethod(env, context, get_service, j_service);
        if (manager) {
            jstring j_label = (*env)->NewStringUTF(env, label);
            jstring j_text = (*env)->NewStringUTF(env, text);
            jobject clip = (*env)->CallStaticObjectMethod(env, clip_cls, new_plain, j_label, j_text);
            jclass cm_cls = (*env)->GetObjectClass(env, manager);
            jmethodID set_clip = (*env)->GetMethodID(env, cm_cls, "setPrimaryClip", "(Landroid/content/ClipData;)V");
            if (clip && set_clip) {
                (*env)->CallVoidMethod(env, manager, set_clip, clip);
                ok = !(*env)->ExceptionCheck(env);
            }
            (*env)->DeleteLocalRef(env, j_label);
            (*env)->DeleteLocalRef(env, j_text);
            if (clip) (*env)->DeleteLocalRef(env, clip);
            (*env)->DeleteLocalRef(env, cm_cls);
            (*env)->DeleteLocalRef(env, manager);
        }
        (*env)->DeleteLocalRef(env, j_service);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        ok = false;
    }
    if (clip_cls) (*env)->DeleteLocalRef(env, clip_cls);
    (*env)->DeleteLocalRef(env, ctx_cls);
    LOGI("[JNI] copy to clipboard %s", ok ? "done" : "failed");
    return ok;
}

static bool jni_method_launch_url(const jni_call_context_t *ctx) {
    const char *url = jvm_get_last_string();
    uint32_t type = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 1);
    LOGI("[JNI] LaunchURL called: url='%s', type=%u", url ? url : "(null)", type);

    static const char *GUNBROS_WIKI_URL = "https://gunbros.fandom.com/wiki/Gun_Bros_Wiki";
    if (url && (strstr(url, "ghome") != NULL || strstr(url, "m.glu.com") != NULL)) {
        LOGI("[JNI] LaunchURL: Redirecting dead Glu URL '%s' -> '%s'", url, GUNBROS_WIKI_URL);
        url = GUNBROS_WIKI_URL;
    }

    jni_launch_url(url, (int)type);
    *ctx->r0 = 1;
    return true;
}

static const jni_method_entry_t s_jni_method_table[] = {
    /* Separated audio subsystem methods */
    { "InitialiseSoundEngine",     jni_audio_initialise_sound_engine },
    { "StartAudioStream",         jni_audio_start_audio_stream },
    { "StopAudioStream",          jni_audio_stop_audio_stream },
    { "GetAndroidMinBufferSize",  jni_audio_get_min_buffer_size },
    { "mp3Event",                 jni_audio_mp3_event },
    { "SetVolume",                jni_audio_set_volume },

    /* Engine, GL & filesystem methods */
    { "resdlStringEvent",         jni_method_resdl_string_event },
    { "OpenAPKFile",              jni_method_open_apk_file },
    { "eglGetError",              jni_method_egl_get_error },
    { "GetDeviceOrientation",     jni_method_get_device_orientation },
    { "resdlEvent",               jni_method_resdl_event },
    { "GetDeviceAvailableMemory", jni_method_get_device_available_memory },
    { "EnableMultipleTouch",      jni_method_enable_multitouch },
    { "SetTickRate",              jni_method_set_tick_rate },
    { "StartMovieActivity",       jni_method_start_movie_activity },

    /* URL Launch (Browser redirect) */
    { "LaunchURL",                jni_method_launch_url },

    /* Connectivity & network queries */
    { "IsUserOnWiFi",             jni_method_is_user_on_wifi },
    { "isUserOnWiFi",             jni_method_is_user_on_wifi },
    { "getType",                  jni_method_get_type },

    /* GluTextBox UI Text Fields */
    { "SetBounds",                jni_method_textbox_set_bounds },
    { "SetVisible",               jni_method_textbox_set_visible },
    { "SetPasswordMask",          jni_method_textbox_set_password_mask },
    { "SetText",                  jni_method_textbox_set_text },
    { "GetText",                  jni_method_textbox_get_text },
    { "RemoveTextView",           jni_method_textbox_remove_text_view },

    /* Feature queries returning false */
    { "IsPSP",                    jni_method_return_false },
    { "interfaceIsOpen",          jni_method_return_false },
    { "isVisible",                jni_method_return_false },
    { "canDisplayInterface",      jni_method_return_false },
    { "getFeaturedAppQueryState", jni_method_return_false },
};

bool jni_methods_dispatch(const char *name, const jni_call_context_t *ctx) {
    if (!name || !ctx) return false;
    size_t count = sizeof(s_jni_method_table) / sizeof(s_jni_method_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, s_jni_method_table[i].name) == 0) {
            return s_jni_method_table[i].handler(ctx);
        }
    }
    return false;
}
