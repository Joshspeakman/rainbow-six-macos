/* dp8test: the DirectPlay 8 transport (runtime/win32/dp8proto.c) against the example frames in Microsoft's
 * [MC-DPL8R] section 4, then a two-endpoint session over loopback, optionally with simulated loss (DP8_DROP=<percent>).
 *   clang -O1 -I runtime/win32 -o /tmp/dp8test tests/dp8test.c runtime/win32/dp8proto.c && /tmp/dp8test */
#include "dp8proto.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void expect(const char *what, const uint8_t *got, size_t n, const char *hex)
{
    uint8_t want[64]; size_t m = 0; for (const char *p = hex; *p; ) { while (*p == ' ') p++; if (!*p) break; unsigned v; sscanf(p, "%2x", &v); want[m++] = (uint8_t)v; p += 2; }
    int ok = n == m && !memcmp(got, want, n); if (!ok) fails++;
    printf("%-46s %s\n", what, ok ? "ok" : "MISMATCH");
    if (!ok) { printf("   got "); for (size_t i = 0; i < n; i++) printf("%02X ", got[i]); printf("\n   want %s\n", hex); }
}
static void check(const char *what, int ok) { printf("%-46s %s\n", what, ok ? "ok" : "FAILED"); if (!ok) fails++; }
typedef struct { int n; size_t len[4]; uint8_t user[4], first[4]; } Co;
static void co_one(void *ctx, uint8_t user, const uint8_t *d, size_t n) { Co *c = ctx; if (c->n < 4) { c->len[c->n] = n; c->user[c->n] = user; c->first[c->n] = n ? d[0] : 0; } c->n++; }

/* ---- session test ---- */
typedef struct { pthread_mutex_t m; int accepted, connected, closed; size_t got; uint32_t sum; int nmsg; dp8_conn *conn; } Side;
static void on_accept(void *ctx, dp8_conn *c) { Side *s = ctx; pthread_mutex_lock(&s->m); s->accepted = 1; s->conn = c; pthread_mutex_unlock(&s->m); }
static void on_connect(void *ctx, dp8_conn *c) { Side *s = ctx; (void)c; pthread_mutex_lock(&s->m); s->connected = 1; pthread_mutex_unlock(&s->m); }
static void on_recv(void *ctx, dp8_conn *c, uint8_t user, const uint8_t *d, size_t n)
{
    Side *s = ctx; (void)c; pthread_mutex_lock(&s->m);
    uint32_t k; memcpy(&k, d, 4);
    if ((int)k != s->nmsg) printf("   out of order: got message %u, expected %d\n", k, s->nmsg);
    for (size_t i = 0; i < n; i++) s->sum += d[i]; s->got += n; s->nmsg++; (void)user;
    pthread_mutex_unlock(&s->m);
}
static void on_closed(void *ctx, dp8_conn *c, int r) { Side *s = ctx; (void)c; (void)r; pthread_mutex_lock(&s->m); s->closed = 1; pthread_mutex_unlock(&s->m); }

