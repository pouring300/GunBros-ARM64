#include "gunbros/compatibility/jni_virtual_machine.h"
#include "gunbros/compatibility/jni_methods.h"
#include "gunbros/compatibility/jni_textbox.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/platform/jni_env_helper.h"
#include "gunbros/platform/android_audio.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    char name[64];
    char sig[64];
} JniMethodInfo;

#define MAX_JNI_METHODS 512
static JniMethodInfo s_jni_methods[MAX_JNI_METHODS];
static int s_jni_method_count = 1;
static char s_last_jni_string[512] = {0};
static char s_prev_jni_string[512] = {0};  /* the string created before the last one */
static char s_last_byte_array[512] = {0};

void jvm_virtual_init(uint8_t *heap_base, uint8_t *stubs_base, const char *game_dir) {
    if (!heap_base || !stubs_base) return;

    jni_textbox_init();

    // Setup 16 fake JavaVM stubs at 0x8000b000
    for (int i = 0; i < 16; i++) {
        uint32_t *s = (uint32_t*)(stubs_base + 0xb000 + i * 8);
        s[0] = 0xef000000 | (0x1000 + i); // svc 0x1000 + i
        s[1] = 0xe12fff1e;                 // bx lr
    }

    // Setup 256 fake JNIEnv stubs at 0x8000c000
    for (int i = 0; i < 256; i++) {
        uint32_t *s = (uint32_t*)(stubs_base + 0xc000 + i * 8);
        s[0] = 0xef000000 | (0x2000 + i); // svc 0x2000 + i
        s[1] = 0xe12fff1e;                 // bx lr
    }

    // Setup fake JavaVM object at 0x60000100 -> vtable at 0x60000120
    *(uint32_t*)(heap_base + 0x100) = GUEST_JVM_VTABLE;
    uint32_t *jvm_table = (uint32_t*)(heap_base + 0x120);
    for (int i = 0; i < 16; i++) {
        jvm_table[i] = GUEST_JVM_STUBS + i * 8;
    }

    // Setup fake JNIEnv object at 0x60001000 -> vtable at 0x60001100
    *(uint32_t*)(heap_base + 0x1000) = GUEST_ENV_VTABLE;
    uint32_t *env_table = (uint32_t*)(heap_base + 0x1100);
    for (int i = 0; i < 256; i++) {
        env_table[i] = GUEST_ENV_STUBS + i * 8;
    }

    // Initialize dummy heap pointers and handles
    *(uint32_t*)(heap_base + 0x2000) = GUEST_DUMMY_OBJ;
    *(uint32_t*)(heap_base + 0x2080) = GUEST_SENTINEL_OBJ; // EGL_NO_xxx
    *(uint32_t*)(heap_base + 0x2100) = GUEST_RESDL_STRING;
    *(char*)(heap_base + 0x3000) = '\0';                   // empty string
    if (game_dir) {
        strncpy((char*)(heap_base + 0x4000), game_dir, 511);
    }
    *(uint32_t*)(heap_base + 0x6000) = 0;                  // general array / direct buffer
    *(uint32_t*)(heap_base + 0x7000) = GUEST_TLS_BASE;     // tls[0] = self
    *(uint32_t*)(heap_base + 0x7020) = 0;                  // errno = 0

    s_jni_method_count = 1;
}

int jvm_register_method(const char *name, const char *sig) {
    if (!name) return 0;
    if (strcmp(name, "eglGetError") == 0) return 0x00003000;
    if (strcmp(name, "GetDeviceOrientation") == 0) return 0x00003001;
    for (int i = 1; i < s_jni_method_count; i++) {
        if (strcmp(s_jni_methods[i].name, name) == 0) {
            return i;
        }
    }
    if (s_jni_method_count < MAX_JNI_METHODS) {
        int id = s_jni_method_count++;
        strncpy(s_jni_methods[id].name, name, sizeof(s_jni_methods[id].name) - 1);
        s_jni_methods[id].name[sizeof(s_jni_methods[id].name) - 1] = '\0';
        if (sig) {
            strncpy(s_jni_methods[id].sig, sig, sizeof(s_jni_methods[id].sig) - 1);
            s_jni_methods[id].sig[sizeof(s_jni_methods[id].sig) - 1] = '\0';
        }
        return id;
    }
    return 1;
}

