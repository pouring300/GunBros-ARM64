#include "gunbros/compatibility/game_patches.h"
#include "gunbros/compatibility/guest_helpers_bin.h"
#include "gunbros/compatibility/guest_symbols.h"
#include <string.h>

#define GUEST_HELPERS_OFF 0xd000u /* offset in g_host_stubs -> guest 0x8000d000 */

static const game_patch_entry_t s_game_patches[] = {
    /* CResourceLoader::LoadNext */
    { GUEST_PATCH_RESLOADER_LOADNEXT,  true, 0x0affffc8, 0xe1a00000, 4, "CResourceLoader::LoadNext nop" },

    /* Patch A: CGun::Draw (hooks into Trampoline A) */
    { GUEST_PATCH_GUN_DRAW,            true, 0xe5d030e8, 0xea08eb2a, 4, "CGun::Draw trampoline branch" },
    { GUEST_PATCH_GUN_DRAW_NOP1,       true, 0xe3530000, 0xe1a00000, 4, "CGun::Draw nop1" },
    { GUEST_PATCH_GUN_DRAW_NOP2,       true, 0x0a000098, 0xe1a00000, 4, "CGun::Draw nop2" },

    /* Patch B: CMesh::GetNodeAt (hooks into Trampoline B) */
    { GUEST_PATCH_MESH_GETNODEAT,      true, 0xe92d4ff0, 0xea06e173, 4, "CMesh::GetNodeAt trampoline branch" },

    /* Patch C: CMesh::GetInterpolationValues */
    { GUEST_PATCH_MESH_GETINTERP,      true, 0x0a000052, 0x9a000052, 4, "CMesh::GetInterpolationValues bls" },

    /* Patch D: Multitouch support */
    { GUEST_PATCH_MULTITOUCH_FLAG,     true, 0x00,       0x00000001, 1, "g_multipleTouchSupported = 1" },
    { GUEST_PATCH_CAN_MULTITOUCH_MOV,  true, 0xe59f3008, 0xe3a00001, 4, "CanMultipleTouch mov r0, 1" },
    { GUEST_PATCH_CAN_MULTITOUCH_BX,   true, 0xe79f3003, 0xe12fff1e, 4, "CanMultipleTouch bx lr" },
    { GUEST_PATCH_IS_MULTITOUCH_MOV,   true, 0xe5d00044, 0xe3a00001, 4, "IsMultipleTouchEnabled mov r0, 1" },
    { GUEST_PATCH_IS_MULTITOUCH_BX,    true, 0xe12fff1e, 0xe12fff1e, 4, "IsMultipleTouchEnabled bx lr" },

    /* Patch E: CGameApp::OnSystemEvent (preserve pointer ID) */
    { GUEST_PATCH_SYSTEM_EVENT_PID,    true, 0xe3a07000, 0xe1a00000, 4, "CGameApp::OnSystemEvent preserve pid nop" },

    /* Patch F: Hardware::GetSupportedArtSetEv */
    { GUEST_PATCH_ART_SET_MOV,         true, 0xe59f3024, 0xe3a00002, 4, "Hardware::GetSupportedArtSetEv mov r0, 2" },
    { GUEST_PATCH_ART_SET_BX,          true, 0xe79f3003, 0xe12fff1e, 4, "Hardware::GetSupportedArtSetEv bx lr" },

    /* Patch G: CResourceLoader::Init (hooks into Trampoline G) */
    { GUEST_PATCH_RESLOADER_INIT,      true, 0xe92d41f0, 0xea0625cb, 4, "CResourceLoader::Init trampoline branch" },


    /* Patch I: CFileMgr_Android local file operations */
    { GUEST_PATCH_FILEMGR_LOCAL_DATA,  true, 0x0a000025, 0xe1a00000, 4, "CFileMgr_Android skip data: strip nop" },
    { GUEST_PATCH_FILEMGR_LOCAL_CACHE, true, 0x0a000017, 0xe1a00000, 4, "CFileMgr_Android skip cache: strip nop" },
    { GUEST_PATCH_FILEMGR_LOCAL_TEMP,  true, 0x0a000021, 0xe1a00000, 4, "CFileMgr_Android skip temp: strip nop" },
    { GUEST_PATCH_FILEMGR_MKDIR,       true, 0x1a000002, 0xea000002, 4, "CFileMgr_Android MkDir branch" },
    { GUEST_PATCH_FILEMGR_SIZE,        true, 0x0a00000c, 0xe1a00000, 4, "CFileMgr_Android Size nop" },
    { GUEST_PATCH_FILEMGR_EXISTS,      true, 0x1a00000c, 0xea00000c, 4, "CFileMgr_Android Exists branch" },
    { GUEST_PATCH_FILEMGR_DELETE,      true, 0x1a000002, 0xea000002, 4, "CFileMgr_Android Delete branch" },
    { GUEST_PATCH_FILEMGR_RENAME_SRC,  true, 0x0afffff3, 0xe1a00000, 4, "CFileMgr_Android Rename src nop" },
    { GUEST_PATCH_FILEMGR_RENAME_DST,  true, 0x0affffed, 0xe1a00000, 4, "CFileMgr_Android Rename dst nop" },

    /* Patch J: GameSpy network polling busy loops - disabled for OpenSpy multiplayer */
    // { GUEST_PATCH_GAMESPY_GT2_THINK,   true, 0xe92d4010, 0xe12fff1e, 4, "gt2Think bx lr" },
    // { GUEST_PATCH_GAMESPY_PEER_THINK,  true, 0xe92d4070, 0xe12fff1e, 4, "peerThink bx lr" },
    // { GUEST_PATCH_GAMESPY_NN_THINK,    true, 0xe92d4ff0, 0xe12fff1e, 4, "NNThink bx lr" },
    // { GUEST_PATCH_GAMESPY_GTI2_MOV,    true, 0xe92d41f0, 0xe3a00000, 4, "gti2ConnectionThink mov r0, 0" },
    // { GUEST_PATCH_GAMESPY_GTI2_BX,     true, 0xe590300c, 0xe12fff1e, 4, "gti2ConnectionThink bx lr" },

    /* Patch K: CProfileManager::LoginInitResourceLoaderCallback bypass server isReady to load local profile */
    { GUEST_PATCH_PROFILE_LOGIN_INIT_LDR,  true, 0xeb0322dd, 0xe59f41d0, 4, "ProfileManager LoginInit ldr r4, [pc, 0x1d0]" },
    { GUEST_PATCH_PROFILE_LOGIN_INIT_ADD,  true, 0xe59f41cc, 0xe08f4004, 4, "ProfileManager LoginInit add r4, pc, r4" },
    { GUEST_PATCH_PROFILE_LOGIN_INIT_LDR7, true, 0xe3500000, 0xe59f71cc, 4, "ProfileManager LoginInit ldr r7, [pc, 0x1cc]" },
    { GUEST_PATCH_PROFILE_LOGIN_INIT_B,    true, 0xe08f4004, 0xea00001a, 4, "ProfileManager LoginInit b 0x35c2e0" },

    /* Patch L: SyncLocalDataResourceLoaderCallback return 1 (success) immediately */
    { GUEST_PATCH_PROFILE_SYNC_LOCAL_MOV,  true, 0xe92d4070, 0xe3a00001, 4, "SyncLocalDataResourceLoaderCallback mov r0, 1" },
    { GUEST_PATCH_PROFILE_SYNC_LOCAL_BX,   true, 0xe1a04000, 0xe12fff1e, 4, "SyncLocalDataResourceLoaderCallback bx lr" },

    /* Patch M: CMeshAnimationController::Render (hooks into Trampoline M) */
    { GUEST_PATCH_MESH_ANIM_RENDER,        true, 0xe3500000, 0xea06dc8d, 4, "CMeshAnimationController::Render trampoline branch" },

    /* Patch N: refinery meters enabled without a Glu NGS profile (bl IsProfileValid -> mov r0, 1) */
    { GUEST_PATCH_REFINERY_METERS_ENABLED, true, 0xeb0196e3, 0xe3a00001, 4, "Refinery meters enabled mov r0, 1" },

    /* Patch O: run CGunBros::UpdateOnlineStatus without a Glu NGS profile (bl IsProfileValid -> mov r0, 1) */
    { GUEST_PATCH_ONLINE_STATUS_UPDATE,    true, 0xeb02844c, 0xe3a00001, 4, "UpdateOnlineStatus online mov r0, 1" },
    { GUEST_PATCH_ONLINE_STATUS_POLL,      true, 0xeb02726e, 0xe3a00001, 4, "CGunBros::Update online mov r0, 1" },

    /* Patch P: validate the local user after a Glu account login (bl isValid -> mov r0, 0) */
    { GUEST_PATCH_POST_LOGIN_VALIDATE,     true, 0xebffebff, 0xe3a00000, 4, "handlePostUserLogin validate mov r0, 0" },
};

