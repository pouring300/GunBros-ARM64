#include "hle_internal.h"
#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/compatibility/jni_virtual_machine.h"
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_HLE_HANDLERS 256
#define MAX_HLE_STUBS 512

_Static_assert(MAX_HLE_STUBS <= 0x1000, "MAX_HLE_STUBS must not collide with JVM SWI range 0x1000");
_Static_assert(MAX_HLE_HANDLERS <= MAX_HLE_STUBS, "Handler registry capacity must fit within stubs");

typedef struct {
    const char *name;
    hle_fn fn;
} hle_registered_entry_t;

typedef struct {
    char name[64];
    hle_fn fn;
} hle_stub_entry_t;

static hle_registered_entry_t s_registered_handlers[MAX_HLE_HANDLERS];
static size_t s_registered_handlers_count = 0;

static hle_stub_entry_t s_stubs[MAX_HLE_STUBS];
static int s_stub_count = 0;

static volatile int s_guest_dead = 0;
static volatile uint64_t s_call_start_ns = 0;

void hle_register_module(const hle_def_t *defs, size_t count) {
    if (!defs || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        const char *name = defs[i].name;
        hle_fn fn = defs[i].fn;
        if (!name || !*name) continue;

        // Check for duplicate registration and update
        bool found = false;
        for (size_t j = 0; j < s_registered_handlers_count; j++) {
            if (strcmp(s_registered_handlers[j].name, name) == 0) {
                s_registered_handlers[j].fn = fn;
                found = true;
                break;
            }
        }
        if (found) continue;

        if (s_registered_handlers_count >= MAX_HLE_HANDLERS) {
            LOGE("hle_register_module: registry full (max %d), cannot register '%s'",
                 MAX_HLE_HANDLERS, name);
            return;
        }

        s_registered_handlers[s_registered_handlers_count].name = name;
        s_registered_handlers[s_registered_handlers_count].fn = fn;
        s_registered_handlers_count++;
    }
}

int hle_resolve_import(const char *name) {
    if (!name || !*name) return -1;

    // Check if this symbol already has an assigned stub slot
    for (int i = 0; i < s_stub_count; i++) {
        if (strcmp(s_stubs[i].name, name) == 0) {
            return i;
        }
    }

    if (s_stub_count >= MAX_HLE_STUBS) {
        LOGE("hle_resolve_import: stub table full (max %d), cannot resolve '%s'",
             MAX_HLE_STUBS, name);
        return -1;
    }

    // Look up handler in registered modules
    hle_fn fn = NULL;
    for (size_t i = 0; i < s_registered_handlers_count; i++) {
        if (strcmp(s_registered_handlers[i].name, name) == 0) {
            fn = s_registered_handlers[i].fn;
            break;
        }
    }

    int idx = s_stub_count++;
    strncpy(s_stubs[idx].name, name, sizeof(s_stubs[idx].name) - 1);
    s_stubs[idx].name[sizeof(s_stubs[idx].name) - 1] = '\0';
    s_stubs[idx].fn = fn;

    if (!fn) {
        LOGW("ELF loader: guest import '%s' has no HLE handler registered", name);
    }

    return idx;
}

int hle_find_plt_index(const char *name) {
    return hle_resolve_import(name);
}

const char* hle_get_plt_name(int idx) {
    if (idx >= 0 && idx < s_stub_count) {
        return s_stubs[idx].name;
    }
    return "unknown";
}