const char* jvm_get_method_name(uint32_t id) {
    if (id == 0x00003000) return "eglGetError";
    if (id == 0x00003001) return "GetDeviceOrientation";
    if (id > 0 && id < (uint32_t)s_jni_method_count) {
        return s_jni_methods[id].name;
    }
    return "unknown";
}

static inline bool is_frequent_jni_method_name(const char *name) {
    if (!name) return false;
    return (strcmp(name, "getSystemService") == 0 ||
            strcmp(name, "getActiveNetworkInfo") == 0 ||
            strcmp(name, "isAvailable") == 0 ||
            strcmp(name, "IsUserOnWiFi") == 0 ||
            strcmp(name, "isUserOnWiFi") == 0 ||
            strcmp(name, "interfaceIsOpen") == 0 ||
            strcmp(name, "canDisplayInterface") == 0 ||
            strcmp(name, "tick") == 0 ||
            strcmp(name, "eglGetError") == 0 ||
            strcmp(name, "GetDeviceOrientation") == 0);
}

static inline bool is_frequent_jni_method(uint32_t mid) {
    const char *name = jvm_get_method_name(mid);
    return is_frequent_jni_method_name(name);
}

void handle_jvm_call(int idx, uint32_t *r0, uint32_t *r1, uint32_t *r2, uint32_t *r3, uint32_t sp, uint32_t lr) {
    (void)r2; (void)r3; (void)sp; (void)lr;
    if (idx != 6) {
        DLOG("JVM Call: idx=%d (r0=0x%x, r1=0x%x, lr=0x%x)", idx, *r0, *r1, lr);
    }
    if (idx == 4) { // AttachCurrentThread(JavaVM*, JNIEnv**, void*)
        uint32_t *p_env = (uint32_t*)guest_to_host(*r1);
        if (p_env) *p_env = GUEST_ENV_OBJ;
        *r0 = 0; // JNI_OK
    } else if (idx == 6) { // GetEnv(JavaVM*, void**, jint)
        uint32_t *p_env = (uint32_t*)guest_to_host(*r1);
        if (p_env) *p_env = GUEST_ENV_OBJ;
        *r0 = 0; // JNI_OK
    } else {
        *r0 = 0;
    }
}

const char* jvm_get_last_string(void) {
    return s_last_jni_string;
}

const char* jvm_get_previous_string(void) {
    return s_prev_jni_string;
}

const char* jvm_get_last_byte_array(void) {
    return s_last_byte_array;
}

