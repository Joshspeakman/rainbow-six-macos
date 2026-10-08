/* dp8fuzz: hostile input for the DirectPlay 8 transport (runtime/win32/dp8proto.c), loopback only (every bind of the
 * transport is forced to 127.0.0.1 through -Dbind=fz_bind).
 *   dp8fuzz [seconds]        random buffers through the frame decoders, then a listening endpoint under a flood of
 *                            random, mutated and truncated frames, CONNECTs, SACKs, disconnects and enumeration packets
 *                            from 8 sockets while a well-behaved client sends (NOGOOD=1: no client). Watch the process's
 *                            memory: it must stay flat (closed connections are freed, half-open ones capped).
 *   dp8fuzz join <port>      against a game hosting on 127.0.0.1:<port> (DS_NO_PORTMAP=1 W32_DPLOG=1): connect, then send
 *                            ACK_CONNECT_INFO and REQ_PROCESS_COMPLETION without CONNECT_INFO. The host must not create
 *                            a player (no "accepted player" / message 0xffff0007 in its log).
 * Build (from recomp/), with sanitizers:
 *   clang -fsanitize=address,undefined -g -O1 -Dbind=fz_bind -I runtime/win32 -c runtime/win32/dp8proto.c -o /tmp/dp8p.o
 *   clang -fsanitize=address,undefined -g -O1 -I runtime/win32 tests/dp8fuzz.c /tmp/dp8p.o -o /tmp/dp8fuzz
 * (without the sanitizers: `MallocStackLogging=1 leaks --atExit -- /tmp/dp8fuzz 40` for leaks) */
#include "dp8proto.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdatomic.h>

int fz_bind(int fd, const struct sockaddr *a, socklen_t l)
{
    struct sockaddr_in s; memcpy(&s, a, sizeof s); s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(fd, (struct sockaddr *)&s, l);
}
static atomic_long n_acc, n_rx, n_rx_bytes, n_closed, n_conn, n_enumq;
static void cb_enumq(void *x, dp8_ep *ep, const struct sockaddr_in *f, const uint8_t *m, size_t len) { (void)x; (void)ep; (void)f; volatile uint8_t s = 0; for (size_t i = 0; i < len; i++) s += m[i]; n_enumq++; }
static void cb_enumr(void *x, dp8_ep *ep, const struct sockaddr_in *f, const uint8_t *m, size_t len) { (void)x; (void)ep; (void)f; (void)m; (void)len; }
static void cb_acc(void *x, dp8_conn *c) { (void)x; (void)c; n_acc++; }
static void cb_conn(void *x, dp8_conn *c) { (void)x; (void)c; n_conn++; }
static void cb_rx(void *x, dp8_conn *c, uint8_t u, const uint8_t *d, size_t len) { (void)x; (void)c; (void)u; volatile uint8_t s = 0; for (size_t i = 0; i < len; i++) s += d[i]; n_rx++; n_rx_bytes += (long)len; }
static void cb_closed(void *x, dp8_conn *c, int r) { (void)x; (void)c; (void)r; n_closed++; }

static void sink(void *ctx, uint8_t u, const uint8_t *d, size_t len) { (void)ctx; (void)u; volatile uint8_t s = 0; for (size_t i = 0; i < len; i++) s += d[i]; }

