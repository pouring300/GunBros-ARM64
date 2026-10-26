#ifndef GUNBROS_GUEST_SYMBOLS_H
#define GUNBROS_GUEST_SYMBOLS_H

#include <stdint.h>

/* ========================================================================= */
/* Guest Binary Verification                                                 */
/* ========================================================================= */
/**
 * Expected SHA-256 hash (in lowercase hex) of the original ARM32
 * libandroidplatformjni.so binary. If empty (""), a warning is logged.
 * If non-empty and the calculated hash does not match, patches are refused.
 */
#define EXPECTED_GUEST_SHA256 "e7b7f30b1dafb8381472b5d91184bb9ebd435c5a75952a4290d490254aec4c40"

/* ========================================================================= */
/* Application Lifecycle & JNI Entry Points (in guest ARM32 code)            */
/* ========================================================================= */
#define GUEST_FN_JNI_ON_LOAD              0x00161cacu
#define GUEST_FN_ON_CREATE                0x00162638u
#define GUEST_FN_ON_START                 0x00156610u
#define GUEST_FN_ON_RESUME                0x00156614u
#define GUEST_FN_ON_PAUSE                 0x001569d4u
#define GUEST_FN_ON_STOP                  0x00156620u
#define GUEST_FN_ON_RESTART               0x00156624u
#define GUEST_FN_ON_DESTROY               0x001569bcu
#define GUEST_FN_PROCESS_EVENTS           0x00156a40u
#define GUEST_FN_ON_DRAW_FRAME            0x00156b10u
#define GUEST_FN_SURFACE_CREATED          0x00156718u
#define GUEST_FN_SURFACE_CHANGED          0x001568d8u
#define GUEST_FN_SURFACE_DESTROYED        0x0015671cu
#define GUEST_FN_GL_SURFACE_CREATED       0x0015676cu
#define GUEST_FN_GL_SURFACE_CHANGED       0x00156750u
#define GUEST_FN_ON_LOW_MEMORY            0x001569a4u
#define GUEST_FN_ON_ORIENTATION_CHANGED   0x00156864u

/* ========================================================================= */
/* Touch Input System Entry Points                                           */
/* ========================================================================= */
#define GUEST_FN_TOUCH_DOWN               0x00162440u
#define GUEST_FN_TOUCH_MOVE               0x00162418u
#define GUEST_FN_TOUCH_UP                 0x001623f0u
#define GUEST_FN_TOUCH_CANCEL             0x001623c8u

/* ========================================================================= */
/* Audio Subsystem                                                           */
/* ========================================================================= */
#define GUEST_FN_FEED_PCM                 0x00156ee0u
#define GUEST_ADDR_AUDIO_PCM_BUF          0x60080000u

/* ========================================================================= */
/* Character Selection & Menu System                                         */
/* ========================================================================= */
#define GUEST_FN_SELECT_CHAR              0x0031f1bcu

/* ========================================================================= */
/* Global Variables & Data Pointers in Guest ELF                             */
/* ========================================================================= */
#define GUEST_ADDR_APP_PTR                0x004c909cu
#define GUEST_ADDR_SCREEN_WIDTH           0x004c9688u
#define GUEST_ADDR_SCREEN_HEIGHT          0x004c968cu
#define GUEST_ADDR_FALLBACK_CAPPLET       0x60010160u

#define GUEST_ADDR_JAVA_VM                0x004c95b8u
#define GUEST_ADDR_GOT_CALLBACK_JNI       0x004c4a94u
#define GUEST_ADDR_CALLBACK_JNI           0x004c95bcu
#define GUEST_ADDR_RES_FILE_PATH          0x004c91a4u
#define GUEST_ADDR_EXTERNAL_PATH          0x004c9658u
#define GUEST_ADDR_EXTERNAL_PATH_LEN      0x004c964cu
#define GUEST_ADDR_INTERNAL_PATH          0x004c9654u
#define GUEST_ADDR_INTERNAL_PATH_LEN      0x004c9648u
#define GUEST_ADDR_SEPARATOR              0x004c965cu
#define GUEST_ADDR_SEPARATOR_LEN          0x004c9650u

