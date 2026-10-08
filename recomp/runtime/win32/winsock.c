/* Winsock 1.1 (WSOCK32 by ordinal) on BSD sockets: the TCP and UDP sockets Dungeon Siege II's network layer opens for
 * LAN and direct-IP games. SOCKETs are small handles into a table of host descriptors; fd_sets, sockaddrs, option
 * levels and error codes are translated. (Router port forwarding is separate: portmap.c, off with DS_NO_PORTMAP=1.) */
#include "w32.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void socket_no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd; /* Linux suppresses SIGPIPE on each send with MSG_NOSIGNAL. */
#endif
}

#define INVALID_SOCKET_ 0xffffffffu
#define SOCKET_ERROR_ 0xffffffffu
enum { MAXS = 256, SBASE = 0x1000 };
static struct { int fd; int nb; } S[MAXS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t wsa_err(int e)
{
    switch (e) {
    case EWOULDBLOCK: case EINPROGRESS: return 10035;    /* WSAEWOULDBLOCK (also for a non-blocking connect) */
    case EALREADY: return 10037; case ENOTSOCK: return 10038; case EDESTADDRREQ: return 10039; case EMSGSIZE: return 10040;
    case ENOPROTOOPT: return 10042; case EPROTONOSUPPORT: return 10043; case EOPNOTSUPP: return 10045; case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048; case EADDRNOTAVAIL: return 10049; case ENETDOWN: return 10050; case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053; case ECONNRESET: case EPIPE: return 10054; case ENOBUFS: return 10055; case EISCONN: return 10056;
    case ENOTCONN: return 10057; case ESHUTDOWN: return 10058; case ETIMEDOUT: return 10060; case ECONNREFUSED: return 10061;
    case EHOSTDOWN: return 10064; case EHOSTUNREACH: return 10065; case EACCES: return 10013; case EFAULT: return 10014;
    case EINTR: return 10004; case EMFILE: return 10024; case EBADF: return 10038;
    }
    return 10022;                                       /* WSAEINVAL */
}
/* W32_NETLOG=1: each send/recv with its first bytes (development) */
static void netlog(const char *what, uint32_t sock, const uint8_t *p, long n)
{
    static int on = -1; if (on < 0) on = getenv("W32_NETLOG") != 0;
    if (!on) return;
    fprintf(stderr, "net: %s %x %ld:", what, sock, n);
    for (long k = 0; k < n && k < 48; k++) fprintf(stderr, " %02x", p[k]);
    fprintf(stderr, " |"); for (long k = 0; k < n && k < 48; k++) fputc(p[k] >= 32 && p[k] < 127 ? p[k] : '.', stderr); fprintf(stderr, "|\n");
}
static uint32_t fail(Ctx *c, uint32_t e) { w32_set_last_error(c, e); return SOCKET_ERROR_; }
static uint32_t fail_errno(Ctx *c) { return fail(c, wsa_err(errno)); }
static int sfd(uint32_t s)
{
    if (s < SBASE || s >= SBASE + MAXS) return -1;
    pthread_mutex_lock(&s_lock); int fd = S[s - SBASE].fd - 1; pthread_mutex_unlock(&s_lock);
    return fd;
}
#define SOCK(arg, n) int fd = sfd(arg); if (fd < 0) RET(fail(c, 10038), n)   /* WSAENOTSOCK */

/* sockaddr_in: Windows {u16 family, u16 port, u32 addr, 8 zero} <-> BSD {u8 len, u8 family, ...} */
static int to_host(uint32_t g, uint32_t len, struct sockaddr_in *a)
{
    if (!g || len < 8 || rt_r16(G_MEM, g) != 2) return 0;
    memset(a, 0, sizeof *a);
#ifdef __APPLE__
    a->sin_len = sizeof *a;
#endif
    a->sin_family = AF_INET;
    memcpy(&a->sin_port, GP(g + 2), 2); memcpy(&a->sin_addr, GP(g + 4), 4);
    return 1;
}
static void to_guest(const struct sockaddr_in *a, uint32_t g, uint32_t lenp)
{
    if (!g) return;
    uint32_t cap = lenp ? rt_r32(G_MEM, lenp) : 16; if (cap < 16) return;
    rt_w16(G_MEM, g, 2); memcpy(GP(g + 2), &a->sin_port, 2); memcpy(GP(g + 4), &a->sin_addr, 4); memset(GP(g + 8), 0, 8);
    if (lenp) rt_w32(G_MEM, lenp, 16);
}

IMPL(wsock32, ord23)                                    /* socket(af, type, protocol) */
{
    uint32_t af = ARG(0), type = ARG(1), proto = ARG(2);
    if (af != 2) { w32_set_last_error(c, 10047); RET(INVALID_SOCKET_, 3); }
    if (type != 1 && type != 2) { w32_set_last_error(c, 10044); RET(INVALID_SOCKET_, 3); }
    int fd = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, (int)proto);
    if (fd < 0) { w32_set_last_error(c, wsa_err(errno)); RET(INVALID_SOCKET_, 3); }
    socket_no_sigpipe(fd);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    pthread_mutex_lock(&s_lock); int k = 0; while (k < MAXS && S[k].fd) k++;
    if (k < MAXS) { S[k].fd = fd + 1; S[k].nb = 0; }
    pthread_mutex_unlock(&s_lock);
    if (k == MAXS) { close(fd); w32_set_last_error(c, 10024); RET(INVALID_SOCKET_, 3); }
    RET(SBASE + (uint32_t)k, 3);
}
IMPL(wsock32, ord3)                                     /* closesocket(s) */
{
    SOCK(ARG(0), 1);
    pthread_mutex_lock(&s_lock); S[ARG(0) - SBASE].fd = 0; pthread_mutex_unlock(&s_lock);
    close(fd); RET(0, 1);
}
IMPL(wsock32, ord2)                                     /* bind(s, addr, len) */
{
    SOCK(ARG(0), 3); struct sockaddr_in a; if (!to_host(ARG(1), ARG(2), &a)) RET(fail(c, 10014), 3);
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) RET(fail_errno(c), 3);
    RET(0, 3);
}
IMPL(wsock32, ord4)                                     /* connect(s, addr, len) */
{
    SOCK(ARG(0), 3); struct sockaddr_in a; if (!to_host(ARG(1), ARG(2), &a)) RET(fail(c, 10014), 3);
    if (getenv("W32_NETLOG")) { char ip[32]; inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip); fprintf(stderr, "net: connect %x to %s:%u (fd %d)\n", ARG(0), ip, ntohs(a.sin_port), fd); }
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) RET(fail_errno(c), 3);
    RET(0, 3);
}
IMPL(wsock32, ord13) { SOCK(ARG(0), 2); if (listen(fd, (int)ARG(1))) RET(fail_errno(c), 2); RET(0, 2); }   /* listen */
IMPL(wsock32, ord1)                                     /* accept(s, addr, len*) */
{
    SOCK(ARG(0), 3); struct sockaddr_in a; socklen_t l = sizeof a;
    int n = accept(fd, (struct sockaddr *)&a, &l);
    if (n < 0) { w32_set_last_error(c, wsa_err(errno)); RET(INVALID_SOCKET_, 3); }
    socket_no_sigpipe(n); fcntl(n, F_SETFD, FD_CLOEXEC);
    pthread_mutex_lock(&s_lock); int k = 0; while (k < MAXS && S[k].fd) k++;
    if (k < MAXS) { S[k].fd = n + 1; S[k].nb = 0; }
    pthread_mutex_unlock(&s_lock);
    if (k == MAXS) { close(n); w32_set_last_error(c, 10024); RET(INVALID_SOCKET_, 3); }
    fcntl(n, F_SETFL, fcntl(n, F_GETFL) & ~O_NONBLOCK);  /* Windows: the new socket starts blocking */
    to_guest(&a, ARG(1), ARG(2));
    RET(SBASE + (uint32_t)k, 3);
}
IMPL(wsock32, ord19)                                    /* send(s, buf, len, flags) */
{
    SOCK(ARG(0), 4); ssize_t n = send(fd, GP(ARG(1)), ARG(2), (int)(ARG(3) & 1) | MSG_NOSIGNAL);
    if (n < 0) RET(fail_errno(c), 4);
    netlog("send", ARG(0), GP(ARG(1)), n);
    RET((uint32_t)n, 4);
}
IMPL(wsock32, ord16)                                    /* recv(s, buf, len, flags) */
{
    SOCK(ARG(0), 4); ssize_t n = recv(fd, GP(ARG(1)), ARG(2), (int)(ARG(3) & 3));
    if (n < 0) RET(fail_errno(c), 4);
    netlog("recv", ARG(0), GP(ARG(1)), n);
    RET((uint32_t)n, 4);
}
IMPL(wsock32, ord20)                                    /* sendto(s, buf, len, flags, to, tolen) */
{
    SOCK(ARG(0), 6); struct sockaddr_in a; ssize_t n;
    if (ARG(4)) { if (!to_host(ARG(4), ARG(5), &a)) RET(fail(c, 10014), 6); n = sendto(fd, GP(ARG(1)), ARG(2), (int)(ARG(3) & 1) | MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a); }
    else n = send(fd, GP(ARG(1)), ARG(2), (int)(ARG(3) & 1) | MSG_NOSIGNAL);
    if (n < 0) RET(fail_errno(c), 6);
    RET((uint32_t)n, 6);
}
IMPL(wsock32, ord17)                                    /* recvfrom(s, buf, len, flags, from, fromlen*) */
{
    SOCK(ARG(0), 6); struct sockaddr_in a; socklen_t l = sizeof a; memset(&a, 0, sizeof a);
    ssize_t n = recvfrom(fd, GP(ARG(1)), ARG(2), (int)(ARG(3) & 3), (struct sockaddr *)&a, &l);
    if (n < 0) RET(fail_errno(c), 6);
    if (ARG(4)) to_guest(&a, ARG(4), ARG(5));
    RET((uint32_t)n, 6);
}
IMPL(wsock32, ord22) { SOCK(ARG(0), 2); if (shutdown(fd, (int)ARG(1))) RET(fail_errno(c), 2); RET(0, 2); }  /* shutdown */
IMPL(wsock32, ord6)                                     /* getsockname(s, addr, len*) */
{
    SOCK(ARG(0), 3); struct sockaddr_in a; socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &l)) RET(fail_errno(c), 3);
    to_guest(&a, ARG(1), ARG(2)); RET(0, 3);
}
IMPL(wsock32, ord5)                                     /* getpeername(s, addr, len*) */
{
    SOCK(ARG(0), 3); struct sockaddr_in a; socklen_t l = sizeof a;
    if (getpeername(fd, (struct sockaddr *)&a, &l)) RET(fail_errno(c), 3);
    to_guest(&a, ARG(1), ARG(2)); RET(0, 3);
}
/* WinSock option numbers follow Windows/BSD, not Linux. Translate explicitly;
 * linger uses two u16s and timeouts use milliseconds on the guest side. */
