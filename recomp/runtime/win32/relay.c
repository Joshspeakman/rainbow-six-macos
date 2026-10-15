/* Internet co-op through r6relay (recomp/relay/r6relay.py; the protocol is in docs/R6-RELAY.md). Every connection is
 * outgoing, so no player opens a port, a firewall rule or a router mapping, and the game never listens on a real
 * network interface. On with R6_RELAY=<relay host[:port]> and R6_ROOM=host|<room code> (the launcher sets them):
 *   every guest bind() is to 127.0.0.1 (relay_bind_loopback), so nothing is reachable from outside this machine;
 *   host:   when the game listens on its join port (R6_RELAY_GAME_PORT, 2346), a thread dials the relay and gets a
 *           room code (a listen on any other port is left alone); each joiner the
 *           relay announces becomes one local TCP connection to the game's port (from 127.0.0.<1 + joiner> where the
 *           host allows it, so the game sees joiners as different addresses), and its bytes are carried both ways;
 *   joiner: a thread listens on 127.0.0.1:R6_RELAY_LOCAL_PORT (2356) from the first WSAStartup; the game joins that
 *           address (MultiplayerManualHostAddress/JoinPort; a connect to the join port elsewhere is sent there too), and each
 *           connection it makes is carried to the room's host.
 * R6_RELAY_CODE (host: the room code to ask for, chosen by the launcher so it can show it before the game starts;
 * without it the relay picks one), R6_RELAY_NAME (host's name), R6_RELAY_BUILD (the first 16 hex digits of the game executable's SHA-256: the relay
 * keeps different builds apart) and R6_RELAY_STATUS (a file rewritten with key=value lines: state, room, players,
 * message) come from the launcher too.
 * The pumps never block on a send: each connection has a queue, and a side whose queue is past HIGH is not read until
 * it drains (one thread carries both directions, so a blocking send could wait on a peer that waits on us). */
#include "net_host.h"
#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum { R_HELLO = 1, R_ROOM, R_OPEN, R_DATA, R_CLOSE, R_PING, R_PONG, R_ERROR };
enum { MAX_PAYLOAD = 4096, PEERS = 15, PING_S = 15, IDLE_S = 60, DIAL_MS = 10000, TICK_MS = 200, HIGH = 256 << 10 };

static const char *cfg(const char *n) { const char *v = getenv(n); return v && *v ? v : 0; }
int relay_on(void) { static int on = -1; if (on < 0) on = cfg("R6_RELAY") && cfg("R6_ROOM"); return on; }
static int hosting(void) { return relay_on() && !strcasecmp(cfg("R6_ROOM"), "host"); }
static time_t now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec; }
#ifdef _WIN32
static void nap(int ms) { Sleep((DWORD)ms); }
#else
static void nap(int ms) { struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&ts, 0); }
#endif
static uint16_t port_cfg(const char *n, int dflt) { const char *v = cfg(n); int p = v ? atoi(v) : 0; return (uint16_t)(p > 0 && p < 65536 ? p : dflt); }
/* the host game's join port (MultiplayerGameJoinPort); a joiner's local listener: not 2346, which the joining game
 * itself may also bind */
static uint16_t game_port(void) { return port_cfg("R6_RELAY_GAME_PORT", 2346); }
static uint16_t local_port(void) { return port_cfg("R6_RELAY_LOCAL_PORT", 2356); }

/* the launcher's view: R6_RELAY_STATUS rewritten whole (key=value lines), and the same on stderr */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static void status(const char *fmt, ...)
{
    char s[512]; va_list ap; va_start(ap, fmt); vsnprintf(s, sizeof s, fmt, ap); va_end(ap);
    pthread_mutex_lock(&st_lock);
    fprintf(stderr, "relay: %s", s);
    const char *path = cfg("R6_RELAY_STATUS");
    if (path) { char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "w");
                if (f) { fputs(s, f); fclose(f);
#ifdef _WIN32
                         remove(path);                          /* rename replaces atomically elsewhere, not here */
#endif
                         rename(tmp, path); } }
    pthread_mutex_unlock(&st_lock);
}

