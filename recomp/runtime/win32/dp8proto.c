#include "../platform.h"
/* DirectPlay 8 transport on UDP, written from Microsoft's published specifications:
 *   [MC-DPL8R]  DirectPlay 8 Protocol: Reliable           (connections, sequencing, acknowledgements, retries)
 *   [MC-DPLHP]  DirectPlay 8 Protocol: Host and Port Enumeration (EnumQuery / EnumResponse, first byte 0)
 * Unsigned connections at protocol version 1.5 (coalescing understood on receive; signing, which needs 1.6, is never
 * negotiated). Everything this layer sends is reliable; received unreliable frames and send masks are handled.
 * One network thread per endpoint; callbacks run on it with no lock held. No dependency on the game or the Win32 layer,
 * so tests/dp8test.c can check it against the specifications' example packets. */
#include "dp8proto.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    CMD_DATA = 0x01, CMD_RELIABLE = 0x02, CMD_SEQUENTIAL = 0x04, CMD_POLL = 0x08, CMD_NEW_MSG = 0x10, CMD_END_MSG = 0x20,
    CMD_USER_1 = 0x40, CMD_USER_2 = 0x80, CMD_CFRAME = 0x80,
    CTL_RETRY = 0x01, CTL_KEEPALIVE = 0x02, CTL_COALESCE = 0x04, CTL_END_STREAM = 0x08, CTL_SACK1 = 0x10, CTL_SACK2 = 0x20,
    CTL_SEND1 = 0x40, CTL_SEND2 = 0x80,
    OP_CONNECT = 1, OP_CONNECTED = 2, OP_CONNECTED_SIGNED = 3, OP_HARD_DISCONNECT = 4, OP_SACK = 6,
    SACK_RESPONSE = 0x01, SACK_MASK1 = 0x02, SACK_MASK2 = 0x04, SACK_SEND1 = 0x08, SACK_SEND2 = 0x10,
    /* MAX_FRAME: payload per frame; with the frame header (up to 16) and IP/UDP (28) a datagram stays under 1,210 bytes,
     * within the 1,280 every internet path carries (VPN tunnels such as WireGuard's 1,380 dropped 1,432-byte ones) */
    VERSION = 0x00010005u, MAX_FRAME = 1160, WINDOW = 64,
    /* limits against a hostile peer (nothing the game sends comes near them): one message's size, connections still
     * in the handshake in all and from one address, and how long a closed connection is kept before it is freed */
    MAX_MESSAGE = 32 << 20, MAX_HALF_OPEN = 64, MAX_HALF_OPEN_PER_IP = 8, REAP_MS = 15000,
};
uint32_t dp8_tick(void) { return (uint32_t)(ds_clock_ns() / 1000000); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* ---------------------------------------------------------------- frame encoding (exported for the tests) */
size_t dp8_enc_cframe(uint8_t *out, uint8_t cmd, uint8_t op, uint8_t msgid, uint8_t rspid, uint32_t version, uint32_t sessid, uint32_t ts)
{
    out[0] = cmd; out[1] = op; out[2] = msgid; out[3] = rspid; put32(out + 4, version); put32(out + 8, sessid); put32(out + 12, ts);
    return 16;
}
size_t dp8_enc_sack(uint8_t *out, uint8_t retry_flag, uint8_t nseq, uint8_t nrcv, uint32_t ts, uint64_t sack_mask)
{
    uint8_t flags = SACK_RESPONSE; size_t n = 12;
    out[0] = CMD_CFRAME; out[1] = OP_SACK; out[4] = nseq; out[5] = nrcv; out[6] = out[7] = 0; put32(out + 8, ts);
    if ((uint32_t)sack_mask) { flags |= SACK_MASK1; put32(out + n, (uint32_t)sack_mask); n += 4; }
    if (sack_mask >> 32) { flags |= SACK_MASK2; put32(out + n, (uint32_t)(sack_mask >> 32)); n += 4; }
    out[2] = flags; out[3] = retry_flag;
    return n;
}
/* DFRAME header: bCommand bControl bSeq bNRcv [sack1 sack2] (no send masks: everything we send is reliable) */
size_t dp8_enc_dframe(uint8_t *out, uint8_t cmd, uint8_t ctl, uint8_t seq, uint8_t nrcv, uint64_t sack_mask)
{
    size_t n = 4;
    ctl &= (uint8_t)~(CTL_SACK1 | CTL_SACK2 | CTL_SEND1 | CTL_SEND2);
    if ((uint32_t)sack_mask) { ctl |= CTL_SACK1; put32(out + n, (uint32_t)sack_mask); n += 4; }
    if (sack_mask >> 32) { ctl |= CTL_SACK2; put32(out + n, (uint32_t)(sack_mask >> 32)); n += 4; }
    out[0] = cmd | CMD_DATA; out[1] = ctl; out[2] = seq; out[3] = nrcv;
    return n;
}
/* parsed DFRAME header; returns the header size or 0 if malformed */
size_t dp8_dec_dframe(const uint8_t *p, size_t len, dp8_dframe *f)
{
    if (len < 4 || !(p[0] & CMD_DATA)) return 0;
    memset(f, 0, sizeof *f); f->cmd = p[0]; f->ctl = p[1]; f->seq = p[2]; f->nrcv = p[3];
    size_t n = 4;
    if (f->ctl & CTL_SACK1) { if (len < n + 4) return 0; f->sack |= get32(p + n); n += 4; }
    if (f->ctl & CTL_SACK2) { if (len < n + 4) return 0; f->sack |= (uint64_t)get32(p + n) << 32; n += 4; }
    if (f->ctl & CTL_SEND1) { if (len < n + 4) return 0; f->sendmask |= get32(p + n); n += 4; }
    if (f->ctl & CTL_SEND2) { if (len < n + 4) return 0; f->sendmask |= (uint64_t)get32(p + n) << 32; n += 4; }
    return n;
}

/* ---------------------------------------------------------------- connections */
typedef struct Frame { uint8_t *buf; size_t len; uint32_t sent, rto; int retries; int acked; } Frame;
typedef struct Msg { struct Msg *next; uint8_t *data; size_t len, offset; uint8_t user; } Msg;
typedef struct Rx { int have; uint8_t cmd, ctl; uint8_t *data; size_t len; } Rx;
enum { ST_CONNECTING, ST_ACCEPTING, ST_ESTABLISHED, ST_CLOSING, ST_HARD_CLOSING, ST_CLOSED };
struct dp8_conn {
    dp8_ep *ep; struct sockaddr_in addr; int state; uint32_t sessid, remote_version; void *user;
    uint8_t cmsgid, crsp; int cretries; uint32_t cnext, cdelay;           /* handshake retries */
    uint8_t next_send, next_recv; Frame win[256]; Msg *q, *qtail;          /* sender: in flight + queued */
    Rx rx[256]; uint8_t *part; size_t partlen, partcap; uint8_t partuser; int inpart;   /* receiver */
    uint32_t ack_due; int ack_poll, last_retry_seen;                       /* delayed acknowledgement */
    uint32_t last_recv, last_send, rtt;
    uint32_t st_tx, st_rx, st_retx, st_rtt_max, st_t0;                    /* DP8_STATS */
    int end_sent, end_recv, end_seq_acked; uint8_t end_seq; int hard_left; uint32_t hard_next;
    int closed_reason; int reported; uint32_t closed_at;
    struct dp8_conn *next;
};
typedef struct Pend { struct Pend *next; dp8_conn *c; uint8_t user; size_t len; uint8_t data[]; } Pend;
typedef struct Delayed { struct Delayed *next; uint32_t due; struct sockaddr_in to; size_t len; int k; uint8_t data[]; } Delayed;
struct dp8_ep {
    Delayed *lagq, *rxq; pthread_mutex_t lagm;                             /* DP8_LAG: datagrams waiting to go out / be handled */
    int fd, fd_enum; uint16_t port; int listening; dp8_callbacks cb; pthread_mutex_t lock; pthread_t thread; int stop;
    dp8_conn *conns; int wake[2];
    Pend *pend, *pend_tail;                 /* received messages, handed up after the lock is released */
};

/* DP8_TRACE=1: every datagram sent and received (address, size, first bytes; =2 all bytes) */
static void trace(const char *dir, const struct sockaddr_in *a, const uint8_t *p, size_t len, int enum_port)
{
    static int on = -1; if (on < 0) on = getenv("DP8_TRACE") ? atoi(getenv("DP8_TRACE")) : 0;
    if (!on) return;
    char ip[32]; inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    fprintf(stderr, "dp8: %s %s:%u%s %zu:", dir, ip, ntohs(a->sin_port), enum_port ? " (enum port)" : "", len);
    for (size_t i = 0; i < len && (i < 40 || on >= 2 || (len > 1 && p[0] == 0 && p[1] == 3)); i++) fprintf(stderr, " %02x", p[i]);   /* enumeration answers in full */
    fprintf(stderr, "\n");
}
/* DP8_LAG=<ms>[,<jitter ms>]: every datagram this side sends or receives waits that long (plus a random part up to the
 * jitter, so some arrive out of order), for testing internet-like delays on a LAN with only this side changed: the round
 * trip grows by twice that */
static _Atomic int lag_ms, lag_jitter;
static pthread_once_t lag_once = PTHREAD_ONCE_INIT;
static void lag_env_init(void) { const char *e = getenv("DP8_LAG"); lag_ms = e ? atoi(e) : 0; const char *j = e ? strchr(e, ',') : 0; lag_jitter = j ? atoi(j + 1) : 0; }
static void lag_init(void) { pthread_once(&lag_once, lag_env_init); }
void dp8_set_lag(int ms, int jitter) { lag_init(); lag_ms = ms > 0 ? ms : 0; lag_jitter = jitter > 0 ? jitter : 0; }
static uint32_t lag_due(void) { int ms = lag_ms, jitter = lag_jitter; return dp8_tick() + (uint32_t)(ms > 0 ? ms : 0) + (jitter > 0 ? arc4random_uniform((uint32_t)jitter + 1) : 0); }
static void lag_flush(dp8_ep *ep)
{
    uint32_t now = dp8_tick();
    pthread_mutex_lock(&ep->lagm);
    for (Delayed **pp = &ep->lagq; *pp; ) {
        Delayed *d = *pp;
        if ((int32_t)(now - d->due) >= 0) { sendto(ep->fd, d->data, d->len, 0, (const struct sockaddr *)&d->to, sizeof d->to); *pp = d->next; free(d); }
        else pp = &d->next;
    }
    pthread_mutex_unlock(&ep->lagm);
}
static void raw_send(dp8_ep *ep, const struct sockaddr_in *to, const void *buf, size_t len)
{
    trace("send", to, buf, len, 0);
    lag_init();
    if (getenv("DP8_DROP") && arc4random_uniform(100) < (uint32_t)atoi(getenv("DP8_DROP"))) return;   /* also applies when delay is enabled */
    if (lag_ms > 0) {
        Delayed *d = malloc(sizeof *d + len); if (!d) return;
        d->next = 0; d->to = *to; d->len = len; memcpy(d->data, buf, len);
        d->due = lag_due();
        pthread_mutex_lock(&ep->lagm); Delayed **pp = &ep->lagq; while (*pp) pp = &(*pp)->next; *pp = d; pthread_mutex_unlock(&ep->lagm);
        return;
    }
    sendto(ep->fd, buf, len, 0, (const struct sockaddr *)to, sizeof *to);
}
void dp8_send_raw(dp8_ep *ep, const struct sockaddr_in *to, const void *buf, size_t len) { raw_send(ep, to, buf, len); }
static uint64_t rx_sack_mask(dp8_conn *c)
{
    uint64_t m = 0;
    for (int i = 1; i <= 64; i++) if (c->rx[(uint8_t)(c->next_recv + i)].have) m |= 1ull << (i - 1);   /* bit 0 = next_recv + 1 */
    return m;
}
static void send_sack(dp8_conn *c)
{
    uint8_t b[20]; size_t n = dp8_enc_sack(b, (uint8_t)(c->last_retry_seen ? 1 : 0), c->next_send, c->next_recv, dp8_tick(), rx_sack_mask(c));
    raw_send(c->ep, &c->addr, b, n); c->ack_due = 0; c->ack_poll = 0;
}
static void send_cframe(dp8_conn *c, uint8_t cmd, uint8_t op, uint8_t rsp)
{
    uint8_t b[16]; dp8_enc_cframe(b, cmd, op, c->cmsgid, rsp, VERSION, c->sessid, dp8_tick()); raw_send(c->ep, &c->addr, b, 16);
}
static int in_flight(dp8_conn *c)          /* sequence numbers in use (selectively acknowledged ones still count) */
{
    int n = 0; for (int i = 0; i < 256; i++) if (c->win[i].buf) n++; return n;
}
/* put one frame on the wire (new or retry) */
static void transmit(dp8_conn *c, uint8_t seq, int retry)
{
    Frame *f = &c->win[seq]; uint8_t b[MAX_FRAME + 64];
    uint8_t cmd = f->buf[0], ctl = (uint8_t)(f->buf[1] | (retry ? CTL_RETRY : 0));
    /* refresh the acknowledgement fields: bNRcv and the SACK mask are current, not what they were at first send */
    size_t h = dp8_enc_dframe(b, cmd, ctl, seq, c->next_recv, rx_sack_mask(c));
    memcpy(b + h, f->buf + 4, f->len - 4);
    raw_send(c->ep, &c->addr, b, h + f->len - 4); c->st_tx++; if (retry) c->st_retx++;
    f->sent = dp8_tick(); c->last_send = f->sent; c->ack_due = 0; c->ack_poll = 0;   /* the frame carries our ACK */
}
/* move queued messages into the window as space allows; messages larger than a frame are split */
static void pump(dp8_conn *c)
{
    while (c->q && (c->state == ST_ESTABLISHED || c->state == ST_CLOSING) && !c->end_sent) {
        Msg *m = c->q;
        if (in_flight(c) >= WINDOW - 1) break;
        size_t off = m->offset, n = m->len - off < MAX_FRAME ? m->len - off : MAX_FRAME;
        int last = off + n == m->len;
        uint8_t cmd = CMD_DATA | CMD_RELIABLE | CMD_SEQUENTIAL | m->user |
                      (off == 0 ? CMD_NEW_MSG : 0) | (last ? CMD_END_MSG : 0);
        /* Ask for an ACK at a full window as well as the end of a burst. */
        if (in_flight(c) == WINDOW - 2 || (last && !m->next)) cmd |= CMD_POLL;
        uint8_t seq = c->next_send++; Frame *f = &c->win[seq];
        free(f->buf); f->buf = malloc(4 + n); f->buf[0] = cmd; f->buf[1] = 0; memcpy(f->buf + 4, m->data + off, n); f->len = 4 + n;
        f->retries = 0; f->acked = 0; f->rto = c->rtt * 5 / 2 + 100;
        transmit(c, seq, 0);
        m->offset += n;
        if (!last) continue;
        c->q = m->next; if (!c->q) c->qtail = 0; free(m->data); free(m);
    }
}
static void enqueue(dp8_conn *c, const void *data, size_t len, uint8_t user)
{
    Msg *m = calloc(1, sizeof *m); m->data = malloc(len ? len : 1); memcpy(m->data, data, len); m->len = len; m->user = user;
    if (c->qtail) c->qtail->next = m; else c->q = m; c->qtail = m;
}
static void send_keepalive(dp8_conn *c, uint8_t ctl_extra)
{
    uint8_t seq = c->next_send++; Frame *f = &c->win[seq];
    free(f->buf); f->buf = malloc(8); f->buf[0] = CMD_DATA | CMD_RELIABLE | CMD_SEQUENTIAL | CMD_POLL | CMD_NEW_MSG | CMD_END_MSG; f->buf[1] = CTL_KEEPALIVE | ctl_extra;
    put32(f->buf + 4, c->sessid); f->len = 8; f->retries = 0; f->acked = 0; f->rto = c->rtt * 5 / 2 + 100;
    if (ctl_extra & CTL_END_STREAM) { c->end_seq = seq; f->buf[1] = CTL_END_STREAM; f->len = 4; }   /* END_STREAM: no payload */
    transmit(c, seq, 0);
}
static void conn_free_buffers(dp8_conn *c)
{
    for (int i = 0; i < 256; i++) { free(c->win[i].buf); c->win[i].buf = 0; free(c->rx[i].data); c->rx[i].data = 0; c->rx[i].have = 0; }
    while (c->q) { Msg *m = c->q; c->q = m->next; free(m->data); free(m); } c->qtail = 0;
    free(c->part); c->part = 0; c->partcap = c->partlen = 0; c->inpart = 0;
}
static void conn_close(dp8_conn *c, int reason)
{
    if (c->state == ST_CLOSED) return;
    c->state = ST_CLOSED; c->closed_reason = reason; c->closed_at = dp8_tick();
    conn_free_buffers(c);                       /* the struct stays until it is reaped (TIME_WAIT, the upper layer let go) */
}
/* acknowledgements from the remote side */
static void ack_frame(dp8_conn *c, Frame *f)
{
    /* Sample the first acknowledgement only. A selective ACK already proves
     * delivery; a later cumulative ACK may have waited seconds for a lost gap. */
    if (!f->acked && !f->retries) {
        uint32_t s = dp8_tick() - f->sent;
        c->rtt = (c->rtt * 7 + s) / 8;
        if (s > c->st_rtt_max) c->st_rtt_max = s;
    }
    f->acked = 1;
}
static void handle_acks(dp8_conn *c, uint8_t nrcv, uint64_t sack)
{
    for (int i = 0; i < 256; i++) {
        Frame *f = &c->win[i]; if (!f->buf) continue;
        uint8_t seq = (uint8_t)i, d = (uint8_t)(c->next_send - seq);                   /* how far behind next_send */
        if (d == 0 || d > WINDOW) continue;                                             /* not in flight */
        if ((uint8_t)(nrcv - seq) >= 1 && (uint8_t)(nrcv - seq) <= WINDOW && (uint8_t)(c->next_send - nrcv) < WINDOW + 1) {   /* seq < nrcv */
            ack_frame(c, f);
            if (c->end_sent && seq == c->end_seq) c->end_seq_acked = 1;
            free(f->buf); f->buf = 0;
        } else {
            uint8_t k = (uint8_t)(seq - nrcv - 1);
            if (k < 64 && (sack >> k) & 1) ack_frame(c, f);
        }
    }
}
/* [MC-DPL8R] 2.2.3: 1-32 two-byte headers {bSize, bCommand} (the last with END_COALESCE, padded to 4 bytes), then the
 * payloads, each but the last padded to 4 bytes; sizes are 11 bits (BIG_1..3 in bCommand). Returns the payload count. */
int dp8_dec_coalesced(const uint8_t *p, size_t len, void (*fn)(void *, uint8_t user, const uint8_t *, size_t), void *ctx)
{
    size_t nh = 0; while (nh < 32 && 2 * nh + 2 <= len) { nh++; if (p[2 * nh - 1] & 0x01) break; }
    size_t off = 2 * nh; if (nh & 1) off += 2;
    int n = 0;
    for (size_t i = 0; i < nh; i++) {
        uint8_t bsize = p[2 * i], bcmd = p[2 * i + 1];
        size_t sz = bsize | ((size_t)(bcmd & 0x38) << 5);
        if (off + sz > len) break;
        fn(ctx, bcmd & (CMD_USER_1 | CMD_USER_2), p + off, sz); n++;
        off += sz; if (i + 1 < nh) off = (off + 3) & ~(size_t)3;
    }
    return n;
}
static void deliver(dp8_conn *c, uint8_t user, const uint8_t *data, size_t len);
static void coalesced_one(void *c, uint8_t user, const uint8_t *data, size_t len) { deliver(c, user, data, len); }
static void deliver(dp8_conn *c, uint8_t user, const uint8_t *data, size_t len)
{
    Pend *m = malloc(sizeof *m + len); m->next = 0; m->c = c; m->user = user; m->len = len; memcpy(m->data, data, len);
    dp8_ep *ep = c->ep; if (ep->pend_tail) ep->pend_tail->next = m; else ep->pend = m; ep->pend_tail = m;
}
static void flush_pending(dp8_ep *ep)        /* called without the lock */
{
    pthread_mutex_lock(&ep->lock); Pend *m = ep->pend; ep->pend = ep->pend_tail = 0; pthread_mutex_unlock(&ep->lock);
    while (m) { Pend *n = m->next; if (ep->cb.receive) ep->cb.receive(ep->cb.ctx, m->c, m->user, m->data, m->len); free(m); m = n; }
}
/* in-order processing of one received DFRAME's payload */
static void consume(dp8_conn *c, uint8_t cmd, uint8_t ctl, const uint8_t *p, size_t len)
{
    if (ctl & CTL_END_STREAM) { c->end_recv = 1; return; }
    if (ctl & CTL_KEEPALIVE) return;                     /* KeepAlive: no data (1.5 and later carry the session id) */
    uint8_t user = cmd & (CMD_USER_1 | CMD_USER_2);
    if (ctl & CTL_COALESCE) { dp8_dec_coalesced(p, len, coalesced_one, c); return; }
    if ((cmd & CMD_NEW_MSG) && (cmd & CMD_END_MSG)) { c->inpart = 0; deliver(c, user, p, len); return; }
    if (cmd & CMD_NEW_MSG) { c->inpart = 1; c->partlen = 0; c->partuser = user; }
    if (!c->inpart) return;
    if (c->partlen + len > MAX_MESSAGE) { conn_close(c, DP8_CLOSE_LOST); return; }      /* a hostile peer: no end */
    if (c->partlen + len > c->partcap) {
        size_t cap = (c->partlen + len) * 2; uint8_t *n = realloc(c->part, cap);
        if (!n) { conn_close(c, DP8_CLOSE_LOST); return; }
        c->part = n; c->partcap = cap;
    }
    memcpy(c->part + c->partlen, p, len); c->partlen += len;
    if (cmd & CMD_END_MSG) { c->inpart = 0; deliver(c, c->partuser, c->part, c->partlen); }
}
static void handle_dframe(dp8_conn *c, const uint8_t *p, size_t len)
{
    dp8_dframe f; size_t h = dp8_dec_dframe(p, len, &f); if (!h) return;
    c->last_recv = dp8_tick();
    handle_acks(c, f.nrcv, f.sack);
    if (f.sendmask) {                                                                    /* frames that will never come */
        for (int k = 0; k < 64; k++) if ((f.sendmask >> k) & 1) {
            uint8_t s = (uint8_t)(f.seq - 1 - k);
            if ((uint8_t)(s - c->next_recv) < 64 && !c->rx[s].have) { c->rx[s].have = 2; c->rx[s].len = 0; }   /* placeholder: dropped */
        }
    }
    uint8_t d = (uint8_t)(f.seq - c->next_recv); c->st_rx++;
    c->last_retry_seen = f.ctl & CTL_RETRY;
    if (d >= 64) {                                                                       /* duplicate or out of window */
        if (f.cmd & CMD_POLL) send_sack(c); else if (!c->ack_due) c->ack_due = dp8_tick() + 20;
        return;
    }
    if (!c->rx[f.seq].have) {
        Rx *r = &c->rx[f.seq]; r->have = 1; r->cmd = f.cmd; r->ctl = f.ctl; r->len = len - h;
        r->data = malloc(r->len ? r->len : 1); memcpy(r->data, p + h, r->len);
    }
    while (c->rx[c->next_recv].have) {
        Rx *r = &c->rx[c->next_recv];
        if (r->have == 1) consume(c, r->cmd, r->ctl, r->data, r->len);
        free(r->data); r->data = 0; r->have = 0; c->next_recv++;
        if (c->state == ST_CLOSED) return;
    }
    if (f.cmd & CMD_POLL) send_sack(c);
    else if (!c->ack_due) c->ack_due = dp8_tick() + (d ? 20 : 100);
    if (c->end_recv && !c->end_sent && !c->q) { c->end_sent = 1; send_keepalive(c, CTL_END_STREAM); }
}
static void handle_sack(dp8_conn *c, const uint8_t *p, size_t len)
{
    if (len < 12) return;
    uint8_t flags = p[2]; uint64_t sack = 0; size_t n = 12;
    if (flags & SACK_MASK1) { if (len < n + 4) return; sack |= get32(p + n); n += 4; }
    if (flags & SACK_MASK2) { if (len < n + 4) return; sack |= (uint64_t)get32(p + n) << 32; n += 4; }
    c->last_recv = dp8_tick();
    handle_acks(c, p[5], sack);
    if (sack) for (int i = 0; i < 256; i++) if (c->win[i].buf && !c->win[i].acked) {   /* first unacknowledged frame: retry soon */
        /* Repeated gap reports may arrive while the first retry is still in
         * flight. Do not reset its backed-off RTO to 10 ms on every report:
         * that exhausts all retries inside one RTT on a jittery connection. */
        if ((uint8_t)(p[5] - (uint8_t)i) == 0 && !c->win[i].retries) c->win[i].rto = 10;
    }
}
static dp8_conn *find_conn(dp8_ep *ep, const struct sockaddr_in *a)
{
    for (dp8_conn *c = ep->conns; c; c = c->next)
        if (c->state != ST_CLOSED && c->addr.sin_addr.s_addr == a->sin_addr.s_addr && c->addr.sin_port == a->sin_port) return c;
    return 0;
}
static dp8_conn *new_conn(dp8_ep *ep, const struct sockaddr_in *a, int state, uint32_t sessid)
{
    dp8_conn *c = calloc(1, sizeof *c); c->ep = ep; c->addr = *a; c->state = state; c->sessid = sessid; c->rtt = 200;
    c->cdelay = 200; c->cnext = dp8_tick() + 200; c->last_recv = c->last_send = dp8_tick();
    c->next = ep->conns; ep->conns = c; return c;
}
static void established(dp8_conn *c)
{
    c->state = ST_ESTABLISHED; c->last_recv = dp8_tick();
    send_keepalive(c, 0);                                           /* [MC-DPL8R] 4.1: both sides confirm with a KeepAlive */
}
static void handle_cframe(dp8_ep *ep, dp8_conn *c, const struct sockaddr_in *from, const uint8_t *p, size_t len, int *new_conn_out)
{
    uint8_t op = p[1], cmd = p[0];
    if (op == OP_SACK) { if (c && c->state >= ST_ESTABLISHED) handle_sack(c, p, len); return; }
    if (len < 16) return;
    uint32_t ver = get32(p + 4), sess = get32(p + 8);
    if (op == OP_CONNECT) {
        if ((ver >> 16) != 1) return;
        if (c) { if (c->state == ST_ACCEPTING && c->sessid == sess) send_cframe(c, CMD_CFRAME | CMD_POLL, OP_CONNECTED, p[2]); return; }
        if (!ep->listening) return;
        int half = 0, same = 0;                     /* a flood of (spoofable) CONNECTs must not pile up connections */
        for (dp8_conn *z = ep->conns; z; z = z->next)
            if (z->state == ST_ACCEPTING) { half++; same += z->addr.sin_addr.s_addr == from->sin_addr.s_addr; }
        if (half >= MAX_HALF_OPEN || same >= MAX_HALF_OPEN_PER_IP) return;
        c = new_conn(ep, from, ST_ACCEPTING, sess); c->remote_version = ver; c->crsp = p[2];
        send_cframe(c, CMD_CFRAME | CMD_POLL, OP_CONNECTED, p[2]);
        return;
    }
    if (op == OP_CONNECTED) {
        if (!c || c->sessid != sess) return;
        if (c->state == ST_CONNECTING && (cmd & CMD_POLL)) {                        /* listener accepted: confirm */
            c->remote_version = ver; c->cmsgid++; send_cframe(c, CMD_CFRAME, OP_CONNECTED, p[2]);
            established(c); if (new_conn_out) *new_conn_out = 2;
        } else if (c->state == ST_ACCEPTING && !(cmd & CMD_POLL)) {                 /* connector confirmed */
            established(c); if (new_conn_out) *new_conn_out = 1;
        } else if (c->state >= ST_ESTABLISHED && (cmd & CMD_POLL)) send_cframe(c, CMD_CFRAME, OP_CONNECTED, p[2]);   /* our confirmation was lost */
        return;
    }
    if (op == OP_HARD_DISCONNECT) {
        if (!c || c->state < ST_ESTABLISHED || sess != c->sessid) return;
        if (c->state == ST_HARD_CLOSING) { conn_close(c, DP8_CLOSE_NORMAL); return; }
        for (int i = 0; i < 3; i++) send_cframe(c, CMD_CFRAME, OP_HARD_DISCONNECT, 0);
        conn_close(c, DP8_CLOSE_REMOTE);
    }
}
static void on_packet(dp8_ep *ep, const struct sockaddr_in *from, const uint8_t *p, size_t len, int via_enum_port)
{
    trace("recv", from, p, len, via_enum_port);
    if (len < 4) return;
    if (p[0] == 0) {                                                                     /* [MC-DPLHP] */
        if (p[1] == 2 && ep->listening && ep->cb.enum_query) ep->cb.enum_query(ep->cb.ctx, ep, from, p, len);
        else if (p[1] == 3 && ep->cb.enum_response) ep->cb.enum_response(ep->cb.ctx, ep, from, p, len);
        return;
    }
    if (via_enum_port) return;
    pthread_mutex_lock(&ep->lock);
    dp8_conn *c = find_conn(ep, from); int fresh = 0;
    if (!c && (p[0] & CMD_DATA)) {             /* a connection closed moments ago: keep acknowledging retries (TIME_WAIT) */
        for (dp8_conn *z = ep->conns; z; z = z->next)
            /* Retain the final ACK through the peer's entire retry window.
             * A lost ACK plus exponential backoff can exceed five seconds. */
            if (z->state == ST_CLOSED && z->closed_reason == DP8_CLOSE_NORMAL && dp8_tick() - z->closed_at < 60000 &&
                z->addr.sin_addr.s_addr == from->sin_addr.s_addr && z->addr.sin_port == from->sin_port) {
                dp8_dframe f; if (dp8_dec_dframe(p, len, &f)) {
                    uint8_t b[20]; size_t n = dp8_enc_sack(b, 0, z->next_send, z->next_recv, dp8_tick(), 0);
                    raw_send(ep, from, b, n);
                }
                break;
            }
    }
    if (p[0] & CMD_DATA) { if (c && c->state >= ST_ESTABLISHED && c->state != ST_CLOSED) handle_dframe(c, p, len); }
    else if ((p[0] == 0x80 || p[0] == 0x88) && len >= 12) handle_cframe(ep, c, from, p, len, &fresh);
    if (!c) c = find_conn(ep, from);
    pthread_mutex_unlock(&ep->lock);
    if (fresh == 1 && ep->cb.accepted) ep->cb.accepted(ep->cb.ctx, c);
    if (fresh == 2 && ep->cb.connected) ep->cb.connected(ep->cb.ctx, c);
    flush_pending(ep);
}
/* timers: handshake retries, retransmission, delayed ACKs, keep-alives, closing */
static void tick(dp8_ep *ep)
{
    dp8_conn *report[64]; int nr = 0;
    pthread_mutex_lock(&ep->lock);
    uint32_t now = dp8_tick();
    for (dp8_conn *c = ep->conns; c; c = c->next) {
        if (c->state == ST_CONNECTING || c->state == ST_ACCEPTING) {
            if ((int32_t)(now - c->cnext) >= 0) {
                if (++c->cretries > 14) { conn_close(c, DP8_CLOSE_LOST); }
                else {
                    if (c->state == ST_CONNECTING) { c->cmsgid++; send_cframe(c, CMD_CFRAME | CMD_POLL, OP_CONNECT, 0); }
                    else send_cframe(c, CMD_CFRAME | CMD_POLL, OP_CONNECTED, c->crsp);
                    c->cdelay = c->cdelay * 2 > 5000 ? 5000 : c->cdelay * 2; c->cnext = now + c->cdelay;
                }
            }
        } else if (c->state == ST_ESTABLISHED || c->state == ST_CLOSING) {
            for (int i = 0; i < 256; i++) {
                Frame *f = &c->win[i]; if (!f->buf || f->acked) continue;
                if ((int32_t)(now - f->sent) >= (int32_t)f->rto) {
                    if (++f->retries > 10) { conn_close(c, DP8_CLOSE_LOST); break; }
                    f->rto = f->retries <= 3 ? f->rto + c->rtt + 100 : f->rto * 2; if (f->rto > 5000) f->rto = 5000;
                    transmit(c, (uint8_t)i, 1);
                }
            }
            if (c->state == ST_CLOSED) goto closed;
            {   static int on = -1; if (on < 0) on = getenv("DP8_STATS") != 0;      /* every 5 s: the link's figures */
                if (on && now - c->st_t0 >= 5000) {
                    char ip[32]; inet_ntop(AF_INET, &c->addr.sin_addr, ip, sizeof ip);
                    if (c->st_t0) fprintf(stderr, "dp8 link %s: rtt %u ms (max %u), sent %u (resent %u), received %u, in flight %d, queued %s\n",
                                          ip, c->rtt, c->st_rtt_max, c->st_tx, c->st_retx, c->st_rx, in_flight(c), c->q ? "yes" : "no");
                    c->st_t0 = now; c->st_tx = c->st_rx = c->st_retx = c->st_rtt_max = 0;
                }
            }
            pump(c);
            if (c->ack_due && (int32_t)(now - c->ack_due) >= 0) send_sack(c);
            /* pump/transmit can stamp a send later than this tick's snapshot.
             * Signed deltas handle that and the 32-bit millisecond wrap without
             * treating a newly sent/received packet as a multi-day timeout. */
            if ((int32_t)(now - c->last_send) > 25000 && (int32_t)(now - c->last_recv) > 25000 && !c->q) send_keepalive(c, 0);
            if ((int32_t)(now - c->last_recv) > 60000) conn_close(c, DP8_CLOSE_LOST);
            if ((c->state == ST_CLOSING || c->end_recv) && !c->q && !c->end_sent) { c->end_sent = 1; send_keepalive(c, CTL_END_STREAM); }
            if (c->end_sent && c->end_seq_acked && c->end_recv) conn_close(c, DP8_CLOSE_NORMAL);
        } else if (c->state == ST_HARD_CLOSING) {
            if ((int32_t)(now - c->hard_next) >= 0) {
                if (c->hard_left-- > 0) { send_cframe(c, CMD_CFRAME, OP_HARD_DISCONNECT, 0); c->hard_next = now + (c->rtt / 2 < 10 ? 10 : c->rtt / 2 > 500 ? 500 : c->rtt / 2); }
                else conn_close(c, DP8_CLOSE_NORMAL);
            }
        }
    closed:
        if (c->state == ST_CLOSED && !c->reported && nr < 64) { c->reported = 1; report[nr++] = c; }
    }
    pthread_mutex_unlock(&ep->lock);
    for (int i = 0; i < nr; i++) {
        dp8_conn *c = report[i];
        if (c->ep->cb.closed) c->ep->cb.closed(c->ep->cb.ctx, c, c->closed_reason);
    }
    /* free connections closed a while ago: reported to the upper layer (which lets go of them in its callback), past
     * TIME_WAIT, and long enough for a call that picked one up just before to have finished */
    pthread_mutex_lock(&ep->lock);
    for (dp8_conn **pp = &ep->conns; *pp; ) {
        dp8_conn *c = *pp;
        if (c->state == ST_CLOSED && c->reported && (int32_t)(now - c->closed_at) > (c->closed_reason == DP8_CLOSE_NORMAL ? 60000 : REAP_MS)) { *pp = c->next; conn_free_buffers(c); free(c); }
        else pp = &c->next;
    }
    pthread_mutex_unlock(&ep->lock);
}
static void *net_thread(void *arg)
{
    dp8_ep *ep = arg; uint8_t buf[2048];
    while (!ep->stop) {
        struct pollfd pf[3] = {{ep->fd, POLLIN, 0}, {ep->fd_enum, POLLIN, 0}, {ep->wake[0], POLLIN, 0}};
        poll(pf, 3, lag_ms > 0 || ep->rxq ? 1 : 10);
        lag_flush(ep);
        for (int k = 0; k < 2; k++) if (pf[k].revents & POLLIN) for (;;) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(k ? ep->fd_enum : ep->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            lag_init();
            if (lag_ms > 0) {                                   /* DP8_LAG: handled later (this thread only) */
                Delayed *d = malloc(sizeof *d + (size_t)n); if (!d) continue;
                d->next = 0; d->to = from; d->len = (size_t)n; d->k = k; memcpy(d->data, buf, (size_t)n);
                d->due = lag_due();
                Delayed **pp = &ep->rxq; while (*pp) pp = &(*pp)->next; *pp = d;
                continue;
            }
            on_packet(ep, &from, buf, (size_t)n, k);
        }
        for (Delayed **pp = &ep->rxq; *pp; ) {
            Delayed *d = *pp;
            if ((int32_t)(dp8_tick() - d->due) >= 0) { *pp = d->next; on_packet(ep, &d->to, d->data, d->len, d->k); free(d); }
            else pp = &d->next;
        }
        if (pf[2].revents & POLLIN) { char t[64]; while (read(ep->wake[0], t, sizeof t) > 0) {} }
        tick(ep);
    }
    return 0;
}
static int bind_udp(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}
dp8_ep *dp8_open(uint16_t port_lo, uint16_t port_hi, int listen, uint16_t enum_port, const dp8_callbacks *cb)
{
    int fd = -1; uint16_t port = 0;
    for (uint32_t p = port_lo; p <= port_hi && fd < 0; p++) { fd = bind_udp((uint16_t)p); port = (uint16_t)p; }
    if (fd < 0) return 0;
    if (!port_lo) { struct sockaddr_in a; socklen_t l = sizeof a; getsockname(fd, (struct sockaddr *)&a, &l); port = ntohs(a.sin_port); }
    dp8_ep *ep = calloc(1, sizeof *ep); ep->fd = fd; ep->port = port; ep->listening = listen; ep->cb = *cb;
    ep->fd_enum = enum_port && enum_port != port ? bind_udp(enum_port) : -1;           /* like DPNSVR: queries on the well-known port */
    pthread_mutex_init(&ep->lock, 0); pthread_mutex_init(&ep->lagm, 0); pipe(ep->wake); fcntl(ep->wake[0], F_SETFL, O_NONBLOCK);
    pthread_create(&ep->thread, 0, net_thread, ep);
    return ep;
}
uint16_t dp8_port(dp8_ep *ep) { return ep->port; }
int dp8_enum_port_bound(dp8_ep *ep) { return ep->fd_enum >= 0; }
dp8_conn *dp8_connect(dp8_ep *ep, const struct sockaddr_in *to)
{
    pthread_mutex_lock(&ep->lock);
    dp8_conn *c = new_conn(ep, to, ST_CONNECTING, arc4random() | 1);
    send_cframe(c, CMD_CFRAME | CMD_POLL, OP_CONNECT, 0);
    pthread_mutex_unlock(&ep->lock);
    return c;
}
int dp8_send(dp8_conn *c, const void *data, size_t len, uint8_t user_flags)
{
    dp8_ep *ep = c->ep; int ok;
    pthread_mutex_lock(&ep->lock);
    ok = c->state == ST_ESTABLISHED && !c->end_sent && !c->end_recv;
    if (ok) { enqueue(c, data, len, user_flags & (CMD_USER_1 | CMD_USER_2)); pump(c); }
    pthread_mutex_unlock(&ep->lock);
    return ok ? 0 : -1;
}
void dp8_disconnect(dp8_conn *c, int hard)
{
    dp8_ep *ep = c->ep;
    pthread_mutex_lock(&ep->lock);
    if (c->state == ST_ESTABLISHED) {
        if (hard) { c->state = ST_HARD_CLOSING; c->hard_left = 3; c->hard_next = dp8_tick(); conn_free_buffers(c); }
        else c->state = ST_CLOSING;
    } else if (c->state != ST_CLOSED) conn_close(c, DP8_CLOSE_NORMAL);
    pthread_mutex_unlock(&ep->lock);
}
int dp8_pending(dp8_conn *c) { pthread_mutex_lock(&c->ep->lock); int n = in_flight(c) + (c->q ? 1 : 0); pthread_mutex_unlock(&c->ep->lock); return n; }
void dp8_set_user(dp8_conn *c, void *u) { c->user = u; }
void *dp8_user(dp8_conn *c) { return c->user; }
const struct sockaddr_in *dp8_addr(dp8_conn *c) { return &c->addr; }
uint32_t dp8_rtt(dp8_conn *c) { return c->rtt; }
void dp8_close(dp8_ep *ep)
{
    ep->stop = 1; write(ep->wake[1], "x", 1); pthread_join(ep->thread, 0);
    close(ep->fd); if (ep->fd_enum >= 0) close(ep->fd_enum); close(ep->wake[0]); close(ep->wake[1]);
    while (ep->conns) { dp8_conn *c = ep->conns; ep->conns = c->next; conn_free_buffers(c); free(c); }
    while (ep->lagq) { Delayed *d = ep->lagq; ep->lagq = d->next; free(d); }
    while (ep->rxq) { Delayed *d = ep->rxq; ep->rxq = d->next; free(d); }
    while (ep->pend) { Pend *m = ep->pend; ep->pend = m->next; free(m); }
    pthread_mutex_destroy(&ep->lock); pthread_mutex_destroy(&ep->lagm); free(ep);
}