static int socket_option(uint32_t level, uint32_t name, int *host_level)
{
    if (level == 6 && name == 1) { *host_level = IPPROTO_TCP; return TCP_NODELAY; }
    if (level != 0xffff) return -1;
    *host_level = SOL_SOCKET;
    switch (name) {
    case 1: return SO_DEBUG; case 2: return SO_ACCEPTCONN;
    case 4: return SO_REUSEADDR; case 8: return SO_KEEPALIVE;
    case 0x10: return SO_DONTROUTE; case 0x20: return SO_BROADCAST;
    case 0x80: case 0xff7f: return SO_LINGER; case 0x100: return SO_OOBINLINE;
    case 0x1001: return SO_SNDBUF; case 0x1002: return SO_RCVBUF;
    case 0x1003: return SO_SNDLOWAT; case 0x1004: return SO_RCVLOWAT;
    case 0x1005: return SO_SNDTIMEO; case 0x1006: return SO_RCVTIMEO;
    case 0x1007: return SO_ERROR; case 0x1008: return SO_TYPE;
    default: return -1;
    }
}
IMPL(wsock32, ord21)                                    /* setsockopt(s, level, name, value, len) */
{
    SOCK(ARG(0), 5); uint32_t lvl = ARG(1), nm = ARG(2), v = ARG(3), len = ARG(4);
    if (!v || len < 4) RET(fail(c, 10014), 5);
    int hl = 0, opt = socket_option(lvl, nm, &hl);
    if (opt < 0) RET(fail(c, 10042), 5);
    int iv = (int)rt_r32(G_MEM, v), rc;
    if (hl == SOL_SOCKET && opt == SO_LINGER) {
        struct linger lg = nm == 0xff7f ? (struct linger){!iv, 0} : (struct linger){rt_r16(G_MEM, v), rt_r16(G_MEM, v + 2)};
        rc = setsockopt(fd, hl, opt, &lg, sizeof lg);
    } else if (hl == SOL_SOCKET && (opt == SO_SNDTIMEO || opt == SO_RCVTIMEO)) {
        if (iv < 0) RET(fail(c, 10022), 5);
        struct timeval tv = {iv / 1000, (iv % 1000) * 1000};
        rc = setsockopt(fd, hl, opt, &tv, sizeof tv);
    } else rc = setsockopt(fd, hl, opt, &iv, sizeof iv);
    if (rc) RET(fail_errno(c), 5);
    RET(0, 5);
}
IMPL(wsock32, ord7)                                     /* getsockopt(s, level, name, value, len*) */
{
    SOCK(ARG(0), 5); uint32_t lvl = ARG(1), nm = ARG(2), v = ARG(3), lp = ARG(4);
    if (!v || !lp || rt_r32(G_MEM, lp) < 4) RET(fail(c, 10014), 5);
    int hl = 0, opt = socket_option(lvl, nm, &hl), iv = 0, rc;
    if (opt < 0) RET(fail(c, 10042), 5);
    socklen_t l;
    if (hl == SOL_SOCKET && opt == SO_LINGER) {
        struct linger lg = {0}; l = sizeof lg;
        rc = getsockopt(fd, hl, opt, &lg, &l);
        iv = nm == 0xff7f ? !lg.l_onoff : (lg.l_onoff & 65535) | ((uint32_t)lg.l_linger << 16);
    } else if (hl == SOL_SOCKET && (opt == SO_SNDTIMEO || opt == SO_RCVTIMEO)) {
        struct timeval tv = {0}; l = sizeof tv;
        rc = getsockopt(fd, hl, opt, &tv, &l);
        iv = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    } else { l = sizeof iv; rc = getsockopt(fd, hl, opt, &iv, &l); }
    if (rc) RET(fail_errno(c), 5);
    if (hl == SOL_SOCKET && opt == SO_ERROR) iv = iv ? (int)wsa_err(iv) : 0;
    rt_w32(G_MEM, v, (uint32_t)iv); rt_w32(G_MEM, lp, 4);
    RET(0, 5);
}
IMPL(wsock32, ord12)                                    /* ioctlsocket(s, cmd, u_long*) */
{
    SOCK(ARG(0), 3); uint32_t cmd = ARG(1), p = ARG(2);
    if (cmd == 0x8004667eu) {                           /* FIONBIO */
        int on = rt_r32(G_MEM, p) != 0, fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK); RET(0, 3);
    }
    if (cmd == 0x4004667fu) { int n = 0; if (ioctl(fd, FIONREAD, &n)) RET(fail_errno(c), 3); rt_w32(G_MEM, p, (uint32_t)n); RET(0, 3); }   /* FIONREAD */
    RET(fail(c, 10022), 3);
}
/* select(nfds, read, write, except, timeout): a Windows fd_set is {u32 count; SOCKET array[64]}. One poll entry per
 * socket with the events of every set it is in (macOS's poll misreports a descriptor listed twice). */
