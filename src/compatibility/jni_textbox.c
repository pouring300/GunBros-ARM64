#include "gunbros/compatibility/jni_textbox.h"
#include "gunbros/compatibility/jni_virtual_machine.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/platform/jni_env_helper.h"
#include "gunbros/platform/android_window.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    bool in_use;
    uint32_t handle;       // GUEST_TEXTBOX_BASE + i * 0x100
    jobject real_obj;      // GlobalRef to the Java GluTextBox
    char text_buf[256];    // UTF-8 text buffer returned to the guest
    int last_x, last_y, last_w, last_h;
    bool is_visible;
    bool is_masked;
} textbox_slot_t;

static textbox_slot_t s_textboxes[MAX_TEXTBOXES];
static pthread_mutex_t s_textbox_mutex = PTHREAD_MUTEX_INITIALIZER;
static jclass s_glu_textbox_class = NULL;

static inline int get_slot_by_handle(uint32_t handle) {
    if (handle < GUEST_TEXTBOX_BASE) return -1;
    uint32_t offset = handle - GUEST_TEXTBOX_BASE;
    uint32_t slot = offset / 0x100;
    if (slot >= MAX_TEXTBOXES) return -1;
    if (!s_textboxes[slot].in_use) return -1;
    return (int)slot;
}

static jclass get_glu_textbox_class(JNIEnv *env) {
    if (s_glu_textbox_class != NULL) {
        return s_glu_textbox_class;
    }
    jobject cb = platform_get_callback();
    if (!cb) {
        LOGE("[GluTextBox] platform_get_callback returned NULL");
        return NULL;
    }
    jclass act_cls = (*env)->GetObjectClass(env, cb);
    if (!act_cls) return NULL;

    // Use the Activity's ClassLoader to ensure resolution through DexClassLoader
    jmethodID get_cl_mid = (*env)->GetMethodID(env, act_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (get_cl_mid) {
        jobject cl_obj = (*env)->CallObjectMethod(env, cb, get_cl_mid);
        if (cl_obj) {
            jclass cl_cls = (*env)->GetObjectClass(env, cl_obj);
            jmethodID load_class_mid = (*env)->GetMethodID(env, cl_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
            if (load_class_mid) {
                jstring name_str = (*env)->NewStringUTF(env, "com.glu.platform.android.GluTextBox");
                jclass loaded_cls = (jclass)(*env)->CallObjectMethod(env, cl_obj, load_class_mid, name_str);
                (*env)->DeleteLocalRef(env, name_str);
                if (loaded_cls) {
                    s_glu_textbox_class = (jclass)(*env)->NewGlobalRef(env, loaded_cls);
                    (*env)->DeleteLocalRef(env, loaded_cls);
                    LOGI("[GluTextBox] Loaded com.glu.platform.android.GluTextBox via ClassLoader (%p)", s_glu_textbox_class);
                    return s_glu_textbox_class;
                }
            }
        }
    }

    // Fall back to FindClass
    jclass direct_cls = (*env)->FindClass(env, "com/glu/platform/android/GluTextBox");
    if (direct_cls) {
        s_glu_textbox_class = (jclass)(*env)->NewGlobalRef(env, direct_cls);
        (*env)->DeleteLocalRef(env, direct_cls);
        LOGI("[GluTextBox] Loaded via FindClass (%p)", s_glu_textbox_class);
    } else {
        (*env)->ExceptionClear(env);
        LOGE("[GluTextBox] Failed to find com.glu.platform.android.GluTextBox");
    }
    return s_glu_textbox_class;
}

void jni_textbox_init(void) {
    pthread_mutex_lock(&s_textbox_mutex);
    memset(s_textboxes, 0, sizeof(s_textboxes));
    pthread_mutex_unlock(&s_textbox_mutex);
}

uint32_t jni_textbox_create(void) {
    pthread_mutex_lock(&s_textbox_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_TEXTBOXES; i++) {
        if (!s_textboxes[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        pthread_mutex_unlock(&s_textbox_mutex);
        LOGE("[GluTextBox] No free textbox slots!");
        return 0;
    }

    uint32_t handle = GUEST_TEXTBOX_BASE + slot * 0x100;
    s_textboxes[slot].in_use = true;
    s_textboxes[slot].handle = handle;
    s_textboxes[slot].real_obj = NULL;
    s_textboxes[slot].text_buf[0] = '\0';
    s_textboxes[slot].last_x = 0;
    s_textboxes[slot].last_y = 0;
    s_textboxes[slot].last_w = 0;
    s_textboxes[slot].last_h = 0;
    s_textboxes[slot].is_visible = false;
    s_textboxes[slot].is_masked = false;

    // Initialize guest memory at 'handle'
    uint8_t *guest_slot = (uint8_t*)guest_to_host(handle);
    if (guest_slot) {
        memset(guest_slot, 0, 0x100);
    }

    JNIEnv *env = platform_get_env();
    if (env) {
        jclass cls = get_glu_textbox_class(env);
        if (cls) {
            jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "()V");
            if (ctor) {
                jobject obj = (*env)->NewObject(env, cls, ctor);
                if (obj) {
                    s_textboxes[slot].real_obj = (*env)->NewGlobalRef(env, obj);
                    (*env)->DeleteLocalRef(env, obj);
                    LOGI("[GluTextBox] Created Java instance %p for slot %d (handle 0x%x)",
                         s_textboxes[slot].real_obj, slot, handle);
                } else {
                    LOGE("[GluTextBox] Failed to instantiate GluTextBox in Java");
                }
            }
        }
    }

    pthread_mutex_unlock(&s_textbox_mutex);
    return handle;
}

bool jni_textbox_is_handle(uint32_t handle) {
    if (handle < GUEST_TEXTBOX_BASE) return false;
    uint32_t offset = handle - GUEST_TEXTBOX_BASE;
    uint32_t slot = offset / 0x100;
    if (slot >= MAX_TEXTBOXES) return false;
    pthread_mutex_lock(&s_textbox_mutex);
    bool in_use = s_textboxes[slot].in_use && (s_textboxes[slot].handle == handle);
    pthread_mutex_unlock(&s_textbox_mutex);
    return in_use;
}

void jni_textbox_destroy(uint32_t handle) {
    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        LOGI("[GluTextBox] Destroying textbox slot %d (handle 0x%x)", slot, handle);
        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "RemoveTextView", "()V");
                    if (mid) {
                        (*env)->CallVoidMethod(env, real_obj, mid);
                    }
                }
                (*env)->DeleteGlobalRef(env, real_obj);
            }
        }
        memset(&s_textboxes[slot], 0, sizeof(textbox_slot_t));
    }
    pthread_mutex_unlock(&s_textbox_mutex);
}

