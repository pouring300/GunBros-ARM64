#include "gunbros/platform/ngs_config.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#ifndef GUNBROS_NGS_HOST
#define GUNBROS_NGS_HOST ""
#endif
#ifndef GUNBROS_NGS_PORT
#define GUNBROS_NGS_PORT ""
#endif

static char s_host[92];
static uint16_t s_port = 80;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void load(void) {
    const char *host = GUNBROS_NGS_HOST, *port = GUNBROS_NGS_PORT;
#ifdef __ANDROID__
    static char prop_host[PROP_VALUE_MAX], prop_port[PROP_VALUE_MAX];
    if (__system_property_get("debug.gunbros.ngs_host", prop_host) > 0) {
        host = prop_host;
        __system_property_get("debug.gunbros.ngs_port", prop_port);
        port = prop_port;
    }
#endif
    if (strcmp(host, "off") == 0) host = "";
    strncpy(s_host, host, sizeof(s_host) - 1);
    if (s_host[0] && port[0]) {
        int p = atoi(port);
        if (p > 0 && p < 65536) s_port = (uint16_t)p;
    }
    if (s_host[0]) LOGI("[NGS] server %s port %u", s_host, s_port);
}

bool ngs_config_enabled(void) {
    pthread_once(&s_once, load);
    return s_host[0] != '\0';
}

const char *ngs_config_host(void) {
    pthread_once(&s_once, load);
    return s_host;
}

uint16_t ngs_config_port(void) {
    pthread_once(&s_once, load);
    return s_port;
}
