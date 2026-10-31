#include "hle_internal.h"
#include "gunbros/platform/ngs_config.h"
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

/* ---------------- Abort / Halt ---------------- */
static void h_abort(hle_t *c) {
    LOGE("guest abort()/raise() lr=0x%x r0=0x%x", c->lr, c->r0);
    dynarmic_backend_halt();
}

static void h_ret0(hle_t *c)     { c->r0 = 0; }
static void h_retm1(hle_t *c)    { c->r0 = (uint32_t)-1; }

static void log_stub_call_once(const char *name, uint32_t lr, const char *extra) {
    #define MAX_STUB_SEEN 64
    static struct { const char *name; uint32_t lr; } s_seen[MAX_STUB_SEEN];
    static int s_seen_count = 0;

    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].name == name && s_seen[i].lr == lr) return;
    }
    if (s_seen_count < MAX_STUB_SEEN) {
        s_seen[s_seen_count].name = name;
        s_seen[s_seen_count].lr = lr;
        s_seen_count++;
    }

    if (extra && extra[0]) {
        LOGI("[STUB:%s] called from guest lr=0x%08x: %s", name, lr, extra);
    } else {
        LOGI("[STUB:%s] called from guest lr=0x%08x", name, lr);
    }
}

static void h_stub_fprintf(hle_t *c) {
    uint32_t stream_val = c->r0;
    const char *fmt = (const char*)guest_to_host(c->r1);
    char extra[256];
    bool is_std = (stream_val == 1 || stream_val == 2);
    if ((void*)(uintptr_t)stream_val == (void*)stdout || (void*)(uintptr_t)stream_val == (void*)stderr) {
        is_std = true;
    }

    if (!is_std) {
        snprintf(extra, sizeof(extra), "NON-STANDARD stream fp=0x%x (fmt='%s') - possible lost data write!",
                 stream_val, fmt ? fmt : "NULL");
        LOGE("[STUB:fprintf] NON-STANDARD stream fp=0x%x (fmt='%s') lr=0x%x - possible lost data write!",
             stream_val, fmt ? fmt : "NULL", c->lr);
    } else {
        snprintf(extra, sizeof(extra), "stream=std (fp=0x%x, fmt='%s')", stream_val, fmt ? fmt : "NULL");
    }
    log_stub_call_once("fprintf", c->lr, extra);
    c->r0 = 0;
}

static void h_stub_fdopen(hle_t *c) {
    int fd = (int)c->r0;
    const char *mode = (const char*)guest_to_host(c->r1);
    char extra[128];
    bool is_std = (fd == 0 || fd == 1 || fd == 2);
    if (!is_std) {
        snprintf(extra, sizeof(extra), "NON-STANDARD fd=%d mode='%s' - possible lost data file!",
                 fd, mode ? mode : "NULL");
        LOGE("[STUB:fdopen] NON-STANDARD fd=%d mode='%s' lr=0x%x - possible lost data file!",
             fd, mode ? mode : "NULL", c->lr);
    } else {
        snprintf(extra, sizeof(extra), "fd=%d mode='%s'", fd, mode ? mode : "NULL");
    }
    log_stub_call_once("fdopen", c->lr, extra);
    c->r0 = 0;
}

static void h_stub_puts(hle_t *c) {
    const char *str = (const char*)guest_to_host(c->r0);
    char extra[128] = {0};
    if (str) snprintf(extra, sizeof(extra), "msg='%.64s'", str);
    log_stub_call_once("puts", c->lr, extra);
#ifdef __ANDROID__
    if (str) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[GUEST:puts] %s", str);
#else
    if (str) printf("[GUEST:puts] %s\n", str);
#endif
    c->r0 = 0;
}

static void h_stub_dup(hle_t *c) {
    char extra[64];
    snprintf(extra, sizeof(extra), "fd=%d", (int)c->r0);
    log_stub_call_once("dup", c->lr, extra);
    set_guest_errno(EBADF);
    c->r0 = (uint32_t)-1;
}

static void h_stub_mkstemp(hle_t *c) {
    const char *tmpl = (const char*)guest_to_host(c->r0);
    char extra[128];
    snprintf(extra, sizeof(extra), "template='%s'", tmpl ? tmpl : "NULL");
    log_stub_call_once("mkstemp", c->lr, extra);
    set_guest_errno(EACCES);
    c->r0 = (uint32_t)-1;
}

/* ---------------- Network Passthrough Implementations ---------------- */

