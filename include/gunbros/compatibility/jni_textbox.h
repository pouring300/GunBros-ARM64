#ifndef GUNBROS_JNI_TEXTBOX_H
#define GUNBROS_JNI_TEXTBOX_H

#include "gunbros/common/common.h"
#include "gunbros/compatibility/jni_methods.h"

GUNBROS_EXTERN_C_BEGIN

#define GUEST_CLASS_GLU_TEXTBOX 0x00007701u
#define GUEST_TEXTBOX_BASE      0x6000e000u
#define MAX_TEXTBOXES           8

void jni_textbox_init(void);
uint32_t jni_textbox_create(void);
bool jni_textbox_is_handle(uint32_t handle);
void jni_textbox_destroy(uint32_t handle);

bool jni_textbox_is_string_obj(uint32_t str_obj);
uint32_t jni_textbox_get_string_ptr(uint32_t str_obj);

/* Read a text box's current text (UTF-8) from the bridge, e.g. while handling a game hook. */
bool jni_textbox_read_text(uint32_t handle, char *out, size_t out_size);

/* Clear a text box and show `hint` (grey placeholder text) in it. */
bool jni_textbox_set_hint(uint32_t handle, const char *hint);

/* JNI Dispatch method handlers */
bool jni_method_textbox_set_bounds(const jni_call_context_t *ctx);
bool jni_method_textbox_set_visible(const jni_call_context_t *ctx);
bool jni_method_textbox_set_password_mask(const jni_call_context_t *ctx);
bool jni_method_textbox_set_text(const jni_call_context_t *ctx);
bool jni_method_textbox_get_text(const jni_call_context_t *ctx);
bool jni_method_textbox_remove_text_view(const jni_call_context_t *ctx);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_JNI_TEXTBOX_H
