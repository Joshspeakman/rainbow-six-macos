/* DirectPlay 8 (client/server) for the game's multiplayer, compatible with Microsoft's DirectPlay on Windows: the COM
 * objects with the DirectX 8 method tables, sessions per [MC-DPL8CS] on the transport in dp8proto.c ([MC-DPL8R] reliable
 * delivery, [MC-DPLHP] enumeration). The game's message handler runs on one dispatcher thread, in order, with no lock
 * held. W32_DPLOG=1 logs the API calls and session events. */
#include "w32.h"
#include "../platform.h"
#include "dp8proto.h"
#include <unistd.h>
int w32_spawn_service(void (*fn)(Ctx *, void *), void *arg);
#include <arpa/inet.h>

#define LOG(...) do { if (getenv("W32_DPLOG")) { fprintf(stderr, "dpnet %7.3f: ", dp8_tick() / 1000.0); fprintf(stderr, __VA_ARGS__); } } while (0)
enum { S_OK_ = 0, E_NOINTERFACE_ = 0x80004002u, E_POINTER_ = 0x80004003u, DPNERR_BUFFERTOOSMALL = 0x80158000u + 0x100,
       DPNERR_DOESNOTEXIST = 0x80158000u + 0x170, DPNERR_INVALIDPARAM = 0x80070057u, DPNERR_UNSUPPORTED = 0x80004001u,
       DPNERR_GENERIC = 0x80004005u };
static const uint8_t GUID_TCPIP[16] = {0xa0, 0x7b, 0xfe, 0xeb, 0x8d, 0x62, 0xd2, 0x11, 0xae, 0x0f, 0x00, 0x60, 0x97, 0xb0, 0x14, 0x11};   /* CLSID_DP8SP_TCPIP */

/* ---- objects: a guest block {vtable, kind, index} ---- */
enum { K_CLIENT = 1, K_SERVER, K_ADDRESS };
typedef struct Comp { char name[64]; uint32_t type; uint8_t *data; uint32_t size; } Comp;
typedef struct Obj {
    int used, kind, refs; uint32_t g;              /* guest object */
    uint32_t handler, context;                     /* client/server: the game's message handler */
    Comp comps[32]; int ncomps;                    /* address: components in insertion order */
    uintptr_t context_sess;                        /* client/server: the Sess */
} Obj;
static Obj objs[256]; static pthread_mutex_t olock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vt[4];
static Obj *O(uint32_t g) { uint32_t k = g ? rt_r32(G_MEM, g + 8) : 999; return k < 256 && objs[k].used && objs[k].g == g ? &objs[k] : 0; }
static uint32_t obj_new(int kind)
{
    pthread_mutex_lock(&olock);
    int k = 0; while (k < 256 && objs[k].used) k++;
    if (k == 256) { pthread_mutex_unlock(&olock); return 0; }
    Obj *o = &objs[k]; memset(o, 0, sizeof *o); o->used = 1; o->kind = kind; o->refs = 1;
    o->g = heap_alloc(w32_process_heap, 8, 16); rt_w32(G_MEM, o->g, vt[kind]); rt_w32(G_MEM, o->g + 4, (uint32_t)kind); rt_w32(G_MEM, o->g + 8, (uint32_t)k);
    pthread_mutex_unlock(&olock);
    return o->g;
}
static void obj_free(Obj *o) { if (!o) return; for (int i = 0; i < o->ncomps; i++) free(o->comps[i].data); o->used = 0; }
/* drop the reference DirectPlay holds on an address it handed to the game (the game AddRefs what it keeps) */
static void obj_release(Obj *o) { if (o && --o->refs <= 0) obj_free(o); }

/* ---- wide strings in guest memory ---- */
static void wstr_get(uint32_t p, char *out, size_t cap) { size_t n = 0; for (; n + 1 < cap; n++) { uint16_t ch = (uint16_t)rt_r16(G_MEM, p + 2 * (uint32_t)n); if (!ch) break; out[n] = ch < 128 ? (char)ch : '?'; } out[n] = 0; }