const game_patch_entry_t* game_patches_get_table(size_t *count) {
    if (count) *count = sizeof(s_game_patches) / sizeof(s_game_patches[0]);
    return s_game_patches;
}

void game_patches_apply_all(uint8_t *code_base) {
    if (!code_base) return;

    // Trampoline A: CGun::Draw Muzzle Flash Guard
    uint32_t tramp_a[7] = {
        0xe5d030e8, // ldrb r3, [r0, #232]  ; isShooting
        0xe3530000, // cmp  r3, #0
        0x0af7156c, // beq  0x002b55c0      ; if !isShooting -> return
        0xe5903008, // ldr  r3, [r0, #8]    ; m_flashMesh
        0xe3530000, // cmp  r3, #0
        0x0af71569, // beq  0x002b55c0      ; if !m_flashMesh -> return
        0xeaf714cf  // b    0x002b535c      ; continue flash drawing
    };
    memcpy(code_base + GUEST_PATCH_TRAMPOLINE_A, tramp_a, sizeof(tramp_a));

    // Trampoline B: CMesh::GetNodeAt NULL & uninitialized m_frames Guard
    uint32_t tramp_b[8] = {
        0xe3500206, // cmp  r0, #0x60000000  ; valid heap CMesh*?
        0x312fff1e, // bxlo lr
        0xe590c028, // ldr  r12, [r0, #0x28] ; r12 = m_frames
        0xe35c0206, // cmp  r12, #0x60000000 ; valid heap m_frames?
        0x312fff1e, // bxlo lr
        0xe92d4ff0, // push {r4, r5, r6, r7, r8, r9, sl, fp, lr}
        0xe24dd02c, // sub  sp, sp, #44
        0xeaf91e84  // b    0x00337a74
    };
    memcpy(code_base + GUEST_PATCH_TRAMPOLINE_B, tramp_b, sizeof(tramp_b));

    // Trampoline G: CResourceLoader::Init NULL this guard
    uint32_t tramp_g[5] = {
        0xe3500000, // cmp  r0, #0
        0x012fff1e, // bxeq lr
        0xe92d41f0, // push {r4, r5, r6, r7, r8, lr}
        0xe1a05000, // mov  r5, r0
        0xeaf9da2f  // b    0x00366934
    };
    memcpy(code_base + GUEST_PATCH_TRAMPOLINE_G, tramp_g, sizeof(tramp_g));

    // Trampoline M: CMeshAnimationController::Render mesh/frames/vertexBuffer validation & sync
    uint32_t tramp_m[22] = {
        0xe3500206, // cmp   r0, #0x60000000 ; valid heap m_mesh?
        0x38bd8010, // poplo {r4, pc}
        0xe5903028, // ldr   r3, [r0, #0x28] ; r3 = m_mesh->m_frames
        0xe3530206, // cmp   r3, #0x60000000 ; valid heap m_frames?
        0x38bd8010, // poplo {r4, pc}
        0xe590302c, // ldr   r3, [r0, #0x2c] ; r3 = m_mesh->m_framesCount
        0xe3530000, // cmp   r3, #0          ; framesCount > 0?
        0x08bd8010, // popeq {r4, pc}
        0xe5903004, // ldr   r3, [r0, #0x4]  ; r3 = m_mesh->m_indexBuffer
        0xe3530206, // cmp   r3, #0x60000000 ; valid heap m_indexBuffer?
        0x38bd8010, // poplo {r4, pc}
        0xe5902000, // ldr   r2, [r0, #0x0]  ; r2 = m_mesh->m_vertexBuffer
        0xe3520206, // cmp   r2, #0x60000000 ; valid heap m_vertexBuffer?
        0x38bd8010, // poplo {r4, pc}
        0xe5923000, // ldr   r3, [r2, #0x0]  ; r3 = m_vertexBuffer vtable
        0xe3530601, // cmp   r3, #0x00100000 ; vtable >= 0x00100000?
        0x38bd8010, // poplo {r4, pc}
        0xe3530605, // cmp   r3, #0x00500000 ; vtable < 0x00500000?
        0x28bd8010, // pophs {r4, pc}
        0xe5842008, // str   r2, [r4, #0x8]  ; sync controller->m_vertexBuffer
        0xe594100c, // ldr   r1, [r4, #0xc]  ; r1 = m_time
        0xeaf9235e  // b     0x00338e54      ; call CMesh::GetVerticesAt
    };
    memcpy(code_base + GUEST_PATCH_TRAMPOLINE_M, tramp_m, sizeof(tramp_m));

    // Apply patch table with verification and BRIDGE_DEBUG capture
    size_t num_patches = sizeof(s_game_patches) / sizeof(s_game_patches[0]);
    for (size_t i = 0; i < num_patches; i++) {
        const game_patch_entry_t *p = &s_game_patches[i];
        if (p->size == 1) {
            uint8_t actual_old = *(uint8_t*)(code_base + p->addr);
#ifdef BRIDGE_DEBUG
            LOGI("[PATCH:CAPTURE] %s at 0x%08x: actual_old=0x%02x (new=0x%02x)",
                 p->name, p->addr, actual_old, (uint8_t)p->new_value);
#endif
            if (p->check_expected && actual_old != (uint8_t)p->expected_old) {
                LOGE("[PATCH:MISMATCH] '%s' at 0x%08x: expected 0x%02x, found 0x%02x. Patch aborted!",
                     p->name, p->addr, (uint8_t)p->expected_old, actual_old);
                continue;
            }
            *(uint8_t*)(code_base + p->addr) = (uint8_t)p->new_value;
            LOGI("[PATCH] %s at 0x%08x -> 0x%02x", p->name, p->addr, (uint8_t)p->new_value);
        } else {
            uint32_t actual_old = *(uint32_t*)(code_base + p->addr);
#ifdef BRIDGE_DEBUG
            LOGI("[PATCH:CAPTURE] %s at 0x%08x: actual_old=0x%08x (new=0x%08x)",
                 p->name, p->addr, actual_old, p->new_value);
#endif
            if (p->check_expected && actual_old != p->expected_old) {
                LOGE("[PATCH:MISMATCH] '%s' at 0x%08x: expected 0x%08x, found 0x%08x. Patch aborted!",
                     p->name, p->addr, p->expected_old, actual_old);
                continue;
            }
            *(uint32_t*)(code_base + p->addr) = p->new_value;
            LOGI("[PATCH] %s at 0x%08x -> 0x%08x", p->name, p->addr, p->new_value);
        }
    }
}

void game_patches_install_helpers(uint8_t *stubs_base, int qsort_plt_idx) {
    if (!stubs_base) return;
    if (guest_helpers_bin_len > 0x1000) {
        LOGE("guest_helpers too large (%u bytes)", guest_helpers_bin_len);
        return;
    }
    memcpy(stubs_base + GUEST_HELPERS_OFF, guest_helpers_bin, guest_helpers_bin_len);
    if (qsort_plt_idx >= 0) {
        // Replace "svc; bx lr" with "ldr pc, [pc, #-4]; .word destination"
        uint32_t slot[2] = { 0xe51ff004u, 0x80000000u + GUEST_HELPERS_OFF };
        memcpy(stubs_base + qsort_plt_idx * 8, slot, 8);
        LOGI("[PATCH] Installed guest_qsort helper at 0x8000d000 (PLT slot %d)", qsort_plt_idx);
    }
}