void handle_jni_call(int idx, uint32_t *r0, uint32_t *r1, uint32_t *r2, uint32_t *r3, uint32_t sp, uint32_t lr) {
    (void)lr;
    switch (idx) {
        case 4: // GetVersion
            *r0 = 0x00010006; // JNI 1.6
            break;

        case 6: // FindClass(env, name)
        case 10: // GetSuperclass
        case 31: // GetObjectClass
        {
            const char *cname = (const char*)guest_to_host(*r1);
            if (idx == 6 && cname) {
                if (strcmp(cname, "com/glu/platform/android/GluTextBox") == 0) {
                    LOGI("JNI FindClass: '%s' -> GUEST_CLASS_GLU_TEXTBOX (0x%x)", cname, GUEST_CLASS_GLU_TEXTBOX);
                    *r0 = GUEST_CLASS_GLU_TEXTBOX;
                    break;
                }
                if (strcmp(cname, "com/glu/platform/android/GluPlatformActivity") != 0 &&
                    strcmp(cname, "android/net/ConnectivityManager") != 0 &&
                    strcmp(cname, "android/net/NetworkInfo") != 0 &&
                    strcmp(cname, "com/glu/tools/tapjoy/TapjoyInterface") != 0) {
                    DLOG("JNI FindClass: '%s'", cname);
                }
            }
            *r0 = 0x00001111;
            break;
        }

        case 11: // IsAssignableFrom
        case 32: // IsInstanceOf
            *r0 = 1;
            break;

        case 21: // NewGlobalRef
        case 25: // NewLocalRef
            if (jni_textbox_is_handle(*r1)) {
                *r0 = *r1;
                break;
            }
            *r0 = *r1;
            break;

        case 22: // DeleteGlobalRef
        case 23: // DeleteLocalRef
            if (idx == 22 && jni_textbox_is_handle(*r1)) {
                jni_textbox_destroy(*r1);
            }
            *r0 = 0;
            break;

        case 24: // IsSameObject
            *r0 = (*r1 == *r2) ? 1 : 0;
            break;

        case 27: // AllocObject
        case 28: // NewObject
        case 29: // NewObjectV
        case 30: // NewObjectA
            if (*r1 == GUEST_CLASS_GLU_TEXTBOX) {
                uint32_t tb = jni_textbox_create();
                LOGI("JNI NewObject(GluTextBox) -> handle 0x%x", tb);
                *r0 = tb;
                break;
            }
            *r0 = GUEST_DUMMY_OBJ;
            break;

        case 33: // GetMethodID(env, clazz, name, sig)
        case 113: // GetStaticMethodID(env, clazz, name, sig)
        {
            const char *name = (const char*)guest_to_host(*r2);
            const char *sig = (const char*)guest_to_host(*r3);
            int mid = jvm_register_method(name, sig);
            if (!is_frequent_jni_method_name(name)) {
                DLOG("JNI Get%sMethodID: '%s' ('%s') -> 0x%x", (idx == 113 ? "Static" : ""), name ? name : "NULL", sig ? sig : "NULL", mid);
            }
            *r0 = (uint32_t)mid;
            break;
        }

        case 34: // CallObjectMethod
        case 35: // CallObjectMethodV
        case 36: // CallObjectMethodA
        case 64: // CallNonvirtualObjectMethod
        case 65:
        case 66:
        case 114: // CallStaticObjectMethod
        case 115: // CallStaticObjectMethodV
        case 116: // CallStaticObjectMethodA
        {
            uint32_t mid_val = (idx >= 64 && idx <= 93) ? *r3 : *r2;
            const char *mname = jvm_get_method_name(mid_val);
            if (!is_frequent_jni_method(mid_val)) {
                DLOG("JNI CallObjectMethod: mid=0x%x ('%s')", mid_val, mname);
            }
            jni_call_context_t ctx = { idx, r0, r1, r2, r3, sp };
            if (!jni_methods_dispatch(mname, &ctx)) {
                *r0 = GUEST_DUMMY_OBJ;
            }
            break;
        }

        case 37: // CallBooleanMethod
        case 38: // CallBooleanMethodV
        case 39: // CallBooleanMethodA
        case 67: // CallNonvirtualBooleanMethod
        case 68:
        case 69:
        case 117: // CallStaticBooleanMethod
        case 118: // CallStaticBooleanMethodV
        case 119: // CallStaticBooleanMethodA
        {
            uint32_t mid_val = (idx >= 64 && idx <= 93) ? *r3 : *r2;
            const char *mname = jvm_get_method_name(mid_val);
            if (!is_frequent_jni_method(mid_val)) {
                DLOG("JNI CallBooleanMethod: mid=0x%x ('%s')", mid_val, mname);
            }
            jni_call_context_t ctx = { idx, r0, r1, r2, r3, sp };
            if (!jni_methods_dispatch(mname, &ctx)) {
                *r0 = 1;
            }
            break;
        }

        case 49: // CallIntMethod
        case 50: // CallIntMethodV
        case 51: // CallIntMethodA
        case 79: // CallNonvirtualIntMethod
        case 80:
        case 81:
        case 129: // CallStaticIntMethod
        case 130: // CallStaticIntMethodV
        case 131: // CallStaticIntMethodA
        {
            uint32_t mid_val = (idx >= 64 && idx <= 93) ? *r3 : *r2;
            const char *mname = jvm_get_method_name(mid_val);
            if (!is_frequent_jni_method(mid_val)) {
                DLOG("JNI CallIntMethod: mid=0x%x ('%s')", mid_val, mname);
            }
            jni_call_context_t ctx = { idx, r0, r1, r2, r3, sp };
            if (!jni_methods_dispatch(mname, &ctx)) {
                *r0 = 0;
            }
            break;
        }

        case 52: // CallLongMethod
        case 53: // CallLongMethodV
        case 54: // CallLongMethodA
        case 82:
        case 83:
        case 84:
        case 132:
        case 133:
        case 134:
        {
            uint32_t mid_val = (idx >= 64 && idx <= 93) ? *r3 : *r2;
            const char *mname = jvm_get_method_name(mid_val);
            DLOG("JNI CallLongMethod: mid=0x%x ('%s')", mid_val, mname);
            jni_call_context_t ctx = { idx, r0, r1, r2, r3, sp };
            if (!jni_methods_dispatch(mname, &ctx)) {
                *r0 = 0;
                *r1 = 0;
            }
            break;
        }

        case 61: // CallVoidMethod
        case 62: // CallVoidMethodV
        case 63: // CallVoidMethodA
        case 91:
        case 92:
        case 93:
        case 141: // CallStaticVoidMethod
        case 142: // CallStaticVoidMethodV
        case 143: // CallStaticVoidMethodA
        {
            uint32_t mid_val = (idx >= 64 && idx <= 93) ? *r3 : *r2;
            const char *mname = jvm_get_method_name(mid_val);
            if (!is_frequent_jni_method(mid_val)) {
                DLOG("JNI CallVoidMethod: mid=0x%x ('%s')", mid_val, mname);
            }
            jni_call_context_t ctx = { idx, r0, r1, r2, r3, sp };
            if (!jni_methods_dispatch(mname, &ctx)) {
                // unhandled void method
            }
            *r0 = 0;
            break;
        }

        case 94: // GetFieldID
        case 144: // GetStaticFieldID
        {
            const char *fname = (const char*)guest_to_host(*r2);
            DLOG("JNI Get%sFieldID: '%s'", (idx == 144 ? "Static" : ""), fname ? fname : "NULL");
            *r0 = 0x00004444;
            break;
        }

        case 95: // GetObjectField
            *r0 = GUEST_DUMMY_OBJ;
            break;

        case 145: // GetStaticObjectField
            *r0 = GUEST_SENTINEL_OBJ; // EGL_NO_xxx
            break;

        case 96: // GetBooleanField
        case 146: // GetStaticBooleanField
            *r0 = 1;
            break;

        case 100: // GetIntField
        case 150: // GetStaticIntField
            *r0 = 0;
            break;

        case 104: // SetObjectField
        case 105: // SetBooleanField
        case 109: // SetIntField
        case 154: // SetStaticObjectField
        case 155: // SetStaticBooleanField
        case 159: // SetStaticIntField
            *r0 = 0;
            break;

        case 163: // NewString(env, unicodeChars, len)
        {
            const uint16_t *uchars = (const uint16_t*)guest_to_host(*r1);
            uint32_t ulen = *r2;
            memcpy(s_prev_jni_string, s_last_jni_string, sizeof(s_prev_jni_string));
            if (uchars && ulen > 0) {
                if (ulen >= sizeof(s_last_jni_string)) ulen = sizeof(s_last_jni_string) - 1;
                for (uint32_t i = 0; i < ulen; i++) {
                    s_last_jni_string[i] = (char)(uchars[i] & 0x7f);
                }
                s_last_jni_string[ulen] = '\0';
                DLOG("JNI NewString: %u chars", ulen);
            } else {
                s_last_jni_string[0] = '\0';
            }
            *r0 = GUEST_DUMMY_OBJ;
            break;
        }

        case 167: // NewStringUTF(env, bytes)
        {
            const char *str = (const char*)guest_to_host(*r1);
            memcpy(s_prev_jni_string, s_last_jni_string, sizeof(s_prev_jni_string));
            if (str) {
                strncpy(s_last_jni_string, str, sizeof(s_last_jni_string) - 1);
                s_last_jni_string[sizeof(s_last_jni_string) - 1] = '\0';
                DLOG("JNI NewStringUTF: %zu chars", strlen(s_last_jni_string));
            } else {
                s_last_jni_string[0] = '\0';
            }
            *r0 = GUEST_DUMMY_OBJ;
            break;
        }

        case 164: // GetStringLength
        case 168: // GetStringUTFLength
        {
            uint32_t str_obj = *r1;
            if (jni_textbox_is_string_obj(str_obj)) {
                const char *s = (const char*)guest_to_host(str_obj);
                *r0 = s ? (uint32_t)strlen(s) : 0;
            } else {
                *r0 = 0;
            }
            break;
        }

        case 165: // GetStringChars
        case 169: // GetStringUTFChars
        case 224: // GetStringCritical
        {
            uint32_t str_obj = *r1;
            if (jni_textbox_is_string_obj(str_obj)) {
                *r0 = jni_textbox_get_string_ptr(str_obj);
            } else {
                *r0 = GUEST_EMPTY_STRING;
            }
            break;
        }

        case 166: // ReleaseStringChars
        case 170: // ReleaseStringUTFChars
        case 225: // ReleaseStringCritical
            *r0 = 0;
            break;

        case 171: // GetArrayLength
            if (*r1 == GUEST_RESDL_STRING) {
                const char *res_str = (const char*)guest_to_host(GUEST_RESDL_BUF);
                *r0 = res_str ? (uint32_t)strlen(res_str) + 1 : 1;
            } else {
                *r0 = 2;
            }
            break;

        case 172: case 173: case 174: case 175:
        case 177: case 178: case 179: case 180: case 181: case 182:
            *r0 = GUEST_DUMMY_OBJ;
            break;

        case 176: // NewByteArray
            *r0 = GUEST_RESDL_BYTE_ARRAY;
            break;

        case 183: case 184: case 185: case 186: case 187:
        case 188: case 189: case 190:
            if (*r1 == GUEST_RESDL_STRING) {
                *r0 = GUEST_RESDL_BUF;
            } else {
                *r0 = GUEST_DIRECT_BUF;
            }
            break;

        case 191: case 192: case 193: case 194: case 195:
        case 196: case 197: case 198:
            *r0 = 0;
            break;

        case 208: // SetByteArrayRegion(env, array, start, len, buf)
        {
            uint32_t buf_ptr = 0;
            guest_mem_read(sp, &buf_ptr, 4);
            const void *src = guest_to_host(buf_ptr);
            uint32_t len = *r3;
            uint32_t start = *r2;
            uint32_t array_addr = *r1;
            void *dst = guest_to_host(array_addr + start);
            if (src && dst && len > 0) {
                memcpy(dst, src, len);
                if (array_addr == GUEST_RESDL_BYTE_ARRAY && (start + len < 0x800)) {
                    ((char*)dst)[len] = '\0';
                }
            }
            if (src && len > 0) {
                uint32_t copy_len = len < (sizeof(s_last_byte_array) - 1) ? len : (sizeof(s_last_byte_array) - 1);
                memcpy(s_last_byte_array, src, copy_len);
                s_last_byte_array[copy_len] = '\0';
            } else {
                s_last_byte_array[0] = '\0';
            }
            DLOG("JNI SetByteArrayRegion: arr=0x%x, start=%u, len=%u, text='%s'",
                 array_addr, start, len, s_last_byte_array);
            *r0 = 0;
            break;
        }

        case 211: // SetIntArrayRegion
            *r0 = 0;
            break;

        case 203: // GetIntArrayRegion(env, array, start, len, buf)
        {
            uint32_t buf_ptr = 0;
            guest_mem_read(sp, &buf_ptr, 4);
            uint32_t *dst = (uint32_t*)guest_to_host(buf_ptr);
            if (dst) {
                dst[0] = 1; // EGL 1.x
                if (*r3 > 1) dst[1] = 4; // EGL 1.4
            }
            *r0 = 0;
            break;
        }

        case 219: // GetJavaVM(env, JavaVM**)
        {
            uint32_t *dst = (uint32_t*)guest_to_host(*r1);
            if (dst) *dst = GUEST_JVM_OBJ;
            *r0 = 0;
            break;
        }

        case 228: // ExceptionCheck
        case 15:  // ExceptionOccurred
            *r0 = 0;
            break;

        case 16:  // ExceptionDescribe
        case 17:  // ExceptionClear
            *r0 = 0;
            break;

        case 229: // NewDirectByteBuffer
            *r0 = GUEST_DUMMY_OBJ;
            break;

        case 230: // GetDirectBufferAddress
            *r0 = GUEST_DIRECT_BUF;
            break;

        case 231: // GetDirectBufferCapacity
            *r0 = 65536;
            break;

        default:
            DLOG("JNI unhandled index: %d", idx);
            *r0 = 0;
            break;
    }
}