int main(void)
{
    uint8_t b[64]; size_t n;
    /* [MC-DPL8R] 4.1 Sample Connection Sequence */
    n = dp8_enc_cframe(b, 0x88, 1, 0, 0, 0x00010006, 0x79C9AEC6, 0x2367369D); expect("4.1.1 CONNECT", b, n, "88 01 00 00 06 00 01 00 C6 AE C9 79 9D 36 67 23");
    n = dp8_enc_cframe(b, 0x88, 2, 0, 0, 0x00010006, 0x79C9AEC6, 0x0004DFE1); expect("4.1.2 CONNECTED (listener)", b, n, "88 02 00 00 06 00 01 00 C6 AE C9 79 E1 DF 04 00");
    n = dp8_enc_cframe(b, 0x80, 2, 1, 0, 0x00010006, 0x79C9AEC6, 0x2367369D); expect("4.1.3 CONNECTED (connector)", b, n, "80 02 01 00 06 00 01 00 C6 AE C9 79 9D 36 67 23");
    n = dp8_enc_dframe(b, 0x3F, 0x02, 0, 0, 0); uint32_t sess = 0x79C9AEC6; memcpy(b + n, &sess, 4); n += 4;
    expect("4.1.4 KeepAlive DFRAME", b, n, "3F 02 00 00 C6 AE C9 79");
    /* 4.2 Sample Upper-Layer Data Transmission and Acknowledgment */
    static const uint8_t df[] = {0x3D, 0x00, 0x05, 0x03, 0x01, 0x41, 0x42, 0x43, 0x44, 0x45};
    dp8_dframe f; size_t h = dp8_dec_dframe(df, sizeof df, &f);
    check("4.2.1 DFRAME decode (seq 5, next receive 3)", h == 4 && f.seq == 5 && f.nrcv == 3 && f.cmd == 0x3D && !(f.cmd & 0x02) && f.ctl == 0);
    n = dp8_enc_sack(b, 0, 3, 6, 0x00115D07, 0); expect("4.2.2 SACK", b, n, "80 06 01 00 03 06 00 00 07 5D 11 00");
    n = dp8_enc_sack(b, 1, 9, 4, 0, 0x5ull | (1ull << 40));
    check("SACK masks round trip", n == 20 && b[2] == (0x01 | 0x02 | 0x04) && b[12] == 5 && b[19] == 0 && b[17] == 1);

    /* [MC-DPL8R] 2.2.3 coalesced payloads: 3 headers (odd: 2 bytes of padding), sizes 5, 300 (BIG_1) and 2,
     * USER_1 on the second, each payload but the last padded to 4 bytes */
    {
        uint8_t f[400] = {0}; size_t o = 0;
        f[o++] = 5; f[o++] = 0x02 | 0x04;                   /* reliable, sequential */
        f[o++] = 300 & 0xff; f[o++] = 0x02 | 0x04 | 0x08 | 0x40;  /* + BIG_1 (bit 8 of the size), USER_1 */
        f[o++] = 2; f[o++] = 0x02 | 0x04 | 0x01;            /* END_COALESCE */
        o += 2;                                             /* padding */
        f[o] = 'a'; o += 8;                                 /* 5 bytes + 3 padding */
        f[o] = 'b'; o += 300;                               /* 300: already aligned */
        f[o] = 'c'; o += 2;
        Co co = {0}; int n = dp8_dec_coalesced(f, o, co_one, &co);
        check("2.2.3 coalesced payloads decode", n == 3 && co.len[0] == 5 && co.len[1] == 300 && co.len[2] == 2 &&
              co.first[0] == 'a' && co.first[1] == 'b' && co.first[2] == 'c' && co.user[1] == 0x40 && co.user[0] == 0);
    }

    /* ---- a session over loopback ---- */
    for (int pass = 0; pass < 2; pass++) {
        if (pass) setenv("DP8_DROP", "20", 1);
        Side hs = {PTHREAD_MUTEX_INITIALIZER}, cs = {PTHREAD_MUTEX_INITIALIZER};
        dp8_callbacks hcb = {&hs, 0, 0, on_accept, 0, on_recv, on_closed}, ccb = {&cs, 0, 0, 0, on_connect, on_recv, on_closed};
        dp8_ep *host = dp8_open(23400, 23420, 1, 0, &hcb), *cli = dp8_open(23421, 23440, 0, 0, &ccb);
        struct sockaddr_in to = {0}; to.sin_family = AF_INET; to.sin_port = htons(dp8_port(host)); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dp8_conn *c = dp8_connect(cli, &to);
        for (int i = 0; i < 400 && !(cs.connected && hs.accepted); i++) usleep(10000);
        check(pass ? "connect (20% loss)" : "connect", cs.connected && hs.accepted);
        /* 300 messages of varying size, some larger than a frame (split and reassembled) */
        uint32_t sum = 0; size_t total = 0; uint8_t buf[5000];
        for (uint32_t k = 0; k < 300; k++) {
            size_t len = 4 + (k * 37) % (k % 50 == 0 ? 4900 : 900);
            memcpy(buf, &k, 4); for (size_t i = 4; i < len; i++) buf[i] = (uint8_t)(k * 7 + i);
            for (size_t i = 0; i < len; i++) sum += buf[i]; total += len;
            while (dp8_send(c, buf, len, 0)) usleep(1000);
        }
        for (int i = 0; i < 3000; i++) { pthread_mutex_lock(&hs.m); int done = hs.nmsg == 300; pthread_mutex_unlock(&hs.m); if (done) break; usleep(10000); }
        char what[80]; snprintf(what, sizeof what, "300 messages, %zu bytes, in order%s", total, pass ? " (20% loss)" : "");
        check(what, hs.nmsg == 300 && hs.got == total && hs.sum == sum);
        dp8_disconnect(c, 0);
        for (int i = 0; i < 1000 && !(cs.closed && hs.closed); i++) usleep(10000);
        check(pass ? "graceful disconnect (20% loss)" : "graceful disconnect", cs.closed && hs.closed);
        dp8_close(cli); dp8_close(host);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
