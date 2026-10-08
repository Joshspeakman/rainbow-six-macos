/* WSOCK32 host name and addresses (what the multiplayer screens show) and OLE32 basics (DirectPlay 8 objects from dpnet.c). */
#include "w32.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <unistd.h>
IMPL(wsock32, ord115)                                                 /* WSAStartup(version, WSADATA*) */
{
    uint32_t d = ARG(1); memset(GP(d), 0, 400);
    rt_w16(G_MEM, d, ARG(0) & 0xffff); rt_w16(G_MEM, d + 2, 0x0202);
    strcpy((char *)GP(d + 4), "WinSock 2.0"); strcpy((char *)GP(d + 261), "Running");
    rt_w16(G_MEM, d + 390, 100); rt_w16(G_MEM, d + 392, 65467);
    RET(0, 2);
}
IMPL(wsock32, ord116) { RET(0, 0); }                                  /* WSACleanup */
static void local_host_name(char *out, size_t cap)
{
    char h[256] = "localhost"; gethostname(h, sizeof h); h[sizeof h - 1] = 0;
    char *dot = strchr(h, '.'); if (dot && !strcmp(dot, ".local")) *dot = 0;   /* Windows-style short name */
    snprintf(out, cap, "%s", h);
}
/* GameSpy (the game's lobby, server list and NAT service, also used for LAN games) shut down in 2014. Its host names
 * are sent to a replacement, chosen with DS2_LOBBY:
 *   openspy (default)  OpenSpy, the community-run GameSpy replacement: <name>.gamespy.com -> <name>.openspy.net
 *   off                the original names (which no longer resolve)
 * Returns the name to resolve (into buf), or NULL to keep the original. */
static const char *lobby_rewrite(const char *n, char *buf, size_t cap)
{
    static const char *mode; if (!mode) { mode = getenv("DS2_LOBBY"); if (!mode || !*mode) mode = "openspy"; }
    size_t l = strlen(n); static const char suf[] = ".gamespy.com";
    if (l <= sizeof suf - 1 || strcasecmp(n + l - (sizeof suf - 1), suf) || !strcasecmp(mode, "off")) return NULL;
    snprintf(buf, cap, "%.*s.openspy.net", (int)(l - (sizeof suf - 1)), n);
    static int logged; if (logged++ < 8) fprintf(stderr, "w32: lobby: %s -> %s\n", n, buf);
    return buf;
}
/* gethostbyname: one hostent per thread, as on Windows. Our own name gives every IPv4 interface address (what a host
 * shows its players); other names go to the resolver. Layout: hostent{h_name, h_aliases, h_addrtype, h_length,
 * h_addr_list}, then the pointer arrays, addresses and name. */