#define GUEST_ADDR_GOT_CTYPE              0x004c50c0u
#define GUEST_ADDR_GOT_TOLOWER            0x004c4ce8u
#define GUEST_ADDR_GOT_TOUPPER            0x004c4a2cu

#define GUEST_ADDR_INIT_ARRAY             0x0049406cu
#define GUEST_INIT_ARRAY_COUNT            51

/* ========================================================================= */
/* Struct Offsets                                                            */
/* ========================================================================= */
#define GUEST_OFF_APP_RES_LOADER          96u   /* App -> sm_pResourceLoader */
#define GUEST_OFF_APP_GUNBROS             108u  /* App -> sm_pGunBros */
#define GUEST_OFF_RESLOADER_TOTAL         12u   /* ResourceLoader -> total */
#define GUEST_OFF_RESLOADER_CURRENT       14u   /* ResourceLoader -> current */
#define GUEST_OFF_GB_MENUSYS              4u    /* GunBros -> m_pMenuSystem */
#define GUEST_OFF_GB_GSTATE               320u  /* GunBros -> m_gameState */
#define GUEST_OFF_GB_PROFILE_MGR          0x48u /* GunBros -> CProfileManager* */
#define GUEST_OFF_PROFILE_SESSION         0x64u /* CProfileManager -> CNGSSession* */
#define GUEST_OFF_SESSION_TIME_START_MS   0x28u /* CNGSSession -> network time base (u64 ms) */
#define GUEST_OFF_SESSION_TIME_NOW_MS     0x30u /* CNGSSession -> network time now (u64 ms, 0 = invalid) */
#define GUEST_OFF_MENU_TOP                256u  /* MenuSystem -> m_pTopMenu */

/* ========================================================================= */
/* Binary Patch Addresses & Trampolines                                      */
/* ========================================================================= */
#define GUEST_PATCH_RESLOADER_LOADNEXT    0x00366878u

#define GUEST_PATCH_TRAMPOLINE_A          0x004f0000u
#define GUEST_PATCH_GUN_DRAW              0x002b5350u
#define GUEST_PATCH_GUN_DRAW_NOP1         0x002b5354u
#define GUEST_PATCH_GUN_DRAW_NOP2         0x002b5358u

#define GUEST_PATCH_TRAMPOLINE_B          0x004f0040u
#define GUEST_PATCH_MESH_GETNODEAT        0x00337a6cu
#define GUEST_PATCH_MESH_GETINTERP        0x00337904u

#define GUEST_PATCH_MULTITOUCH_FLAG       0x004c966cu
#define GUEST_PATCH_CAN_MULTITOUCH_MOV    0x0015751cu
#define GUEST_PATCH_CAN_MULTITOUCH_BX     0x00157520u
#define GUEST_PATCH_IS_MULTITOUCH_MOV     0x00157530u
#define GUEST_PATCH_IS_MULTITOUCH_BX      0x00157534u
#define GUEST_PATCH_SYSTEM_EVENT_PID      0x0024d5d4u

#define GUEST_PATCH_ART_SET_MOV           0x002e5170u
#define GUEST_PATCH_ART_SET_BX            0x002e5174u

#define GUEST_PATCH_TRAMPOLINE_G          0x004f0060u
#define GUEST_PATCH_RESLOADER_INIT        0x0036692cu

#define GUEST_PATCH_NAV_BAR_LABEL_R10     0x0031d418u
#define GUEST_PATCH_NAV_BAR_SUB_R7        0x0031db1cu
#define GUEST_PATCH_NAV_BAR_WARBUCKS      0x0031d4d8u
#define GUEST_PATCH_NAV_BAR_COINS         0x0031d4fcu
#define GUEST_PATCH_NAV_BAR_GUNBADGE      0x0031d518u