/* ---- sockets and frames ---- */
static void nodelay(net_sock s) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
static int would_block(void) { int e = net_errno(); return e == NET_EWOULDBLOCK || e == EAGAIN || e == EINTR; }
/* blocking: the handshake, before a connection is handed to its pump */
static int send_all(net_sock s, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) { long k = send(s, b, n, MSG_NOSIGNAL); if (k <= 0) return -1; b += k; n -= (size_t)k; }
    return 0;
}
static int recv_all(net_sock s, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) { long k = recv(s, b, n, 0); if (k <= 0) return -1; b += k; n -= (size_t)k; }
    return 0;
}
static int readable(net_sock s, int ms)
{
    struct pollfd p = {0}; p.fd = s; p.events = POLLIN;
    return net_poll(&p, 1, ms) > 0;
}
/* send and receive time limits on a blocking socket (the handshake: a relay that stalls mid-frame can't hang us); 0: none */
static void time_limit(net_sock s, int ms)
{
#ifdef _WIN32
    DWORD v = (DWORD)ms;
#else
    struct timeval v = {ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &v, sizeof v); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &v, sizeof v);
}
static void put_header(uint8_t *h, int kind, int peer, size_t n) { h[0] = (uint8_t)kind; h[1] = (uint8_t)peer; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; }
static int send_frame(net_sock s, int kind, int peer, const void *p, size_t n)   /* n <= MAX_PAYLOAD */
{
    uint8_t f[4 + MAX_PAYLOAD]; put_header(f, kind, peer, n); if (n) memcpy(f + 4, p, n);
    return send_all(s, f, 4 + n);
}
static int read_frame(net_sock s, uint8_t *kind, uint8_t *peer, uint8_t *buf, size_t *n)
{
    uint8_t h[4]; if (recv_all(s, h, 4)) return -1;
    *kind = h[0]; *peer = h[1]; *n = (size_t)h[2] << 8 | h[3];
    if (*n > MAX_PAYLOAD) return -1;
    return *n ? recv_all(s, buf, *n) : 0;
}

/* a growable byte queue: what is waiting to be sent on a connection, or received bytes not yet a whole frame */
typedef struct { uint8_t *b; size_t n, cap; } Buf;
static int buf_push(Buf *q, const void *p, size_t n)
{
    if (q->n + n > q->cap) {
        size_t c = q->cap ? q->cap : 8192; while (c < q->n + n) c *= 2;
        uint8_t *nb = realloc(q->b, c); if (!nb) return -1;
        q->b = nb; q->cap = c;
    }
    if (n) memcpy(q->b + q->n, p, n);
    q->n += n; return 0;
}
static void buf_drop(Buf *q, size_t k) { memmove(q->b, q->b + k, q->n - k); q->n -= k; }
static void buf_free(Buf *q) { free(q->b); memset(q, 0, sizeof *q); }
static int queue_frame(Buf *q, int kind, int peer, const void *p, size_t n)
{
    const uint8_t *b = p; uint8_t h[4];
    do {                                                        /* larger payloads in MAX_PAYLOAD pieces; an empty one once */
        size_t k = n < MAX_PAYLOAD ? n : MAX_PAYLOAD;
        put_header(h, kind, peer, k);
        if (buf_push(q, h, 4) || buf_push(q, b, k)) return -1;
        b += k; n -= k;
    } while (n);
    return 0;
}
/* send what the (non-blocking) socket takes now; -1: the connection failed */
static int buf_flush(net_sock s, Buf *q)
{
    while (q->n) {
        long k = send(s, q->b, q->n, MSG_NOSIGNAL);
        if (k > 0) { buf_drop(q, (size_t)k); continue; }
        return k < 0 && would_block() ? 0 : -1;
    }
    return 0;
}
/* receive what is there into q; -1: closed or failed */
static int buf_fill(net_sock s, Buf *q)
{
    uint8_t tmp[16384]; long k = recv(s, tmp, sizeof tmp, 0);
    if (k > 0) return buf_push(q, tmp, (size_t)k);
    return k < 0 && would_block() ? 0 : -1;
}