/* ---- IDirectPlay8Address ---- */
enum { DT_STRING = 1, DT_DWORD = 2, DT_GUID = 3, DT_BINARY = 4, DT_STRING_ANSI = 5 };
static Comp *comp_find(Obj *o, const char *name) { for (int i = 0; i < o->ncomps; i++) if (!strcasecmp(o->comps[i].name, name)) return &o->comps[i]; return 0; }
static int comp_set(Obj *o, const char *name, uint32_t type, const void *data, uint32_t size)
{
    Comp *c = comp_find(o, name);
    if (!c) { if (o->ncomps == 32) return -1; c = &o->comps[o->ncomps++]; memset(c, 0, sizeof *c); snprintf(c->name, sizeof c->name, "%s", name); }
    free(c->data); c->data = malloc(size ? size : 1); memcpy(c->data, data, size); c->size = size; c->type = type;
    return 0;
}
/* strings are kept as UTF-16 (what the W calls hand out), including the terminator */
static void comp_set_str(Obj *o, const char *name, const char *s)
{
    size_t n = strlen(s); uint16_t *w = malloc(2 * (n + 1)); for (size_t i = 0; i <= n; i++) w[i] = (uint8_t)s[i];
    comp_set(o, name, DT_STRING, w, (uint32_t)(2 * (n + 1))); free(w);
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void url_unescape(const char *s, char *out, size_t cap)
{
    size_t n = 0; for (; *s && n + 1 < cap; s++) { if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { out[n++] = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2; } else out[n++] = *s; }
    out[n] = 0;
}
static int parse_guid(const char *s, uint8_t g[16])
{
    unsigned a, b, c, d[8];
    if (sscanf(s, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return -1;
    memcpy(g, &a, 4); uint16_t b16 = (uint16_t)b, c16 = (uint16_t)c; memcpy(g + 4, &b16, 2); memcpy(g + 6, &c16, 2); for (int i = 0; i < 8; i++) g[8 + i] = (uint8_t)d[i];
    return 0;
}
static void fmt_guid(const uint8_t g[16], char *out)
{
    uint32_t a; uint16_t b, c; memcpy(&a, g, 4); memcpy(&b, g + 4, 2); memcpy(&c, g + 6, 2);
    sprintf(out, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", a, b, c, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
/* x-directplay:/provider=%7B...%7D;hostname=1.2.3.4;port=2302 */
static int build_from_url(Obj *o, const char *url)
{
    const char *p = strstr(url, ":/"); if (!p) return -1; p += 2;
    for (int i = 0; i < o->ncomps; i++) free(o->comps[i].data); o->ncomps = 0;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", p); char *save, *tok = strtok_r(buf, ";", &save);
    while (tok) {
        char *eq = strchr(tok, '='); if (eq) {
            *eq = 0; char val[512]; url_unescape(eq + 1, val, sizeof val);
            uint8_t g[16];
            if (!parse_guid(val, g)) comp_set(o, tok, DT_GUID, g, 16);
            else { char *end; unsigned long v = strtoul(val, &end, 10); if (*val && !*end && strcasecmp(tok, "hostname")) comp_set(o, tok, DT_DWORD, &(uint32_t){(uint32_t)v}, 4); else comp_set_str(o, tok, val); }
        }
        tok = strtok_r(0, ";", &save);
    }
    return 0;
}
static void get_url(Obj *o, char *out, size_t cap)
{
    size_t n = (size_t)snprintf(out, cap, "x-directplay:/");
    for (int i = 0; i < o->ncomps && n < cap; i++) {
        Comp *c = &o->comps[i]; char v[256] = "";
        if (c->type == DT_GUID && c->size == 16) { char g[40]; fmt_guid(c->data, g); snprintf(v, sizeof v, "%%7B%.36s%%7D", g + 1); }
        else if (c->type == DT_DWORD && c->size == 4) snprintf(v, sizeof v, "%u", *(uint32_t *)c->data);
        else if (c->type == DT_STRING) { size_t k = 0; for (; k + 1 < sizeof v && k < c->size / 2 && ((uint16_t *)c->data)[k]; k++) v[k] = (char)((uint16_t *)c->data)[k]; v[k] = 0; }
        else if (c->type == DT_STRING_ANSI) snprintf(v, sizeof v, "%.*s", (int)c->size, (char *)c->data);
        n += (size_t)snprintf(out + n, cap - n, "%s%s=%s", i ? ";" : "", c->name, v);
    }
}
static void a_QueryInterface(Ctx *c) { uint32_t p = ARG(2); if (p) { rt_w32(G_MEM, p, ARG(0)); Obj *o = O(ARG(0)); if (o) o->refs++; } RET(S_OK_, 3); }
static void a_AddRef(Ctx *c) { Obj *o = O(ARG(0)); RET(o ? ++o->refs : 0, 1); }
static void a_Release(Ctx *c) { Obj *o = O(ARG(0)); if (!o) RET(0, 1); int r = --o->refs; if (!r) obj_free(o); RET(r, 1); }
static void a_BuildFromURLW(Ctx *c) { Obj *o = O(ARG(0)); char u[1024]; wstr_get(ARG(1), u, sizeof u); LOG("Address.BuildFromURLW %s\n", u); RET(o && !build_from_url(o, u) ? S_OK_ : DPNERR_INVALIDPARAM, 2); }
static void a_BuildFromURLA(Ctx *c) { Obj *o = O(ARG(0)); LOG("Address.BuildFromURLA %s\n", GS(ARG(1))); RET(o && !build_from_url(o, GS(ARG(1))) ? S_OK_ : DPNERR_INVALIDPARAM, 2); }
static void a_Duplicate(Ctx *c)
{
    Obj *o = O(ARG(0)); uint32_t g = obj_new(K_ADDRESS); Obj *n = O(g); if (!o || !n) RET(DPNERR_GENERIC, 2);
    for (int i = 0; i < o->ncomps; i++) comp_set(n, o->comps[i].name, o->comps[i].type, o->comps[i].data, o->comps[i].size);
    rt_w32(G_MEM, ARG(1), g); RET(S_OK_, 2);
}
static void a_SetEqual(Ctx *c)
{
    Obj *o = O(ARG(0)), *s = O(ARG(1)); if (!o || !s) RET(DPNERR_INVALIDPARAM, 2);
    for (int i = 0; i < o->ncomps; i++) free(o->comps[i].data); o->ncomps = 0;
    for (int i = 0; i < s->ncomps; i++) comp_set(o, s->comps[i].name, s->comps[i].type, s->comps[i].data, s->comps[i].size);
    RET(S_OK_, 2);
}
static void a_IsEqual(Ctx *c) { Obj *o = O(ARG(0)), *s = O(ARG(1)); char a[1024], b[1024]; if (!o || !s) RET(DPNERR_INVALIDPARAM, 2); get_url(o, a, sizeof a); get_url(s, b, sizeof b); RET(strcasecmp(a, b) ? 1 : S_OK_, 2); }
static void a_Clear(Ctx *c) { Obj *o = O(ARG(0)); if (o) { for (int i = 0; i < o->ncomps; i++) free(o->comps[i].data); o->ncomps = 0; } RET(S_OK_, 1); }
static void url_out(Ctx *c, int wide)
{
    Obj *o = O(ARG(0)); uint32_t buf = ARG(1), pn = ARG(2); char u[1024]; if (!o) RET(DPNERR_INVALIDPARAM, 3);
    get_url(o, u, sizeof u); uint32_t need = (uint32_t)strlen(u) + 1, have = rt_r32(G_MEM, pn);
    rt_w32(G_MEM, pn, need);
    if (!buf || have < need) RET(DPNERR_BUFFERTOOSMALL, 3);
    for (uint32_t i = 0; i < need; i++) { if (wide) rt_w16(G_MEM, buf + 2 * i, (uint8_t)u[i]); else G_MEM[buf + i] = (uint8_t)u[i]; }
    LOG("Address.GetURL -> %s\n", u); RET(S_OK_, 3);
}
static void a_GetURLW(Ctx *c) { url_out(c, 1); }
static void a_GetURLA(Ctx *c) { url_out(c, 0); }
static void a_GetSP(Ctx *c) { Obj *o = O(ARG(0)); Comp *k = o ? comp_find(o, "provider") : 0; if (!k || k->size != 16) RET(DPNERR_DOESNOTEXIST, 2); memcpy(GP(ARG(1)), k->data, 16); RET(S_OK_, 2); }
static void a_GetUserData(Ctx *c) { RET(DPNERR_DOESNOTEXIST, 3); }
static void a_SetSP(Ctx *c) { Obj *o = O(ARG(0)); if (!o) RET(DPNERR_INVALIDPARAM, 2); comp_set(o, "provider", DT_GUID, GP(ARG(1)), 16); LOG("Address.SetSP\n"); RET(S_OK_, 2); }
static void a_SetUserData(Ctx *c) { RET(S_OK_, 3); }
static void a_GetNumComponents(Ctx *c) { Obj *o = O(ARG(0)); rt_w32(G_MEM, ARG(1), o ? (uint32_t)o->ncomps : 0); RET(S_OK_, 2); }
static void comp_out(Ctx *c, Comp *k, uint32_t buf, uint32_t psize, uint32_t ptype, int nargs)
{
    uint32_t have = rt_r32(G_MEM, psize); rt_w32(G_MEM, psize, k->size); if (ptype) rt_w32(G_MEM, ptype, k->type);
    if (!buf || have < k->size) RET(DPNERR_BUFFERTOOSMALL, nargs);
    memcpy(GP(buf), k->data, k->size); RET(S_OK_, nargs);
}
static void a_GetComponentByName(Ctx *c)
{
    Obj *o = O(ARG(0)); char n[64]; wstr_get(ARG(1), n, sizeof n); Comp *k = o ? comp_find(o, n) : 0;
    LOG("Address.GetComponentByName %s -> %s\n", n, k ? "found" : "missing");
    if (!k) RET(DPNERR_DOESNOTEXIST, 5);
    comp_out(c, k, ARG(2), ARG(3), ARG(4), 5);
}
static void a_GetComponentByIndex(Ctx *c)
{
    Obj *o = O(ARG(0)); uint32_t i = ARG(1); if (!o || i >= (uint32_t)o->ncomps) RET(DPNERR_DOESNOTEXIST, 7);
    Comp *k = &o->comps[i]; uint32_t pname = ARG(2), pnlen = ARG(3), nl = (uint32_t)strlen(k->name) + 1, have = rt_r32(G_MEM, pnlen);
    rt_w32(G_MEM, pnlen, nl);
    if (!pname || have < nl) { rt_w32(G_MEM, ARG(5), k->size); RET(DPNERR_BUFFERTOOSMALL, 7); }
    for (uint32_t j = 0; j < nl; j++) rt_w16(G_MEM, pname + 2 * j, (uint8_t)k->name[j]);
    comp_out(c, k, ARG(4), ARG(5), ARG(6), 7);
}
static void a_AddComponent(Ctx *c)
{
    Obj *o = O(ARG(0)); char n[64]; wstr_get(ARG(1), n, sizeof n); uint32_t data = ARG(2), size = ARG(3), type = ARG(4);
    if (!o) RET(DPNERR_INVALIDPARAM, 5);
    if (type == DT_STRING_ANSI) { char s[512]; snprintf(s, sizeof s, "%.*s", (int)size, GS(data)); comp_set_str(o, n, s); }
    else comp_set(o, n, type, GP(data), size);
    if (getenv("W32_DPLOG")) { char u[1024]; get_url(o, u, sizeof u); LOG("Address.AddComponent %s (type %u, %u bytes) -> %s\n", n, type, size, u); }
    RET(S_OK_, 5);
}
static void a_GetDevice(Ctx *c) { Obj *o = O(ARG(0)); Comp *k = o ? comp_find(o, "device") : 0; if (!k || k->size != 16) RET(DPNERR_DOESNOTEXIST, 2); memcpy(GP(ARG(1)), k->data, 16); RET(S_OK_, 2); }
static void a_SetDevice(Ctx *c) { Obj *o = O(ARG(0)); if (!o) RET(DPNERR_INVALIDPARAM, 2); comp_set(o, "device", DT_GUID, GP(ARG(1)), 16); RET(S_OK_, 2); }
static void a_BuildFromDirectPlay4Address(Ctx *c) { RET(DPNERR_UNSUPPORTED, 3); }

/* ================================================================ sessions ([MC-DPL8CS], client/server mode) */
enum {
    MSG_CONNECT_INFO = 0xC1, MSG_SEND_CONNECT_INFO = 0xC2, MSG_ACK_CONNECT_INFO = 0xC3, MSG_CONNECT_FAILED = 0xC5,
    MSG_DESTROY_PLAYER = 0xD1, MSG_REQ_UPDATE_INFO = 0xD6, MSG_UPDATE_INFO = 0xDB, MSG_TERMINATE_SESSION = 0xDF, MSG_REQ_PROCESS_COMPLETION = 0xE0, MSG_PROCESS_COMPLETION = 0xE1, MSG_UPDATE_APPLICATION_DESC = 0xCF,
    NT_LOCAL = 0x1, NT_HOST = 0x2, NT_CLIENT = 0x200, NT_SERVER = 0x400,
    SESS_CLIENT_SERVER = 0x1, SESS_REQUIREPASSWORD = 0x80,
    DNET_VERSION = 2,                                   /* DirectX 8.1, like the game's own SDK */
};
/* DirectPlay messages to the game (DPN_MSGID_*) */
enum { M_APPLICATION_DESC = 0xFFFF0001u, M_ASYNC_OP_COMPLETE = 0xFFFF0003u, M_CLIENT_INFO = 0xFFFF0004u, M_CONNECT_COMPLETE = 0xFFFF0005u, M_CREATE_PLAYER = 0xFFFF0007u,
       M_DESTROY_PLAYER = 0xFFFF0009u, M_ENUM_HOSTS_QUERY = 0xFFFF000Au, M_ENUM_HOSTS_RESPONSE = 0xFFFF000Bu,
       M_INDICATE_CONNECT = 0xFFFF000Eu, M_INDICATED_CONNECT_ABORTED = 0xFFFF000Fu, M_RECEIVE = 0xFFFF0011u,
       M_RETURN_BUFFER = 0xFFFF0013u, M_SEND_COMPLETE = 0xFFFF0014u, M_SERVER_INFO = 0xFFFF0015u, M_TERMINATE_SESSION = 0xFFFF0016u };
enum { DPNSUCCESS_PENDING = 0x0015800Eu, DPNSUCCESS_EQUAL = 0x00158005u, DPNSUCCESS_NOTEQUAL = 0x0015800Au,
       DPNERR_CONNECTIONLOST = 0x801580C0u, DPNERR_HOSTTERMINATEDSESSION = 0x80158270u, DPNERR_HOSTREJECTEDCONNECTION = 0x80158260u,
       DPNERR_INVALIDINSTANCE = 0x80158380u, DPNERR_INVALIDPASSWORD = 0x80158410u, DPNERR_INVALIDPLAYER = 0x80158420u,
       DPNERR_NORESPONSE = 0x801584D0u, DPNERR_USERCANCEL = 0x80158570u, DPNERR_NOCONNECTION = 0x801584A0u };
enum { DPNSEND_SYNC = 0x80000000u, DPNSEND_NOCOMPLETE = 0x2, DPNSEND_COMPLETEONPROCESS = 0x4, DPNSEND_NOLOOPBACK = 0x20,
       DPNCONNECT_SYNC = 0x80000000u, DPNCANCEL_CONNECT = 0x1, DPNCANCEL_ENUM = 0x2, DPNCANCEL_SEND = 0x4, DPNCANCEL_ALL = 0x8000 };

typedef struct Blob { uint8_t *p; uint32_t n; } Blob;
static void blob_set(Blob *b, const void *p, uint32_t n) { free(b->p); b->p = n ? malloc(n) : 0; if (n) memcpy(b->p, p, n); b->n = n; }
typedef struct Player {
    int used; uint32_t dpnid, ctx; dp8_conn *conn; int created, leaving, indicated;
    Blob name, data; uint32_t dnet_version; struct sockaddr_in addr;
} Player;
typedef struct EnumOp { int used; uint32_t handle, ctx, count, retry, timeout, started, next, sent, last_tx; Blob user;
                        struct sockaddr_in dest[24]; int ndest; uint32_t device; } EnumOp;
typedef struct Sess {
    Obj *o; int server; dp8_ep *ep; pthread_mutex_t m;
    Blob info_name, info_data;                       /* SetClientInfo / SetServerInfo */
    /* application description (host: ours; client: the server's) */
    uint32_t flags, maxplayers; uint8_t instance[16], app[16]; Blob sess_name, password, app_reserved;
    /* server */
    int hosting; Player pl[256]; uint32_t ntver, server_dpnid, server_ctx;
    /* client */
    dp8_conn *srv; int connecting, connected; uint32_t conn_handle, conn_ctx, conn_hr, local_dpnid; Blob conn_data, conn_reply;
    pthread_cond_t conn_cv; Player server_pl; int in_connect_call;
    EnumOp en[8]; int enum_thread;
} Sess;
static Sess *sess_of(Obj *o) { return o ? (Sess *)(uintptr_t)o->context_sess : 0; }
/* the session this Mac hosts / has joined (sessions are never freed), for dpnet_host_max_rtt / dpnet_client_rtt */
static Sess *g_hosting, *g_joined;

/* ---- byte buffers for wire messages ---- */
typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;
static void bput(Buf *b, const void *d, size_t n) { if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); } memcpy(b->p + b->n, d, n); b->n += n; }
static void bput32(Buf *b, uint32_t v) { bput(b, &v, 4); }
static void bset32(Buf *b, size_t at, uint32_t v) { memcpy(b->p + at, &v, 4); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void hexlog(const char *what, const uint8_t *p, uint32_t n)
{
    if (!getenv("W32_DPLOG") || atoi(getenv("W32_DPLOG")) < 2) return;
    char hex[3 * 128 + 1] = ""; uint32_t k = 0; for (; k < n && k < 128; k++) sprintf(hex + 3 * k, "%02x ", p[k]);
    LOG("%s (%u bytes): %s\n", what, n, hex);
}
/* a field at "offset from the end of dwPacketType" (msg points at dwPacketType) */
static int field(const uint8_t *msg, size_t len, uint32_t off, uint32_t size, Blob *out)
{
    memset(out, 0, sizeof *out); if (!off || !size) return 0;
    if ((size_t)off + 4 + size > len) return -1;
    out->p = (uint8_t *)msg + 4 + off; out->n = size; return 0;
}
static uint32_t make_dpnid(Sess *s, uint32_t index) { return ((s->ntver & 0xfff) << 20 | (index & 0xfffff)) ^ rd32(s->instance); }
static Player *player_by_id(Sess *s, uint32_t id) { uint32_t i = (id ^ rd32(s->instance)) & 0xfffff; return i < 256 && s->pl[i].used && s->pl[i].dpnid == id ? &s->pl[i] : 0; }
static Player *player_by_conn(Sess *s, dp8_conn *c) { for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].conn == c) return &s->pl[i]; return 0; }
static void ip_str(const struct sockaddr_in *a, char *out, size_t cap) { inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)cap); }
static uint32_t addr_dup(uint32_t g)
{
    Obj *src = O(g); uint32_t d = obj_new(K_ADDRESS); Obj *dst = O(d);
    if (src && dst) for (int i = 0; i < src->ncomps; i++) comp_set(dst, src->comps[i].name, src->comps[i].type, src->comps[i].data, src->comps[i].size);
    return d;
}
static uint32_t address_object(const struct sockaddr_in *a)          /* x-directplay:/provider=TCP/IP;hostname=…;port=… */
{
    uint32_t g = obj_new(K_ADDRESS); Obj *o = O(g); if (!o) return 0;
    char ip[64]; ip_str(a, ip, sizeof ip); uint32_t port = ntohs(a->sin_port);
    comp_set(o, "provider", DT_GUID, GUID_TCPIP, 16); comp_set_str(o, "hostname", ip); comp_set(o, "port", DT_DWORD, &port, 4);
    return g;
}
static void address_url(const struct sockaddr_in *a, char *out, size_t cap)
{
    char ip[64]; ip_str(a, ip, sizeof ip);
    snprintf(out, cap, "x-directplay:/provider=%%7BEBFE7BA0-628D-11D2-AE0F-006097B01411%%7D;hostname=%s;port=%u", ip, ntohs(a->sin_port));
}
static uint32_t gstr_w(const Blob *b)                                  /* guest copy of a UTF-16 string (0 if none) */
{
    if (!b->n || b->n > 0x100000) return 0; uint32_t g = heap_alloc(w32_process_heap, 8, b->n + 2); if (g) memcpy(GP(g), b->p, b->n); return g;
}
static uint32_t gcopy(const void *p, uint32_t n) { if (!n) return 0; uint32_t g = heap_alloc(w32_process_heap, 0, n); if (g) memcpy(GP(g), p, n); return g; }
static void gfree(uint32_t g) { if (g) heap_free(w32_process_heap, g); }
static void wide_blob(Blob *b, uint32_t p)                             /* guest UTF-16 string -> blob incl. terminator */
{
    if (!p) { blob_set(b, 0, 0); return; }
    uint32_t n = 0; while (rt_r16(G_MEM, p + 2 * n)) n++;
    blob_set(b, GP(p), 2 * (n + 1));
}

