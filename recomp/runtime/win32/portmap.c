/* Router port mapping, so a game hosted on this Mac can be joined over the internet without forwarding ports by hand
 * (what Microsoft's DirectPlay did with its UPnP helper). While hosting, UDP 2302 (the game) and 6073 (enumeration)
 * are mapped on the router with NAT-PMP (RFC 6886), or failing that UPnP IGD (WANIPConnection / WANPPPConnection);
 * the mapping is renewed while the game runs and removed when hosting ends. The router's public address is also
 * learned, so the game can show it. DS_NO_PORTMAP=1 leaves the router alone. */
#include <arpa/inet.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <unistd.h>

static pthread_mutex_t pm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t public_ip;                       /* network order; 0 = not known */
static int public_tried;

/* ---- the default IPv4 gateway, from the routing table ---- */
static uint32_t gateway(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return 0;
    char line[512], iface[64]; unsigned long dst, gw; unsigned flags, metric, best = ~0u;
    uint32_t result = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %lx %lx %x %*u %*u %u", iface, &dst, &gw, &flags, &metric) == 5 &&
            dst == 0 && (flags & 3) == 3 && metric < best) { result = (uint32_t)gw; best = metric; }
    }
    fclose(f); return result;
#else
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
    size_t len = 0; if (sysctl(mib, 6, 0, &len, 0, 0) < 0 || !len) return 0;
    char *buf = malloc(len); if (!buf || sysctl(mib, 6, buf, &len, 0, 0) < 0) { free(buf); return 0; }
    uint32_t gw = 0;
    for (char *p = buf; p + sizeof(struct rt_msghdr) <= buf + len; ) {
        struct rt_msghdr *rt = (struct rt_msghdr *)p; struct sockaddr *sa = (struct sockaddr *)(rt + 1), *dst = 0, *gwa = 0;
        for (int i = 0; i < RTAX_MAX; i++) if (rt->rtm_addrs & (1 << i)) {
            if (i == RTAX_DST) dst = sa;
            if (i == RTAX_GATEWAY) gwa = sa;
            sa = (struct sockaddr *)((char *)sa + (sa->sa_len ? ((sa->sa_len + 3) & ~3) : 4));
        }
        if (dst && gwa && dst->sa_family == AF_INET && gwa->sa_family == AF_INET && !((struct sockaddr_in *)dst)->sin_addr.s_addr) {
            gw = ((struct sockaddr_in *)gwa)->sin_addr.s_addr; break;
        }
        if (!rt->rtm_msglen) break;
        p += rt->rtm_msglen;
    }
    free(buf); return gw;
#endif
}
/* this Mac's address on the router's network */
static uint32_t local_ip_toward(uint32_t gw)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return 0;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(9); a.sin_addr.s_addr = gw;
    uint32_t r = 0; socklen_t l = sizeof a;
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &l)) r = a.sin_addr.s_addr;
    close(s); return r;
}

/* ---- NAT-PMP ---- */
static int natpmp(uint32_t gw, const uint8_t *req, size_t rl, uint8_t *resp, size_t cap, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(5351); a.sin_addr.s_addr = gw;
    int n = -1;
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) {
        for (int tries = 0, wait = timeout_ms / 3 > 0 ? timeout_ms / 3 : 1; tries < 3 && n < 0; tries++, wait *= 2) {
            if (send(s, req, rl, 0) < 0) break;
            struct pollfd pf = {s, POLLIN, 0};
            if (poll(&pf, 1, wait) > 0) { ssize_t k = recv(s, resp, cap, 0); if (k >= 4 && resp[1] == (uint8_t)(req[1] + 128)) n = (int)k; }
        }
    }
    close(s); return n;
}
static uint32_t natpmp_public(uint32_t gw)
{
    uint8_t req[2] = {0, 0}, r[16];
    int n = natpmp(gw, req, 2, r, sizeof r, 300);
    if (n < 12 || r[2] || r[3]) return 0;
    uint32_t ip; memcpy(&ip, r + 8, 4); return ip;
}
/* map (lifetime > 0) or remove (0) UDP port `port` on the router; returns the lifetime granted, or -1 */
static int natpmp_map(uint32_t gw, uint16_t port, uint32_t lifetime)
{
    uint8_t req[12] = {0, 1, 0, 0}, r[16];
    req[4] = (uint8_t)(port >> 8); req[5] = (uint8_t)port; req[6] = lifetime ? (uint8_t)(port >> 8) : 0; req[7] = lifetime ? (uint8_t)port : 0;
    req[8] = (uint8_t)(lifetime >> 24); req[9] = (uint8_t)(lifetime >> 16); req[10] = (uint8_t)(lifetime >> 8); req[11] = (uint8_t)lifetime;
    int n = natpmp(gw, req, 12, r, sizeof r, 600);
    if (n < 16 || r[2] || r[3]) return -1;
    uint16_t ext = (uint16_t)(r[10] << 8 | r[11]);
    if (lifetime && ext != port) { natpmp_map(gw, port, 0); return -1; }   /* the router gave another port: of no use to joiners */
    return (int)((uint32_t)r[12] << 24 | (uint32_t)r[13] << 16 | (uint32_t)r[14] << 8 | r[15]);
}