// Verbose traffic logging, read once at the first network call:
//   adb shell setprop debug.gunbros.net 1   (then restart the game)
static int net_trace(void) {
    static int s_trace = -1;
    if (s_trace < 0) {
#ifdef __ANDROID__
        char val[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.gunbros.net", val);
        s_trace = (val[0] == '1');
        if (s_trace) LOGI("[NET] verbose traffic logging enabled (debug.gunbros.net=1)");
#else
        s_trace = 0;
#endif
    }
    return s_trace;
}

// Glu's backend (*.glu.com) is redirected to the replacement server from ngs_config.h, or
// blocked if none is configured. Connections to it are marked so that the HTTP Host header can
// be rewritten: the game still names Glu's host, and some hosting platforms route
// requests by that header.
typedef struct {
    uint32_t ip;        /* resolved server address, network order; used to match connects */
} ngs_redirect_t;

#define NGS_MAX_FDS 1024

typedef struct {
    bool active;        /* connected to the NGS server */
    uint8_t *pending;   /* rewritten bytes not yet accepted by the socket */
    size_t pending_len;
    struct sockaddr_in addr;  /* where it is connected, for a retry */
    uint8_t *request;   /* the request sent on it (as rewritten), for a retry */
    size_t request_len;
    bool replied;       /* the reply's status line has been seen */
    int retries;
} ngs_socket_t;

#define NGS_MAX_RETRIES 2
#define NGS_RETRY_TIMEOUT_MS 3000

static ngs_socket_t s_ngs_sockets[NGS_MAX_FDS];

static ngs_redirect_t *ngs_redirect(void) {
    static ngs_redirect_t s_cfg;
    return &s_cfg;
}

static ngs_socket_t *ngs_socket(int fd) {
    return (fd >= 0 && fd < NGS_MAX_FDS && s_ngs_sockets[fd].active) ? &s_ngs_sockets[fd] : NULL;
}

static void ngs_socket_reset(int fd) {
    if (fd < 0 || fd >= NGS_MAX_FDS) return;
    free(s_ngs_sockets[fd].pending);
    free(s_ngs_sockets[fd].request);
    memset(&s_ngs_sockets[fd], 0, sizeof(s_ngs_sockets[fd]));
}

/* Remember what was sent, to send it again if the server's front end fails (ngs_retry). */
static void ngs_record(ngs_socket_t *s, const uint8_t *data, size_t len) {
    if (s->replied) {  /* a new request on the same connection */
        s->request_len = 0;
        s->replied = false;
        s->retries = 0;
    }
    uint8_t *grown = realloc(s->request, s->request_len + len);
    if (!grown) return;
    memcpy(grown + s->request_len, data, len);
    s->request = grown;
    s->request_len += len;
}

/* Wait until `fd` is ready for `events`, up to `ms`. */
static bool ngs_wait(int fd, short events, int ms) {
    struct pollfd p = { fd, events, 0 };
    return poll(&p, 1, ms) == 1 && (p.revents & events);
}

/* The game never finishes a request whose reply is an error from the hosting platform's front
   end (502/503/504) or that the connection drops before replying: the screen waiting for it
   stays busy. Connect again on the same descriptor and send the request again instead. */
static bool ngs_retry(int fd, ngs_socket_t *s) {
    int flags = fcntl(fd, F_GETFL, 0);
    int nfd = socket(AF_INET, SOCK_STREAM, 0);
    if (nfd < 0) return false;
    fcntl(nfd, F_SETFL, O_NONBLOCK);
    bool ok = connect(nfd, (struct sockaddr *)&s->addr, sizeof(s->addr)) == 0 ||
              (errno == EINPROGRESS && ngs_wait(nfd, POLLOUT, NGS_RETRY_TIMEOUT_MS));
    int err = 0;
    socklen_t el = sizeof(err);
    if (ok && (getsockopt(nfd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0)) ok = false;
    for (size_t sent = 0; ok && sent < s->request_len;) {
        ssize_t n = send(nfd, s->request + sent, s->request_len - sent, 0);
        if (n > 0) sent += (size_t)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ok = ngs_wait(nfd, POLLOUT, NGS_RETRY_TIMEOUT_MS);
        else ok = false;
    }
    if (!ok || flags < 0 || fcntl(nfd, F_SETFL, flags) != 0 || dup2(nfd, fd) < 0) {
        close(nfd);
        return false;
    }
    close(nfd);
    s->retries++;
    return true;
}

/* Before the game reads a reply: on a front-end error or a dropped connection, send the request
   again (at most NGS_MAX_RETRIES times). Returns true if the game should try reading later. */
static bool ngs_check_reply(int fd, ngs_socket_t *s) {
    if (s->replied || s->request_len == 0) return false;
    char head[16];
    ssize_t n = recv(fd, head, sizeof(head), MSG_PEEK | MSG_DONTWAIT);
    bool failed;
    if (n >= 12) {
        failed = memcmp(head, "HTTP/1.", 7) == 0 && head[9] == '5' && head[10] == '0' &&
                 (head[11] == '2' || head[11] == '3' || head[11] == '4');
    } else if (n > 0) {
        return true;  /* wait for the whole status line */
    } else {
        failed = n == 0 || errno == ECONNRESET || errno == EPIPE;
    }
    if (!failed) {
        if (n >= 12) s->replied = true;
        return false;
    }
    if (s->retries < NGS_MAX_RETRIES && ngs_retry(fd, s)) {
        LOGW("[NGS] %s, sending the request again", n >= 12 ? "server front end answered 5xx" : "connection dropped");
        return true;
    }
    s->replied = true;  /* give up: let the game see it */
    return false;
}

/* Send what's left of a rewritten request. Returns false if some is still pending. */
static bool ngs_flush(int fd, ngs_socket_t *s) {
    while (s->pending_len > 0) {
        ssize_t n = send(fd, s->pending, s->pending_len, 0);
        if (n <= 0) return false;
        memmove(s->pending, s->pending + n, s->pending_len - (size_t)n);
        s->pending_len -= (size_t)n;
    }
    return true;
}

/* `data` with its "Host:" header line replaced by the server's, or NULL if it has none. */
static uint8_t *ngs_rewrite_host(const uint8_t *data, size_t len, size_t *out_len) {
    const uint8_t *end = data + len, *line = NULL;
    for (const uint8_t *p = data; p + 6 <= end; p++) {
        if ((p == data || p[-1] == '\n') && strncasecmp((const char *)p, "Host:", 5) == 0) {
            line = p;
            break;
        }
    }
    if (!line) return NULL;
    const uint8_t *eol = memchr(line, '\n', (size_t)(end - line));
    if (!eol) return NULL;
    eol++;
    char header[128];
    if (ngs_config_port() == 80) {
        snprintf(header, sizeof(header), "Host: %s\r\n", ngs_config_host());
    } else {
        snprintf(header, sizeof(header), "Host: %s:%u\r\n", ngs_config_host(), ngs_config_port());
    }
    size_t hlen = strlen(header), before = (size_t)(line - data), after = (size_t)(end - eol);
    uint8_t *out = malloc(before + hlen + after);
    if (!out) return NULL;
    memcpy(out, data, before);
    memcpy(out + before, header, hlen);
    memcpy(out + before + hlen, eol, after);
    *out_len = before + hlen + after;
    return out;
}

static const char *fmt_sockaddr(const struct sockaddr *sa, char *buf, size_t n) {
    if (!sa) {
        snprintf(buf, n, "-");
    } else if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in*)sa;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        snprintf(buf, n, "%s:%u", ip, ntohs(sin->sin_port));
    } else {
        snprintf(buf, n, "family=%d", sa->sa_family);
    }
    return buf;
}