/* the relay: "host[:port]" (2346 by default), a blocking TCP connection with TCP_NODELAY, or NET_BAD and why */
static net_sock dial(char *why, size_t cap)
{
    char host[256], port[16] = "2346"; snprintf(host, sizeof host, "%s", cfg("R6_RELAY"));
    char *colon = strrchr(host, ':'); if (colon) { snprintf(port, sizeof port, "%s", colon + 1); *colon = 0; }
    net_startup();
    struct addrinfo hint = {0}, *res = 0; hint.ai_family = AF_INET; hint.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hint, &res) || !res) { snprintf(why, cap, "can't find the relay %s", host); return NET_BAD; }
    net_sock s = NET_BAD;
    for (struct addrinfo *a = res; a && s == NET_BAD; a = a->ai_next) {
        s = net_socket(AF_INET, SOCK_STREAM, 0); if (s == NET_BAD) continue;
        net_set_nonblock(s, 1);
        int ok = !connect(s, a->ai_addr, (socklen_t)a->ai_addrlen);
        if (!ok && (net_errno() == NET_EINPROGRESS || net_errno() == NET_EWOULDBLOCK)) {
            fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);   /* select: WSAPoll misses failed connects */
            struct timeval tv = {DIAL_MS / 1000, 0};
            int err = 0; socklen_t el = sizeof err;
            ok = select((int)s + 1, 0, &w, &e, &tv) > 0 && FD_ISSET(s, &w) && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) && !err;
        }
        if (!ok) { net_close(s); s = NET_BAD; }
    }
    freeaddrinfo(res);
    if (s == NET_BAD) { snprintf(why, cap, "can't reach the relay %s:%s", host, port); return NET_BAD; }
    net_set_nonblock(s, 0); nodelay(s); time_limit(s, DIAL_MS);   /* the pump makes it non-blocking (time_limit 0) */
    return s;
}
/* a connection to the game's own port on this machine, from 127.0.0.<1 + peer> where the host allows that */
static net_sock dial_game(uint16_t port, int peer)
{
    net_sock s = net_socket(AF_INET, SOCK_STREAM, 0); if (s == NET_BAD) return s;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET;
#ifndef __APPLE__                                              /* macOS has only 127.0.0.1 without an alias */
    a.sin_addr.s_addr = htonl(0x7f000001u + (uint32_t)peer);
    bind(s, (struct sockaddr *)&a, sizeof a);                    /* if refused, the connection comes from 127.0.0.1 */
#endif
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { net_close(s); return NET_BAD; }
    nodelay(s); net_set_nonblock(s, 1);
    return s;
}
static const char *build_id(void) { const char *b = cfg("R6_RELAY_BUILD"); return b ? b : "unknown"; }

/* ---- the pump: one relay connection and its local connections to the game ----
 * host: slots 1..PEERS are the joiners (frames carry the slot), opened and closed by the relay; joiner: slot 0 is the
 * game's one connection (frames carry 0). Returns NULL when it ended normally (*stop, the joiner's game hung up, or
 * the host closed the joiner), else why. */
typedef struct { net_sock loc[PEERS + 1]; Buf out[PEERS + 1]; int host; uint16_t port; const char *room; } Pump;