#define GUEST_PATCH_FILEMGR_LOCAL_DATA    0x00159ca0u
#define GUEST_PATCH_FILEMGR_LOCAL_CACHE   0x00159cc0u
#define GUEST_PATCH_FILEMGR_LOCAL_TEMP    0x00159ce0u
#define GUEST_PATCH_FILEMGR_MKDIR         0x00159dccu
#define GUEST_PATCH_FILEMGR_SIZE          0x00159e60u
#define GUEST_PATCH_FILEMGR_EXISTS        0x00159f48u
#define GUEST_PATCH_FILEMGR_DELETE        0x0015a27cu
#define GUEST_PATCH_FILEMGR_RENAME_SRC    0x0015a30cu
#define GUEST_PATCH_FILEMGR_RENAME_DST    0x0015a324u

#define GUEST_PATCH_GAMESPY_GT2_THINK     0x003c5c88u
#define GUEST_PATCH_GAMESPY_PEER_THINK    0x003a65d0u
#define GUEST_PATCH_GAMESPY_NN_THINK      0x003cbc68u
#define GUEST_PATCH_GAMESPY_GTI2_MOV      0x003c44f0u
#define GUEST_PATCH_GAMESPY_GTI2_BX       0x003c44f4u

/* Patch K: CProfileManager::LoginInitResourceLoaderCallback bypass server isReady to load local profile */
#define GUEST_PATCH_PROFILE_LOGIN_INIT_LDR   0x0035c264u
#define GUEST_PATCH_PROFILE_LOGIN_INIT_ADD   0x0035c268u
#define GUEST_PATCH_PROFILE_LOGIN_INIT_LDR7  0x0035c26cu
#define GUEST_PATCH_PROFILE_LOGIN_INIT_B     0x0035c270u

/* Patch L: SyncLocalDataResourceLoaderCallback return 1 immediately to stop server sync retry loop */
#define GUEST_PATCH_PROFILE_SYNC_LOCAL_MOV   0x0035c964u
#define GUEST_PATCH_PROFILE_SYNC_LOCAL_BX    0x0035c968u

/* Patch M: CMeshAnimationController::Render validate mesh, frames, and vertexBuffer */
#define GUEST_PATCH_TRAMPOLINE_M             0x004f0080u
#define GUEST_PATCH_MESH_ANIM_RENDER         0x00338e44u

/* Patch N: refinery meters are only enabled while the Glu NGS profile is "valid" (synced with
   Glu's defunct server). Make menu element 0x51 (GetElementValueBoolean) report enabled. */
#define GUEST_PATCH_REFINERY_METERS_ENABLED  0x002f302cu

/* Patch O: CGunBros::UpdateOnlineStatus() (daily bonus, inactivity bonus, refinery timers,
   friend-XP timer, challenges, offers) only runs while the Glu NGS profile is valid. Treat the
   player as online at these two checks; CProfileManager save/load/sync keep their own checks. */
#define GUEST_PATCH_ONLINE_STATUS_UPDATE     0x002b7a88u  /* in CGunBros::UpdateOnlineStatus */
#define GUEST_PATCH_ONLINE_STATUS_POLL       0x002bc200u  /* in CGunBros::Update */

/* Refinery: Premium/Standard slot chain (slot N unlocks after collecting from gate[N]) */
#define GUEST_OFF_GB_REFINERY                0x18u   /* GunBros -> CRefinementManager* */
#define GUEST_OFF_REFINERY_GATE              200u    /* u8[12]: slot that must be collected first */
#define GUEST_OFF_REFINERY_SLOTS             0xd8u   /* CRefinementSlot[12], 0x20 bytes each, +0 = state */
#define GUEST_REFINERY_SLOT_COUNT            12
#define GUEST_REFINERY_STATE_LOCKED          0u
#define GUEST_REFINERY_STATE_READY           1u
#define GUEST_REFINERY_STATE_COMPLETE        3u
#define GUEST_FN_REFINERY_SAVE_STATE_CHANGE  0x0035fd58u  /* CRefinementManager::SaveStateChange() */

#endif // GUNBROS_GUEST_SYMBOLS_H