// First bytes of a packet as hex plus printable text, for spotting protocol messages.
static const char *fmt_preview(const void *data, ssize_t len, char *buf, size_t n) {
    const unsigned char *p = (const unsigned char*)data;
    size_t show = len > 32 ? 32 : (size_t)(len < 0 ? 0 : len);
    size_t o = 0;
    for (size_t i = 0; i < show && o + 3 < n; i++) o += snprintf(buf + o, n - o, "%02x", p[i]);
    if (o + 3 < n) o += snprintf(buf + o, n - o, " |");
    for (size_t i = 0; i < show && o + 2 < n; i++) buf[o++] = (p[i] >= 0x20 && p[i] < 0x7f) ? p[i] : '.';
    if (o + 1 < n) buf[o++] = '|';
    buf[o < n ? o : n - 1] = '\0';
    return buf;
}

static const char *fmt_local(int fd, char *buf, size_t n) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr*)&ss, &sl) != 0) return "?";
    return fmt_sockaddr((struct sockaddr*)&ss, buf, n);
}

typedef struct {
    int32_t tv_sec;
    int32_t tv_usec;
} timeval32_t;

#define GUEST_HOSTENT_ADDR     0x60008000u
#define GUEST_HOSTENT_ALIASES  0x60008020u
#define GUEST_HOSTENT_ADDRLIST 0x60008030u
#define GUEST_HOSTENT_IP       0x60008038u
#define GUEST_HOSTENT_NAME     0x60008040u
#define GUEST_INET_NTOA_BUF    0x60008200u

typedef struct {
    uint32_t h_name;      // char *
    uint32_t h_aliases;   // char **
    int32_t  h_addrtype;  // int
    int32_t  h_length;    // int
    uint32_t h_addr_list; // char **
} hostent32_t;

static void h_socket(hle_t *c) {
    int domain = (int)c->r0;
    int type = (int)c->r1;
    int protocol = (int)c->r2;
    int fd = socket(domain, type, protocol);
    if (fd < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        LOGE("[NET:socket] domain=%d type=%d proto=%d FAILED errno=%d (%s)",
             domain, type, protocol, errno, strerror(errno));
    } else {
        c->r0 = (uint32_t)fd;
        LOGI("[NET:socket] domain=%d type=%d proto=%d -> fd=%d", domain, type, protocol, fd);
    }
}