/* ================================================================ the dispatcher: DirectPlay messages to the game */
typedef struct Ev {
    struct Ev *next; Sess *s; uint32_t msg; uint32_t a[12]; Blob d1, d2; struct sockaddr_in addr; dp8_conn *conn;
    int (*post)(Ctx *, struct Ev *, uint32_t pmsg, uint32_t hr);      /* after the handler (read results, reply on the wire) */
    uint32_t pmsg_size; uint64_t queued_us;
} Ev;
static int enum_events; enum { MAX_ENUM_EVENTS = 32 };        /* enumeration queries/responses waiting for the game */
static Ev *evq, *evq_tail; static pthread_mutex_t evm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t evcv = PTHREAD_COND_INITIALIZER;
static int dispatcher_started;
static void dispatch_loop(Ctx *c, void *arg);
static uint64_t now_us(void);
static void post_event(Ev *e)
{
    pthread_mutex_lock(&evm);
    if (!dispatcher_started) { dispatcher_started = 1; w32_spawn_service(dispatch_loop, 0); }
    e->queued_us = now_us();
    if (evq_tail) evq_tail->next = e; else evq = e; evq_tail = e;
    pthread_cond_signal(&evcv); pthread_mutex_unlock(&evm);
}
static Ev *new_event(Sess *s, uint32_t msg) { Ev *e = calloc(1, sizeof *e); e->s = s; e->msg = msg; return e; }
/* DP8_STATS=1: every 5 s, how long received messages wait in the queue before the game's handler runs, and how long
 * the handler takes (the game's own processing) */
static uint64_t now_us(void) { return ds_clock_ns() / 1000; }
static int stats_on(void) { static int on = -1; if (on < 0) on = getenv("DP8_STATS") != 0; return on; }
static struct { uint64_t t0, n, wait, wait_max, handler, handler_max; } st;
static void stats_report(void)
{
    uint64_t now = now_us(); if (!st.t0) st.t0 = now;
    if (now - st.t0 < 5000000 || !st.n) return;
    fprintf(stderr, "dp8 stats: %llu received; queue wait avg %.2f ms, max %.2f ms; handler avg %.2f ms, max %.2f ms\n",
            (unsigned long long)st.n, st.wait / 1000.0 / st.n, st.wait_max / 1000.0, st.handler / 1000.0 / st.n, st.handler_max / 1000.0);
    memset(&st, 0, sizeof st); st.t0 = now;
}
static uint32_t call_handler(Ctx *c, Sess *s, uint32_t msg, uint32_t pmsg)
{
    if (!s->o->handler) return 0;
    uint32_t a[3] = {s->o->context, msg, pmsg};
    return w32_callback(c, s->o->handler, 3, a);
}
static void dispatch_loop(Ctx *c, void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&evm);
        while (!evq) pthread_cond_wait(&evcv, &evm);
        Ev *e = evq; evq = e->next; if (!evq) evq_tail = 0;
        pthread_mutex_unlock(&evm);
        uint32_t pm = 0, hr = 0;
        uint64_t got_us = 0;
        if (e->msg == M_RECEIVE && stats_on()) { got_us = now_us(); st.n++; uint64_t w = got_us - e->queued_us; st.wait += w; if (w > st.wait_max) st.wait_max = w; }
        if (e->msg == M_CONNECT_COMPLETE) {   /* never before the game's Connect call has returned (a fast loopback can) */
            pthread_mutex_lock(&e->s->m); while (e->s->in_connect_call) pthread_cond_wait(&e->s->conn_cv, &e->s->m); pthread_mutex_unlock(&e->s->m);
        }
        if (e->pmsg_size) { pm = heap_alloc(w32_process_heap, 8, e->pmsg_size); rt_w32(G_MEM, pm, e->pmsg_size); for (uint32_t i = 1; i < e->pmsg_size / 4; i++) rt_w32(G_MEM, pm + 4 * i, e->a[i - 1]); }
        if (e->s->o->used) {
            LOG("-> game: message %#x\n", e->msg);
            hr = call_handler(c, e->s, e->msg, pm);
        }
        if (got_us) { uint64_t h = now_us() - got_us; st.handler += h; if (h > st.handler_max) st.handler_max = h; stats_report(); }
        int keep = e->post ? e->post(c, e, pm, hr) : 0;
        if (pm && !keep) gfree(pm);
        free(e->d1.p); free(e->d2.p); free(e);
    }
}
static uint32_t next_handle(void) { static uint32_t h = 0x100; return __atomic_add_fetch(&h, 1, __ATOMIC_RELAXED); }
/* keep a received buffer until ReturnBuffer when the handler answers DPNSUCCESS_PENDING */
static uint32_t pending_bufs[256];
static int post_receive(Ctx *c, Ev *e, uint32_t pm, uint32_t hr)
{
    (void)c; uint32_t data = e->a[2], h = e->a[4];
    if (hr == DPNSUCCESS_PENDING) { pending_bufs[h & 255] = data; return 0; }
    gfree(data); return 0;
    (void)pm;
}
static void ev_receive(Sess *s, uint32_t from, uint32_t ctx, const uint8_t *d, size_t n)
{
    if (getenv("W32_DPLOG") && atoi(getenv("W32_DPLOG")) >= 2) {
        char hex[3 * 160 + 1] = ""; size_t k = 0; for (; k < n && k < 160; k++) sprintf(hex + 3 * k, "%02x ", d[k]);
        LOG("recv %zu bytes from %08x: %s%s\n", n, from, hex, n > 160 ? "..." : "");
    }
    Ev *e = new_event(s, M_RECEIVE); uint32_t h = next_handle();
    e->a[0] = from; e->a[1] = ctx; e->a[2] = gcopy(d, (uint32_t)n); e->a[3] = (uint32_t)n; e->a[4] = h;
    e->pmsg_size = 24; e->post = post_receive; post_event(e);
}
static void ev_simple(Sess *s, uint32_t msg, uint32_t size, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    Ev *e = new_event(s, msg); e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; e->a[3] = a3; e->a[4] = a4; e->pmsg_size = size; post_event(e);
}

/* ================================================================ wire messages */
static void send_core(dp8_conn *c, Buf *b) { dp8_send(c, b->p, b->n, DP8_USER_1); free(b->p); }
static void put_blob_field(Buf *b, size_t hdr_off, size_t size_off, const Blob *v)   /* append v, fill its offset/size */
{
    if (!v->n) return;
    bset32(b, hdr_off, (uint32_t)(b->n - 4)); bset32(b, size_off, v->n); bput(b, v->p, v->n);
}
/* client -> server: DN_INTERNAL_MESSAGE_PLAYER_CONNECT_INFO ([MC-DPL8CS] 2.2.1.1) */
static void send_connect_info(Sess *s, dp8_conn *c, const struct sockaddr_in *local)
{
    Buf b = {0}; bput32(&b, MSG_CONNECT_INFO); bput32(&b, 0x2 /* client */); bput32(&b, DNET_VERSION);
    for (int i = 0; i < 10; i++) bput32(&b, 0);                    /* name, data, password, connect data, url: offsets/sizes */
    bput(&b, s->instance, 16); bput(&b, s->app, 16);
    char url[256]; address_url(local, url, sizeof url); Blob u = {(uint8_t *)url, (uint32_t)strlen(url) + 1};
    put_blob_field(&b, 44, 48, &u); put_blob_field(&b, 36, 40, &s->conn_data); put_blob_field(&b, 28, 32, &s->password);
    put_blob_field(&b, 20, 24, &s->info_data); put_blob_field(&b, 12, 16, &s->info_name);
    send_core(c, &b);
}
/* server -> client: DN_SEND_CONNECT_INFO (2.2.1.4) with the two client/server name table entries */
static void send_connect_reply(Sess *s, Player *p, const Blob *reply)
{
    Buf b = {0}; bput32(&b, MSG_SEND_CONNECT_INFO); bput32(&b, 0); bput32(&b, 0);
    bput32(&b, 0x50); bput32(&b, s->flags & ~(uint32_t)SESS_CLIENT_SERVER); bput32(&b, s->maxplayers);
    uint32_t cur = 1; for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created) cur++;
    bput32(&b, cur);
    for (int i = 0; i < 8; i++) bput32(&b, 0);                     /* session name, password, reserved, app reserved */
    bput(&b, s->instance, 16); bput(&b, s->app, 16);
    bput32(&b, p->dpnid); bput32(&b, s->ntver); bput32(&b, 0); bput32(&b, 2); bput32(&b, 0);
    size_t e1 = b.n; for (int i = 0; i < 12; i++) bput32(&b, 0);  /* server entry */
    size_t e2 = b.n; for (int i = 0; i < 12; i++) bput32(&b, 0);  /* the joining client */
    bset32(&b, e1, s->server_dpnid); bset32(&b, e1 + 8, NT_HOST | NT_SERVER); bset32(&b, e1 + 12, 1); bset32(&b, e1 + 20, DNET_VERSION);
    bset32(&b, e2, p->dpnid); bset32(&b, e2 + 8, NT_CLIENT); bset32(&b, e2 + 12, s->ntver); bset32(&b, e2 + 20, p->dnet_version);
    char url[256]; address_url(&p->addr, url, sizeof url); Blob u = {(uint8_t *)url, (uint32_t)strlen(url) + 1};
    put_blob_field(&b, e2 + 40, e2 + 44, &u);
    put_blob_field(&b, e2 + 32, e2 + 36, &p->data); put_blob_field(&b, e2 + 24, e2 + 28, &p->name);
    put_blob_field(&b, e1 + 32, e1 + 36, &s->info_data); put_blob_field(&b, e1 + 24, e1 + 28, &s->info_name);
    put_blob_field(&b, 52, 56, &s->app_reserved);
    if (s->flags & SESS_REQUIREPASSWORD) put_blob_field(&b, 36, 40, &s->password);
    put_blob_field(&b, 28, 32, &s->sess_name);
    if (reply) put_blob_field(&b, 4, 8, reply);
    send_core(p->conn, &b);
}
static void send_connect_failed(dp8_conn *c, uint32_t hr, const Blob *reply)
{
    Buf b = {0}; bput32(&b, MSG_CONNECT_FAILED); bput32(&b, hr); bput32(&b, 0); bput32(&b, 0);
    if (reply) put_blob_field(&b, 8, 12, reply);
    send_core(c, &b);
}
/* server -> every client: the session's new application description after SetApplicationDesc (the game keeps its state
 * in the description's reserved data, and Windows clients wait for it when a game starts); offsets count from after
 * the message type, the strings follow the 0x50-byte description as in Microsoft's own messages */
static void send_app_desc_update(Sess *s)
{
    for (int i = 0; i < 256; i++) {
        Player *p = &s->pl[i]; if (!p->used || !p->created || !p->conn) continue;
        Buf b = {0}; bput32(&b, MSG_UPDATE_APPLICATION_DESC);
        size_t a = b.n; bput32(&b, 0x50); bput32(&b, s->flags & ~(uint32_t)SESS_CLIENT_SERVER); bput32(&b, s->maxplayers);
        uint32_t cur = 1; for (int k = 0; k < 256; k++) if (s->pl[k].used && s->pl[k].created) cur++;
        bput32(&b, cur); for (int k = 0; k < 8; k++) bput32(&b, 0);
        bput(&b, s->instance, 16); bput(&b, s->app, 16);
        put_blob_field(&b, a + 40, a + 44, &s->app_reserved);
        if (s->flags & SESS_REQUIREPASSWORD) put_blob_field(&b, a + 24, a + 28, &s->password);
        put_blob_field(&b, a + 16, a + 20, &s->sess_name);
        send_core(p->conn, &b);
    }
}
/* DN_UPDATE_INFO ([MC-DPL8CS] 2.2.5.2): a player's new name/data, server -> client */
static void send_update_info(Sess *s, dp8_conn *c, uint32_t context, uint32_t dpnid, uint32_t requesting, const Blob *name, const Blob *data)
{
    Buf b = {0}; bput32(&b, MSG_UPDATE_INFO); bput32(&b, context); bput32(&b, dpnid); bput32(&b, s->ntver); bput32(&b, 0);
    bput32(&b, (name->n ? 1 : 0) | (data->n ? 2 : 0)); for (int i = 0; i < 4; i++) bput32(&b, 0); bput32(&b, requesting);
    put_blob_field(&b, 32, 36, data); put_blob_field(&b, 24, 28, name);
    send_core(c, &b);
}
/* DN_REQ_UPDATE_INFO (2.2.5.1): our new name/data, client -> server */
static void send_req_update_info(Sess *s, dp8_conn *c)
{
    Buf b = {0}; bput32(&b, MSG_REQ_UPDATE_INFO); bput32(&b, next_handle()); bput32(&b, s->local_dpnid);
    bput32(&b, (s->info_name.n ? 1 : 0) | (s->info_data.n ? 2 : 0)); for (int i = 0; i < 4; i++) bput32(&b, 0);
    put_blob_field(&b, 24, 28, &s->info_data); put_blob_field(&b, 16, 20, &s->info_name);
    send_core(c, &b);
}
static void send_terminate(dp8_conn *c, const Blob *data)
{
    Buf b = {0}; bput32(&b, MSG_TERMINATE_SESSION); bput32(&b, 0); bput32(&b, 0);
    if (data) put_blob_field(&b, 4, 8, data);
    send_core(c, &b);
}

