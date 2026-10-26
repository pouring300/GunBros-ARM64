#ifndef GUNBROS_NETWORK_TIME_H
#define GUNBROS_NETWORK_TIME_H

#include "gunbros/common/common.h"

GUNBROS_EXTERN_C_BEGIN

/*
 * Wall-clock time for the game's "network time", replacing Glu's defunct time server.
 * The first call starts a background SNTP query (time.google.com, then pool.ntp.org,
 * then time.cloudflare.com). Once it answers, time is NTP-based and advances with the
 * monotonic boot clock, so changing the phone's clock has no effect. If no server answers
 * within NETWORK_TIME_TIMEOUT_MS, the device clock is used instead.
 *
 * Returns Unix time in milliseconds, or 0 while the first query is still in flight.
 */
#define NETWORK_TIME_TIMEOUT_MS 5000u

uint64_t network_time_now_ms(void);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_NETWORK_TIME_H
