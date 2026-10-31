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
/* CProfileManager derives from CNGSLocalUser (CNGSServerObject: +4 = isValid). +0x48 is a second
   server object that IsProfileValid also requires; +0xe0 is the "profile online" flag. */
#define GUEST_OFF_SERVER_OBJECT_VALID     0x04u
#define GUEST_OFF_PROFILE_SERVER_OBJ      0x48u
#define GUEST_OFF_PROFILE_ONLINE          0xe0u
/* One sync-status byte per data store 1000..1019 (saved as <clientid>_PDST). */
#define GUEST_OFF_PROFILE_STORE_STATUS    0xccu
#define GUEST_PROFILE_STORE_COUNT         20
#define GUEST_PROFILE_STORE_CHANGED       1u
#define GUEST_PROFILE_STORE_LOAD_FROM_SERVER 4u
#define GUEST_FN_PROFILE_SAVE             0x0035c528u /* CProfileManager::Save(store, loader, no_upload): saves, and uploads when signed in */
#define GUEST_PROFILE_STORE_PROGRESS      1000u /* CPlayerProgress: level, XP (public part) */
#define GUEST_PROFILE_STORE_CONFIG        1001u /* CPlayerConfiguration: character and loadout */
#define GUEST_OFF_SESSION_TIME_START_MS   0x28u /* CNGSSession -> network time base (u64 ms) */
#define GUEST_OFF_SESSION_TIME_NOW_MS     0x30u /* CNGSSession -> network time now (u64 ms, 0 = invalid) */
#define GUEST_OFF_MENU_TOP                256u  /* MenuSystem -> m_pTopMenu */
#define GUEST_OFF_MENU_SYS_BRANCH         0x59cu /* CMenuSystem: current menu branch */
#define GUEST_OFF_MENU_SYS_PREV_BRANCH    0x598u /* CMenuSystem: branch under a popup */
#define GUEST_MENU_BRANCH_POPUP           0xbu
#define GUEST_OFF_MENU_SYS_STACKS         0x98u  /* CMenuSystem: one CMenuStack per branch */
#define GUEST_MENU_STACK_SIZE             0x80u
#define GUEST_OFF_MENU_STACK_MENU         0x68u  /* CMenuStack -> current CMenu */

/* ========================================================================= */
/* Binary Patch Addresses & Trampolines                                      */
/* ========================================================================= */
#define GUEST_ADDR_RESLOADER_STEP_CHECK   0x00366874u /* CResourceLoader::LoadNext: cmp r0, #0; beq <return> after a function step */
#define GUEST_ADDR_RESLOADER_RETURN       0x003667a0u /* CResourceLoader::LoadNext: return (the step stays queued) */
#define GUEST_ADDR_RESLOADER_NEXT_STEP    0x00366838u /* CResourceLoader::LoadNext: move on to the next step */
#define GUEST_OFF_RESLOADER_STEP_FN       0x08u       /* loader step: function (r0 = arg at +0x10, r1 = +0x0c) */

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

/* Patch P: after a Glu account login, CNGSSocialInterface::handlePostUserLogin only calls
   CNGSLocalUser::ValidateUser (which gets the client id and session, and writes Credentials.dat /
   Session.dat) if the local user isn't "valid" yet. Any successful NGS reply (e.g. server time)
   marks it valid, so it took the AssociateUser branch and the game never got an id. The original
   game validated through the (now dead) Facebook login instead. Always validate here. */
#define GUEST_PATCH_POST_LOGIN_VALIDATE      0x00429ec0u  /* bl CNGSServerObject::isValid */

/* Refinery: Premium/Standard slot chain (slot N unlocks after collecting from gate[N]) */
#define GUEST_OFF_GB_REFINERY                0x18u   /* GunBros -> CRefinementManager* */
#define GUEST_OFF_REFINERY_GATE              200u    /* u8[12]: slot that must be collected first */
#define GUEST_OFF_REFINERY_SLOTS             0xd8u   /* CRefinementSlot[12], 0x20 bytes each, +0 = state */
#define GUEST_REFINERY_SLOT_COUNT            12
#define GUEST_REFINERY_STATE_LOCKED          0u
#define GUEST_REFINERY_STATE_READY           1u
#define GUEST_REFINERY_STATE_COMPLETE        3u
#define GUEST_FN_REFINERY_SAVE_STATE_CHANGE  0x0035fd58u  /* CRefinementManager::SaveStateChange() */