/* ================================================================ server side */
static void player_free(Player *p) { free(p->name.p); free(p->data.p); memset(p, 0, sizeof *p); }
static int post_create_player(Ctx *c, Ev *e, uint32_t pm, uint32_t hr)
{
    (void)c; (void)hr; Sess *s = e->s;
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, e->a[0]); if (p) p->ctx = rt_r32(G_MEM, pm + 8); pthread_mutex_unlock(&s->m);
    return 0;
}
static int post_destroy_player(Ctx *c, Ev *e, uint32_t pm, uint32_t hr)
{
    (void)c; (void)pm; (void)hr; Sess *s = e->s;
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, e->a[0]); if (p) player_free(p); pthread_mutex_unlock(&s->m);
    return 0;
}
/* DPNMSG_INDICATE_CONNECT: the game accepts (S_OK, optional reply data and player context) or rejects */
static int post_indicate_connect(Ctx *c, Ev *e, uint32_t pm, uint32_t hr)
{
    Sess *s = e->s; uint32_t reply = rt_r32(G_MEM, pm + 12), rsize = rt_r32(G_MEM, pm + 16), rctx = rt_r32(G_MEM, pm + 20), pctx = rt_r32(G_MEM, pm + 24);
    Blob r = {0}; if (reply && rsize) blob_set(&r, GP(reply), rsize);
    gfree(e->a[0]); obj_release(O(e->a[6])); obj_release(O(e->a[7]));
    pthread_mutex_lock(&s->m);
    Player *p = e->conn ? player_by_conn(s, e->conn) : 0;                 /* 0: the connection closed meanwhile */
    if (p && (int32_t)hr >= 0) {
        p->ctx = pctx; s->ntver++; p->dpnid = make_dpnid(s, (uint32_t)(p - s->pl));
        send_connect_reply(s, p, &r);
        LOG("server: accepted player %08x\n", p->dpnid);
    } else if (p) {
        send_connect_failed(e->conn, DPNERR_HOSTREJECTEDCONNECTION, &r); dp8_disconnect(e->conn, 0); player_free(p);
        LOG("server: the game rejected a connection (%08x)\n", hr);
    }
    pthread_mutex_unlock(&s->m);
    free(r.p);
    if (reply) { Ev *rb = new_event(s, M_RETURN_BUFFER); rb->a[0] = 0; rb->a[1] = reply; rb->a[2] = rctx; rb->pmsg_size = 16; post_event(rb); }
    (void)c;
    return 0;
}
static void server_connect_info(Sess *s, dp8_conn *conn, const uint8_t *m, size_t n)
{
    if (n < 4 + 48 + 32) return;
    uint32_t ver = rd32(m + 8);
    Blob name, data, pw, cdata, url;
    if (field(m, n, rd32(m + 12), rd32(m + 16), &name) || field(m, n, rd32(m + 20), rd32(m + 24), &data) ||
        field(m, n, rd32(m + 28), rd32(m + 32), &pw) || field(m, n, rd32(m + 36), rd32(m + 40), &cdata) ||
        field(m, n, rd32(m + 44), rd32(m + 48), &url)) return;
    const uint8_t *inst = m + 52; static const uint8_t zero[16];
    pthread_mutex_lock(&s->m);
    Player *p = player_by_conn(s, conn);
    if (!p || p->indicated) { pthread_mutex_unlock(&s->m); return; }       /* once per connection */
    uint32_t fail = 0;
    if (memcmp(inst, zero, 16) && memcmp(inst, s->instance, 16)) fail = DPNERR_INVALIDINSTANCE;
    else if ((s->flags & SESS_REQUIREPASSWORD) && (pw.n != s->password.n || memcmp(pw.p, s->password.p, pw.n))) fail = DPNERR_INVALIDPASSWORD;
    if (fail) { send_connect_failed(conn, fail, 0); dp8_disconnect(conn, 0); player_free(p); pthread_mutex_unlock(&s->m); return; }
    blob_set(&p->name, name.p, name.n); blob_set(&p->data, data.p, data.n); p->dnet_version = ver;
    pthread_mutex_unlock(&s->m);
    /* DPNMSG_INDICATE_CONNECT {pvUserConnectData, dwUserConnectDataSize, pvReplyData, dwReplyDataSize, pvReplyContext,
     *                          pvPlayerContext, pAddressPlayer, pAddressDevice} */
    pthread_mutex_lock(&s->m); p->indicated = 1; pthread_mutex_unlock(&s->m);
    Ev *e = new_event(s, M_INDICATE_CONNECT); e->conn = conn;
    e->a[0] = gcopy(cdata.p, cdata.n); e->a[1] = cdata.n; e->a[6] = address_object(dp8_addr(conn)); e->a[7] = address_object(dp8_addr(conn));
    e->pmsg_size = 36; e->post = post_indicate_connect; post_event(e);
}
static void server_receive(Sess *s, dp8_conn *conn, uint8_t user, const uint8_t *d, size_t n)
{
    pthread_mutex_lock(&s->m); Player *p = player_by_conn(s, conn); uint32_t id = p ? p->dpnid : 0, ctx = p ? p->ctx : 0;
    int created = p && p->created; pthread_mutex_unlock(&s->m);
    if (!p) return;
    if (user & DP8_USER_1) {
        if (n < 4) return;
        uint32_t type = rd32(d);
        /* the handshake in order: CONNECT_INFO once; its ACK only after the game accepted the player (it has a DPNID) */
        if (type == MSG_CONNECT_INFO) server_connect_info(s, conn, d, n);
        else if (type == MSG_ACK_CONNECT_INFO && !created && id) {
            pthread_mutex_lock(&s->m); p->created = 1; pthread_mutex_unlock(&s->m);
            Ev *e = new_event(s, M_CREATE_PLAYER); e->a[0] = id; e->a[1] = ctx; e->pmsg_size = 12; e->post = post_create_player; post_event(e);
        } else if (type == MSG_REQ_PROCESS_COMPLETION && n >= 8 && created) {
            ev_receive(s, id, ctx, d + 8, n - 8);
            Buf b = {0}; bput32(&b, MSG_PROCESS_COMPLETION); bput32(&b, rd32(d + 4)); send_core(conn, &b);
        } else if (type == MSG_REQ_UPDATE_INFO && n >= 32 && created) {   /* a client changed its name or data */
            uint32_t fl = rd32(d + 12); Blob nm, dt;
            if (field(d, n, rd32(d + 16), rd32(d + 20), &nm) || field(d, n, rd32(d + 24), rd32(d + 28), &dt)) return;
            pthread_mutex_lock(&s->m);
            if (fl & 1) blob_set(&p->name, nm.p, nm.n);
            if (fl & 2) blob_set(&p->data, dt.p, dt.n);
            send_update_info(s, conn, rd32(d + 4), id, id, &p->name, &p->data);
            pthread_mutex_unlock(&s->m);
            ev_simple(s, M_CLIENT_INFO, 12, id, ctx, 0, 0, 0);       /* {dwSize, dpnidClient, pvPlayerContext} */
        }
        return;
    }
    if (created) ev_receive(s, id, ctx, d, n);
}
static void server_closed(Sess *s, dp8_conn *conn, int reason)
{
    pthread_mutex_lock(&evm); for (Ev *q = evq; q; q = q->next) if (q->conn == conn) q->conn = 0; pthread_mutex_unlock(&evm);   /* it is freed later */
    pthread_mutex_lock(&s->m); Player *p = player_by_conn(s, conn);
    if (!p) { pthread_mutex_unlock(&s->m); return; }
    uint32_t id = p->dpnid, ctx = p->ctx; int created = p->created, leaving = p->leaving, accepted = p->indicated && p->dpnid; p->conn = 0;
    if (!created) player_free(p);
    pthread_mutex_unlock(&s->m);
    if (!created && accepted) ev_simple(s, M_INDICATED_CONNECT_ABORTED, 8, ctx, 0, 0, 0, 0);   /* {dwSize, pvPlayerContext} */
    if (created) {
        Ev *e = new_event(s, M_DESTROY_PLAYER); e->a[0] = id; e->a[1] = ctx;
        e->a[2] = leaving ? 4 /* host destroyed player */ : reason == DP8_CLOSE_LOST ? 2 /* connection lost */ : 1 /* normal */;
        e->pmsg_size = 16; e->post = post_destroy_player; post_event(e);
    }
}

/* enumeration: the server answers EnumQuery ([MC-DPLHP] 2.2.1) with EnumResponse (2.2.2) after asking the game */
typedef struct EnumReq { Sess *s; dp8_ep *ep; struct sockaddr_in from; uint16_t payload; } EnumReq;
static int post_enum_query(Ctx *c, Ev *e, uint32_t pm, uint32_t hr)
{
    __atomic_sub_fetch(&enum_events, 1, __ATOMIC_RELAXED);
    (void)c; Sess *s = e->s;
    obj_release(O(e->a[0])); obj_release(O(e->a[1])); gfree(e->a[2]);
    uint32_t rdata = rt_r32(G_MEM, pm + 24), rsize = rt_r32(G_MEM, pm + 28), rctx = rt_r32(G_MEM, pm + 32);
    if ((int32_t)hr >= 0) {
        pthread_mutex_lock(&s->m);
        Buf b = {0}; uint8_t h[4] = {0, 3, (uint8_t)e->a[8], (uint8_t)(e->a[8] >> 8)}; bput(&b, h, 4);
        size_t base = b.n;                                        /* offsets count from here (ReplyOffset) */
        for (int i = 0; i < 2; i++) bput32(&b, 0);
        bput32(&b, 0x50); bput32(&b, s->flags & ~(uint32_t)SESS_CLIENT_SERVER); bput32(&b, s->maxplayers);
        uint32_t cur = 1; for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created) cur++;
        bput32(&b, cur); for (int i = 0; i < 8; i++) bput32(&b, 0);
        bput(&b, s->instance, 16); bput(&b, s->app, 16);
        if (s->sess_name.n) { bset32(&b, base + 24, (uint32_t)(b.n - base)); bset32(&b, base + 28, s->sess_name.n); bput(&b, s->sess_name.p, s->sess_name.n); }
        if (s->app_reserved.n) { bset32(&b, base + 48, (uint32_t)(b.n - base)); bset32(&b, base + 52, s->app_reserved.n); bput(&b, s->app_reserved.p, s->app_reserved.n); }
        if (rdata && rsize) { bset32(&b, base, (uint32_t)(b.n - base)); bset32(&b, base + 4, rsize); bput(&b, GP(rdata), rsize); }
        pthread_mutex_unlock(&s->m);
        if (s->ep) dp8_send_raw(s->ep, &e->addr, b.p, b.n);
        free(b.p);
    }
    if (rdata) { Ev *rb = new_event(s, M_RETURN_BUFFER); rb->a[0] = 0; rb->a[1] = rdata; rb->a[2] = rctx; rb->pmsg_size = 16; post_event(rb); }
    return 0;
}
static void net_enum_query(void *ctx, dp8_ep *ep, const struct sockaddr_in *from, const uint8_t *m, size_t n)
{
    Sess *s = ctx; (void)ep;
    if (n < 5 || !s->hosting) return;
    size_t data_at = 5;
    if (m[4] == 1) { if (n < 21 || memcmp(m + 5, s->app, 16)) return; data_at = 21; }
    if (__atomic_load_n(&enum_events, __ATOMIC_RELAXED) >= MAX_ENUM_EVENTS) return;   /* a flood: the rest are dropped */
    __atomic_add_fetch(&enum_events, 1, __ATOMIC_RELAXED);
    /* DPNMSG_ENUM_HOSTS_QUERY {pAddressSender, pAddressDevice, pvReceivedData, dwReceivedDataSize, dwMaxResponseDataSize,
     *                          pvResponseData, dwResponseDataSize, pvResponseContext} */
    Ev *e = new_event(s, M_ENUM_HOSTS_QUERY); e->addr = *from;
    e->a[0] = address_object(from); e->a[1] = address_object(from);
    e->a[2] = gcopy(m + data_at, (uint32_t)(n - data_at)); e->a[3] = (uint32_t)(n - data_at); e->a[4] = 1000;
    e->a[8] = m[2] | (uint32_t)m[3] << 8;                            /* EnumPayload, echoed (not part of the message) */
    e->pmsg_size = 36; e->post = post_enum_query; post_event(e);
}