/* ---- UPnP IGD ---- */
static char upnp_control[512], upnp_service[128];   /* control URL and service type, once found */
static int http(const char *url, const char *action, const char *body, char *out, size_t cap)
{
    char host[128] = ""; int port = 80; const char *path = "/";
    if (strncmp(url, "http://", 7)) return -1;
    const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    size_t hl = (size_t)((colon && (!slash || colon < slash)) ? colon - h : (slash ? slash - h : (long)strlen(h)));
    if (hl >= sizeof host) return -1;
    memcpy(host, h, hl); host[hl] = 0;
    if (colon && (!slash || colon < slash)) port = atoi(colon + 1);
    if (slash) path = slash;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
    struct timeval tv = {2, 0}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    char req[4096]; int rl;
    if (body) rl = snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                            "SOAPAction: \"%s#%s\"\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", path, host, port, upnp_service, action, strlen(body), body);
    else rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
    if (rl <= 0 || send(s, req, (size_t)rl, 0) != rl) { close(s); return -1; }
    size_t n = 0; ssize_t k; time_t until = time(0) + 5;       /* all of it within 5 s: a router that drips bytes can't stall the game */
    while (n + 1 < cap && time(0) < until && (k = recv(s, out + n, cap - 1 - n, 0)) > 0) n += (size_t)k;
    out[n] = 0; close(s);
    return strstr(out, " 200 ") ? 0 : -1;
}
static int xml_value(const char *x, const char *tag, char *out, size_t cap)   /* text of the first <tag> */
{
    char open[96]; snprintf(open, sizeof open, "<%s>", tag);
    const char *p = strcasestr(x, open); if (!p) return -1;
    p += strlen(open); const char *e = strchr(p, '<'); if (!e || (size_t)(e - p) >= cap) return -1;
    memcpy(out, p, (size_t)(e - p)); out[e - p] = 0; return 0;
}
static int upnp_find(void)
{
    if (*upnp_control) return 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
    const char *msg = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                      "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    struct sockaddr_in m = {0}; m.sin_family = AF_INET; m.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &m.sin_addr);
    sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&m, sizeof m);
    char loc[512] = ""; char buf[2048];
    struct pollfd pf = {s, POLLIN, 0};
    while (!*loc && poll(&pf, 1, 2500) > 0) {
        ssize_t k = recv(s, buf, sizeof buf - 1, 0); if (k <= 0) break; buf[k] = 0;
        char *l = strcasestr(buf, "\nlocation:"); if (!l) continue;
        l += 10; while (*l == ' ') l++;
        size_t n = strcspn(l, "\r\n"); if (n < sizeof loc) { memcpy(loc, l, n); loc[n] = 0; }
    }
    close(s);
    if (!*loc) return -1;
    static char desc[65536];
    if (http(loc, 0, 0, desc, sizeof desc)) return -1;
    static const char *types[] = {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                  "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (int t = 0; t < 3; t++) {
        const char *p = strstr(desc, types[t]); if (!p) continue;
        char ctl[400]; if (xml_value(p, "controlURL", ctl, sizeof ctl)) continue;
        snprintf(upnp_service, sizeof upnp_service, "%s", types[t]);
        if (!strncmp(ctl, "http://", 7)) snprintf(upnp_control, sizeof upnp_control, "%s", ctl);
        else {                                       /* relative to the description's host */
            const char *hostend = strchr(loc + 7, '/'); int hl = hostend ? (int)(hostend - loc) : (int)strlen(loc);
            snprintf(upnp_control, sizeof upnp_control, "%.*s%s%s", hl, loc, *ctl == '/' ? "" : "/", ctl);
        }
        return 0;
    }
    return -1;
}
static int upnp_call(const char *action, const char *args, char *out, size_t cap)
{
    char body[2048];
    snprintf(body, sizeof body, "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, upnp_service, args, action);
    return http(upnp_control, action, body, out, cap);
}
static uint32_t upnp_public(void)
{
    char out[4096], ip[64]; uint32_t r = 0;
    if (upnp_find() || upnp_call("GetExternalIPAddress", "", out, sizeof out) || xml_value(out, "NewExternalIPAddress", ip, sizeof ip)) return 0;
    if (inet_pton(AF_INET, ip, &r) != 1) return 0;
    return r;
}
static int upnp_map(uint16_t port, uint32_t lan, int add)
{
    char out[4096], args[1024], lanip[32]; inet_ntop(AF_INET, &lan, lanip, sizeof lanip);
    if (upnp_find()) return -1;
    if (!add) {
        snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>", port);
        return upnp_call("DeletePortMapping", args, out, sizeof out);
    }
    for (int lease = 3600; ; lease = 0) {            /* some routers only take permanent mappings (lease 0) */
        snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                 "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                 "<NewPortMappingDescription>Dungeon Siege</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
                 port, port, lanip, lease);
        if (!upnp_call("AddPortMapping", args, out, sizeof out)) return lease;
        if (!lease) return -1;
    }
}