static void h_connect(hle_t *c) {
    int fd = (int)c->r0;
    uint32_t g_addr = c->r1;
    socklen_t len = (socklen_t)c->r2;
    const struct sockaddr *addr = (const struct sockaddr*)guest_to_host(g_addr);
    if (!addr && g_addr != 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    struct sockaddr_in redirected;
    ngs_socket_reset(fd);
    if (ngs_config_enabled() && addr && addr->sa_family == AF_INET && len >= (socklen_t)sizeof(redirected)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in*)addr;
        if (sin->sin_addr.s_addr == ngs_redirect()->ip && ntohs(sin->sin_port) == 80) {
            redirected = *sin;
            redirected.sin_port = htons(ngs_config_port());
            addr = (const struct sockaddr*)&redirected;
            if (fd >= 0 && fd < NGS_MAX_FDS) {
                s_ngs_sockets[fd].active = true;
                s_ngs_sockets[fd].addr = redirected;
            }
        }
    }
    int ret = connect(fd, addr, len);
    int err = errno;
    char a[64];
    fmt_sockaddr(addr, a, sizeof(a));
    if (ret < 0) {
        set_guest_errno(err);
        c->r0 = (uint32_t)-1;
        if (err == EINPROGRESS) {
            LOGI("[NET:connect] fd=%d -> %s EINPROGRESS (non-blocking connect initiated)", fd, a);
        } else {
            LOGE("[NET:connect] fd=%d -> %s FAILED errno=%d (%s)", fd, a, err, strerror(err));
        }
    } else {
        c->r0 = 0;
        LOGI("[NET:connect] fd=%d -> %s connected successfully", fd, a);
    }
}

static void h_bind(hle_t *c) {
    int fd = (int)c->r0;
    uint32_t g_addr = c->r1;
    socklen_t len = (socklen_t)c->r2;
    const struct sockaddr *addr = (const struct sockaddr*)guest_to_host(g_addr);
    if (!addr && g_addr != 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    int ret = bind(fd, addr, len);
    char a[64], l[64];
    /* GameSpy binds a fixed UDP port (4747). A second copy of the game (e.g. in another Android
       profile) can't get it, and GameSpy's error path then crashes; any free port does as well
       behind NAT. */
    if (ret < 0 && errno == EADDRINUSE && addr && addr->sa_family == AF_INET &&
        len >= (socklen_t)sizeof(struct sockaddr_in) && ((const struct sockaddr_in *)addr)->sin_port != 0) {
        struct sockaddr_in any_port = *(const struct sockaddr_in *)addr;
        any_port.sin_port = 0;
        LOGW("[NET:bind] fd=%d %s is in use, binding to a free port", fd, fmt_sockaddr(addr, a, sizeof(a)));
        ret = bind(fd, (const struct sockaddr *)&any_port, sizeof(any_port));
    }
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        LOGE("[NET:bind] fd=%d %s FAILED errno=%d (%s)", fd, fmt_sockaddr(addr, a, sizeof(a)), errno, strerror(errno));
    } else {
        c->r0 = 0;
        LOGI("[NET:bind] fd=%d requested %s, bound to %s", fd,
             fmt_sockaddr(addr, a, sizeof(a)), fmt_local(fd, l, sizeof(l)));
    }
}

static void h_send(hle_t *c) {
    int fd = (int)c->r0;
    uint32_t g_buf = c->r1;
    size_t len = (size_t)c->r2;
    int flags = (int)c->r3;
    const void *buf = guest_to_host(g_buf);
    if (!buf && g_buf != 0 && len > 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    ngs_socket_t *ngs = ngs_socket(fd);
    if (ngs && !ngs_flush(fd, ngs)) {
        set_guest_errno(EAGAIN);
        c->r0 = (uint32_t)-1;
        return;
    }
    size_t rewritten_len = 0;
    uint8_t *rewritten = (ngs && buf) ? ngs_rewrite_host(buf, len, &rewritten_len) : NULL;
    ssize_t ret;
    if (rewritten) {
        /* The guest sees its own byte count; whatever the socket doesn't take now is sent
           before anything else on this connection (here or in recv). */
        ret = send(fd, rewritten, rewritten_len, flags);
        if (ret >= 0) {
            ngs_record(ngs, rewritten, rewritten_len);
            free(ngs->pending);
            ngs->pending = rewritten;
            ngs->pending_len = rewritten_len;
            memmove(rewritten, rewritten + ret, rewritten_len - (size_t)ret);
            ngs->pending_len -= (size_t)ret;
            ret = (ssize_t)len;
        } else {
            free(rewritten);
        }
    } else {
        ret = send(fd, buf, len, flags);
        if (ngs && ret > 0) ngs_record(ngs, buf, (size_t)ret);
    }
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("[NET:send] fd=%d len=%zu FAILED errno=%d (%s)", fd, len, errno, strerror(errno));
        }
    } else {
        c->r0 = (uint32_t)ret;
        if (net_trace()) {
            char pv[128];
            LOGI("[NET:send] fd=%d %zd bytes %s", fd, ret, fmt_preview(buf, ret, pv, sizeof(pv)));
        }
    }
}

