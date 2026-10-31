#include "gunbros/bridge/bridge_core.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/dynarmic_backend.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/emulator/elf_loader.h"
#include "gunbros/filesystem/path_mapper.h"
#include "gunbros/compatibility/jni_virtual_machine.h"
#include "gunbros/compatibility/guest_symbols.h"
#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/platform/android_window.h"
#include "gunbros/platform/android_audio.h"
#include "gunbros/platform/jni_env_helper.h"
#include <time.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <GLES/gl.h>
#endif

pthread_mutex_t g_emu_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_capplet_inst = 0;
static int s_frame_count = 0;

#define TOUCH_Q_SIZE 1024
typedef struct { int type, x, y, pid; } TouchEvt;
static TouchEvt s_touch_q[TOUCH_Q_SIZE];
static int s_touch_h = 0, s_touch_t = 0;
static pthread_mutex_t s_touch_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t bridge_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

void bridge_init(JNIEnv *env, jobject context) {
    if (dynarmic_backend_is_active()) return;
    LOGI("bridge_init: Initializing GunBros ARM64 Dynarmic bridge...");

    audio_init();
    window_init();

#ifdef __ANDROID__
    if (context != NULL && env != NULL) {
        jclass contextClass = (*env)->GetObjectClass(env, context);
        jmethodID getExtFilesMethod = (*env)->GetMethodID(env, contextClass, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
        if (getExtFilesMethod != NULL) {
            jobject fileObj = (*env)->CallObjectMethod(env, context, getExtFilesMethod, NULL);
            if (fileObj != NULL) {
                jclass fileClass = (*env)->GetObjectClass(env, fileObj);
                jmethodID getAbsPathMethod = (*env)->GetMethodID(env, fileClass, "getAbsolutePath", "()Ljava/lang/String;");
                if (getAbsPathMethod != NULL) {
                    jstring pathStr = (jstring)(*env)->CallObjectMethod(env, fileObj, getAbsPathMethod);
                    if (pathStr != NULL) {
                        const char *cpath = (*env)->GetStringUTFChars(env, pathStr, NULL);
                        if (cpath != NULL) {
                            path_mapper_set_base_dir(cpath);
                            (*env)->ReleaseStringUTFChars(env, pathStr, cpath);
                            LOGI("Resolved canonical storage base dir: '%s'", path_mapper_get_base_dir());
                        }
                    }
                }
            }
        }

        jmethodID getAssetsMethod = (*env)->GetMethodID(env, contextClass, "getAssets", "()Landroid/content/res/AssetManager;");
        if (getAssetsMethod != NULL) {
            jobject assetManagerObj = (*env)->CallObjectMethod(env, context, getAssetsMethod);
            if (assetManagerObj != NULL) {
                AAssetManager* mgr = AAssetManager_fromJava(env, assetManagerObj);
                AAsset* asset = AAssetManager_open(mgr, "libandroidplatformjni.so", AASSET_MODE_BUFFER);
                if (asset != NULL) {
                    const void* buffer = AAsset_getBuffer(asset);
                    off_t length = AAsset_getLength(asset);
                    LOGI("Loaded libandroidplatformjni.so asset: %ld bytes", length);
                    elf_loader_load((const uint8_t*)buffer, (size_t)length, path_mapper_get_base_dir());
                    AAsset_close(asset);
                } else {
                    LOGE("Could not open libandroidplatformjni.so from assets!");
                }
            }
        }
    }
#endif

    if (dynarmic_backend_is_active()) {
        int gw = 1024, gh = 640;
        window_get_dimensions(&gw, &gh);
        uint32_t sw = (uint32_t)gw;
        uint32_t sh = (uint32_t)gh;
        guest_mem_write(GUEST_ADDR_SCREEN_WIDTH, &sw, 4);
        guest_mem_write(GUEST_ADDR_SCREEN_HEIGHT, &sh, 4);
        LOGI("Initialized guest g_screenWidth=%u, g_screenHeight=%u", sw, sh);
    }
}

uint32_t bridge_get_capplet(void) {
    return s_capplet_inst;
}

void bridge_set_capplet(uint32_t capplet) {
    s_capplet_inst = capplet;
}

void bridge_touch_push(int type, int x, int y, int pid) {
    window_screen_to_game(x, y, &x, &y);
    pthread_mutex_lock(&s_touch_mutex);
    int n = (s_touch_t + 1) & (TOUCH_Q_SIZE - 1);
    if (n == s_touch_h) {
        s_touch_h = (s_touch_h + 1) & (TOUCH_Q_SIZE - 1);
    }
    s_touch_q[s_touch_t] = (TouchEvt){type, x, y, pid};
    s_touch_t = n;
    pthread_mutex_unlock(&s_touch_mutex);
}

static void touch_dispatch(int type, int x, int y, int pid) {
    static const uint32_t entry[4] = {
        GUEST_FN_TOUCH_DOWN,
        GUEST_FN_TOUCH_MOVE,
        GUEST_FN_TOUCH_UP,
        GUEST_FN_TOUCH_CANCEL
    };
    prepare_call_arm(0);
    dynarmic_backend_set_reg(2, s_capplet_inst);
    dynarmic_backend_set_reg(3, (uint32_t)x);
    uint32_t sp = GUEST_STACK_TOP;
    uint32_t sp_args[2] = { (uint32_t)y, (uint32_t)pid };
    uint8_t *host_stack = guest_memory_get_stack();
    if (host_stack) {
        memcpy(host_stack + (sp - GUEST_STACK_BASE), sp_args, 8);
    }
    run_guest(entry[type], "touch");

    if (type == 2) { // TouchEnded: Character selection hook
        game_hooks_on_touch_ended(x, y);
    }
}

void bridge_touch_drain(void) {
    for (;;) {
        TouchEvt e;
        pthread_mutex_lock(&s_touch_mutex);
        if (s_touch_h == s_touch_t) {
            pthread_mutex_unlock(&s_touch_mutex);
            return;
        }
        e = s_touch_q[s_touch_h];
        s_touch_h = (s_touch_h + 1) & (TOUCH_Q_SIZE - 1);
        pthread_mutex_unlock(&s_touch_mutex);
        DLOG("Touch type=%d x=%d y=%d pid=%d", e.type, e.x, e.y, e.pid);
        touch_dispatch(e.type, e.x, e.y, e.pid);
    }
}

size_t bridge_audio_mixer(uint8_t *out, size_t len, void *user_data) {
    (void)user_data;
    if (!s_capplet_inst || !guest_memory_get_heap() || !audio_is_engine_ready()) return 0;
    uint32_t guest_buf = GUEST_ADDR_AUDIO_PCM_BUF;
    uint32_t guest_req = (uint32_t)len;
    uint8_t *host_pcm = guest_memory_get_heap() + 0x80000;
    memset(host_pcm, 0, len);
    prepare_call_arm(s_capplet_inst);
    dynarmic_backend_set_reg(1, guest_buf);
    dynarmic_backend_set_reg(2, guest_req);
    run_guest(GUEST_FN_FEED_PCM, "feedPCMMediaPlayer");
    memcpy(out, host_pcm, len);
    return len;
}

void bridge_on_draw_frame(JNIEnv *env, jobject thiz, jint id, jlong time, jboolean b) {
    (void)env; (void)thiz; (void)id;
    uint64_t frame_start_ns = bridge_monotonic_ns();
    s_frame_count++;
    start_watchdog_once();

    if (s_frame_count <= 10 || s_frame_count % 60 == 0) {
        LOGI("onDrawFrame [#%d] time=%ld, flag=%d, capplet=0x%x",
             s_frame_count, (long)time, b, s_capplet_inst);
    }

    if (window_ensure_egl()) {
        if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
            uint64_t lock_wait_start = bridge_monotonic_ns();
            pthread_mutex_lock(&g_emu_mutex);
            uint64_t lock_acquired = bridge_monotonic_ns();

            bridge_touch_drain();

            uint32_t t = (uint32_t)time;
            uint32_t flag = b ? 1 : 0;
            prepare_call_arm(s_capplet_inst);
            dynarmic_backend_set_reg(1, t);
            dynarmic_backend_set_reg(2, flag);

            uint64_t guest_start = bridge_monotonic_ns();
            run_guest(GUEST_FN_ON_DRAW_FRAME, "frame");
            uint64_t guest_elapsed = bridge_monotonic_ns() - guest_start;
            game_hooks_seed_network_time();
            game_hooks_refinery_unlock_chain();
            game_hooks_ngs_warmup();
            game_hooks_auto_register();
            game_hooks_default_bro();
            game_hooks_ngs_profile_online();
            game_hooks_add_bro();
            uint64_t lock_wait = lock_acquired - lock_wait_start;

            if (guest_elapsed > 50000000ull || lock_wait > 10000000ull) {
                LOGI("FRAME STALL: guest=%.2f ms emu_lock_wait=%.2f ms frame=%d",
                     guest_elapsed / 1000000.0, lock_wait / 1000000.0, s_frame_count);
            }

            // Periodic diagnostics
            if (s_frame_count <= 10 || s_frame_count % 60 == 0) {
                uint32_t app_ptr = 0;
                guest_mem_read(GUEST_ADDR_APP_PTR, &app_ptr, 4);
                if (app_ptr != 0) {
                    uint32_t res_loader = 0, gunbros = 0;
                    guest_mem_read(app_ptr + GUEST_OFF_APP_RES_LOADER, &res_loader, 4);
                    guest_mem_read(app_ptr + GUEST_OFF_APP_GUNBROS, &gunbros, 4);
                    uint16_t res_tot = 0, res_cur = 0;
                    if (res_loader) {
                        guest_mem_read(res_loader + GUEST_OFF_RESLOADER_TOTAL, &res_tot, 2);
                        guest_mem_read(res_loader + GUEST_OFF_RESLOADER_CURRENT, &res_cur, 2);
                    }
                    uint32_t gstate = 0;
                    if (gunbros) {
                        guest_mem_read(gunbros + GUEST_OFF_GB_GSTATE, &gstate, 4);
                    }
                    LOGI("STATE [#%d]: app=0x%x res=%d/%d gb=0x%x gstate=%d",
                         s_frame_count, app_ptr, res_cur, res_tot, gunbros, gstate);
                }
            }

            audio_pump_locked(bridge_audio_mixer, NULL);
            pthread_mutex_unlock(&g_emu_mutex);
        } else {
#ifdef __ANDROID__
            glClearColor(0.1f, 0.5f, 0.2f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
        }
        window_swap_buffers();
    }

    uint64_t frame_elapsed = bridge_monotonic_ns() - frame_start_ns;
    if (frame_elapsed > 50000000ull) {
        LOGI("SLOW onDrawFrame: %.2f ms frame=%d", frame_elapsed / 1000000.0, s_frame_count);
    }
}

jint bridge_on_create(JNIEnv *env, jobject thiz, jbyteArray array, jobject callback) {
    (void)array;
    LOGI("bridge_on_create called");
    if (env != NULL) {
        JavaVM *jvm = NULL;
        (*env)->GetJavaVM(env, &jvm);
        platform_set_jvm(jvm);
    }
    platform_set_callback(env, callback);

    if (dynarmic_backend_is_active() && s_capplet_inst == 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(GUEST_ENV_OBJ);
        uint32_t dummy_thiz = GUEST_DUMMY_OBJ;
        uint32_t dummy_arr = GUEST_DUMMY_OBJ;
        uint32_t dummy_cb = GUEST_DUMMY_OBJ;
        dynarmic_backend_set_reg(1, dummy_thiz);
        dynarmic_backend_set_reg(2, dummy_arr);
        dynarmic_backend_set_reg(3, dummy_cb);

        LOGI("Calling original onCreate at 0x162638...");
        run_result_t err = run_guest(GUEST_FN_ON_CREATE, "onCreate");
        uint32_t ret_capplet = dynarmic_backend_get_reg(0);
        LOGI("Original onCreate returned res=%d, capplet=0x%x", err, ret_capplet);

        if (err == RUN_OK && ret_capplet != 0) {
            s_capplet_inst = ret_capplet;
        } else {
            s_capplet_inst = ret_capplet ? ret_capplet : GUEST_ADDR_FALLBACK_CAPPLET;
        }
        pthread_mutex_unlock(&g_emu_mutex);
    }
    LOGI("bridge_on_create returning CApplet pointer 0x%x", s_capplet_inst);
    return (jint)s_capplet_inst;
}

void bridge_on_surface_created(jint id) {
    (void)id;
    LOGI("bridge_on_surface_created");
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_SURFACE_CREATED, "surfaceCreated");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_surface_changed(jint id, jint format, jint w, jint h) {
    (void)id;
    LOGI("bridge_on_surface_changed: format=%d w=%d h=%d", format, w, h);
    window_set_screen_dimensions(w, h);
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        int gw = 1024, gh = 640;
        window_get_dimensions(&gw, &gh);
        prepare_call_arm(s_capplet_inst);
        dynarmic_backend_set_reg(1, (uint32_t)format);
        dynarmic_backend_set_reg(2, (uint32_t)gw);
        dynarmic_backend_set_reg(3, (uint32_t)gh);
        run_guest(GUEST_FN_SURFACE_CHANGED, "surfaceChanged");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_gl_surface_created(jint id) {
    (void)id;
    LOGI("bridge_on_gl_surface_created");
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_GL_SURFACE_CREATED, "onSurfaceCreated");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_gl_surface_changed(jint id, jint w, jint h) {
    (void)id;
    LOGI("bridge_on_gl_surface_changed w=%d, h=%d", w, h);
    window_set_screen_dimensions(w, h);
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        int gw = 1024, gh = 640;
        window_get_dimensions(&gw, &gh);
        prepare_call_arm(s_capplet_inst);
        dynarmic_backend_set_reg(1, (uint32_t)gw);
        dynarmic_backend_set_reg(2, (uint32_t)gh);
        run_guest(GUEST_FN_GL_SURFACE_CHANGED, "onSurfaceChanged");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_surface_destroyed(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_SURFACE_DESTROYED, "surfaceDestroyed");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_destroy(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_DESTROY, "onDestroy");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_pause(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_PAUSE, "onPause");
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_PROCESS_EVENTS, "processEvents");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_resume(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_RESUME, "onResume");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_start(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_START, "onStart");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_stop(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_STOP, "onStop");
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_PROCESS_EVENTS, "processEvents");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_restart(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_RESTART, "onRestart");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_low_memory(jint id) {
    (void)id;
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        run_guest(GUEST_FN_ON_LOW_MEMORY, "onLowMemory");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}

void bridge_on_orientation_changed(jint orientation) {
    if (dynarmic_backend_is_active() && s_capplet_inst != 0) {
        pthread_mutex_lock(&g_emu_mutex);
        prepare_call_arm(s_capplet_inst);
        dynarmic_backend_set_reg(1, (uint32_t)orientation);
        run_guest(GUEST_FN_ON_ORIENTATION_CHANGED, "onOrientationChanged");
        pthread_mutex_unlock(&g_emu_mutex);
    }
}