/* ================================================================ client side */
static void client_complete_connect(Sess *s, uint32_t hr, const Blob *reply)
{
    pthread_mutex_lock(&s->m);
    if (!s->connecting) { pthread_mutex_unlock(&s->m); return; }
    s->connecting = 0; s->connected = hr == 0; s->conn_hr = hr; blob_set(&s->conn_reply, reply ? reply->p : 0, reply ? reply->n : 0);
    pthread_cond_broadcast(&s->conn_cv);
    uint32_t h = s->conn_handle, ctx = s->conn_ctx; uint32_t rp = gcopy(s->conn_reply.p, s->conn_reply.n), rn = s->conn_reply.n;
    pthread_mutex_unlock(&s->m);
    LOG("client: connect complete %08x\n", hr);
    /* DPNMSG_CONNECT_COMPLETE {hAsyncOp, pvUserContext, hResultCode, pvApplicationReplyData, dwApplicationReplyDataSize} */
    ev_simple(s, M_CONNECT_COMPLETE, 24, h, ctx, hr, rp, rn);
}
static void client_receive(Sess *s, dp8_conn *conn, uint8_t user, const uint8_t *d, size_t n)
{
    if (user & DP8_USER_1) {
        if (n < 4) return; uint32_t type = rd32(d);
        if (type == MSG_SEND_CONNECT_INFO && n >= 4 + 8 + 0x50 + 20) {
            pthread_mutex_lock(&s->m);
            const uint8_t *ad = d + 12;                            /* application description */
            s->flags = rd32(ad + 4) | SESS_CLIENT_SERVER; s->maxplayers = rd32(ad + 8); memcpy(s->instance, ad + 48, 16); memcpy(s->app, ad + 64, 16);
            Blob sn; if (!field(d, n, rd32(ad + 16), rd32(ad + 20), &sn)) blob_set(&s->sess_name, sn.p, sn.n);
            Blob ar; if (!field(d, n, rd32(ad + 40), rd32(ad + 44), &ar)) blob_set(&s->app_reserved, ar.p, ar.n);
            const uint8_t *nt = ad + 0x50; s->local_dpnid = rd32(nt); uint32_t entries = rd32(nt + 12);
            for (uint32_t i = 0; i < entries && (size_t)(nt + 20 + 48 * (i + 1) - d) <= n; i++) {
                const uint8_t *e = nt + 20 + 48 * i;
                if (rd32(e + 8) & (NT_SERVER | NT_HOST)) {
                    s->server_pl.dpnid = rd32(e); Blob nm, dt;
                    if (!field(d, n, rd32(e + 24), rd32(e + 28), &nm)) blob_set(&s->server_pl.name, nm.p, nm.n);
                    if (!field(d, n, rd32(e + 32), rd32(e + 36), &dt)) blob_set(&s->server_pl.data, dt.p, dt.n);
                }
            }
            Blob reply; field(d, n, rd32(d + 4), rd32(d + 8), &reply); Blob r = {0}; blob_set(&r, reply.p, reply.n);
            pthread_mutex_unlock(&s->m);
            Buf b = {0}; bput32(&b, MSG_ACK_CONNECT_INFO); send_core(conn, &b);
            client_complete_connect(s, 0, &r); free(r.p);
        } else if (type == MSG_CONNECT_FAILED && n >= 16) {
            Blob reply; field(d, n, rd32(d + 8), rd32(d + 12), &reply); Blob r = {0}; blob_set(&r, reply.p, reply.n);
            client_complete_connect(s, rd32(d + 4), &r); free(r.p); dp8_disconnect(conn, 0);
        } else if (type == MSG_TERMINATE_SESSION && n >= 12) {
            Blob td; field(d, n, rd32(d + 4), rd32(d + 8), &td);
            pthread_mutex_lock(&s->m); int was = s->connected; s->connected = 0; pthread_mutex_unlock(&s->m);
            if (was) ev_simple(s, M_TERMINATE_SESSION, 16, DPNERR_HOSTTERMINATEDSESSION, gcopy(td.p, td.n), td.n, 0, 0);
            dp8_disconnect(conn, 0);
        } else if (type == MSG_UPDATE_INFO && n >= 44) {
            uint32_t who = rd32(d + 8), fl = rd32(d + 20); Blob nm, dt;
            if (field(d, n, rd32(d + 24), rd32(d + 28), &nm) || field(d, n, rd32(d + 32), rd32(d + 36), &dt)) return;
            pthread_mutex_lock(&s->m); int server = who == s->server_pl.dpnid;
            if (server && (fl & 1)) blob_set(&s->server_pl.name, nm.p, nm.n);
            if (server && (fl & 2)) blob_set(&s->server_pl.data, dt.p, dt.n);
            pthread_mutex_unlock(&s->m);
            if (server) ev_simple(s, M_SERVER_INFO, 12, who, 0, 0, 0, 0);   /* {dwSize, dpnidServer, pvPlayerContext} */
        } else if (type == MSG_UPDATE_APPLICATION_DESC && n >= 4 + 0x50) {
            const uint8_t *ad = d + 4; Blob sn, ar;
            pthread_mutex_lock(&s->m);
            s->flags = rd32(ad + 4) | SESS_CLIENT_SERVER; s->maxplayers = rd32(ad + 8);
            if (!field(d, n, rd32(ad + 16), rd32(ad + 20), &sn) && sn.n) blob_set(&s->sess_name, sn.p, sn.n);
            if (!field(d, n, rd32(ad + 40), rd32(ad + 44), &ar)) blob_set(&s->app_reserved, ar.p, ar.n);
            pthread_mutex_unlock(&s->m);
            ev_simple(s, M_APPLICATION_DESC, 4, 0, 0, 0, 0, 0);       /* DPNMSG_APPLICATION_DESC {dwSize} */
        } else if (type == MSG_REQ_PROCESS_COMPLETION && n >= 8) {
            ev_receive(s, s->server_pl.dpnid, 0, d + 8, n - 8);
            Buf b = {0}; bput32(&b, MSG_PROCESS_COMPLETION); bput32(&b, rd32(d + 4)); send_core(conn, &b);
        }
        return;
    }
    if (s->connected) ev_receive(s, s->server_pl.dpnid, 0, d, n);
}
static void client_closed(Sess *s, dp8_conn *conn, int reason)
{
    (void)conn;
    pthread_mutex_lock(&s->m); int was = s->connected, connecting = s->connecting; s->connected = 0; s->srv = 0; pthread_mutex_unlock(&s->m);
    if (connecting) client_complete_connect(s, DPNERR_NORESPONSE, 0);
    else if (was) ev_simple(s, M_TERMINATE_SESSION, 16, reason == DP8_CLOSE_LOST ? DPNERR_CONNECTIONLOST : DPNERR_HOSTTERMINATEDSESSION, 0, 0, 0, 0);
}
/* EnumResponse -> DPNMSG_ENUM_HOSTS_RESPONSE {pAddressSender, pAddressDevice, pApplicationDescription, pvResponseData,
 * dwResponseDataSize, pvUserContext, dwRoundTripLatencyMS} */
static int post_enum_response(Ctx *c, Ev *e, uint32_t pm, uint32_t hr)
{
    __atomic_sub_fetch(&enum_events, 1, __ATOMIC_RELAXED);
    (void)c; (void)pm; (void)hr;
    obj_release(O(e->a[0])); obj_release(O(e->a[1]));
    uint32_t ad = e->a[2]; gfree(rt_r32(G_MEM, ad + 48)); gfree(rt_r32(G_MEM, ad + 64)); gfree(ad); gfree(e->a[3]);
    return 0;
}
static void net_enum_response(void *ctx, dp8_ep *ep, const struct sockaddr_in *from, const uint8_t *m, size_t n)
{
    Sess *s = ctx; (void)ep;
    if (n < 4 + 88) return;
    const uint8_t *r = m + 4;                                         /* offsets count from ReplyOffset */
    uint32_t payload = m[2] | (uint32_t)m[3] << 8;
    pthread_mutex_lock(&s->m); EnumOp *op = 0;
    for (int i = 0; i < 8; i++) if (s->en[i].used && (payload >> 8) == (s->en[i].handle & 0xff)) op = &s->en[i];
    uint32_t uctx = op ? op->ctx : 0, rtt = op ? dp8_tick() - op->last_tx : 0, devcopy = op ? addr_dup(op->device) : 0;
    pthread_mutex_unlock(&s->m);
    if (!op) return;
    if (__atomic_load_n(&enum_events, __ATOMIC_RELAXED) >= MAX_ENUM_EVENTS) { obj_release(O(devcopy)); return; }
    __atomic_add_fetch(&enum_events, 1, __ATOMIC_RELAXED);
    #define OFF(x) (rd32(r + (x)))
    size_t rlen = n - 4;
    Blob sn = {0}, ar = {0}, rd = {0};
    #define FITS(o, l) (OFF(o) && OFF(o) <= rlen && OFF(l) <= rlen - OFF(o))     /* no uint32 wrap: the sender is anyone */
    if (FITS(24, 28)) { sn.p = (uint8_t *)r + OFF(24); sn.n = OFF(28); }
    if (FITS(48, 52)) { ar.p = (uint8_t *)r + OFF(48); ar.n = OFF(52); }
    if (FITS(0, 4)) { rd.p = (uint8_t *)r + OFF(0); rd.n = OFF(4); }
    #undef FITS
    /* DPN_APPLICATION_DESC (72 bytes) */
    uint32_t g = heap_alloc(w32_process_heap, 8, 72); if (!g) { obj_release(O(devcopy)); __atomic_sub_fetch(&enum_events, 1, __ATOMIC_RELAXED); return; }
    rt_w32(G_MEM, g, 72); rt_w32(G_MEM, g + 4, OFF(12)); memcpy(GP(g + 8), r + 56, 16); memcpy(GP(g + 24), r + 72, 16);
    rt_w32(G_MEM, g + 40, OFF(16)); rt_w32(G_MEM, g + 44, OFF(20)); rt_w32(G_MEM, g + 48, gstr_w(&sn));
    if (ar.n) { rt_w32(G_MEM, g + 64, gcopy(ar.p, ar.n)); rt_w32(G_MEM, g + 68, ar.n); }
    hexlog("application reserved data received", ar.p, ar.n);
    #undef OFF
    Ev *e = new_event(s, M_ENUM_HOSTS_RESPONSE);
    e->a[0] = address_object(from); e->a[1] = devcopy; e->a[2] = g; e->a[3] = gcopy(rd.p, rd.n); e->a[4] = rd.n; e->a[5] = uctx; e->a[6] = rtt;
    e->pmsg_size = 32; e->post = post_enum_response; post_event(e);
}

/* ================================================================ network callbacks */
static void net_accepted(void *ctx, dp8_conn *c)
{
    Sess *s = ctx; pthread_mutex_lock(&s->m);
    int k = 1; while (k < 256 && s->pl[k].used) k++;            /* index 0 is never used: DPNID 0 is invalid */
    if (k < 256 && s->hosting) { Player *p = &s->pl[k]; memset(p, 0, sizeof *p); p->used = 1; p->conn = c; p->addr = *dp8_addr(c); }
    else dp8_disconnect(c, 1);
    pthread_mutex_unlock(&s->m);
}
static void net_connected(void *ctx, dp8_conn *c)
{
    Sess *s = ctx; struct sockaddr_in local = {0}; local.sin_family = AF_INET; local.sin_port = htons(dp8_port(s->ep));
    local.sin_addr = dp8_addr(c)->sin_addr;                          /* best guess for our own address as the server sees it */
    pthread_mutex_lock(&s->m); send_connect_info(s, c, &local); pthread_mutex_unlock(&s->m);
}
static void net_receive(void *ctx, dp8_conn *c, uint8_t user, const uint8_t *d, size_t n)
{
    Sess *s = ctx; if (s->server) server_receive(s, c, user, d, n); else client_receive(s, c, user, d, n);
}
static void net_closed(void *ctx, dp8_conn *c, int reason) { Sess *s = ctx; if (s->server) server_closed(s, c, reason); else client_closed(s, c, reason); }
static dp8_callbacks callbacks(Sess *s)
{
    dp8_callbacks cb = {s, net_enum_query, net_enum_response, net_accepted, net_connected, net_receive, net_closed}; return cb;
}