static void host_players(Pump *P)
{
    int n = 0; for (int k = 1; k <= PEERS; k++) n += P->loc[k] != NET_BAD;
    status("state=hosting\nroom=%s\nplayers=%d\n", P->room, n);
}
static void pump_close(Pump *P, Buf *rout, int p, int tell)
{
    if (P->loc[p] == NET_BAD) return;
    net_close(P->loc[p]); P->loc[p] = NET_BAD; buf_free(&P->out[p]);
    if (tell) queue_frame(rout, R_CLOSE, p, 0, 0);
    if (P->host) host_players(P);
}
static const char *pump(Pump *P, net_sock r, volatile int *stop)
{
    Buf rin = {0}, rout = {0}; const char *why = 0; int done = 0;
    time_t pinged = now_s(), heard = pinged;
    time_limit(r, 0); net_set_nonblock(r, 1);
    while (!done && !(stop && *stop)) {
        struct pollfd pf[PEERS + 2]; int who[PEERS + 2], np = 0, stuck = 0;
        for (int p = 0; p <= PEERS; p++) if (P->loc[p] != NET_BAD && P->out[p].n > HIGH) stuck = 1;
        short rev = (short)((stuck ? 0 : POLLIN) | (rout.n ? POLLOUT : 0));
        if (rev) { pf[np].fd = r; pf[np].events = rev; pf[np].revents = 0; who[np++] = -1; }
        for (int p = 0; p <= PEERS; p++) {
            if (P->loc[p] == NET_BAD) continue;
            short ev = (short)((rout.n < HIGH ? POLLIN : 0) | (P->out[p].n ? POLLOUT : 0));
            if (ev) { pf[np].fd = P->loc[p]; pf[np].events = ev; pf[np].revents = 0; who[np++] = p; }
        }
        int got = np ? net_poll(pf, (unsigned)np, TICK_MS) : (nap(TICK_MS), 0);
        if (got < 0) { why = "network error"; break; }
        for (int i = 0; i < np && !done; i++) {
            short re = pf[i].revents; int p = who[i];
            if (!re) continue;
            if (p < 0) {                                         /* the relay */
                if ((re & POLLOUT) && buf_flush(r, &rout)) { why = "lost the relay"; done = 1; break; }
                if (!(re & (POLLIN | POLLHUP | POLLERR))) continue;
                if (buf_fill(r, &rin)) { why = "lost the relay"; done = 1; break; }
                heard = now_s();
                while (rin.n >= 4 && !done) {
                    size_t n = (size_t)rin.b[2] << 8 | rin.b[3]; uint8_t kind = rin.b[0], q = rin.b[1];
                    if (n > MAX_PAYLOAD) { why = "bad data from the relay"; done = 1; break; }
                    if (rin.n < 4 + n) break;
                    const uint8_t *body = rin.b + 4; int slot = P->host ? (q >= 1 && q <= PEERS ? q : -1) : 0;
                    if (kind == R_OPEN && P->host && slot > 0) {
                        pump_close(P, &rout, slot, 0);
                        P->loc[slot] = dial_game(P->port, slot);
                        if (P->loc[slot] == NET_BAD) queue_frame(&rout, R_CLOSE, slot, 0, 0);
                        else host_players(P);
                    } else if (kind == R_DATA && slot >= 0 && P->loc[slot] != NET_BAD) {
                        if (buf_push(&P->out[slot], body, n) || buf_flush(P->loc[slot], &P->out[slot])) {
                            if (P->host) pump_close(P, &rout, slot, 1); else { done = 1; }
                        }
                    } else if (kind == R_CLOSE && slot >= 0) {
                        if (P->host) pump_close(P, &rout, slot, 0); else done = 1;   /* joiner: the host closed it */
                    } else if (kind == R_PING) queue_frame(&rout, R_PONG, q, body, n);
                    else if (kind == R_ERROR) { static __thread char msg[MAX_PAYLOAD + 1]; snprintf(msg, sizeof msg, "%.*s", (int)n, (const char *)body); why = msg; done = 1; }
                    buf_drop(&rin, 4 + n);
                }
            } else {                                             /* the game, slot p */
                if (P->loc[p] == NET_BAD || P->loc[p] != pf[i].fd) continue;   /* closed (or reopened) above */
                if ((re & POLLOUT) && buf_flush(P->loc[p], &P->out[p])) { if (P->host) pump_close(P, &rout, p, 1); else done = 1; continue; }
                if (!(re & (POLLIN | POLLHUP | POLLERR))) continue;
                uint8_t tmp[MAX_PAYLOAD]; long k = recv(P->loc[p], tmp, sizeof tmp, 0);
                if (k > 0) queue_frame(&rout, R_DATA, p, tmp, (size_t)k);
                else if (k == 0 || !would_block()) {               /* the game hung up */
                    if (P->host) pump_close(P, &rout, p, 1);
                    else { queue_frame(&rout, R_CLOSE, 0, 0, 0); done = 1; }
                }
            }
        }
        time_t t = now_s();
        if (!done && t - pinged >= PING_S) { pinged = t; queue_frame(&rout, R_PING, 0, 0, 0); }
        if (!done && t - heard > IDLE_S) { why = "the relay stopped answering"; done = 1; }
        if (buf_flush(r, &rout) && !why) { why = "lost the relay"; done = 1; }
    }
    for (int k = 0; !why && rout.n && k < 5; k++) {              /* a last CLOSE, for at most 0.5 s: the game thread may */
        struct pollfd p = {0}; p.fd = r; p.events = POLLOUT;      /* be waiting on this (relay_closed) */
        if (net_poll(&p, 1, 100) < 0 || buf_flush(r, &rout)) break;
    }
    buf_free(&rin); buf_free(&rout);
    for (int p = 0; p <= PEERS; p++) { if (P->loc[p] != NET_BAD) net_close(P->loc[p]); P->loc[p] = NET_BAD; buf_free(&P->out[p]); }
    return why;
}