static void h_recv(hle_t *c) {
    int fd = (int)c->r0;
    uint32_t g_buf = c->r1;
    size_t len = (size_t)c->r2;
    int flags = (int)c->r3;
    void *buf = guest_to_host(g_buf);
    if (!buf && g_buf != 0 && len > 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    ngs_socket_t *ngs = ngs_socket(fd);
    if (ngs && (!ngs_flush(fd, ngs) || ngs_check_reply(fd, ngs))) {
        set_guest_errno(EAGAIN);
        c->r0 = (uint32_t)-1;
        return;
    }
    ssize_t ret = recv(fd, buf, len, flags);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("[NET:recv] fd=%d len=%zu FAILED errno=%d (%s)", fd, len, errno, strerror(errno));
        }
    } else {
        c->r0 = (uint32_t)ret;
        if (net_trace()) {
            char pv[128];
            LOGI("[NET:recv] fd=%d %zd bytes %s", fd, ret, ret ? fmt_preview(buf, ret, pv, sizeof(pv)) : "(peer closed)");
        }
    }
}

static void h_sendto(hle_t *c) {
    int fd = (int)c->r0;
    uint32_t g_buf = c->r1;
    size_t len = (size_t)c->r2;
    int flags = (int)c->r3;

    uint32_t sp_args[2];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    uint32_t g_dest_addr = sp_args[0];
    socklen_t addrlen = (socklen_t)sp_args[1];

    const void *buf = guest_to_host(g_buf);
    if (!buf && g_buf != 0 && len > 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    const struct sockaddr *dest_addr = g_dest_addr ? (const struct sockaddr*)guest_to_host(g_dest_addr) : NULL;
    ssize_t ret = sendto(fd, buf, len, flags, dest_addr, addrlen);
    char a[64];
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("[NET:sendto] fd=%d -> %s len=%zu FAILED errno=%d (%s)", fd,
                 fmt_sockaddr(dest_addr, a, sizeof(a)), len, errno, strerror(errno));
        }
    } else {
        c->r0 = (uint32_t)ret;
        if (net_trace()) {
            char pv[128], l[64];
            LOGI("[NET:sendto] fd=%d %s -> %s %zd bytes %s", fd, fmt_local(fd, l, sizeof(l)),
                 fmt_sockaddr(dest_addr, a, sizeof(a)), ret, fmt_preview(buf, ret, pv, sizeof(pv)));
        }
    }
}

static void h_recvfrom(hle_t *c) {
    int fd = (int)c->r0;
    uint32_t g_buf = c->r1;
    size_t len = (size_t)c->r2;
    int flags = (int)c->r3;

    uint32_t sp_args[2];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    uint32_t g_src_addr = sp_args[0];
    uint32_t g_addrlen_ptr = sp_args[1];

    void *buf = guest_to_host(g_buf);
    if (!buf && g_buf != 0 && len > 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    struct sockaddr *src_addr = g_src_addr ? (struct sockaddr*)guest_to_host(g_src_addr) : NULL;
    socklen_t *p_addrlen = g_addrlen_ptr ? (socklen_t*)guest_to_host(g_addrlen_ptr) : NULL;

    ssize_t ret = recvfrom(fd, buf, len, flags, src_addr, p_addrlen);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("[NET:recvfrom] fd=%d len=%zu FAILED errno=%d (%s)", fd, len, errno, strerror(errno));
        }
    } else {
        c->r0 = (uint32_t)ret;
        if (net_trace()) {
            char pv[128], a[64];
            LOGI("[NET:recvfrom] fd=%d <- %s %zd bytes %s", fd, fmt_sockaddr(src_addr, a, sizeof(a)),
                 ret, fmt_preview(buf, ret, pv, sizeof(pv)));
        }
    }
}

static void h_select(hle_t *c) {
    int nfds = (int)c->r0;
    uint32_t g_read = c->r1;
    uint32_t g_write = c->r2;
    uint32_t g_except = c->r3;

    uint32_t sp_args[1];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    uint32_t g_timeout = sp_args[0];

    fd_set host_read, host_write, host_except;
    fd_set *p_read = NULL, *p_write = NULL, *p_except = NULL;

    if (g_read) {
        void *g = guest_to_host(g_read);
        if (g) {
            memcpy(&host_read, g, sizeof(fd_set));
            p_read = &host_read;
        }
    }
    if (g_write) {
        void *g = guest_to_host(g_write);
        if (g) {
            memcpy(&host_write, g, sizeof(fd_set));
            p_write = &host_write;
        }
    }
    if (g_except) {
        void *g = guest_to_host(g_except);
        if (g) {
            memcpy(&host_except, g, sizeof(fd_set));
            p_except = &host_except;
        }
    }

    struct timeval htv;
    struct timeval *p_htv = NULL;
    if (g_timeout) {
        timeval32_t *gtv = (timeval32_t*)guest_to_host(g_timeout);
        if (gtv) {
            htv.tv_sec = gtv->tv_sec;
            htv.tv_usec = gtv->tv_usec;
            p_htv = &htv;
        }
    }

    int ret = select(nfds, p_read, p_write, p_except, p_htv);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
    } else {
        c->r0 = (uint32_t)ret;
        if (p_read && g_read)   memcpy(guest_to_host(g_read), &host_read, sizeof(fd_set));
        if (p_write && g_write) memcpy(guest_to_host(g_write), &host_write, sizeof(fd_set));
        if (p_except && g_except) memcpy(guest_to_host(g_except), &host_except, sizeof(fd_set));
        if (p_htv && g_timeout) {
            timeval32_t *gtv = (timeval32_t*)guest_to_host(g_timeout);
            if (gtv) {
                gtv->tv_sec = (int32_t)htv.tv_sec;
                gtv->tv_usec = (int32_t)htv.tv_usec;
            }
        }
    }
}