hle_fn hle_get_handler(int idx) {
    if (idx >= 0 && idx < s_stub_count) {
        return s_stubs[idx].fn;
    }
    return NULL;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

void hle_init(void) {
    s_registered_handlers_count = 0;
    s_stub_count = 0;
    memset(s_registered_handlers, 0, sizeof(s_registered_handlers));
    memset(s_stubs, 0, sizeof(s_stubs));

    hle_register_module(g_hle_libc_string_defs, g_hle_libc_string_defs_count);
    hle_register_module(g_hle_libc_time_defs, g_hle_libc_time_defs_count);
    hle_register_module(g_hle_libc_stdio_defs, g_hle_libc_stdio_defs_count);
    hle_register_module(g_hle_net_stubs_defs, g_hle_net_stubs_defs_count);
    hle_register_module(g_hle_gles1_defs, g_hle_gles1_defs_count);

    LOGI("HLE initialized with %zu registered function handlers", s_registered_handlers_count);
}

void on_dynarmic_svc(uint32_t swi) {
    if (swi >= 0x1000 && swi < 0x1010) {
        int vm_idx = swi - 0x1000;
        uint32_t r0 = dynarmic_backend_get_reg(0);
        uint32_t r1 = dynarmic_backend_get_reg(1);
        uint32_t r2 = dynarmic_backend_get_reg(2);
        uint32_t r3 = dynarmic_backend_get_reg(3);
        uint32_t sp = dynarmic_backend_get_reg(13);
        uint32_t lr = dynarmic_backend_get_reg(14);
        handle_jvm_call(vm_idx, &r0, &r1, &r2, &r3, sp, lr);
        dynarmic_backend_set_reg(0, r0);
        dynarmic_backend_set_reg(1, r1);
        return;
    }

    if (swi >= 0x2000 && swi < 0x2100) {
        int jni_idx = swi - 0x2000;
        uint32_t r0 = dynarmic_backend_get_reg(0);
        uint32_t r1 = dynarmic_backend_get_reg(1);
        uint32_t r2 = dynarmic_backend_get_reg(2);
        uint32_t r3 = dynarmic_backend_get_reg(3);
        uint32_t sp = dynarmic_backend_get_reg(13);
        uint32_t lr = dynarmic_backend_get_reg(14);
        handle_jni_call(jni_idx, &r0, &r1, &r2, &r3, sp, lr);
        dynarmic_backend_set_reg(0, r0);
        dynarmic_backend_set_reg(1, r1);
        return;
    }

    if (swi >= GAME_HOOK_SVC_BASE && swi < GAME_HOOK_SVC_BASE + GAME_HOOK_SVC_COUNT) {
        game_hooks_on_svc(swi - GAME_HOOK_SVC_BASE);
        return;
    }

    if (swi < MAX_HLE_STUBS) {
        int idx = (int)swi;
        hle_fn fn = (idx < s_stub_count) ? s_stubs[idx].fn : NULL;
        if (fn) {
            hle_t c;
            memset(&c, 0, sizeof(c));
            c.r0 = dynarmic_backend_get_reg(0);
            c.r1 = dynarmic_backend_get_reg(1);
            c.r2 = dynarmic_backend_get_reg(2);
            c.r3 = dynarmic_backend_get_reg(3);
            c.sp = dynarmic_backend_get_reg(13);
            c.lr = dynarmic_backend_get_reg(14);
            c.pc = dynarmic_backend_get_reg(15);
            fn(&c);
            dynarmic_backend_set_reg(0, c.r0);
            if (c.set_r1) {
                dynarmic_backend_set_reg(1, c.out_r1);
            }
            return;
        }

        static uint32_t seen[MAX_HLE_STUBS] = {0};
        if (seen[idx]++ < 3) {
            uint32_t r0 = dynarmic_backend_get_reg(0);
            uint32_t r1 = dynarmic_backend_get_reg(1);
            uint32_t r2 = dynarmic_backend_get_reg(2);
            uint32_t lr = dynarmic_backend_get_reg(14);
            const char *sym_name = (idx < s_stub_count) ? s_stubs[idx].name : "unknown";
            LOGW("UNHANDLED PLT %s (idx=%d) r0=0x%x r1=0x%x r2=0x%x lr=0x%x",
                 sym_name, idx, r0, r1, r2, lr);
        }
        return;
    }

    LOGE("Unhandled on_dynarmic_svc: swi=0x%x", swi);
}

void prepare_call_arm(uint32_t arg0) {
    uint32_t cpsr = 0x10; // USR mode, ARM state
    uint32_t sp = GUEST_STACK_TOP;
    uint32_t stop_lr = STOP_ADDR;
    dynarmic_backend_set_cpsr(cpsr);
    dynarmic_backend_set_reg(13, sp);
    dynarmic_backend_set_reg(14, stop_lr);
    dynarmic_backend_set_reg(0, arg0);
}

static volatile const char *s_current_call_tag = NULL;
static volatile uint32_t s_current_entry = 0;

run_result_t run_guest(uint32_t entry, const char *tag) {
    if (s_guest_dead || !dynarmic_backend_is_active()) return RUN_DEAD;
    s_current_call_tag = tag;
    s_current_entry = entry;
    s_call_start_ns = now_ns();
    dynarmic_backend_run(entry);
    s_call_start_ns = 0;
    s_current_call_tag = NULL;
    if (s_guest_dead) return RUN_DEAD;
    return RUN_OK;
}

static void *watchdog_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        uint64_t st = s_call_start_ns;
        if (st) {
            uint64_t elapsed_ns = now_ns() - st;
            if (elapsed_ns > 2ull * 1000000000ull) {
                uint32_t pc = dynarmic_backend_get_reg(15);
                uint32_t lr = dynarmic_backend_get_reg(14);
                uint32_t r0 = dynarmic_backend_get_reg(0);
                uint32_t r1 = dynarmic_backend_get_reg(1);
                uint32_t r2 = dynarmic_backend_get_reg(2);
                uint32_t r3 = dynarmic_backend_get_reg(3);
                LOGI("WATCHDOG: call '%s' (entry=0x%08x) running for %.1fs pc=0x%08x lr=0x%08x r0=0x%x r1=0x%x r2=0x%x r3=0x%x",
                     s_current_call_tag ? s_current_call_tag : "none", s_current_entry,
                     elapsed_ns / 1000000000.0, pc, lr, r0, r1, r2, r3);
                if (elapsed_ns > 10ull * 1000000000ull && (elapsed_ns / 1000000000ull) % 5 == 0) {
                    LOGW("WATCHDOG: guest call '%s' running for %.1fs (long operation)",
                         s_current_call_tag ? s_current_call_tag : "none",
                         elapsed_ns / 1000000000.0);
                }
            }
        }
    }
    return NULL;
}

void start_watchdog_once(void) {
    static int started = 0;
    if (started) return;
    started = 1;
    pthread_t t;
    pthread_create(&t, NULL, watchdog_thread, NULL);
}