IMPL(wsock32, ord52)
{
    static __thread uint32_t blk; if (!blk) blk = heap_alloc(w32_process_heap, 8, 1024);
    char name[256]; snprintf(name, sizeof name, "%s", GS(ARG(0))); char me[256]; local_host_name(me, sizeof me);
    uint32_t addrs[16]; int n = 0;
    if (!strcasecmp(name, me) || !*name) {
        struct ifaddrs *ifs = 0;
        if (!getifaddrs(&ifs)) {
            for (struct ifaddrs *i = ifs; i && n < 16; i = i->ifa_next)
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && !(i->ifa_flags & IFF_LOOPBACK))
                    addrs[n++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
            freeifaddrs(ifs);
        }
        if (!n) addrs[n++] = htonl(INADDR_LOOPBACK);
        uint32_t portmap_public_ip(void); uint32_t pub = getenv("DS_NO_PORTMAP") ? 0 : portmap_public_ip();   /* for internet players */
        int dup = 0; for (int i = 0; i < n; i++) if (addrs[i] == pub) dup = 1;
        if (pub && !dup && n < 16) { memmove(addrs + 1, addrs, sizeof *addrs * (size_t)n); addrs[0] = pub; n++; }   /* first: the one to give out */
    } else {
        struct addrinfo hint = {0}, *res = 0; hint.ai_family = AF_INET; char alt[300]; const char *q = lobby_rewrite(name, alt, sizeof alt);
        if (getaddrinfo(q ? q : name, 0, &hint, &res)) RET(0, 1);
        for (struct addrinfo *r = res; r && n < 16; r = r->ai_next) addrs[n++] = ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        if (!n) RET(0, 1);
    }
    uint32_t list = blk + 16, alias = list + 4 * 17, data = alias + 4, nm = data + 4 * 16;
    for (int i = 0; i < n; i++) { rt_w32(G_MEM, data + 4 * (uint32_t)i, addrs[i]); rt_w32(G_MEM, list + 4 * (uint32_t)i, data + 4 * (uint32_t)i); }
    rt_w32(G_MEM, list + 4 * (uint32_t)n, 0); rt_w32(G_MEM, alias, 0);
    snprintf((char *)GP(nm), 256, "%s", name);
    rt_w32(G_MEM, blk, nm); rt_w32(G_MEM, blk + 4, alias); rt_w16(G_MEM, blk + 8, AF_INET); rt_w16(G_MEM, blk + 10, 4); rt_w32(G_MEM, blk + 12, list);
    RET(blk, 1);
}
IMPL(wsock32, ord57)                                                  /* gethostname(buf, len) */
{
    char h[256]; local_host_name(h, sizeof h);
    if (strlen(h) + 1 > ARG(1)) RET(0xffffffffu, 2);
    strcpy((char *)GP(ARG(0)), h); RET(0, 2);
}
IMPL(wsock32, ord10)                                                  /* inet_addr */
{
    unsigned a, b, c2, d; if (sscanf(GS(ARG(0)), "%u.%u.%u.%u", &a, &b, &c2, &d) == 4) RET((d << 24) | (c2 << 16) | (b << 8) | a, 1);
    RET(0xffffffffu, 1);
}
IMPL(wsock32, ord14) { RET(__builtin_bswap32(ARG(0)), 1); }           /* ntohl */
IMPL(ole32, CoInitializeEx) { RET(0, 2); }
IMPL(ole32, CoUninitialize) { RET(0, 0); }
IMPL(ole32, CoCreateInstance)
{
    uint32_t clsid = ARG(0), out = ARG(4);
    uint32_t w32_dpnet_create(uint32_t clsid_first_dword);
    uint32_t obj = w32_dpnet_create(rt_r32(G_MEM, clsid));           /* DirectPlay 8 client, server, address */
    if (obj) { if (out) rt_w32(G_MEM, out, obj); RET(0, 5); }
    uint32_t w32_dshow_create(uint32_t clsid_first_dword);
    obj = w32_dshow_create(rt_r32(G_MEM, clsid));                     /* DirectShow filter graph (music) */
    if (obj) { if (out) rt_w32(G_MEM, out, obj); RET(0, 5); }
    const uint8_t *g = GP(clsid);
    fprintf(stderr, "w32: CoCreateInstance({%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}) -> class not registered\n", rt_r32(G_MEM, clsid),
            rt_r16(G_MEM, clsid + 4), rt_r16(G_MEM, clsid + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    if (out) rt_w32(G_MEM, out, 0);
    RET(0x80040154u, 5);
}
IMPL(ole32, CoCreateGuid)
{
    static uint32_t n = 1; uint32_t g = ARG(0);
    for (int k = 0; k < 4; k++) rt_w32(G_MEM, g + 4 * (uint32_t)k, w32_deterministic ? 0x12340000u + n++ : arc4random());
    RET(0, 1);
}
IMPL(ole32, StringFromGUID2)
{
    uint32_t g = ARG(0), out = ARG(1), cap = ARG(2); uint8_t b[16]; memcpy(b, GP(g), 16); char s[40];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rt_r32(G_MEM, g), rt_r16(G_MEM, g + 4), rt_r16(G_MEM, g + 6),
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    if (cap < 39) RET(0, 3);
    w32_mb_to_wide(1252, (const uint8_t *)s, 39, (uint16_t *)GP(out), 39); RET(39, 3);
}
IMPL(ole32, IIDFromString)
{
    char s[64]; w32_wide_to_mb(1252, GW(ARG(0)), -1, (uint8_t *)s, sizeof s);
    unsigned a, b, c2, d[8];
    if (sscanf(s, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &a, &b, &c2, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) RET(0x80040064u, 2);
    uint32_t g = ARG(1); rt_w32(G_MEM, g, a); rt_w16(G_MEM, g + 4, b); rt_w16(G_MEM, g + 6, c2);
    for (int k = 0; k < 8; k++) rt_w8(G_MEM, g + 8 + (uint32_t)k, d[k]);
    RET(0, 2);
}
