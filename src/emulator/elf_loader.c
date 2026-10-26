#include "gunbros/emulator/elf_loader.h"
#include "gunbros/emulator/guest_memory.h"
#include "gunbros/emulator/dynarmic_backend.h"
#include "gunbros/emulator/hle_dispatcher.h"
#include "gunbros/compatibility/game_patches.h"
#include "gunbros/compatibility/game_hooks.h"
#include "gunbros/compatibility/guest_symbols.h"
#include "gunbros/compatibility/jni_virtual_machine.h"
#include "gunbros/common/sha256.h"
#include <string.h>

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

#include <strings.h>

#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))

static void apply_relocations(const uint8_t *buf, uint8_t *code_base) {
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)buf;
    Elf32_Phdr *phdr = (Elf32_Phdr*)(buf + ehdr->e_phoff);
    Elf32_Phdr *dyn_phdr = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 2) { // PT_DYNAMIC
            dyn_phdr = &phdr[i];
            break;
        }
    }
    if (!dyn_phdr) return;

    uint32_t *dyn = (uint32_t*)(buf + dyn_phdr->p_offset);
    uint32_t rel_off = 0, rel_sz = 0, jmprel_off = 0, jmprel_sz = 0;
    uint32_t symtab_off = 0, strtab_off = 0;

    for (int i = 0; dyn[i] != 0; i += 2) {
        uint32_t tag = dyn[i];
        uint32_t val = dyn[i+1];
        if (tag == 23) jmprel_off = val;        // DT_JMPREL
        else if (tag == 2) jmprel_sz = val;     // DT_PLTRELSZ
        else if (tag == 17) rel_off = val;      // DT_REL
        else if (tag == 18) rel_sz = val;       // DT_RELSZ
        else if (tag == 6) symtab_off = val;    // DT_SYMTAB
        else if (tag == 5) strtab_off = val;    // DT_STRTAB
    }

    Elf32_Sym *symtab = (Elf32_Sym*)(buf + symtab_off);
    const char *strtab = (const char*)(buf + strtab_off);

    // Relocate JMPREL (PLT imports)
    if (jmprel_off && jmprel_sz) {
        Elf32_Rel *rel = (Elf32_Rel*)(buf + jmprel_off);
        uint32_t count = jmprel_sz / sizeof(Elf32_Rel);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t sym_idx = ELF32_R_SYM(rel[i].r_info);
            const char *sym_name = strtab + symtab[sym_idx].st_name;
            int stub_idx = hle_resolve_import(sym_name);
            if (stub_idx >= 0) {
                uint32_t stub_addr = GUEST_STUBS_BASE + stub_idx * 8;
                *(uint32_t*)(code_base + rel[i].r_offset) = stub_addr;
            }
        }
    }

    // Relocate REL (data/vtable/relative relocations)
    if (rel_off && rel_sz) {
        Elf32_Rel *rel = (Elf32_Rel*)(buf + rel_off);
        uint32_t count = rel_sz / sizeof(Elf32_Rel);
        int relocated_count = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t type = ELF32_R_TYPE(rel[i].r_info);
            if (type == 23) { // R_ARM_RELATIVE
                relocated_count++;
            } else if (type == 2 || type == 21) { // R_ARM_ABS32 or R_ARM_GLOB_DAT
                uint32_t sym_idx = ELF32_R_SYM(rel[i].r_info);
                uint32_t sym_val = symtab[sym_idx].st_value;
                uint32_t addend = *(uint32_t*)(code_base + rel[i].r_offset);
                if (sym_val != 0) {
                    uint32_t target = sym_val + addend;
                    *(uint32_t*)(code_base + rel[i].r_offset) = target;
                } else {
                    const char *sym_name = strtab + symtab[sym_idx].st_name;
                    int stub_idx = hle_resolve_import(sym_name);
                    if (stub_idx >= 0) {
                        uint32_t stub_addr = GUEST_STUBS_BASE + stub_idx * 8;
                        *(uint32_t*)(code_base + rel[i].r_offset) = stub_addr;
                    }
                }
                relocated_count++;
            }
        }
        LOGI("Relocated %d symbols in REL section", relocated_count);
    }
}

