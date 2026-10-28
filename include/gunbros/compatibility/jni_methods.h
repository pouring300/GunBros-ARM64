#ifndef GUNBROS_JNI_METHODS_H
#define GUNBROS_JNI_METHODS_H

#include "gunbros/common/common.h"

GUNBROS_EXTERN_C_BEGIN

typedef struct {
    int idx;
    uint32_t *r0;
    uint32_t *r1;
    uint32_t *r2;
    uint32_t *r3;
    uint32_t sp;
} jni_call_context_t;

typedef bool (*jni_method_handler_t)(const jni_call_context_t *ctx);

typedef struct {
    const char *name;
    jni_method_handler_t handler;
} jni_method_entry_t;

/**
 * Extracts method argument from r3 or guest stack depending on JNI call index and signature.
 */
uint32_t get_jni_method_arg(int idx, uint32_t r3, uint32_t sp, int arg_idx);

/**
 * Dispatches method call by name using the registered method table.
 * Returns true if handled by table, false if default return value should be used.
 */
bool jni_methods_dispatch(const char *name, const jni_call_context_t *ctx);

/* Open a URL through the activity's LaunchURL (type 0: browser). */
bool jni_launch_url(const char *url, int type);

/* Put plain text on the Android clipboard. */
bool jni_copy_to_clipboard(const char *label, const char *text);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_JNI_METHODS_H