/* ================================================================ enumeration (client) */
static void enum_end(EnumOp *op) { op->used = 0; obj_release(O(op->device)); op->device = 0; free(op->user.p); op->user.p = 0; op->user.n = 0; }
static void *enum_thread(void *arg)
{
    Sess *s = arg;
    for (;;) {
        usleep(50000);
        pthread_mutex_lock(&s->m);
        int any = 0; uint32_t now = dp8_tick();
        for (int i = 0; i < 8; i++) {
            EnumOp *op = &s->en[i]; if (!op->used) continue; any = 1;
            if (op->timeout != 0xffffffffu && now - op->started > op->timeout && op->count != 0xffffffffu && op->sent >= op->count) {
                enum_end(op); ev_simple(s, M_ASYNC_OP_COMPLETE, 16, op->handle, op->ctx, 0, 0, 0); continue;
            }
            if ((int32_t)(now - op->next) < 0 || (op->count != 0xffffffffu && op->sent >= op->count)) continue;
            Buf b = {0}; uint8_t h[4] = {0, 2, (uint8_t)op->sent, (uint8_t)op->handle}; bput(&b, h, 4);
            static const uint8_t zero[16]; uint8_t qt = memcmp(s->app, zero, 16) ? 1 : 2; bput(&b, &qt, 1);
            if (qt == 1) bput(&b, s->app, 16);
            if (op->user.n) bput(&b, op->user.p, op->user.n);
            for (int d = 0; d < op->ndest; d++) dp8_send_raw(s->ep, &op->dest[d], b.p, b.n);
            free(b.p); op->sent++; op->last_tx = now; op->next = now + op->retry;
            if (op->sent == 1 || op->sent % 8 == 0) LOG("client: enum query %u to %d destinations\n", op->sent, op->ndest);
        }
        if (!any) { s->enum_thread = 0; pthread_mutex_unlock(&s->m); break; }   /* restarted by the next EnumHosts */
        pthread_mutex_unlock(&s->m);
    }
    return 0;
}
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
static int resolve(const char *host, uint16_t port, struct sockaddr_in *out)
{
    struct addrinfo hint = {0}, *res = 0; hint.ai_family = AF_INET; hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, 0, &hint, &res) || !res) return -1;
    *out = *(struct sockaddr_in *)res->ai_addr; out->sin_port = htons(port); freeaddrinfo(res); return 0;
}
static int broadcast_targets(struct sockaddr_in *out, int cap, uint16_t port)
{
    int n = 0; struct ifaddrs *ifs = 0;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST); out[n++] = a;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); out[n++] = a;            /* a host on this Mac */
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && n < cap; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
                a.sin_addr = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr; out[n++] = a;
            }
        freeifaddrs(ifs);
    }
    return n;
}
static Sess *sess_for(Obj *o)
{
    if (!o->context_sess) {
        Sess *s = calloc(1, sizeof *s); s->o = o; s->server = o->kind == K_SERVER;
        pthread_mutex_init(&s->m, 0); pthread_cond_init(&s->conn_cv, 0);
        o->context_sess = (uintptr_t)s;
    }
    return sess_of(o);
}
static int ensure_endpoint(Sess *s, int listen, uint16_t port)
{
    if (s->ep) return 0;
    dp8_callbacks cb = callbacks(s);
    s->ep = port ? dp8_open(port, port, listen, listen ? DP8_ENUM_PORT : 0, &cb) : dp8_open(DP8_PORT_LO, DP8_PORT_HI, listen, listen ? DP8_ENUM_PORT : 0, &cb);
    if (!s->ep) s->ep = dp8_open(0, 0, listen, 0, &cb);
    return s->ep ? 0 : -1;
}
static uint32_t addr_port(Obj *a) { Comp *k = a ? comp_find(a, "port") : 0; return k && k->type == DT_DWORD ? *(uint32_t *)k->data : 0; }
static int addr_host(Obj *a, char *out, size_t cap)
{
    Comp *k = a ? comp_find(a, "hostname") : 0; if (!k) return -1;
    size_t i = 0; if (k->type == DT_STRING) for (; i + 1 < cap && i < k->size / 2 && ((uint16_t *)k->data)[i]; i++) out[i] = (char)((uint16_t *)k->data)[i];
    else for (; i + 1 < cap && i < k->size && k->data[i]; i++) out[i] = (char)k->data[i];
    out[i] = 0; return i ? 0 : -1;
}
/* the application description structure from the game */
static void read_app_desc(Sess *s, uint32_t ad)
{
    if (!ad) return;
    s->flags = rt_r32(G_MEM, ad + 4) | SESS_CLIENT_SERVER; memcpy(s->instance, GP(ad + 8), 16); memcpy(s->app, GP(ad + 24), 16);
    s->maxplayers = rt_r32(G_MEM, ad + 40);
    wide_blob(&s->sess_name, rt_r32(G_MEM, ad + 48)); wide_blob(&s->password, rt_r32(G_MEM, ad + 52));
    uint32_t ar = rt_r32(G_MEM, ad + 64), arn = rt_r32(G_MEM, ad + 68); blob_set(&s->app_reserved, ar ? GP(ar) : 0, ar ? arn : 0);
    hexlog("application reserved data set", s->app_reserved.p, s->app_reserved.n);
}
static void write_player_info(Ctx *c, uint32_t pinfo, uint32_t psize, const Blob *name, const Blob *data, uint32_t flags, int nargs)
{
    uint32_t need = 24 + name->n + data->n, have = rt_r32(G_MEM, psize);
    rt_w32(G_MEM, psize, need);
    if (!pinfo || have < need) RET(DPNERR_BUFFERTOOSMALL, nargs);
    rt_w32(G_MEM, pinfo, 24); rt_w32(G_MEM, pinfo + 4, 3);
    uint32_t at = pinfo + 24;
    if (name->n) { memcpy(GP(at), name->p, name->n); rt_w32(G_MEM, pinfo + 8, at); at += name->n; } else rt_w32(G_MEM, pinfo + 8, 0);
    if (data->n) { memcpy(GP(at), data->p, data->n); rt_w32(G_MEM, pinfo + 12, at); } else rt_w32(G_MEM, pinfo + 12, 0);
    rt_w32(G_MEM, pinfo + 16, data->n); rt_w32(G_MEM, pinfo + 20, flags);
    RET(S_OK_, nargs);
}
static void read_player_info(uint32_t pinfo, Blob *name, Blob *data)
{
    if (!pinfo) return; uint32_t f = rt_r32(G_MEM, pinfo + 4);
    if (f & 1) wide_blob(name, rt_r32(G_MEM, pinfo + 8));
    if (f & 2) { uint32_t p = rt_r32(G_MEM, pinfo + 12), n = rt_r32(G_MEM, pinfo + 16); blob_set(data, p ? GP(p) : 0, p ? n : 0); }
}
static void gather(uint32_t bufs, uint32_t count, Blob *out)
{
    uint32_t total = 0; for (uint32_t i = 0; i < count; i++) total += rt_r32(G_MEM, bufs + 8 * i);
    free(out->p); out->p = malloc(total ? total : 1); out->n = 0;
    for (uint32_t i = 0; i < count; i++) { uint32_t n = rt_r32(G_MEM, bufs + 8 * i), p = rt_r32(G_MEM, bufs + 8 * i + 4); memcpy(out->p + out->n, GP(p), n); out->n += n; }
}

/* ================================================================ IDirectPlay8Client / IDirectPlay8Server */
static void dump_args(const char *what, Ctx *c, int n)
{
    if (!getenv("W32_DPLOG")) return;
    fprintf(stderr, "dpnet %7.3f: %s(", dp8_tick() / 1000.0, what); for (int i = 1; i < n; i++) fprintf(stderr, "%s%08x", i > 1 ? ", " : "", ARG(i)); fprintf(stderr, ")\n");
}
#define STUB(name, n, hr) static void name(Ctx *c) { dump_args(#name, c, n); RET(hr, n); }
static void cs_QueryInterface(Ctx *c) { dump_args("QueryInterface", c, 3); a_QueryInterface(c); }
static void cs_AddRef(Ctx *c) { a_AddRef(c); }
static void cs_Release(Ctx *c) { dump_args("Release", c, 1); a_Release(c); }
static void cs_Initialize(Ctx *c)
{
    Obj *o = O(ARG(0)); dump_args("Initialize", c, 4); if (!o) RET(DPNERR_INVALIDPARAM, 4);
    o->context = ARG(1); o->handler = ARG(2); sess_for(o); RET(S_OK_, 4);
}
/* one provider: TCP/IP. DPN_SERVICE_PROVIDER_INFO = {dwFlags, guid, pwszName, pvReserved, dwReserved}, name after the array */
static void cs_EnumServiceProviders(Ctx *c)
{
    dump_args("EnumServiceProviders", c, 7);
    uint32_t buf = ARG(3), pcb = ARG(4), pcount = ARG(5); static const char name[] = "DirectPlay8 TCP/IP Service Provider";
    uint32_t need = 32 + 2 * (uint32_t)sizeof name, have = rt_r32(G_MEM, pcb);
    rt_w32(G_MEM, pcb, need); rt_w32(G_MEM, pcount, 1);
    if (!buf || have < need) RET(DPNERR_BUFFERTOOSMALL, 7);
    memset(GP(buf), 0, 32); memcpy(GP(buf + 4), GUID_TCPIP, 16); rt_w32(G_MEM, buf + 20, buf + 32);
    for (uint32_t i = 0; i < sizeof name; i++) rt_w16(G_MEM, buf + 32 + 2 * i, (uint8_t)name[i]);
    RET(S_OK_, 7);
}
/* DPN_SP_CAPS {dwSize, dwFlags, dwNumThreads, dwDefaultEnumCount, dwDefaultEnumRetryInterval, dwDefaultEnumTimeout,
 * dwMaxEnumPayloadSize, dwBuffersPerThread, dwSystemBufferSize}: the TCP/IP provider's usual values */
