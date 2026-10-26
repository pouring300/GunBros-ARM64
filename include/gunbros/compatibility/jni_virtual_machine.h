#ifndef GUNBROS_JNI_VIRTUAL_MACHINE_H
#define GUNBROS_JNI_VIRTUAL_MACHINE_H

#include "gunbros/common/common.h"

GUNBROS_EXTERN_C_BEGIN

#define GUEST_JVM_OBJ       0x60000100u
#define GUEST_JVM_VTABLE    0x60000120u
#define GUEST_JVM_STUBS     0x8000b000u

#define GUEST_ENV_OBJ       0x60001000u
#define GUEST_ENV_VTABLE    0x60001100u
#define GUEST_ENV_STUBS     0x8000c000u

#define GUEST_DUMMY_OBJ     0x60002000u
#define GUEST_SENTINEL_OBJ  0x60002080u
#define GUEST_RESDL_STRING  0x60002100u
#define GUEST_EMPTY_STRING  0x60003000u
#define GUEST_RESDL_BUF     0x60004000u
#define GUEST_RESDL_BYTE_ARRAY 0x60004800u
#define GUEST_WSTRING_BUF   0x60005000u
#define GUEST_DIRECT_BUF    0x60006000u
#define GUEST_TLS_BASE      0x60007000u

void jvm_virtual_init(uint8_t *heap_base, uint8_t *stubs_base, const char *game_dir);
int  jvm_register_method(const char *name, const char *sig);
const char* jvm_get_method_name(uint32_t id);

void handle_jvm_call(int idx, uint32_t *r0, uint32_t *r1, uint32_t *r2, uint32_t *r3, uint32_t sp, uint32_t lr);
void handle_jni_call(int idx, uint32_t *r0, uint32_t *r1, uint32_t *r2, uint32_t *r3, uint32_t sp, uint32_t lr);

int safe_guest_snprintf(char *dst, size_t max_len, const char *fmt, uint32_t *reg_args, int reg_count, uint32_t sp);
const char* jvm_get_last_string(void);
/* The string created (NewString/NewStringUTF) before the last one, for calls taking two. */
const char* jvm_get_previous_string(void);
const char* jvm_get_last_byte_array(void);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_JNI_VIRTUAL_MACHINE_H