/* Glu account registration: CNGSUIManager::onRegisterDialogSubmitted(this unused, gamertag, password,
   email; UTF-16) converts its three UTF-16 strings and calls CGluSocialManager::CreateAccount. The Android
   port never shows the register dialog (CNGS_Platform::showDialogUserRegister is an empty stub). */
#define GUEST_FN_NGS_REGISTER_SUBMITTED      0x0043289cu
/* CApplet::m_App + 0x20: CHash of singletons, keyed by class id. CNGS is 0x7a23; its local user
   (CNGSLocalUser, loaded from Credentials.dat) is at +0x18. */
#define GUEST_OFF_APP_SINGLETONS             0x20u
#define GUEST_SINGLETON_NGS                  0x7a23u
#define GUEST_OFF_NGS_LOCAL_USER             0x18u
/* CGluSocialManager (singleton 0x363a14b7): +0x3c is the signed-in account's e-mail (CStrWChar:
   vtable, magic, buffer, length), returned by CGluSocialInterface::getPlayerID. */
#define GUEST_SINGLETON_GLU_SOCIAL           0x363a14b7u
#define GUEST_OFF_GLU_SOCIAL_EMAIL           0x3cu
#define GUEST_FN_GLU_SOCIAL_GET_PLAYER_ID    0x003ec580u  /* CGluSocialInterface::getPlayerID() */
#define GUEST_FN_CSTRWCHAR_CONCATENATE_CHAR  0x001768c8u  /* CStrWChar::Concatenate(const char *) */
#define GUEST_CSTRWCHAR_MAGIC                0x43735eb4u
/* CNGSLocalUser::ValidateUser(network): continues the saved session, or logs in by the network's
   user id (resources/user/validateuser/ with userattribute GLUSOCIALID for network 0). */
#define GUEST_FN_NGS_LOCAL_USER_VALIDATE     0x00411a14u

/* Add a Bro: the bridge hooks CMenuAction::DoAction to catch the Bros tab's "Invite more friends"
   banner, shows the OpenSpy mod's search popup instead of Glu's Facebook help, and hooks the mod's
   popup action handlers to read the friend code and close its own popups. */