/* ---- host: one thread per hosted game, from the game's listen() to its closesocket() ---- */
static struct { pthread_mutex_t m; pthread_t th; int running; volatile int stop; uint32_t guest; uint16_t port; } H = {.m = PTHREAD_MUTEX_INITIALIZER};

static void *host_main(void *arg)
{
    (void)arg; char why[300]; uint8_t kind = 0, peer, buf[MAX_PAYLOAD]; size_t n = 0;
    status("state=connecting\n");
    net_sock r = dial(why, sizeof why);
    if (r == NET_BAD) { status("state=error\nmessage=%s\n", why); return 0; }
    char hello[200]; snprintf(hello, sizeof hello, "R6R1 host %s %s %s", build_id(), cfg("R6_RELAY_CODE") ? cfg("R6_RELAY_CODE") : "*",
                              cfg("R6_RELAY_NAME") ? cfg("R6_RELAY_NAME") : "player");
    if (send_frame(r, R_HELLO, 0, hello, strlen(hello)) || !readable(r, DIAL_MS) || read_frame(r, &kind, &peer, buf, &n) || kind != R_ROOM) {
        if (kind == R_ERROR) status("state=error\nmessage=%.*s\n", (int)n, (char *)buf);
        else status("state=error\nmessage=the relay did not answer\n");
        net_close(r); return 0;
    }
    char room[16]; snprintf(room, sizeof room, "%.*s", (int)(n < 15 ? n : 15), (char *)buf);
    status("state=hosting\nroom=%s\nplayers=0\n", room);
    Pump P; memset(&P, 0, sizeof P); for (int p = 0; p <= PEERS; p++) P.loc[p] = NET_BAD;
    P.host = 1; P.port = H.port; P.room = room;
    const char *end = pump(&P, r, &H.stop);
    net_close(r);
    if (H.stop) status("state=idle\n");
    else status("state=error\nroom=%s\nmessage=%s\n", room, end ? end : "lost the relay");
    return 0;
}
/* winsock.c: the game listens on guest socket s, bound to port */
void relay_listen(uint32_t s, uint16_t port)
{
    if (!hosting()) return;
    if (port != game_port()) { fprintf(stderr, "relay: the game listens on port %u, not relayed\n", port); return; }
    pthread_mutex_lock(&H.m);
    if (H.running && H.guest != s) fprintf(stderr, "relay: the game listens on port %u again, not relayed\n", port);
    if (!H.running) {
        H.guest = s; H.port = port; H.stop = 0;
        H.running = !pthread_create(&H.th, 0, host_main, 0);
    }
    pthread_mutex_unlock(&H.m);
}
/* winsock.c: guest socket s is closed (hosting ends with the listening socket) */
void relay_closed(uint32_t s)
{
    if (!hosting()) return;
    pthread_mutex_lock(&H.m);
    if (H.running && H.guest == s) { H.stop = 1; pthread_join(H.th, 0); H.running = 0; H.guest = 0; }
    pthread_mutex_unlock(&H.m);
}