static atomic_int joined;
static void cb_joined(void *x, dp8_conn *c) { (void)x; (void)c; joined = 1; }
static int attack_join(uint16_t port)
{
    dp8_callbacks cb = {0, cb_enumq, cb_enumr, cb_acc, cb_joined, cb_rx, cb_closed};
    dp8_ep *ep = dp8_open(41200, 41250, 0, 0, &cb); if (!ep) return 1;
    struct sockaddr_in to = {0}; to.sin_len = sizeof to; to.sin_family = AF_INET; to.sin_port = htons(port); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dp8_conn *c = dp8_connect(ep, &to);
    for (int i = 0; i < 100 && !joined; i++) usleep(50000);
    if (!joined) { fprintf(stderr, "join: no transport connection to port %u\n", port); return 1; }
    uint8_t ack[4] = {0xC3, 0, 0, 0}, req[16] = {0xE0, 0, 0, 0, 1, 0, 0, 0, 'h', 'i'};
    dp8_send(c, ack, sizeof ack, DP8_USER_1); dp8_send(c, req, sizeof req, DP8_USER_1);
    fprintf(stderr, "join: sent ACK_CONNECT_INFO and REQ_PROCESS_COMPLETION without CONNECT_INFO; check the host's log\n");
    sleep(3); dp8_disconnect(c, 0); sleep(1); dp8_close(ep); return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "join")) return attack_join((uint16_t)atoi(argv[2]));
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    /* 1: the decoders on random input */
    uint8_t buf[2048];
    for (long it = 0; it < 3000000; it++) {
        size_t len = arc4random_uniform(1500); arc4random_buf(buf, len);
        if (it & 1) buf[0] |= 0x01;
        dp8_dframe f; dp8_dec_dframe(buf, len, &f);
        dp8_dec_coalesced(buf, len, sink, 0);
    }
    fprintf(stderr, "decoders: 3M random buffers ok\n");
    /* 2: a live endpoint under attack, with a legitimate client */
    dp8_callbacks cb = {0, cb_enumq, cb_enumr, cb_acc, cb_conn, cb_rx, cb_closed};
    dp8_ep *srv = dp8_open(41000, 41050, 1, 41999, &cb); if (!srv) { fprintf(stderr, "open failed\n"); return 1; }
    uint16_t port = dp8_port(srv);
    struct sockaddr_in to = {0}; to.sin_len = sizeof to; to.sin_family = AF_INET; to.sin_port = htons(port); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct sockaddr_in toe = to; toe.sin_port = htons(41999);
    dp8_ep *cli = dp8_open(41100, 41150, 0, 0, &cb);
    dp8_conn *good = cli ? dp8_connect(cli, &to) : 0;
    enum { NS = 8 }; int fds[NS]; uint32_t sess[NS]; uint8_t seq[NS];
    for (int k = 0; k < NS; k++) {
        fds[k] = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a = {0}; a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fds[k], (struct sockaddr *)&a, sizeof a); sess[k] = arc4random(); seq[k] = 0;
    }
    time_t end = time(0) + secs; long sent = 0; static uint8_t msg[200000];
    while (time(0) < end) {
        int k = (int)arc4random_uniform(NS); uint8_t p[1600]; size_t n = 0; uint32_t r = arc4random_uniform(100);
        if (r < 10) { n = arc4random_uniform(1500); arc4random_buf(p, n); }
        else if (r < 20) { n = dp8_enc_cframe(p, 0x88, 1, (uint8_t)arc4random(), 0, 0x00010005u, sess[k], arc4random()); }   /* CONNECT */
        else if (r < 30) { n = dp8_enc_cframe(p, 0x80, 2, (uint8_t)arc4random(), 0, 0x00010005u, sess[k], arc4random()); }   /* CONNECTED */
        else if (r < 70) {                                                                                                /* data frames */
            uint8_t cmd = (uint8_t)(arc4random() | 1); if (arc4random_uniform(2)) cmd = 0x01 | 0x02 | 0x04 | 0x10 | 0x20 | (arc4random_uniform(2) ? 0x08 : 0);
            uint8_t ctl = (uint8_t)arc4random(); uint64_t sack = ((uint64_t)arc4random() << 32) | arc4random();
            n = dp8_enc_dframe(p, cmd, ctl, arc4random_uniform(4) ? seq[k]++ : (uint8_t)arc4random(), (uint8_t)arc4random(), sack);
            size_t pl = arc4random_uniform(1300); if (n + pl > sizeof p) pl = sizeof p - n; arc4random_buf(p + n, pl); n += pl;
            if (ctl & 0x04 && arc4random_uniform(2)) { /* coalesced: plausible sub-headers */ for (size_t i = n - pl; i + 4 < n; i += 4 + arc4random_uniform(40)) { p[i] = (uint8_t)arc4random_uniform(60); p[i + 1] = 0; } }
            if (arc4random_uniform(5) == 0 && n) n = arc4random_uniform((uint32_t)n);          /* truncated */
        } else if (r < 85) { n = dp8_enc_sack(p, (uint8_t)arc4random(), (uint8_t)arc4random(), (uint8_t)arc4random(), arc4random(), ((uint64_t)arc4random() << 32) | arc4random()); if (arc4random_uniform(3) == 0) n = arc4random_uniform((uint32_t)n + 1); }
        else if (r < 90) { n = dp8_enc_cframe(p, 0x80, 4, 0, 0, 0x00010005u, sess[k], 0); }                                    /* HARD_DISCONNECT */
        else { n = 4 + arc4random_uniform(600); arc4random_buf(p, n); p[0] = 0; p[1] = (uint8_t)(2 + arc4random_uniform(2)); }  /* enumeration */
        if (n && arc4random_uniform(6) == 0) p[arc4random_uniform((uint32_t)n)] ^= (uint8_t)(1u << arc4random_uniform(8));
        sendto(fds[k], p, n, 0, (struct sockaddr *)(r >= 90 && arc4random_uniform(2) ? &toe : &to), sizeof to); sent++;
        if (arc4random_uniform(2000) == 0) { close(fds[k]); fds[k] = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in a = {0}; a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); bind(fds[k], (struct sockaddr *)&a, sizeof a); sess[k] = arc4random(); seq[k] = 0; }
        if (good && !getenv("NOGOOD") && sent % 50 == 0) { size_t ml = arc4random_uniform(arc4random_uniform(10) ? 3000 : sizeof msg); arc4random_buf(msg, ml); if (ml) dp8_send(good, msg, ml, 0); }
        if (sent % 20 == 0) usleep(200);
    }
    fprintf(stderr, "live: sent %ld hostile datagrams; accepted %ld, received %ld (%ld bytes), closed %ld, connected %ld, enum %ld\n", sent, (long)n_acc, (long)n_rx, (long)n_rx_bytes, (long)n_closed, (long)n_conn, (long)n_enumq);
    sleep(3);
    if (cli) dp8_close(cli);
    dp8_close(srv);
    fprintf(stderr, "closed cleanly\n");
    return 0;
}