#define GUEST_FN_MENU_DO_ACTION              0x002e655cu  /* CMenuAction::DoAction(CMenu*, MenuAction, uint, uint) */
#define GUEST_MENU_ACTION_MENU_SPECIFIC      0x67u        /* DoAction: passes the argument to the menu (vtable +0x1c) */
#define GUEST_MENU_ACTION_NONE               0x9fu        /* DoAction: default case, does nothing */
#define GUEST_MENU_ACTION_FRIEND_LIST_UPDATED 0x58u       /* CProfileManager::HandleFriendListUpdate -> current menu */
#define GUEST_MENU_ACTION_FRIENDS_RESET      0x61u        /* sent by CMenuFriends::Reset */
#define GUEST_MENU_ACTION_SELECT_FRIEND      0x65u        /* Brotherhood: put Bro n on duty (a tap on him) */
#define GUEST_MENU_ACTION_SOCIAL             0x55u        /* social network button; argument = network */
#define GUEST_SOCIAL_NETWORK_FACEBOOK        2u
/* Where the Facebook buttons lead now: the restoration community's Discord. */
#define GUNBROS_COMMUNITY_URL                "https://discord.gg/3mWsGmPAQ"
#define GUEST_VTABLE_CMENUFRIENDS            0x004c1938u  /* Brotherhood menu */
#define GUEST_VT_MENU_REFRESH                0x1cu        /* CMenu vtable: Refresh(MenuAction, int) */
#define GUEST_MENU_REFRESH_BIND              0x2bu        /* Refresh action sent once a menu's data has loaded */
#define GUEST_OFF_MENU_FRIENDS_RELOAD        0x1098u      /* CMenuFriends: friend list reload requested (RefreshFriendsList) */
#define GUEST_OFF_MENU_FRIENDS_LIST_BOUND    0x1099u      /* CMenuFriends: friend list bound (Refresh 0x2b skips it if set) */
#define GUEST_OFF_MENU_FRIENDS_ROWS          0x1d4u       /* CMenuFriends: friends in the bound list (its option group's count) */
#define GUEST_OFF_MENU_FRIENDS_OPTIONS       0x1f4u       /* CMenuFriends -> on-duty Bro panel; null until the menu is loaded */
#define GUEST_OFF_GB_FRIEND_DATA_MGR         0x34u        /* GunBros -> CFriendDataManager* */
#define GUEST_OFF_FRIEND_MGR_FETCH_LIST      0xb4u        /* CFriendDataManager -> friends being fetched (CNGSRemoteUserList*) */
#define GUEST_OFF_FRIEND_MGR_FRIENDS         0xb0u        /* CFriendDataManager -> the Brothers list (count +0x1c, CFriendData** +0x28) */
#define GUEST_OFF_FRIEND_MGR_ACTIVE          0xb8u        /* CFriendDataManager -> on-duty Bro (CFriendData*) */
#define GUEST_OFF_FRIEND_MGR_DEFAULT_BRO     0xecu        /* CFriendDataManager -> built-in default Bro */
#define GUEST_OFF_LIST_COUNT                 0x1cu        /* CNGSRemoteUserList: number of entries */
#define GUEST_OFF_LIST_ITEMS                 0x28u        /* CNGSRemoteUserList: entry pointer array */
#define GUEST_OFF_FRIEND_CLIENT_ID           0x18u        /* CFriendData: credentials, client id first */
#define GUEST_OFF_FRIEND_AVATAR_CONFIG       0xe8u        /* CFriendData: CPlayerConfiguration */
#define GUEST_OFF_FRIEND_LAST_USED           0x7cu        /* CFriendData: network time (s) he was last used; a gift is due 24 h later */
#define GUEST_OFF_GB_MENU_PLAYER_CONFIG      0x268u       /* GunBros: CPlayerConfiguration the menus' 3D Bro shows */
#define GUEST_FN_FRIEND_MGR_SET_ACTIVE       0x002aa3dcu  /* CFriendDataManager::SetActiveFriend(CFriendData*) */
#define GUEST_FN_FRIEND_MGR_SET_DEFAULT      0x002aa2ccu  /* CFriendDataManager::SetActiveToDefault() */
#define GUEST_FN_FRIEND_MGR_INIT_DEFAULT_BRO 0x002ab2a4u  /* CFriendDataManager::InitDefaultBrother(const CPlayerConfiguration*): name + loadout */
#define GUEST_FN_GAMEFLOW_RESET_BRO_CONFIG   0x002afa60u  /* CGameFlow::ResetBrotherConfiguration(): base loadout, the other Gun Bro */
#define GUEST_OFF_GB_GAMEFLOW                0x150u       /* GunBros: CGameFlow (embedded) */
#define GUEST_OFF_GB_BRO_CONFIG              0x1e4u       /* GunBros: the Bro's CPlayerConfiguration (set up by CGameFlow) */
#define GUEST_OFF_FRIEND_MGR_FETCH_BATCH     0xf6u        /* CFriendDataManager: u16, friends in the last fetched batch (list rows) */
#define GUEST_VTABLE_CMENUCHALLENGES         0x0049a780u  /* Bro-Ops menu */
#define GUEST_OFF_MENU_CHALLENGES_RELOAD     0x2028u      /* CMenuChallenges: list reload in progress */
#define GUEST_OFF_MENU_CHALLENGES_BOUND      0x202bu      /* CMenuChallenges: recruit list bound (1) */
#define GUEST_OFF_MENU_CHALLENGES_SHOWN      0x202eu      /* CMenuChallenges: on screen */
#define GUEST_OFF_MENU_CHALLENGES_ROWS       0xa18u       /* CMenuChallenges: rows in the recruit list, end caps included */
#define GUEST_OFF_MENU_CHALLENGES_CAPS       0xa2cu       /* CMenuChallenges: two bytes, set when the list has a top/bottom end cap row */
#define GUEST_FN_PLAYER_CONFIG_ASSIGN        0x0034808cu  /* CPlayerConfiguration::operator=(const CPlayerConfiguration&) */
#define GUEST_OFF_SERVER_OBJECT_PENDING      0x08u        /* CNGSServerObject: outstanding requests (isReady when 0) */
#define GUEST_FN_SERVER_OBJECT_COMPLETE_READ 0x00424e54u  /* CNGSServerObject::CompleteReadRequestOutstanding() */
#define GUEST_FN_NGS_UPDATE_FRIEND_LIST      0x00411e28u  /* CNGSLocalUser::UpdateFriendListFromServer() */
#define GUEST_FN_SHOW_SEARCH_POPUP           0x002606f8u  /* CAndroidSearchPopup::ShowAndroidSearchPopup(title, a, b, c, d) */
#define GUEST_FN_SEARCH_POPUP_REFRESH        0x00260b98u  /* CAndroidSearchPopup::RefreshMidMenuPopup(popup, MenuAction) */
#define GUEST_FN_SHOW_MESSAGE_POPUP          0x0025fcc0u  /* CAndroidMessagePopup::ShowAndroidMessagePopup(type, title, text, a..e) */
#define GUEST_FN_MESSAGE_POPUP_REFRESH       0x0025f6e8u  /* CAndroidMessagePopup::RefreshMidMenuPopup(popup, MenuAction) */
#define GUEST_OFF_SEARCH_POPUP_FIELD         0xf8u        /* CAndroidSearchPopup -> CAndroidUiTextField* */
#define GUEST_OFF_SEARCH_POPUP_BUTTON_B      0x150u       /* CAndroidSearchPopup -> second CMenuMovieButton */
#define GUEST_OFF_SEARCH_POPUP_BUTTON_C      0x1a4u       /* CAndroidSearchPopup -> third CMenuMovieButton */
#define GUEST_OFF_SEARCH_POPUP_BUTTON_D      0x1f8u       /* CAndroidSearchPopup -> fourth CMenuMovieButton */
#define GUEST_ADDR_SEARCH_POPUP_BUTTON_C_Y   0x002602f8u  /* ButtonCCallback: ldrsh r2, [rect, #4] (y); ldrsh r1, [rect] */
#define GUEST_ADDR_SEARCH_POPUP_FIELD_BOUNDS 0x00260378u  /* SearchFieldCallback: str ip, [sp]; bl SetBounds(x, y, w, h) */
#define GUEST_ADDR_SEARCH_POPUP_FIELD_RET    0x00260380u  /* SearchFieldCallback, after that call */
#define GUEST_FN_MENU_MOVIE_BUTTON_DRAW_AT   0x00317720u  /* CMenuMovieButton::Draw(short x, short y) */
#define GUEST_FN_UI_TEXT_FIELD_SET_BOUNDS    0x00261994u  /* CAndroidUiTextField::SetBounds(x, y, w, h) */
#define GUEST_SEARCH_POPUP_FIELD_TO_BUTTON_C 43           /* layout: button C is this far below the field (y 273 -> 316) */
#define GUEST_OFF_MENU_BUTTON_LABEL          0x3cu        /* CMenuMovieButton -> label (CStrWChar*, owned) */
#define GUEST_FN_NP_MALLOC                   0x00165480u  /* np_malloc(size) */
#define GUEST_OFF_TEXT_FIELD_OBJECT          0x18u        /* CAndroidUiTextField -> GluTextBox jobject (bridge text box handle) */
#define GUEST_OFF_MENU_MID_POPUP             0x8cu        /* CMenuSystem -> current CMenuMidPopup* */
#define GUEST_OFF_SESSION_ID                 0x20u        /* CNGSSession -> session id (UTF-16, CNGSSessionConfig at +0x18) */
#define GUEST_VTABLE_CSTRWCHAR               0x004950e8u
/* The mod's popup buttons: label index n (IDS_ANDROID_UI_CANCEL, _SUBMIT, _GO, _INVITE, _RETRY,
   _ADD_BUDDY, _REMOVE_BUDDY, _OK, ...) fires MenuAction 0xa8 + n. */
#define GUEST_POPUP_BUTTON_CANCEL            0u
#define GUEST_POPUP_BUTTON_ADD_BUDDY         5u
#define GUEST_POPUP_BUTTON_COPY              2u           /* "Go", relabelled "Copy" */
#define GUEST_POPUP_ACTION_COPY              0xaau
#define GUEST_POPUP_BUTTON_OK                8u
#define GUEST_POPUP_BUTTON_NAME              4u           /* "Retry", relabelled "Name" */
#define GUEST_POPUP_ACTION_NAME              0xacu
#define GUEST_POPUP_ACTION_CANCEL            0xa8u
#define GUEST_POPUP_ACTION_ADD_BUDDY         0xadu
#define GUEST_SEARCH_POPUP_ACTION_CLOSE      0xa9u        /* search popup: just hides it */
#define GUEST_MESSAGE_POPUP_TYPE_PLAIN       2u           /* message popup type whose cancel just hides it */

#endif // GUNBROS_GUEST_SYMBOLS_H