static void cs_GetSPCaps(Ctx *c)
{
    dump_args("GetSPCaps", c, 4); uint32_t p = ARG(2);
    static const uint32_t caps[9] = {36, 0, 3, 5, 1500, 1500, 983, 1, 8192};
    if (!p || rt_r32(G_MEM, p) < 36) RET(DPNERR_INVALIDPARAM, 4);
    memcpy(GP(p), caps, 36); RET(S_OK_, 4);
}
static void cl_EnumHosts(Ctx *c)
{
    dump_args("EnumHosts", c, 12);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_for(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 12);
    uint32_t ad = ARG(1), host = ARG(2), dev = ARG(3), udata = ARG(4), usize = ARG(5), count = ARG(6), retry = ARG(7), timeout = ARG(8), uctx = ARG(9), ph = ARG(10);
    pthread_mutex_lock(&s->m);
    if (ad) memcpy(s->app, GP(ad + 24), 16);
    int k = 0; while (k < 8 && s->en[k].used) k++;
    if (k == 8 || ensure_endpoint(s, 0, 0)) { pthread_mutex_unlock(&s->m); RET(DPNERR_GENERIC, 12); }
    EnumOp *op = &s->en[k]; memset(op, 0, sizeof *op); op->used = 1; op->handle = next_handle(); op->ctx = uctx;
    op->count = count ? count : 0xffffffffu; op->retry = retry ? retry : 1500; op->timeout = timeout ? timeout : 0xffffffffu;
    op->started = dp8_tick(); op->next = op->started; op->device = addr_dup(dev);   /* the game releases its own */
    if (udata && usize) blob_set(&op->user, GP(udata), usize);
    char h[256]; Obj *ho = O(host); uint16_t port = (uint16_t)(addr_port(ho) ? addr_port(ho) : DP8_ENUM_PORT);
    if (ho && !addr_host(ho, h, sizeof h) && !resolve(h, port, &op->dest[0])) {
        op->ndest = 1;
        if (port == DP8_ENUM_PORT) { op->dest[1] = op->dest[0]; op->dest[1].sin_port = htons(DP8_PORT_LO); op->ndest = 2; }   /* hosts without the well-known port */
        LOG("client: EnumHosts %s:%u\n", h, port);
    } else {                                       /* the enumeration port, and the first game port for hosts without it */
        int nb = broadcast_targets(op->dest, 12, DP8_ENUM_PORT);
        for (int i = 0; i < nb; i++) { op->dest[nb + i] = op->dest[i]; op->dest[nb + i].sin_port = htons(DP8_PORT_LO); }
        op->ndest = 2 * nb;
    }
    if (!s->enum_thread) { s->enum_thread = 1; pthread_t t; pthread_create(&t, 0, enum_thread, s); pthread_detach(t); }
    if (ph) rt_w32(G_MEM, ph, op->handle);
    pthread_mutex_unlock(&s->m);
    RET(DPNSUCCESS_PENDING, 12);
}
static void cs_CancelAsyncOperation(Ctx *c)
{
    dump_args("CancelAsyncOperation", c, 3);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; uint32_t h = ARG(1), fl = ARG(2);
    if (!s) RET(S_OK_, 3);
    pthread_mutex_lock(&s->m);
    for (int i = 0; i < 8; i++) if (s->en[i].used && ((h && s->en[i].handle == h) || (!h && (fl & (DPNCANCEL_ENUM | DPNCANCEL_ALL))))) {
        enum_end(&s->en[i]); ev_simple(s, M_ASYNC_OP_COMPLETE, 16, s->en[i].handle, s->en[i].ctx, DPNERR_USERCANCEL, 0, 0);
    }
    pthread_mutex_unlock(&s->m);
    RET(S_OK_, 3);
}
static void cl_Connect(Ctx *c)
{
    dump_args("Connect", c, 11);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_for(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 11);
    uint32_t ad = ARG(1), host = ARG(2), cdata = ARG(6), csize = ARG(7), uctx = ARG(8), ph = ARG(9), fl = ARG(10);
    char h[256]; Obj *ho = O(host); struct sockaddr_in to;
    if (!ho || addr_host(ho, h, sizeof h) || resolve(h, (uint16_t)(addr_port(ho) ? addr_port(ho) : DP8_PORT_LO), &to)) RET(DPNERR_INVALIDPARAM, 11);
    pthread_mutex_lock(&s->m);
    if (ad) { memcpy(s->instance, GP(ad + 8), 16); memcpy(s->app, GP(ad + 24), 16); wide_blob(&s->password, rt_r32(G_MEM, ad + 52)); }
    blob_set(&s->conn_data, cdata ? GP(cdata) : 0, cdata ? csize : 0);
    if (ensure_endpoint(s, 0, 0)) { pthread_mutex_unlock(&s->m); RET(DPNERR_GENERIC, 11); }
    s->connecting = 1; s->connected = 0; s->conn_handle = next_handle(); s->conn_ctx = uctx; s->in_connect_call = !(fl & DPNCONNECT_SYNC);
    if (ph) rt_w32(G_MEM, ph, s->conn_handle);
    LOG("client: connecting to %s:%u\n", h, ntohs(to.sin_port));
    s->srv = dp8_connect(s->ep, &to); g_joined = s;
    if (fl & DPNCONNECT_SYNC) {
        while (s->connecting) pthread_cond_wait(&s->conn_cv, &s->m);
        uint32_t hr = s->conn_hr; pthread_mutex_unlock(&s->m); RET(hr, 11);
    }
    s->in_connect_call = 0; pthread_cond_broadcast(&s->conn_cv);
    pthread_mutex_unlock(&s->m);
    RET(DPNSUCCESS_PENDING, 11);
}
static uint32_t do_send(Sess *s, dp8_conn *to, uint32_t bufs, uint32_t count, uint32_t uctx, uint32_t ph, uint32_t fl, uint32_t loop_from, uint32_t loop_ctx, int loopback, int nargs, Ctx *c)
{
    (void)nargs; (void)c;
    Blob d = {0}; gather(bufs, count, &d);
    if (getenv("W32_DPLOG") && atoi(getenv("W32_DPLOG")) >= 2) LOG("send %u bytes flags %08x%s%s\n", d.n, fl, to ? " (network)" : "", loopback ? " (to self)" : "");
    if (to) {
        if (fl & DPNSEND_COMPLETEONPROCESS) { Buf b = {0}; bput32(&b, MSG_REQ_PROCESS_COMPLETION); bput32(&b, next_handle()); bput(&b, d.p, d.n); send_core(to, &b); }
        else dp8_send(to, d.p, d.n, 0);
    }
    if (loopback) ev_receive(s, loop_from, loop_ctx, d.p, d.n);
    free(d.p);
    if (fl & DPNSEND_SYNC) return S_OK_;
    uint32_t h = next_handle(); if (ph) rt_w32(G_MEM, ph, h);
    if (!(fl & DPNSEND_NOCOMPLETE)) ev_simple(s, M_SEND_COMPLETE, 20, h, uctx, 0, 0, 0);
    return DPNSUCCESS_PENDING;
}
static void cl_Send(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s || !s->srv || !s->connected) RET(DPNERR_NOCONNECTION, 7);
    RET(do_send(s, s->srv, ARG(1), ARG(2), ARG(4), ARG(5), ARG(6), 0, 0, 0, 7, c), 7);
}
static void sv_SendTo(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s || !s->hosting) RET(DPNERR_NOCONNECTION, 8);
    uint32_t id = ARG(1), bufs = ARG(2), count = ARG(3), uctx = ARG(5), ph = ARG(6), fl = ARG(7);
    if (id == 0) {                                                   /* DPNID_ALL_PLAYERS_GROUP */
        dp8_conn *list[256]; int n = 0;
        pthread_mutex_lock(&s->m); for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created && s->pl[i].conn) list[n++] = s->pl[i].conn; pthread_mutex_unlock(&s->m);
        Blob d = {0}; gather(bufs, count, &d);
        if (getenv("W32_DPLOG") && atoi(getenv("W32_DPLOG")) >= 2) LOG("send %u bytes to all (%d remote) flags %08x\n", d.n, n, fl);
        for (int i = 0; i < n; i++) dp8_send(list[i], d.p, d.n, 0);
        if (!(fl & DPNSEND_NOLOOPBACK)) ev_receive(s, s->server_dpnid, s->server_ctx, d.p, d.n);
        free(d.p);
        if (fl & DPNSEND_SYNC) RET(S_OK_, 8);
        uint32_t h = next_handle(); if (ph) rt_w32(G_MEM, ph, h);
        if (!(fl & DPNSEND_NOCOMPLETE)) ev_simple(s, M_SEND_COMPLETE, 20, h, uctx, 0, 0, 0);
        RET(DPNSUCCESS_PENDING, 8);
    }
    if (id == s->server_dpnid) RET(do_send(s, 0, bufs, count, uctx, ph, fl, s->server_dpnid, s->server_ctx, 1, 8, c), 8);
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, id); dp8_conn *to = p && p->created ? p->conn : 0; pthread_mutex_unlock(&s->m);
    if (!to) RET(DPNERR_INVALIDPLAYER, 8);
    RET(do_send(s, to, bufs, count, uctx, ph, fl, 0, 0, 0, 8, c), 8);
}
static void cs_ReturnBuffer(Ctx *c) { uint32_t h = ARG(1); uint32_t p = pending_bufs[h & 255]; if (p) { gfree(p); pending_bufs[h & 255] = 0; } RET(S_OK_, 3); }
static void cl_SetClientInfo(Ctx *c)
{
    dump_args("SetClientInfo", c, 5); Obj *o = O(ARG(0)); Sess *s = o ? sess_for(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&s->m); read_player_info(ARG(1), &s->info_name, &s->info_data);
    if (s->connected && s->srv) send_req_update_info(s, s->srv);       /* the server keeps the name table */
    pthread_mutex_unlock(&s->m);
    if (ARG(4) & 0x80000000u) RET(S_OK_, 5);
    uint32_t h = next_handle(); if (ARG(3)) rt_w32(G_MEM, ARG(3), h); ev_simple(s, M_ASYNC_OP_COMPLETE, 16, h, ARG(2), 0, 0, 0);
    RET(DPNSUCCESS_PENDING, 5);
}
static void sv_SetServerInfo(Ctx *c)
{
    dump_args("SetServerInfo", c, 5); Obj *o = O(ARG(0)); Sess *s = o ? sess_for(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&s->m); read_player_info(ARG(1), &s->info_name, &s->info_data);
    if (s->hosting) for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created && s->pl[i].conn)   /* tell the clients */
        send_update_info(s, s->pl[i].conn, 0, s->server_dpnid, s->server_dpnid, &s->info_name, &s->info_data);
    pthread_mutex_unlock(&s->m);
    if (ARG(4) & 0x80000000u) RET(S_OK_, 5);
    uint32_t h = next_handle(); if (ARG(3)) rt_w32(G_MEM, ARG(3), h); ev_simple(s, M_ASYNC_OP_COMPLETE, 16, h, ARG(2), 0, 0, 0);
    RET(DPNSUCCESS_PENDING, 5);
}
static void cl_GetServerInfo(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s || !s->connected) RET(DPNERR_NOCONNECTION, 4);
    write_player_info(c, ARG(1), ARG(2), &s->server_pl.name, &s->server_pl.data, 4 /* host */, 4);
}
static void sv_GetClientInfo(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 5);
    uint32_t id = ARG(1);
    if (id == s->server_dpnid) { write_player_info(c, ARG(2), ARG(3), &s->info_name, &s->info_data, 2 | 4, 5); return; }
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, id); Blob nm = {0}, dt = {0}; if (p) { blob_set(&nm, p->name.p, p->name.n); blob_set(&dt, p->data.p, p->data.n); } pthread_mutex_unlock(&s->m);
    if (!p) RET(DPNERR_INVALIDPLAYER, 5);
    write_player_info(c, ARG(2), ARG(3), &nm, &dt, 0, 5); free(nm.p); free(dt.p);
}
static void cl_GetServerAddress(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s || !s->srv) RET(DPNERR_NOCONNECTION, 3);
    rt_w32(G_MEM, ARG(1), address_object(dp8_addr(s->srv))); RET(S_OK_, 3);
}
static void sv_GetClientAddress(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 4);
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, ARG(1)); struct sockaddr_in a = p ? p->addr : (struct sockaddr_in){0}; pthread_mutex_unlock(&s->m);
    if (!p) RET(DPNERR_INVALIDPLAYER, 4);
    LOG("GetClientAddress(%08x) -> %s:%u\n", ARG(1), inet_ntoa(a.sin_addr), ntohs(a.sin_port));
    rt_w32(G_MEM, ARG(2), address_object(&a)); RET(S_OK_, 4);
}
static void cs_GetApplicationDesc(Ctx *c)
{
    dump_args("GetApplicationDesc", c, 4);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 4);
    uint32_t buf = ARG(1), psize = ARG(2);
    pthread_mutex_lock(&s->m);
    uint32_t need = 72 + s->sess_name.n + s->password.n + s->app_reserved.n, have = rt_r32(G_MEM, psize);
    rt_w32(G_MEM, psize, need);
    if (!buf || have < need) { pthread_mutex_unlock(&s->m); RET(DPNERR_BUFFERTOOSMALL, 4); }
    memset(GP(buf), 0, 72); rt_w32(G_MEM, buf, 72); rt_w32(G_MEM, buf + 4, s->flags); memcpy(GP(buf + 8), s->instance, 16); memcpy(GP(buf + 24), s->app, 16);
    rt_w32(G_MEM, buf + 40, s->maxplayers);
    uint32_t cur = 1; if (s->server) for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created) cur++;
    rt_w32(G_MEM, buf + 44, cur);
    uint32_t at = buf + 72;
    if (s->sess_name.n) { memcpy(GP(at), s->sess_name.p, s->sess_name.n); rt_w32(G_MEM, buf + 48, at); at += s->sess_name.n; }
    if (s->password.n) { memcpy(GP(at), s->password.p, s->password.n); rt_w32(G_MEM, buf + 52, at); at += s->password.n; }
    if (s->app_reserved.n) { memcpy(GP(at), s->app_reserved.p, s->app_reserved.n); rt_w32(G_MEM, buf + 64, at); rt_w32(G_MEM, buf + 68, s->app_reserved.n); }
    pthread_mutex_unlock(&s->m);
    RET(S_OK_, 4);
}
static void sv_SetApplicationDesc(Ctx *c)
{
    dump_args("SetApplicationDesc", c, 3); Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&s->m); uint8_t inst[16]; memcpy(inst, s->instance, 16); read_app_desc(s, ARG(1)); memcpy(s->instance, inst, 16);
    send_app_desc_update(s); pthread_mutex_unlock(&s->m);
    RET(S_OK_, 3);
}
/* the network round trip (ms) to the host of the game this Mac has joined; -1 when not in one */
int dpnet_client_rtt(void)
{
    Sess *s = g_joined; if (!s) return -1;
    pthread_mutex_lock(&s->m); int r = !s->server && s->srv && s->connected ? (int)dp8_rtt(s->srv) : -1; pthread_mutex_unlock(&s->m); return r;
}
/* the largest network round trip (ms) to the players in the game this Mac hosts; -1 when not hosting or alone */
int dpnet_host_max_rtt(void)
{
    Sess *s = g_hosting; if (!s) return -1;
    int best = -1; pthread_mutex_lock(&s->m);
    if (s->hosting) for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created && s->pl[i].conn) { int r = (int)dp8_rtt(s->pl[i].conn); if (r > best) best = r; }
    pthread_mutex_unlock(&s->m); return best;
}
static void sv_Host(Ctx *c)
{
    dump_args("Host", c, 8);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_for(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 8);
    uint32_t ad = ARG(1), devs = ARG(2), ndev = ARG(3), pctx = ARG(6);
    pthread_mutex_lock(&s->m);
    read_app_desc(s, ad); static const uint8_t zero[16]; if (!memcmp(s->instance, zero, 16)) arc4random_buf(s->instance, 16);
    uint16_t port = 0; if (ndev && devs) port = (uint16_t)addr_port(O(rt_r32(G_MEM, devs)));
    if (ensure_endpoint(s, 1, port)) { pthread_mutex_unlock(&s->m); LOG("server: no UDP port available\n"); RET(DPNERR_GENERIC, 8); }
    s->hosting = 1; g_hosting = s; s->ntver = 1; s->server_dpnid = make_dpnid(s, 0); s->server_ctx = pctx;
    LOG("server: hosting on UDP %u (enumeration port %s)\n", dp8_port(s->ep), dp8_enum_port_bound(s->ep) ? "6073" : "unavailable");
    { void portmap_open(uint16_t, uint16_t); portmap_open(dp8_port(s->ep), dp8_enum_port_bound(s->ep) ? DP8_ENUM_PORT : 0); }   /* internet players */
    pthread_mutex_unlock(&s->m);
    /* the server's own player, indicated on the calling thread as DirectPlay does */
    uint32_t pm = heap_alloc(w32_process_heap, 8, 12); rt_w32(G_MEM, pm, 12); rt_w32(G_MEM, pm + 4, s->server_dpnid); rt_w32(G_MEM, pm + 8, pctx);
    call_handler(c, s, M_CREATE_PLAYER, pm); s->server_ctx = rt_r32(G_MEM, pm + 8); gfree(pm);
    RET(S_OK_, 8);
}
static void sv_GetLocalHostAddresses(Ctx *c)
{
    dump_args("GetLocalHostAddresses", c, 4);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s || !s->ep) RET(DPNERR_INVALIDPARAM, 4);
    uint32_t arr = ARG(1), pcount = ARG(2);
    struct sockaddr_in addrs[16]; int n = 0; struct ifaddrs *ifs = 0;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && n < 16; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK) && (i->ifa_flags & IFF_UP)) {
                addrs[n] = *(struct sockaddr_in *)i->ifa_addr; addrs[n].sin_port = htons(dp8_port(s->ep)); n++;
            }
        freeifaddrs(ifs);
    }
    if (!n) { addrs[0].sin_family = AF_INET; addrs[0].sin_addr.s_addr = htonl(INADDR_LOOPBACK); addrs[0].sin_port = htons(dp8_port(s->ep)); n = 1; }
    {   /* the router's public address too, for players on the internet (when the router told us) */
        uint32_t portmap_public_ip(void); uint32_t pub = getenv("DS_NO_PORTMAP") ? 0 : portmap_public_ip(); int dup = 0;
        for (int i = 0; i < n; i++) if (addrs[i].sin_addr.s_addr == pub) dup = 1;
        if (pub && !dup && n < 16) { memmove(addrs + 1, addrs, sizeof *addrs * (size_t)n); addrs[0].sin_addr.s_addr = pub; n++; }   /* first: the one to give out */
    }
    uint32_t have = rt_r32(G_MEM, pcount); rt_w32(G_MEM, pcount, (uint32_t)n);
    if (!arr || have < (uint32_t)n) RET(DPNERR_BUFFERTOOSMALL, 4);
    for (int i = 0; i < n; i++) rt_w32(G_MEM, arr + 4 * (uint32_t)i, address_object(&addrs[i]));
    RET(S_OK_, 4);
}
static void sv_DestroyClient(Ctx *c)
{
    dump_args("DestroyClient", c, 5);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, ARG(1)); dp8_conn *conn = p ? p->conn : 0; if (p) p->leaving = 1;
    Blob d = {0}; if (ARG(2) && ARG(3)) blob_set(&d, GP(ARG(2)), ARG(3));
    pthread_mutex_unlock(&s->m);
    if (!conn) { free(d.p); RET(DPNERR_INVALIDPLAYER, 5); }
    send_terminate(conn, &d); dp8_disconnect(conn, 0); free(d.p);
    RET(S_OK_, 5);
}
static void sv_EnumPlayersAndGroups(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 4);
    uint32_t arr = ARG(1), pcount = ARG(2), ids[257]; int n = 0;
    pthread_mutex_lock(&s->m); if (s->hosting) ids[n++] = s->server_dpnid; for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created) ids[n++] = s->pl[i].dpnid; pthread_mutex_unlock(&s->m);
    uint32_t have = rt_r32(G_MEM, pcount); rt_w32(G_MEM, pcount, (uint32_t)n);
    if (!arr || have < (uint32_t)n) RET(DPNERR_BUFFERTOOSMALL, 4);
    for (int i = 0; i < n; i++) rt_w32(G_MEM, arr + 4 * (uint32_t)i, ids[i]);
    RET(S_OK_, 4);
}
static void sv_GetPlayerContext(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 4);
    if (ARG(1) == s->server_dpnid) { rt_w32(G_MEM, ARG(2), s->server_ctx); RET(S_OK_, 4); }
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, ARG(1)); uint32_t ctx = p ? p->ctx : 0; pthread_mutex_unlock(&s->m);
    if (!p) RET(DPNERR_INVALIDPLAYER, 4);
    rt_w32(G_MEM, ARG(2), ctx); RET(S_OK_, 4);
}
/* DPN_CONNECTION_INFO: round-trip time; the counters are not tracked */
static void connection_info(Ctx *c, dp8_conn *conn, uint32_t p, int nargs)
{
    if (!conn || !p) RET(DPNERR_INVALIDPLAYER, nargs);
    uint32_t sz = rt_r32(G_MEM, p); if (sz < 8) RET(DPNERR_INVALIDPARAM, nargs);
    memset(GP(p + 4), 0, sz - 4); rt_w32(G_MEM, p + 4, dp8_rtt(conn)); RET(S_OK_, nargs);
}
static void cl_GetConnectionInfo(Ctx *c) { Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; connection_info(c, s ? s->srv : 0, ARG(1), 3); }
static void sv_GetConnectionInfo(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 4);
    pthread_mutex_lock(&s->m); Player *p = player_by_id(s, ARG(1)); dp8_conn *conn = p ? p->conn : 0; pthread_mutex_unlock(&s->m);
    connection_info(c, conn, ARG(2), 4);
}
/* GetSendQueueInfo: messages not yet acknowledged; the byte count is not tracked */
static void queue_info(Ctx *c, dp8_conn *conn, uint32_t pmsgs, uint32_t pbytes, int nargs)
{
    uint32_t n = conn ? (uint32_t)dp8_pending(conn) : 0;
    if (getenv("DP8_STATS")) {                                   /* how often the game asks, and what it hears */
        static uint64_t t0; static unsigned calls, nonzero, maxn; uint64_t t = now_us(); if (!t0) t0 = t;
        calls++; if (n) nonzero++; if (n > maxn) maxn = n;
        if (t - t0 >= 5000000) { fprintf(stderr, "dpnet: send queue asked %u times in 5 s, non-empty %u times, at most %u\n", calls, nonzero, maxn); t0 = t; calls = nonzero = maxn = 0; }
    }
    if (pmsgs) rt_w32(G_MEM, pmsgs, n);
    if (pbytes) rt_w32(G_MEM, pbytes, 0);
    RET(S_OK_, nargs);
}
static void cl_GetSendQueueInfo(Ctx *c) { Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; queue_info(c, s ? s->srv : 0, ARG(1), ARG(2), 4); }
static void sv_GetSendQueueInfo(Ctx *c)
{
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(DPNERR_INVALIDPARAM, 5);
    pthread_mutex_lock(&s->m); Player *p = ARG(1) == s->server_dpnid ? 0 : player_by_id(s, ARG(1)); dp8_conn *conn = p ? p->conn : 0; pthread_mutex_unlock(&s->m);
    queue_info(c, conn, ARG(2), ARG(3), 5);
}
static void cs_Close(Ctx *c)
{
    dump_args("Close", c, 2);
    Obj *o = O(ARG(0)); Sess *s = o ? sess_of(o) : 0; if (!s) RET(S_OK_, 2);
    pthread_mutex_lock(&s->m);
    for (int i = 0; i < 8; i++) if (s->en[i].used) enum_end(&s->en[i]);
    if (s->server && s->hosting) {
        for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].conn) { send_terminate(s->pl[i].conn, 0); dp8_disconnect(s->pl[i].conn, 0); s->pl[i].leaving = 1; }
        s->hosting = 0; if (g_hosting == s) g_hosting = 0;
        void portmap_close(void); pthread_mutex_unlock(&s->m); portmap_close(); pthread_mutex_lock(&s->m);
    }
    if (!s->server && s->srv) { dp8_disconnect(s->srv, 0); s->connected = 0; }
    dp8_ep *ep = s->ep;
    pthread_mutex_unlock(&s->m);
    if (ep) {                                         /* let the goodbyes go out, then close the port */
        for (int t = 0; t < 50; t++) { int busy = 0; pthread_mutex_lock(&s->m); for (int i = 0; i < 256; i++) if (s->pl[i].conn && dp8_pending(s->pl[i].conn)) busy = 1; if (s->srv && dp8_pending(s->srv)) busy = 1; pthread_mutex_unlock(&s->m); if (!busy) break; usleep(10000); }
        pthread_mutex_lock(&s->m); s->ep = 0; s->srv = 0; for (int i = 0; i < 256; i++) s->pl[i].conn = 0; pthread_mutex_unlock(&s->m);
        dp8_close(ep);
    }
    if (s->server) {                                   /* everyone, the server's own player last */
        for (int i = 0; i < 256; i++) if (s->pl[i].used && s->pl[i].created) {
            uint32_t pm = heap_alloc(w32_process_heap, 8, 16); rt_w32(G_MEM, pm, 16); rt_w32(G_MEM, pm + 4, s->pl[i].dpnid); rt_w32(G_MEM, pm + 8, s->pl[i].ctx); rt_w32(G_MEM, pm + 12, 3);
            call_handler(c, s, M_DESTROY_PLAYER, pm); gfree(pm); player_free(&s->pl[i]);
        }
        if (s->server_dpnid) {
            uint32_t pm = heap_alloc(w32_process_heap, 8, 16); rt_w32(G_MEM, pm, 16); rt_w32(G_MEM, pm + 4, s->server_dpnid); rt_w32(G_MEM, pm + 8, s->server_ctx); rt_w32(G_MEM, pm + 12, 1);
            call_handler(c, s, M_DESTROY_PLAYER, pm); gfree(pm); s->server_dpnid = 0;
        }
    }
    RET(S_OK_, 2);
}
STUB(cs_GetCaps, 3, S_OK_) STUB(cs_SetCaps, 3, S_OK_) STUB(cs_SetSPCaps, 4, S_OK_)
STUB(cs_RegisterLobby, 4, S_OK_) STUB(sv_CreateGroup, 6, DPNERR_UNSUPPORTED)
STUB(sv_DestroyGroup, 5, DPNERR_UNSUPPORTED) STUB(sv_AddPlayerToGroup, 6, DPNERR_UNSUPPORTED) STUB(sv_RemovePlayerFromGroup, 6, DPNERR_UNSUPPORTED)
STUB(sv_SetGroupInfo, 6, DPNERR_UNSUPPORTED) STUB(sv_GetGroupInfo, 5, DPNERR_UNSUPPORTED) STUB(sv_EnumGroupMembers, 5, DPNERR_UNSUPPORTED)
STUB(sv_GetGroupContext, 4, DPNERR_UNSUPPORTED)