/* ---- the router's public address (for showing to the player) ---- */
uint32_t portmap_public_ip(void)
{
    pthread_mutex_lock(&pm_lock);
    if (!public_tried) {
        public_tried = 1;
        uint32_t gw = gateway();
        if (gw) public_ip = natpmp_public(gw);
        if (!public_ip) public_ip = upnp_public();
        if (public_ip) { char s[32]; inet_ntop(AF_INET, &public_ip, s, sizeof s); fprintf(stderr, "portmap: the router's public address is %s\n", s); }
    }
    uint32_t r = public_ip; pthread_mutex_unlock(&pm_lock);
    return r;
}

/* ---- mapping while hosting ---- */
static struct { int active, stop; uint16_t ports[2]; int nports; int method; uint32_t gw, lan; pthread_t th; pthread_cond_t cv; } pm =
    {0, 0, {0, 0}, 0, 0, 0, 0, 0, PTHREAD_COND_INITIALIZER};
static void *pm_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&pm_lock);
    while (!pm.stop) {
        int lease = -1; uint16_t ports[2]; int n = pm.nports; memcpy(ports, pm.ports, sizeof ports);
        uint32_t gw = pm.gw, lan = pm.lan; int method = pm.method;
        pthread_mutex_unlock(&pm_lock);
        int ok = 0;
        if (method != 2 && gw) {                       /* NAT-PMP */
            int all = 1; for (int i = 0; i < n; i++) { int l = natpmp_map(gw, ports[i], 3600); if (l < 0) all = 0; else lease = l; }
            if (all) { ok = 1; method = 1; }
        }
        if (!ok && method != 1) {                      /* UPnP */
            int all = 1; for (int i = 0; i < n; i++) { int l = upnp_map(ports[i], lan, 1); if (l < 0) all = 0; else lease = l; }
            if (all) { ok = 1; method = 2; }
        }
        pthread_mutex_lock(&pm_lock);
        if (ok && !pm.method) {
            pm.method = method;
            char p[32] = "", lip[32]; uint32_t pub = public_ip; inet_ntop(AF_INET, &lan, lip, sizeof lip); if (pub) inet_ntop(AF_INET, &pub, p, sizeof p);
            fprintf(stderr, "portmap: UDP %u%s%u forwarded to %s by the router (%s)%s%s\n", ports[0], n > 1 ? " and " : "", n > 1 ? ports[1] : 0, lip,
                    method == 1 ? "NAT-PMP" : "UPnP", *p ? "; players on the internet can join " : "", p);
        } else if (!ok && !pm.method) fprintf(stderr, "portmap: the router did not forward the game's ports (no NAT-PMP or UPnP); players outside "
                                                  "this network need the ports forwarded by hand (UDP 2302 and 6073)\n");
        if (!ok) break;
        /* renew at half the lease (permanent UPnP mappings: check every 30 min that they are still there) */
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += lease > 0 ? lease / 2 : 1800;
        while (!pm.stop && pthread_cond_timedwait(&pm.cv, &pm_lock, &ts) == 0) {}
    }
    pthread_mutex_unlock(&pm_lock);
    return 0;
}
void portmap_open(uint16_t game_port, uint16_t enum_port)
{
    if (getenv("DS_NO_PORTMAP")) return;
    portmap_public_ip();
    pthread_mutex_lock(&pm_lock);
    if (pm.active) { pthread_mutex_unlock(&pm_lock); return; }
    pm.active = 1; pm.stop = 0; pm.method = 0; pm.nports = 0;
    pm.ports[pm.nports++] = game_port; if (enum_port && enum_port != game_port) pm.ports[pm.nports++] = enum_port;
    pm.gw = gateway(); pm.lan = local_ip_toward(pm.gw);
    pthread_create(&pm.th, 0, pm_thread, 0);
    pthread_mutex_unlock(&pm_lock);
}
void portmap_close(void)
{
    pthread_mutex_lock(&pm_lock);
    if (!pm.active) { pthread_mutex_unlock(&pm_lock); return; }
    pm.stop = 1; pthread_cond_broadcast(&pm.cv);
    pthread_mutex_unlock(&pm_lock);
    pthread_join(pm.th, 0);
    pthread_mutex_lock(&pm_lock);
    for (int i = 0; i < pm.nports; i++) {
        if (pm.method == 1) natpmp_map(pm.gw, pm.ports[i], 0);
        else if (pm.method == 2) upnp_map(pm.ports[i], pm.lan, 0);
    }
    if (pm.method) fprintf(stderr, "portmap: the game's ports are no longer forwarded\n");
    pm.active = 0; pm.method = 0;
    pthread_mutex_unlock(&pm_lock);
}