static void h_setsockopt(hle_t *c) {
    int fd = (int)c->r0;
    int level = (int)c->r1;
    int optname = (int)c->r2;
    uint32_t g_val = c->r3;

    uint32_t sp_args[1];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    socklen_t optlen = (socklen_t)sp_args[0];

    const void *optval = g_val ? guest_to_host(g_val) : NULL;
    int ret;

    if (level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)) {
        if (optval && optlen >= sizeof(timeval32_t)) {
            const timeval32_t *gtv = (const timeval32_t*)optval;
            struct timeval htv;
            htv.tv_sec = gtv->tv_sec;
            htv.tv_usec = gtv->tv_usec;
            ret = setsockopt(fd, level, optname, &htv, sizeof(htv));
        } else {
            ret = setsockopt(fd, level, optname, optval, optlen);
        }
    } else {
        ret = setsockopt(fd, level, optname, optval, optlen);
    }

    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        LOGE("[NET:setsockopt] fd=%d lvl=%d opt=%d FAILED errno=%d (%s)",
             fd, level, optname, errno, strerror(errno));
    } else {
        c->r0 = 0;
        LOGI("[NET:setsockopt] fd=%d lvl=%d opt=%d ok", fd, level, optname);
    }
}

static void h_getsockopt(hle_t *c) {
    int fd = (int)c->r0;
    int level = (int)c->r1;
    int optname = (int)c->r2;
    uint32_t g_val = c->r3;

    uint32_t sp_args[1];
    guest_mem_read(c->sp, sp_args, sizeof(sp_args));
    uint32_t g_len_ptr = sp_args[0];

    void *optval = g_val ? guest_to_host(g_val) : NULL;
    socklen_t *p_optlen = g_len_ptr ? (socklen_t*)guest_to_host(g_len_ptr) : NULL;
    int ret;

    if (level == SOL_SOCKET && (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)) {
        struct timeval htv;
        socklen_t hlen = sizeof(htv);
        ret = getsockopt(fd, level, optname, &htv, &hlen);
        if (ret == 0 && optval && p_optlen) {
            timeval32_t *gtv = (timeval32_t*)optval;
            gtv->tv_sec = (int32_t)htv.tv_sec;
            gtv->tv_usec = (int32_t)htv.tv_usec;
            *p_optlen = sizeof(timeval32_t);
        }
    } else {
        ret = getsockopt(fd, level, optname, optval, p_optlen);
    }

    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
        LOGE("[NET:getsockopt] fd=%d lvl=%d opt=%d FAILED errno=%d (%s)",
             fd, level, optname, errno, strerror(errno));
    } else {
        c->r0 = 0;
        LOGI("[NET:getsockopt] fd=%d lvl=%d opt=%d ok", fd, level, optname);
    }
}

static void h_getsockname(hle_t *c) {
    int fd = (int)c->r0;
    struct sockaddr *addr = c->r1 ? (struct sockaddr*)guest_to_host(c->r1) : NULL;
    socklen_t *p_len = c->r2 ? (socklen_t*)guest_to_host(c->r2) : NULL;
    int ret = getsockname(fd, addr, p_len);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
    } else {
        c->r0 = 0;
        if (net_trace()) {
            char a[64];
            LOGI("[NET:getsockname] fd=%d -> %s", fd, fmt_sockaddr(addr, a, sizeof(a)));
        }
    }
}

static void h_getpeername(hle_t *c) {
    int fd = (int)c->r0;
    struct sockaddr *addr = c->r1 ? (struct sockaddr*)guest_to_host(c->r1) : NULL;
    socklen_t *p_len = c->r2 ? (socklen_t*)guest_to_host(c->r2) : NULL;
    int ret = getpeername(fd, addr, p_len);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
    } else {
        c->r0 = 0;
    }
}

static void h_inet_addr(hle_t *c) {
    const char *cp = (const char*)guest_to_host(c->r0);
    if (!cp) {
        c->r0 = (uint32_t)-1;
        return;
    }
    in_addr_t addr = inet_addr(cp);
    c->r0 = (uint32_t)addr;
}

static void h_inet_ntoa(hle_t *c) {
    struct in_addr in;
    in.s_addr = (in_addr_t)c->r0;
    char *str = inet_ntoa(in);
    if (str) {
        char *g_dst = (char*)guest_to_host(GUEST_INET_NTOA_BUF);
        if (g_dst) {
            strncpy(g_dst, str, 63);
            g_dst[63] = '\0';
            c->r0 = GUEST_INET_NTOA_BUF;
            return;
        }
    }
    c->r0 = 0;
}

