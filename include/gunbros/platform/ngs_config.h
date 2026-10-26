#ifndef GUNBROS_NGS_CONFIG_H
#define GUNBROS_NGS_CONFIG_H

#include "gunbros/common/common.h"

GUNBROS_EXTERN_C_BEGIN

/*
 * Where the replacement for Glu's NGS backend lives (see server/). Built into the bridge with
 * the GUNBROS_NGS_HOST / GUNBROS_NGS_PORT build settings; for testing, these system properties
 * override it:
 *   adb shell setprop debug.gunbros.ngs_host 127.0.0.1   (host name or IPv4; "off" = offline)
 *   adb shell setprop debug.gunbros.ngs_port 18080       (optional, default 80)
 * With no server configured the game stays offline, as on Glu's dead servers.
 */

/* True if a server is configured. */
bool ngs_config_enabled(void);

/* Server host name or IPv4, or "" if none. */
const char *ngs_config_host(void);

/* Server port (80 unless configured otherwise). */
uint16_t ngs_config_port(void);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_NGS_CONFIG_H
