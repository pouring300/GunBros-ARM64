#include "gunbros/platform/ngs_ext.h"
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define NGS_EXT_TIMEOUT_S 5

enum { EXT_IDLE, EXT_RUNNING, EXT_DONE };

static atomic_int s_state = EXT_IDLE;
static char s_path[128];
static char s_body[512];
static char s_reply[NGS_EXT_REPLY_MAX];

static int connect_server(void) {
    char host[92] = "", port[16] = "";
#ifdef __ANDROID__
    __system_property_get("debug.gunbros.ngs_host", host);
    __system_property_get("debug.gunbros.ngs_port", port);
#endif
    if (host[0] == '\0') return -1;
    if (port[0] == '\0') strcpy(port, "80");

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        struct timeval tv = { NGS_EXT_TIMEOUT_S, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

static void *request_thread(void *arg) {
    (void)arg;
    s_reply[0] = '\0';
    int fd = connect_server();
    if (fd >= 0) {
        char req[1024];
        int n = snprintf(req, sizeof(req),
                         "PUT %s HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                         s_path, strlen(s_body), s_body);
        if (n > 0 && n < (int)sizeof(req) && send(fd, req, (size_t)n, 0) == n) {
            char resp[2048];
            size_t got = 0;
            ssize_t r;
            while (got < sizeof(resp) - 1 && (r = recv(fd, resp + got, sizeof(resp) - 1 - got, 0)) > 0) {
                got += (size_t)r;
            }
            resp[got] = '\0';
            const char *body = strstr(resp, "\r\n\r\n");
            if (strncmp(resp, "HTTP/1.", 7) == 0 && strncmp(resp + 9, "200", 3) == 0 && body) {
                strncpy(s_reply, body + 4, sizeof(s_reply) - 1);
                s_reply[sizeof(s_reply) - 1] = '\0';
            }
        }
        close(fd);
    }
    if (s_reply[0] == '\0') LOGW("[NGS] request %s failed", s_path);
    atomic_store(&s_state, EXT_DONE);
    return NULL;
}

bool ngs_ext_start(const char *path, const char *json_body) {
    int expected = EXT_IDLE;
    if (!atomic_compare_exchange_strong(&s_state, &expected, EXT_RUNNING)) return false;
    snprintf(s_path, sizeof(s_path), "%s", path);
    snprintf(s_body, sizeof(s_body), "%s", json_body);
    pthread_t t;
    if (pthread_create(&t, NULL, request_thread, NULL) != 0) {
        s_reply[0] = '\0';
        atomic_store(&s_state, EXT_DONE);
        return true;
    }
    pthread_detach(t);
    return true;
}

bool ngs_ext_poll(char *reply, size_t reply_size) {
    if (atomic_load(&s_state) != EXT_DONE) return false;
    snprintf(reply, reply_size, "%s", s_reply);
    atomic_store(&s_state, EXT_IDLE);
    return true;
}

bool ngs_ext_json_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}