bool jni_textbox_is_string_obj(uint32_t str_obj) {
    if (str_obj >= GUEST_TEXTBOX_BASE && str_obj < GUEST_TEXTBOX_BASE + MAX_TEXTBOXES * 0x100) {
        uint32_t offset = str_obj - GUEST_TEXTBOX_BASE;
        if ((offset % 0x100) == 0x40) {
            int slot = (int)(offset / 0x100);
            pthread_mutex_lock(&s_textbox_mutex);
            bool valid = s_textboxes[slot].in_use;
            pthread_mutex_unlock(&s_textbox_mutex);
            return valid;
        }
    }
    return false;
}

uint32_t jni_textbox_get_string_ptr(uint32_t str_obj) {
    return str_obj;
}

bool jni_method_textbox_set_bounds(const jni_call_context_t *ctx) {
    uint32_t handle = *ctx->r1;
    int gx = (int)get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);
    int gy = (int)get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 1);
    int gw = (int)get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 2);
    int gh = (int)get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 3);

    int sx = 0, sy = 0, sw = 0, sh = 0;
    window_game_to_screen(gx, gy, gw, gh, &sx, &sy, &sw, &sh);

    LOGI("[GluTextBox:SetBounds] handle=0x%x, game=(%d,%d,%d,%d) -> screen=(%d,%d,%d,%d)",
         handle, gx, gy, gw, gh, sx, sy, sw, sh);

    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        s_textboxes[slot].last_x = sx;
        s_textboxes[slot].last_y = sy;
        s_textboxes[slot].last_w = sw;
        s_textboxes[slot].last_h = sh;

        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "SetBounds", "(IIII)V");
                    if (mid) {
                        (*env)->CallVoidMethod(env, real_obj, mid, (jint)sx, (jint)sy, (jint)sw, (jint)sh);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&s_textbox_mutex);

    // Return the handle (!= 0) so the C++ side marks the field as initialized (r4 + 0x1c)
    *ctx->r0 = handle;
    return true;
}

bool jni_method_textbox_set_visible(const jni_call_context_t *ctx) {
    uint32_t handle = *ctx->r1;
    uint32_t visible = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);

    LOGI("[GluTextBox:SetVisible] handle=0x%x, visible=%u", handle, visible);

    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        s_textboxes[slot].is_visible = (visible != 0);
        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "SetVisible", "(Z)V");
                    if (mid) {
                        (*env)->CallVoidMethod(env, real_obj, mid, (jboolean)(visible ? JNI_TRUE : JNI_FALSE));
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&s_textbox_mutex);

    *ctx->r0 = handle;
    return true;
}