bool elf_loader_load(const uint8_t *buf, size_t buf_len, const char *game_dir) {
    if (!buf || buf_len < sizeof(Elf32_Ehdr)) {
        LOGE("ELF buffer invalid or too small");
        return false;
    }

    char sha256_hex[65] = {0};
    gunbros_sha256_hex(buf, buf_len, sha256_hex);
    LOGI("Loaded guest ARM32 .so SHA-256: %s (%zu bytes)", sha256_hex, buf_len);

    bool patches_allowed = true;
    if (strlen(EXPECTED_GUEST_SHA256) == 0) {
        LOGW("EXPECTED_GUEST_SHA256 is empty; skipping strict hash check (calculated %s)", sha256_hex);
    } else if (strcasecmp(sha256_hex, EXPECTED_GUEST_SHA256) != 0) {
        LOGE("GUEST BINARY SHA-256 MISMATCH! Found %s, expected %s. Refusing to apply patches!",
             sha256_hex, EXPECTED_GUEST_SHA256);
        patches_allowed = false;
    } else {
        LOGI("Guest binary SHA-256 verified successfully: %s", sha256_hex);
    }

    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)buf;
    if (memcmp(ehdr->e_ident, "\177ELF", 4) != 0 ||
        ehdr->e_ident[4] != 1 || ehdr->e_ident[5] != 1 ||
        ehdr->e_machine != 40 || ehdr->e_ehsize < sizeof(Elf32_Ehdr) ||
        ehdr->e_phentsize != sizeof(Elf32_Phdr)) {
        LOGE("Invalid or unsupported ELF header (expected little-endian 32-bit ARM)");
        return false;
    }

    if (ehdr->e_phoff > buf_len ||
        (size_t)ehdr->e_phnum > (buf_len - ehdr->e_phoff) / sizeof(Elf32_Phdr)) {
        LOGE("ELF program-header table exceeds input buffer");
        return false;
    }

    Elf32_Phdr *validated_phdr = (Elf32_Phdr*)(buf + ehdr->e_phoff);
    bool has_load_segment = false;
    for (uint32_t i = 0; i < ehdr->e_phnum; ++i) {
        const Elf32_Phdr *ph = &validated_phdr[i];
        if (ph->p_type != 1) continue; // PT_LOAD
        has_load_segment = true;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset > buf_len ||
            (size_t)ph->p_filesz > buf_len - ph->p_offset ||
            ph->p_vaddr > GUEST_CODE_SIZE ||
            ph->p_memsz > GUEST_CODE_SIZE - ph->p_vaddr) {
            LOGE("Invalid PT_LOAD segment %u: off=0x%x filesz=0x%x vaddr=0x%x memsz=0x%x",
                 i, ph->p_offset, ph->p_filesz, ph->p_vaddr, ph->p_memsz);
            return false;
        }
    }

    if (!has_load_segment) {
        LOGE("ELF contains no PT_LOAD segments");
        return false;
    }

    if (!guest_memory_init()) {
        LOGE("Failed to allocate guest memory regions");
        return false;
    }

    uint8_t *code_base = guest_memory_get_code();
    uint8_t *heap_base = guest_memory_get_heap();
    uint8_t *stack_base = guest_memory_get_stack();
    uint8_t *stubs_base = guest_memory_get_stubs();
    uint8_t *commpage_base = guest_memory_get_commpage();

    dynarmic_backend_init(guest_to_host, on_dynarmic_svc);
    dynarmic_backend_map(GUEST_CODE_BASE, GUEST_CODE_SIZE, code_base);
    dynarmic_backend_map(GUEST_HEAP_BASE, GUEST_HEAP_SIZE, heap_base);
    dynarmic_backend_map(GUEST_STACK_BASE, GUEST_STACK_SIZE, stack_base);
    dynarmic_backend_map(GUEST_STUBS_BASE, GUEST_STUBS_SIZE, stubs_base);
    dynarmic_backend_map(GUEST_COMMPAGE_BASE, GUEST_COMMPAGE_SIZE, commpage_base);

    guest_memory_setup_commpage();

    memset(code_base, 0, GUEST_CODE_SIZE);
    Elf32_Phdr *phdr = (Elf32_Phdr*)(buf + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            memcpy(code_base + phdr[i].p_vaddr, buf + phdr[i].p_offset, phdr[i].p_filesz);
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset(code_base + phdr[i].p_vaddr + phdr[i].p_filesz, 0, phdr[i].p_memsz - phdr[i].p_filesz);
            }
            LOGI("Mapped PT_LOAD segment vaddr=0x%x, filesz=0x%x, memsz=0x%x",
                 phdr[i].p_vaddr, phdr[i].p_filesz, phdr[i].p_memsz);
        }
    }

    // Setup HLE import stubs at 0x80000000 (svc i; bx lr)
    for (int i = 0; i < MAX_HLE_STUBS; i++) {
        uint32_t *s = (uint32_t*)(stubs_base + i * 8);
        s[0] = 0xef000000 | i;
        s[1] = 0xe12fff1e;
    }

    hle_init();

    // Setup stop stub at 0x8000e000
    uint32_t *stop_stub = (uint32_t*)(stubs_base + 0xe000);
    stop_stub[0] = 0xef00ffff;
    stop_stub[1] = 0xe12fff1e;

    // Initialize virtual JVM and JNIEnv
    jvm_virtual_init(heap_base, stubs_base, game_dir);

    // Set g_javaVM
    *(uint32_t*)(code_base + GUEST_ADDR_JAVA_VM) = GUEST_JVM_OBJ;

    // Set GOT entry for g_callbackJNI
    *(uint32_t*)(code_base + GUEST_ADDR_GOT_CALLBACK_JNI) = GUEST_ADDR_CALLBACK_JNI;

    // Dynamic relocations
    apply_relocations(buf, code_base);

    // Zero out page 0 (ELF header at 0x00000000..0x00000fff) so any NULL+offset read returns 0
    memset(code_base, 0, 0x1000);

    // Initialize application file paths for CFileMgr_Android
    const char *actual_game_dir = game_dir ? game_dir : "/sdcard/Android/data/com.glu.android.gunbros_free/files/";
    strncpy((char*)(code_base + GUEST_ADDR_RES_FILE_PATH), actual_game_dir, 1023); // sm_resFilePath

    uint16_t *w_game_dir = (uint16_t*)(heap_base + 0x5000);
    int gdir_len = (int)strlen(actual_game_dir);
    for (int i = 0; i <= gdir_len; i++) {
        w_game_dir[i] = (uint16_t)actual_game_dir[i];
    }
    uint32_t w_game_dir_guest = GUEST_WSTRING_BUF;

    // Preserve original scheme prefixes in .data/.rodata:
    // kApplicationPath ("apk:"), kApplicationDataPath ("data:"),
    // kApplicationTempPath ("temp:"), kApplicationCachePath ("cache:").
    // The engine's CFileMgr_Android::Open expects "data:" (len 5) and strips it
    // before concatenating g_externalPath or g_internalPath.

    // Initialize runtime storage paths for CFileMgr_Android
    *(uint32_t*)(code_base + GUEST_ADDR_EXTERNAL_PATH) = w_game_dir_guest; // g_externalPath
    *(uint32_t*)(code_base + GUEST_ADDR_EXTERNAL_PATH_LEN) = (uint32_t)gdir_len; // g_externalPathLength
    *(uint32_t*)(code_base + GUEST_ADDR_INTERNAL_PATH) = w_game_dir_guest; // g_internalPath
    *(uint32_t*)(code_base + GUEST_ADDR_INTERNAL_PATH_LEN) = (uint32_t)gdir_len; // g_internalPathLength

    // Initialize g_separator ("/") in heap
    uint16_t *w_sep = (uint16_t*)(heap_base + 0x5f00);
    w_sep[0] = (uint16_t)'/';
    w_sep[1] = 0;
    *(uint32_t*)(code_base + GUEST_ADDR_SEPARATOR) = 0x60005f00; // g_separator
    *(uint32_t*)(code_base + GUEST_ADDR_SEPARATOR_LEN) = 1; // g_separatorLength

    LOGI("File paths initialized: g_externalPath='%s' (len=%d), g_separator='/'",
         actual_game_dir, gdir_len);

    // GOT entries point to pointer variables in commpage:
    *(uint32_t*)(code_base + GUEST_ADDR_GOT_CTYPE) = 0xffff1800; // _ctype_
    *(uint32_t*)(code_base + GUEST_ADDR_GOT_TOLOWER) = 0xffff1804; // _tolower_tab_
    *(uint32_t*)(code_base + GUEST_ADDR_GOT_TOUPPER) = 0xffff1808; // _toupper_tab_

    // Apply engine patches if verified
    if (patches_allowed) {
        game_patches_apply_all(code_base);
        game_hooks_install(code_base, stubs_base);
    } else {
        LOGE("Skipping game_patches_apply_all due to guest binary SHA-256 mismatch!");
    }

    // Install guest helpers (qsort)
    int qsort_idx = hle_resolve_import("qsort");
    game_patches_install_helpers(stubs_base, qsort_idx);

    // Execute .init_array constructors
    LOGI("Executing .init_array constructors...");
    uint32_t init_array_addr = GUEST_ADDR_INIT_ARRAY;
    int init_count = GUEST_INIT_ARRAY_COUNT;
    for (int i = 0; i < init_count; i++) {
        uint32_t fn = *(uint32_t*)(code_base + init_array_addr + i * 4);
        if (fn != 0 && fn != 0xffffffff) {
            LOGI("Executing .init_array[%d] at 0x%08x...", i, fn);
            prepare_call_arm(0);
            run_result_t r = run_guest(fn, "init_array");
            if (r != RUN_OK) {
                LOGE(".init_array[%d] (0x%08x) failed: %d", i, fn, r);
            }
        }
    }
    LOGI("ELF loaded, relocated, and .init_array constructors executed successfully!");

    // Execute JNI_OnLoad at 0x00161cac
    LOGI("Calling guest JNI_OnLoad at 0x161cac...");
    prepare_call_arm(GUEST_JVM_OBJ);
    dynarmic_backend_set_reg(1, 0); // r1 = reserved
    run_result_t err = run_guest(GUEST_FN_JNI_ON_LOAD, "JNI_OnLoad");
    uint32_t jni_ver = dynarmic_backend_get_reg(0);
    LOGI("Guest JNI_OnLoad returned res=%d, jni_ver=0x%x", err, jni_ver);

    return true;
}