/* ---- joiner: a loopback listener; each connection the game makes is one join to the room ---- */
static void *join_pump(void *arg)
{
    net_sock loc = (net_sock)(intptr_t)arg; char why[300]; uint8_t kind = 0, peer, buf[MAX_PAYLOAD]; size_t n = 0;
    const char *room = cfg("R6_ROOM");
    status("state=connecting\nroom=%s\n", room);
    net_sock r = dial(why, sizeof why);
    if (r == NET_BAD) { status("state=error\nroom=%s\nmessage=%s\n", room, why); net_close(loc); return 0; }
    char hello[200]; snprintf(hello, sizeof hello, "R6R1 join %s %s", room, build_id());
    if (send_frame(r, R_HELLO, 0, hello, strlen(hello)) || !readable(r, DIAL_MS) || read_frame(r, &kind, &peer, buf, &n) || kind != R_OPEN) {
        if (kind == R_ERROR) status("state=error\nroom=%s\nmessage=%.*s\n", room, (int)n, (char *)buf);
        else status("state=error\nroom=%s\nmessage=the relay did not answer\n", room);
        net_close(r); net_close(loc); return 0;
    }
    status("state=joined\nroom=%s\n", room);
    Pump P; memset(&P, 0, sizeof P); for (int p = 0; p <= PEERS; p++) P.loc[p] = NET_BAD;
    P.loc[0] = loc; net_set_nonblock(loc, 1); P.room = room;
    const char *end = pump(&P, r, 0);
    net_close(r);
    if (end) status("state=error\nroom=%s\nmessage=%s\n", room, end);
    else status("state=ready\nroom=%s\n", room);
    return 0;
}
static void *join_main(void *arg)
{
    (void)arg; uint16_t port = local_port();
    net_sock l = net_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
#ifndef _WIN32                                                 /* on Windows it would let a second listener share the port */
    int one = 1; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (l == NET_BAD || bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 4)) {
        status("state=error\nroom=%s\nmessage=port %u on this machine is in use\n", cfg("R6_ROOM"), port);
        if (l != NET_BAD) net_close(l);
        return 0;
    }
    status("state=ready\nroom=%s\n", cfg("R6_ROOM"));
    for (;;) {
        net_sock c = accept(l, 0, 0);
        if (c == NET_BAD) { nap(100); continue; }               /* out of descriptors: don't spin */
        nodelay(c); net_cloexec(c);
        pthread_t th; if (pthread_create(&th, 0, join_pump, (void *)(intptr_t)c)) net_close(c); else pthread_detach(th);
    }
}
/* net_ole.c: WSAStartup; a joiner's listener starts once */
static void join_start(void) { pthread_t th; if (!pthread_create(&th, 0, join_main, 0)) pthread_detach(th); }
void relay_startup(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (relay_on() && !hosting()) pthread_once(&once, join_start);
}
/* winsock.c: a joiner's game connecting to a host's join port elsewhere (an address typed by hand) reaches the room
 * through the local listener instead */
void relay_connect_target(struct sockaddr_in *a)
{
    if (!relay_on() || hosting() || (ntohl(a->sin_addr.s_addr) >> 24) == 127 || ntohs(a->sin_port) != game_port()) return;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); a->sin_port = htons(local_port());
}
/* winsock.c: with the relay on, a bind to any address but loopback is made to 127.0.0.1 (the port is kept) */
void relay_bind_loopback(struct sockaddr_in *a)
{
    if (!relay_on() || (ntohl(a->sin_addr.s_addr) >> 24) == 127) return;
    static int logged; if (!logged++) fprintf(stderr, "relay: binding the game's sockets to 127.0.0.1\n");
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}
