#include "gunbros/platform/network_time.h"
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NTP_UNIX_EPOCH_DELTA 2208988800ull  /* seconds from 1900-01-01 to 1970-01-01 */

enum { NT_IDLE, NT_PENDING, NT_SYNCED, NT_FAILED };

static atomic_int s_state = NT_IDLE;
static uint64_t s_start_boot_ms;
static uint64_t s_ntp_ms;      /* NTP time at s_ref_boot_ms */
static uint64_t s_ref_boot_ms;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static uint64_t clock_ms(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int sntp_query(const char *host) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, "123", &hints, &res) != 0 || !res) return 0;

    int ok = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct timeval tv = { 2, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t pkt[48] = { 0x23 };  /* LI 0, version 4, mode 3 (client) */
        uint64_t t_send = clock_ms(CLOCK_BOOTTIME);
        if (sendto(fd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) == (ssize_t)sizeof(pkt) &&
            recv(fd, pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt) && (pkt[0] & 0x07) == 4) {
            uint64_t t_recv = clock_ms(CLOCK_BOOTTIME);
            /* Transmit timestamp: big-endian 32.32 fixed point, seconds since 1900. */
            uint32_t sec = (uint32_t)pkt[40] << 24 | pkt[41] << 16 | pkt[42] << 8 | pkt[43];
            uint32_t frac = (uint32_t)pkt[44] << 24 | pkt[45] << 16 | pkt[46] << 8 | pkt[47];
            if (sec > NTP_UNIX_EPOCH_DELTA) {
                uint64_t server_ms = (sec - NTP_UNIX_EPOCH_DELTA) * 1000ull + (((uint64_t)frac * 1000u) >> 32);
                s_ntp_ms = server_ms + (t_recv - t_send) / 2;
                s_ref_boot_ms = t_recv;
                int64_t skew = (int64_t)s_ntp_ms - (int64_t)clock_ms(CLOCK_REALTIME);
                LOGI("[NTP] synced with %s (rtt %llu ms, device clock off by %lld ms)", host,
                     (unsigned long long)(t_recv - t_send), (long long)skew);
                ok = 1;
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

static void *sync_thread(void *arg) {
    (void)arg;
    static const char *const servers[] = { "time.google.com", "pool.ntp.org", "time.cloudflare.com" };
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        if (sntp_query(servers[i])) {
            atomic_store(&s_state, NT_SYNCED);
            return NULL;
        }
        LOGW("[NTP] no answer from %s", servers[i]);
    }
    atomic_store(&s_state, NT_FAILED);
    return NULL;
}

static void start_sync(void) {
    s_start_boot_ms = clock_ms(CLOCK_BOOTTIME);
    atomic_store(&s_state, NT_PENDING);
    pthread_t th;
    if (pthread_create(&th, NULL, sync_thread, NULL) == 0) {
        pthread_detach(th);
    } else {
        atomic_store(&s_state, NT_FAILED);
    }
}

uint64_t network_time_now_ms(void) {
    pthread_once(&s_once, start_sync);
    switch (atomic_load(&s_state)) {
    case NT_SYNCED:
        return s_ntp_ms + (clock_ms(CLOCK_BOOTTIME) - s_ref_boot_ms);
    case NT_PENDING:
        if (clock_ms(CLOCK_BOOTTIME) - s_start_boot_ms < NETWORK_TIME_TIMEOUT_MS) return 0;
        /* fall through: give up waiting and use the device clock */
    default: {
        static int s_warned = 0;
        if (!s_warned) {
            s_warned = 1;
            LOGW("[NTP] unavailable, using device clock");
        }
        return clock_ms(CLOCK_REALTIME);
    }
    }
}
