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

GUNBROS_EXTERN_C_END

#endif // GUNBROS_GAME_HOOKS_H
