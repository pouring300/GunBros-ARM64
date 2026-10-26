#ifndef GUNBROS_GAME_HOOKS_H
#define GUNBROS_GAME_HOOKS_H

#include "gunbros/common/common.h"

GUNBROS_EXTERN_C_BEGIN

/**
 * Game-specific hook invoked on TouchEnded (touch type 2).
 * In PLAYER_SELECT state (5), detects tapping Percy Gun vs Francis Gun
 * and calls the guest character selection procedure.
 */
void game_hooks_on_touch_ended(int x, int y);

/**
 * Called every frame. The game only trusts "network time" once Glu's (defunct)
 * server has sent it, and CNGSSession::tick only advances it while non-zero, so
 * the refinery, daily bonus and challenges stay offline forever. Seed it
 * (from NTP, see network_time.h) whenever it is zero.
 */
void game_hooks_seed_network_time(void);

/**
 * Called every frame. Glu's server used to unlock the next refinery in the chain
 * (CRefinementManager gate table) when the player collected from the previous one;
 * the game itself never does. Recreate that: when slot K is collected (complete ->
 * ready), unlock every locked slot gated on K and save via SaveStateChange().
 */
void game_hooks_refinery_unlock_chain(void);

/**
 * Called every frame. When Glu's backend is redirected to a replacement server
 * (debug.gunbros.ngs_host), sign the player in once the main menu is up. The Android
 * port never shows the game's Glu account dialogs (CNGS_Platform::showDialog* are empty
 * stubs) and the original per-launch login went through Facebook, so the bridge calls the
 * game's own handlers itself: on first launch it registers an account with a generated
 * gamertag/password (saved to NgsLogin.dat next to the game's saves); on later launches,
 * with Credentials.dat present, it runs the device-level login (CNGSLocalUser::ValidateUser).
 */
void game_hooks_auto_register(void);

/**
 * Called every frame. Once the NGS sign-in has completed, mark the profile online
 * (CProfileManager +0xe0), so the game syncs profile data with the server
 * (CProfileManager::IsProfileValid). The game only does this at startup.
 */
void game_hooks_ngs_profile_online(void);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_GAME_HOOKS_H