static uint32_t vtable(const char *iface, void (*const *fns)(Ctx *), int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "dpnet!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
static void init_vtables(void)
{
    if (vt[K_ADDRESS]) return;
    static void (*const cl[])(Ctx *) = { cs_QueryInterface, cs_AddRef, cs_Release, cs_Initialize, cs_EnumServiceProviders, cl_EnumHosts,
        cs_CancelAsyncOperation, cl_Connect, cl_Send, cl_GetSendQueueInfo, cs_GetApplicationDesc, cl_SetClientInfo, cl_GetServerInfo,
        cl_GetServerAddress, cs_Close, cs_ReturnBuffer, cs_GetCaps, cs_SetCaps, cs_SetSPCaps, cs_GetSPCaps, cl_GetConnectionInfo, cs_RegisterLobby };
    static void (*const sv[])(Ctx *) = { cs_QueryInterface, cs_AddRef, cs_Release, cs_Initialize, cs_EnumServiceProviders, cs_CancelAsyncOperation,
        sv_GetSendQueueInfo, cs_GetApplicationDesc, sv_SetServerInfo, sv_GetClientInfo, sv_GetClientAddress, sv_GetLocalHostAddresses,
        sv_SetApplicationDesc, sv_Host, sv_SendTo, sv_CreateGroup, sv_DestroyGroup, sv_AddPlayerToGroup, sv_RemovePlayerFromGroup,
        sv_SetGroupInfo, sv_GetGroupInfo, sv_EnumPlayersAndGroups, sv_EnumGroupMembers, cs_Close, sv_DestroyClient, cs_ReturnBuffer,
        sv_GetPlayerContext, sv_GetGroupContext, cs_GetCaps, cs_SetCaps, cs_SetSPCaps, cs_GetSPCaps, sv_GetConnectionInfo, cs_RegisterLobby };
    static void (*const ad[])(Ctx *) = { a_QueryInterface, a_AddRef, a_Release, a_BuildFromURLW, a_BuildFromURLA, a_Duplicate, a_SetEqual,
        a_IsEqual, a_Clear, a_GetURLW, a_GetURLA, a_GetSP, a_GetUserData, a_SetSP, a_SetUserData, a_GetNumComponents,
        a_GetComponentByName, a_GetComponentByIndex, a_AddComponent, a_GetDevice, a_SetDevice, a_BuildFromDirectPlay4Address };
    _Static_assert(sizeof cl / sizeof *cl == 22 && sizeof sv / sizeof *sv == 34 && sizeof ad / sizeof *ad == 22, "vtable sizes");
    vt[K_CLIENT] = vtable("IDirectPlay8Client", cl, 22); vt[K_SERVER] = vtable("IDirectPlay8Server", sv, 34); vt[K_ADDRESS] = vtable("IDirectPlay8Address", ad, 22);
}
/* ole32 CoCreateInstance for the DirectPlay 8 classes; returns 0 if the class is not one of ours */
uint32_t w32_dpnet_create(uint32_t clsid_first_dword)
{
    int kind = clsid_first_dword == 0x743f1dc6u ? K_CLIENT : clsid_first_dword == 0xda825e1bu ? K_SERVER : clsid_first_dword == 0x934a9523u ? K_ADDRESS : 0;
    if (!kind) return 0;
    init_vtables();
    uint32_t g = obj_new(kind);
    LOG("CoCreateInstance -> %s %08x\n", kind == K_CLIENT ? "client" : kind == K_SERVER ? "server" : "address", g);
    return g;
}