bool jni_method_textbox_set_password_mask(const jni_call_context_t *ctx) {
    uint32_t handle = *ctx->r1;
    uint32_t masked = get_jni_method_arg(ctx->idx, *ctx->r3, ctx->sp, 0);

    LOGI("[GluTextBox:SetPasswordMask] handle=0x%x, masked=%u", handle, masked);

    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        s_textboxes[slot].is_masked = (masked != 0);
        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "SetPasswordMask", "(Z)V");
                    if (mid) {
                        (*env)->CallVoidMethod(env, real_obj, mid, (jboolean)(masked ? JNI_TRUE : JNI_FALSE));
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&s_textbox_mutex);

    *ctx->r0 = handle;
    return true;
}

bool jni_method_textbox_set_text(const jni_call_context_t *ctx) {
    uint32_t handle = *ctx->r1;
    const char *text = jvm_get_last_string();
    LOGI("[GluTextBox:SetText] handle=0x%x, text='%s'", handle, text ? text : "(null)");

    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "SetText", "(Ljava/lang/String;Ljava/lang/String;)V");
                    if (mid) {
                        jstring j_text = (*env)->NewStringUTF(env, text ? text : "");
                        jstring j_hint = (*env)->NewStringUTF(env, "");
                        (*env)->CallVoidMethod(env, real_obj, mid, j_text, j_hint);
                        if (j_text) (*env)->DeleteLocalRef(env, j_text);
                        if (j_hint) (*env)->DeleteLocalRef(env, j_hint);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&s_textbox_mutex);

    *ctx->r0 = handle;
    return true;
}

bool jni_method_textbox_get_text(const jni_call_context_t *ctx) {
    uint32_t handle = *ctx->r1;
    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "GetText", "()Ljava/lang/String;");
                    if (mid) {
                        jstring j_res = (jstring)(*env)->CallObjectMethod(env, real_obj, mid);
                        if (j_res) {
                            const char *chars = (*env)->GetStringUTFChars(env, j_res, NULL);
                            if (chars) {
                                strncpy(s_textboxes[slot].text_buf, chars, sizeof(s_textboxes[slot].text_buf) - 1);
                                s_textboxes[slot].text_buf[sizeof(s_textboxes[slot].text_buf) - 1] = '\0';
                                (*env)->ReleaseStringUTFChars(env, j_res, chars);
                            }
                            (*env)->DeleteLocalRef(env, j_res);
                        }
                    }
                }
            }
        }

        // Write the text buffer into guest memory (handle + 0x40)
        uint8_t *guest_buf = (uint8_t*)guest_to_host(handle + 0x40);
        if (guest_buf) {
            size_t len = strlen(s_textboxes[slot].text_buf);
            memcpy(guest_buf, s_textboxes[slot].text_buf, len + 1);
        }

        LOGI("[GluTextBox:GetText] handle=0x%x -> '%s'", handle, s_textboxes[slot].text_buf);
        // Return the guest jstring handle: handle + 0x40
        *ctx->r0 = handle + 0x40;
    } else {
        *ctx->r0 = GUEST_EMPTY_STRING;
    }
    pthread_mutex_unlock(&s_textbox_mutex);
    return true;
}

bool jni_textbox_read_text(uint32_t handle, char *out, size_t out_size) {
    bool ok = false;
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    JNIEnv *env = slot >= 0 && s_textboxes[slot].real_obj ? platform_get_env() : NULL;
    jclass cls = env ? get_glu_textbox_class(env) : NULL;
    jmethodID mid = cls ? (*env)->GetMethodID(env, cls, "GetText", "()Ljava/lang/String;") : NULL;
    if (mid) {
        jstring j_res = (jstring)(*env)->CallObjectMethod(env, s_textboxes[slot].real_obj, mid);
        if (j_res) {
            const char *chars = (*env)->GetStringUTFChars(env, j_res, NULL);
            if (chars) {
                strncpy(out, chars, out_size - 1);
                out[out_size - 1] = '\0';
                (*env)->ReleaseStringUTFChars(env, j_res, chars);
                ok = true;
            }
            (*env)->DeleteLocalRef(env, j_res);
        }
    }
    pthread_mutex_unlock(&s_textbox_mutex);
    return ok;
}

bool jni_method_textbox_remove_text_view(const jni_call_context_t *ctx) {
    uint32_t handle = *ctx->r1;
    LOGI("[GluTextBox:RemoveTextView] handle=0x%x", handle);

    pthread_mutex_lock(&s_textbox_mutex);
    int slot = get_slot_by_handle(handle);
    if (slot >= 0) {
        jobject real_obj = s_textboxes[slot].real_obj;
        if (real_obj) {
            JNIEnv *env = platform_get_env();
            if (env) {
                jclass cls = get_glu_textbox_class(env);
                if (cls) {
                    jmethodID mid = (*env)->GetMethodID(env, cls, "RemoveTextView", "()V");
                    if (mid) {
                        (*env)->CallVoidMethod(env, real_obj, mid);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&s_textbox_mutex);

    *ctx->r0 = 0;
    return true;
}