static void h_gethostbyname(hle_t *c) {
    const char *host = (const char*)guest_to_host(c->r0);
    if (!host || !host[0]) {
        c->r0 = 0;
        return;
    }

    const char *lookup = host;
    int is_glu = strstr(host, "glu.com") != NULL;
    if (is_glu) {
        if (!ngs_config_enabled()) {
            log_stub_call_once("gethostbyname:blocked_glu", c->lr, host);
            c->r0 = 0;
            return;
        }
        lookup = ngs_config_host();
        LOGI("[NGS] '%s' -> %s", host, lookup);
    }

    LOGI("[NET:gethostbyname] Resolving host '%s'...", lookup);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(lookup, NULL, &hints, &res);
    if (gai_err != 0 || !res) {
        LOGE("[NET:gethostbyname] getaddrinfo('%s') failed: %s (err=%d)",
             host, gai_strerror(gai_err), gai_err);
        set_guest_errno(ENOENT);
        c->r0 = 0;
        return;
    }

    struct sockaddr_in *sin = (struct sockaddr_in*)res->ai_addr;
    uint32_t ip = sin->sin_addr.s_addr;
    freeaddrinfo(res);
    if (is_glu) ngs_redirect()->ip = ip;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
    LOGI("[NET:gethostbyname] Host '%s' resolved to %s (0x%08x)", host, ip_str, ip);

    // Populate guest hostent structures at 0x60008000
    uint32_t *p_ip = (uint32_t*)guest_to_host(GUEST_HOSTENT_IP);
    if (p_ip) *p_ip = ip;

    char *p_name = (char*)guest_to_host(GUEST_HOSTENT_NAME);
    if (p_name) {
        strncpy(p_name, host, 127);
        p_name[127] = '\0';
    }

    uint32_t *p_aliases = (uint32_t*)guest_to_host(GUEST_HOSTENT_ALIASES);
    if (p_aliases) {
        p_aliases[0] = 0;
    }

    uint32_t *p_addrlist = (uint32_t*)guest_to_host(GUEST_HOSTENT_ADDRLIST);
    if (p_addrlist) {
        p_addrlist[0] = GUEST_HOSTENT_IP;
        p_addrlist[1] = 0;
    }

    hostent32_t *h32 = (hostent32_t*)guest_to_host(GUEST_HOSTENT_ADDR);
    if (h32) {
        h32->h_name = GUEST_HOSTENT_NAME;
        h32->h_aliases = GUEST_HOSTENT_ALIASES;
        h32->h_addrtype = AF_INET;
        h32->h_length = 4;
        h32->h_addr_list = GUEST_HOSTENT_ADDRLIST;
        c->r0 = GUEST_HOSTENT_ADDR;
    } else {
        c->r0 = 0;
    }
}

static void h_gethostbyaddr(hle_t *c) {
    uint32_t g_addr = c->r0;
    socklen_t len = (socklen_t)c->r1;
    int type = (int)c->r2;
    void *addr = guest_to_host(g_addr);
    if (!addr || len < 4 || type != AF_INET) {
        c->r0 = 0;
        return;
    }

    uint32_t ip = *(uint32_t*)addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));

    uint32_t *p_ip = (uint32_t*)guest_to_host(GUEST_HOSTENT_IP);
    if (p_ip) *p_ip = ip;

    char *p_name = (char*)guest_to_host(GUEST_HOSTENT_NAME);
    if (p_name) {
        strncpy(p_name, ip_str, 127);
        p_name[127] = '\0';
    }

    uint32_t *p_aliases = (uint32_t*)guest_to_host(GUEST_HOSTENT_ALIASES);
    if (p_aliases) {
        p_aliases[0] = 0;
    }

    uint32_t *p_addrlist = (uint32_t*)guest_to_host(GUEST_HOSTENT_ADDRLIST);
    if (p_addrlist) {
        p_addrlist[0] = GUEST_HOSTENT_IP;
        p_addrlist[1] = 0;
    }

    hostent32_t *h32 = (hostent32_t*)guest_to_host(GUEST_HOSTENT_ADDR);
    if (h32) {
        h32->h_name = GUEST_HOSTENT_NAME;
        h32->h_aliases = GUEST_HOSTENT_ALIASES;
        h32->h_addrtype = AF_INET;
        h32->h_length = 4;
        h32->h_addr_list = GUEST_HOSTENT_ADDRLIST;
        c->r0 = GUEST_HOSTENT_ADDR;
    } else {
        c->r0 = 0;
    }
}

static void h_gethostname(hle_t *c) {
    char *name = (char*)guest_to_host(c->r0);
    size_t len = (size_t)c->r1;
    if (!name || len == 0) {
        set_guest_errno(EFAULT);
        c->r0 = (uint32_t)-1;
        return;
    }
    int ret = gethostname(name, len);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
    } else {
        c->r0 = 0;
        LOGI("[NET:gethostname] -> '%s'", name);
    }
}

