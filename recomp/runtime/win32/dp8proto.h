/* DirectPlay 8 transport ([MC-DPL8R] reliable protocol, [MC-DPLHP] enumeration) on UDP; see dp8proto.c. */
#ifndef DP8PROTO_H
#define DP8PROTO_H
#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

typedef struct dp8_ep dp8_ep;          /* one UDP port (plus, for hosts, the well-known enumeration port) */
typedef struct dp8_conn dp8_conn;      /* one reliable connection */
enum { DP8_CLOSE_NORMAL = 0, DP8_CLOSE_REMOTE = 1, DP8_CLOSE_LOST = 2 };
enum { DP8_USER_1 = 0x40, DP8_USER_2 = 0x80 };
enum { DP8_ENUM_PORT = 6073, DP8_PORT_LO = 2302, DP8_PORT_HI = 2400 };

typedef struct dp8_callbacks {          /* called on the endpoint's network thread, no lock held */
    void *ctx;
    void (*enum_query)(void *ctx, dp8_ep *ep, const struct sockaddr_in *from, const uint8_t *msg, size_t len);
    void (*enum_response)(void *ctx, dp8_ep *ep, const struct sockaddr_in *from, const uint8_t *msg, size_t len);
    void (*accepted)(void *ctx, dp8_conn *c);                    /* inbound connection established (listening endpoints) */
    void (*connected)(void *ctx, dp8_conn *c);                   /* outbound connection established */
    void (*receive)(void *ctx, dp8_conn *c, uint8_t user, const uint8_t *data, size_t len);
    void (*closed)(void *ctx, dp8_conn *c, int reason);          /* also: an outbound connection that never completed */
} dp8_callbacks;

dp8_ep *dp8_open(uint16_t port_lo, uint16_t port_hi, int listen, uint16_t enum_port, const dp8_callbacks *cb);
void dp8_close(dp8_ep *ep);
uint16_t dp8_port(dp8_ep *ep);
int dp8_enum_port_bound(dp8_ep *ep);
void dp8_send_raw(dp8_ep *ep, const struct sockaddr_in *to, const void *buf, size_t len);
dp8_conn *dp8_connect(dp8_ep *ep, const struct sockaddr_in *to);
int dp8_send(dp8_conn *c, const void *data, size_t len, uint8_t user_flags);     /* reliable, in order */
void dp8_disconnect(dp8_conn *c, int hard);
int dp8_pending(dp8_conn *c);
void dp8_set_user(dp8_conn *c, void *u);
void *dp8_user(dp8_conn *c);
const struct sockaddr_in *dp8_addr(dp8_conn *c);
uint32_t dp8_rtt(dp8_conn *c);
uint32_t dp8_tick(void);

/* frame encoding/decoding, exposed for tests */
typedef struct dp8_dframe { uint8_t cmd, ctl, seq, nrcv; uint64_t sack, sendmask; } dp8_dframe;
size_t dp8_enc_cframe(uint8_t *out, uint8_t cmd, uint8_t op, uint8_t msgid, uint8_t rspid, uint32_t version, uint32_t sessid, uint32_t ts);
size_t dp8_enc_sack(uint8_t *out, uint8_t retry_flag, uint8_t nseq, uint8_t nrcv, uint32_t ts, uint64_t sack_mask);
size_t dp8_enc_dframe(uint8_t *out, uint8_t cmd, uint8_t ctl, uint8_t seq, uint8_t nrcv, uint64_t sack_mask);
size_t dp8_dec_dframe(const uint8_t *p, size_t len, dp8_dframe *f);
int dp8_dec_coalesced(const uint8_t *p, size_t len, void (*fn)(void *, uint8_t user, const uint8_t *, size_t), void *ctx);
#endif