IMPL(wsock32, ord18)
{
    uint32_t sets[3] = {ARG(1), ARG(2), ARG(3)}, tv = ARG(4);
    struct pollfd pf[192]; uint32_t who[192]; int n = 0;
    static const short ev[3] = {POLLIN, POLLOUT, POLLPRI};
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        uint32_t cnt = rt_r32(G_MEM, sets[k]); if (cnt > 64) cnt = 64;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t s = rt_r32(G_MEM, sets[k] + 4 + 4 * i); int fd = sfd(s);
            if (fd < 0) RET(fail(c, 10038), 5);
            int j = 0; while (j < n && who[j] != s) j++;
            if (j == n) { pf[n].fd = fd; pf[n].events = 0; pf[n].revents = 0; who[n] = s; n++; }
            pf[j].events |= ev[k];
        }
    }
    int ms = tv ? (int)(rt_r32(G_MEM, tv) * 1000 + rt_r32(G_MEM, tv + 4) / 1000) : -1;
    int r = poll(pf, (nfds_t)n, ms);
    if (r < 0) RET(fail_errno(c), 5);
    if (getenv("W32_NETLOG") && atoi(getenv("W32_NETLOG")) > 1) for (int i = 0; i < n; i++) fprintf(stderr, "net: select %x events %x revents %x\n", who[i], pf[i].events, pf[i].revents);
    uint32_t total = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        uint32_t cnt = rt_r32(G_MEM, sets[k]), out = 0; if (cnt > 64) cnt = 64;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t s = rt_r32(G_MEM, sets[k] + 4 + 4 * i); int j = 0; while (j < n && who[j] != s) j++;
            short re = j < n ? pf[j].revents : 0;
            int hit = k == 0 ? (re & (POLLIN | POLLHUP | POLLERR)) != 0 : k == 1 ? (re & POLLOUT) != 0 : (re & (POLLPRI | POLLERR)) != 0;
            if (hit) { rt_w32(G_MEM, sets[k] + 4 + 4 * out, s); out++; total++; }
        }
        rt_w32(G_MEM, sets[k], out);
    }
    RET(total, 5);
}
IMPL(wsock32, ord151)                                   /* __WSAFDIsSet(s, set) */
{
    uint32_t s = ARG(0), set = ARG(1), cnt = rt_r32(G_MEM, set); if (cnt > 64) cnt = 64;
    for (uint32_t i = 0; i < cnt; i++) if (rt_r32(G_MEM, set + 4 + 4 * i) == s) RET(1, 2);
    RET(0, 2);
}
IMPL(wsock32, ord111) { RET(rt_r32(G_MEM, c->fs_base + 0x34), 0); }   /* WSAGetLastError */
IMPL(wsock32, ord8) { RET(__builtin_bswap32(ARG(0)), 1); }             /* htonl */
IMPL(wsock32, ord9) { RET(__builtin_bswap16((uint16_t)ARG(0)), 1); }   /* htons */
IMPL(wsock32, ord15) { RET(__builtin_bswap16((uint16_t)ARG(0)), 1); }  /* ntohs */
IMPL(wsock32, ord11)                                    /* inet_ntoa(in_addr): a per-thread buffer, as on Windows */
{
    static __thread uint32_t buf; if (!buf) buf = heap_alloc(w32_process_heap, 8, 16);
    uint32_t a = ARG(0); snprintf((char *)GP(buf), 16, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
    RET(buf, 1);
}