static void h_shutdown(hle_t *c) {
    int fd = (int)c->r0;
    int how = (int)c->r1;
    int ret = shutdown(fd, how);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
    } else {
        c->r0 = 0;
    }
}

static void h_close(hle_t *c) {
    int fd = (int)c->r0;
    ngs_socket_reset(fd);
    if (fd >= 3) {
        int ret = close(fd);
        if (ret < 0) {
            set_guest_errno(errno);
            c->r0 = (uint32_t)-1;
            return;
        }
    }
    c->r0 = 0;
}

static void h_ioctl(hle_t *c) {
    int fd = (int)c->r0;
    unsigned long req = (unsigned long)c->r1;
    void *arg = c->r2 ? guest_to_host(c->r2) : NULL;
    int ret = ioctl(fd, req, arg);
    if (ret < 0) {
        set_guest_errno(errno);
        c->r0 = (uint32_t)-1;
    } else {
        c->r0 = (uint32_t)ret;
    }
}

/* ---------------- C++ Guards & AtExit ---------------- */
static void h_cxa_guard_acquire(hle_t *c) {
    uint32_t *g = (uint32_t*)guest_to_host(c->r0);
    c->r0 = (g && *g == 0) ? 1 : 0;
}
static void h_cxa_guard_release(hle_t *c) {
    uint32_t *g = (uint32_t*)guest_to_host(c->r0);
    if (g) *g = 1;
    c->r0 = 0;
}
static void h_cxa_pure_virtual(hle_t *c) {
    LOGE("__cxa_pure_virtual called! lr=0x%x", c->lr);
    c->r0 = 0;
}
static void hle_errno(hle_t *c) { c->r0 = GUEST_ERRNO_ADDR; }

/* ---------------- Android Log HLE ---------------- */
static void h_android_log_print(hle_t *c) {
    int prio = (int)c->r0;
    const char *tag = (const char*)guest_to_host(c->r1);
    const char *fmt = (const char*)guest_to_host(c->r2);
    char buf[1024];
    uint32_t reg_args[1] = { c->r3 };
    safe_guest_snprintf(buf, sizeof(buf), fmt, reg_args, 1, c->sp);
#ifdef __ANDROID__
    __android_log_print(prio, tag ? tag : LOG_TAG, "%s", buf);
#else
    printf("[GUEST:%s:%d] %s\n", tag ? tag : "APP", prio, buf);
#endif
    c->r0 = 0;
}

const hle_def_t g_hle_net_stubs_defs[] = {
    // Threading & synchronization stubs
    {"pthread_mutex_init", h_ret0}, {"pthread_mutex_destroy", h_ret0},
    {"pthread_mutex_lock", h_ret0}, {"pthread_mutex_unlock", h_ret0},

    // C++ runtime, guards, exceptions & unwinding
    {"__cxa_guard_acquire", h_cxa_guard_acquire}, {"__cxa_guard_release", h_cxa_guard_release},
    {"__cxa_pure_virtual", h_cxa_pure_virtual}, {"__cxa_call_unexpected", h_cxa_pure_virtual},
    {"__cxa_atexit", h_ret0}, {"atexit", h_ret0},
    {"__cxa_finalize", h_ret0}, {"__gnu_Unwind_Find_exidx", h_ret0}, {"__cxa_begin_cleanup", h_ret0},
    {"__cxa_type_match", h_ret0}, {"__errno", hle_errno},

    // Abort & signals
    {"abort", h_abort}, {"raise", h_abort},

    // Network & POSIX implementations
    {"socket", h_socket}, {"bind", h_bind}, {"connect", h_connect}, {"send", h_send},
    {"recv", h_recv}, {"sendto", h_sendto}, {"recvfrom", h_recvfrom}, {"setsockopt", h_setsockopt},
    {"getsockopt", h_getsockopt}, {"getsockname", h_getsockname}, {"getpeername", h_getpeername},
    {"gethostbyname", h_gethostbyname}, {"gethostbyaddr", h_gethostbyaddr},
    {"inet_addr", h_inet_addr}, {"inet_ntoa", h_inet_ntoa}, {"gethostname", h_gethostname},
    {"shutdown", h_shutdown}, {"close", h_close}, {"usleep", h_ret0}, {"puts", h_stub_puts},
    {"fprintf", h_stub_fprintf}, {"fdopen", h_stub_fdopen}, {"dup", h_stub_dup},
    {"select", h_select}, {"ioctl", h_ioctl}, {"mkstemp", h_stub_mkstemp},
    {"qsort", h_ret0},

    // Android logging
    {"__android_log_print", h_android_log_print}, {"__android_log_assert", h_android_log_print},
    {"__android_log_vprint", h_android_log_print},
};

const size_t g_hle_net_stubs_defs_count = sizeof(g_hle_net_stubs_defs) / sizeof(g_hle_net_stubs_defs[0]);
