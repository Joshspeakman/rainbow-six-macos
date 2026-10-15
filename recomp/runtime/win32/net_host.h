/* Host sockets on Linux, macOS and Windows: the few calls that differ between BSD sockets and Winsock 2, so the network
 * files (winsock.c, net_ole.c, df_net.c, p2p_dht.c, portmap.c, dp8proto.c, dpnet.c) share one code path.
 *   net_sock / NET_BAD          a host socket (SOCKET, 64-bit on Windows) and the failed/none value
 *   net_startup()               WSAStartup(2.2) once, thread-safe (nothing to do elsewhere)
 *   net_socket()                socket() after net_startup(); on Windows not inherited by child processes, and UDP
 *                               sockets with SIO_UDP_CONNRESET off (else an ICMP port-unreachable for one datagram sent
 *                               makes a later recvfrom fail with WSAECONNRESET: the DHT and LAN searches send to many
 *                               addresses that may not answer)
 *   net_close, net_set_nonblock, net_cloexec, net_poll (WSAPoll), net_errno/net_set_errno (WSAGetLastError)
 *   getifaddrs/freeifaddrs      on Windows from GetAdaptersAddresses (IPv4 unicast addresses with netmask and broadcast)
 *   net_loopback_pair()         Windows: a connected pair of loopback UDP sockets, to wake a WSAPoll (which takes sockets
 *                               only, not pipes)
 * Each file includes this first, before w32.h: on Windows it brings in <winsock2.h> (and with it <windows.h>) ahead of
 * the runtime's own names. Link with -lws2_32 -liphlpapi on Windows. */
#ifndef DS_NET_HOST_H
#define DS_NET_HOST_H
#ifdef _WIN32
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L       /* as win_compat.h: <time.h> (from <windows.h>) then has localtime_r */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef SOCKET net_sock;
#define NET_BAD INVALID_SOCKET
#define NET_EWOULDBLOCK WSAEWOULDBLOCK
#define NET_EINPROGRESS WSAEWOULDBLOCK                    /* what a non-blocking connect reports on Windows */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0                                    /* Windows has no SIGPIPE */
#endif
/* buffers and option values are char * (and lengths int) on Windows */
#define setsockopt(s, l, o, v, n) setsockopt((s), (l), (o), (const char *)(v), (int)(n))
#define getsockopt(s, l, o, v, n) getsockopt((s), (l), (o), (char *)(v), (n))
#define send(s, b, n, f) send((s), (const char *)(b), (int)(n), (f))
#define recv(s, b, n, f) recv((s), (char *)(b), (int)(n), (f))
#define sendto(s, b, n, f, a, l) sendto((s), (const char *)(b), (int)(n), (f), (a), (int)(l))
#define recvfrom(s, b, n, f, a, l) recvfrom((s), (char *)(b), (int)(n), (f), (a), (l))

static inline BOOL CALLBACK net_startup_once(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    (void)once; (void)arg; (void)ctx;
    WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0;
}
static inline void net_startup(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, net_startup_once, 0, 0);
}
static inline int net_errno(void) { return WSAGetLastError(); }
static inline void net_set_errno(int e) { WSASetLastError(e); }
static inline int net_close(net_sock s) { return closesocket(s); }
static inline void net_set_nonblock(net_sock s, int on) { u_long v = on != 0; ioctlsocket(s, FIONBIO, &v); }
static inline void net_cloexec(net_sock s) { SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0); }
static inline void net_udp_noreset(net_sock s)
{
    BOOL off = FALSE; DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, 0, 0, &got, 0, 0);
}
static inline net_sock net_socket(int af, int type, int proto)
{
    net_startup();
    net_sock s = socket(af, type, proto);
    if (s == NET_BAD) return s;
    net_cloexec(s);
    if (type == SOCK_DGRAM) net_udp_noreset(s);
    return s;
}
static inline int net_poll(struct pollfd *pf, unsigned n, int ms) { return WSAPoll(pf, (ULONG)n, ms); }

/* a connected pair of UDP sockets on 127.0.0.1: send on [1], [0] becomes readable (non-blocking [0]) */
static inline int net_loopback_pair(net_sock out[2])
{
    out[0] = out[1] = NET_BAD;
    net_sock a = net_socket(AF_INET, SOCK_DGRAM, 0), b = net_socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa); int l = sizeof sa;
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (a == NET_BAD || b == NET_BAD || bind(a, (struct sockaddr *)&sa, sizeof sa) || getsockname(a, (struct sockaddr *)&sa, &l) ||
        connect(b, (struct sockaddr *)&sa, sizeof sa)) {
        if (a != NET_BAD) closesocket(a);
        if (b != NET_BAD) closesocket(b);
        return -1;
    }
    net_set_nonblock(a, 1); out[0] = a; out[1] = b;
    return 0;
}

/* getifaddrs: the IPv4 unicast addresses of every adapter (what Linux and macOS list for AF_INET) */
#ifndef IFF_UP
#define IFF_UP 0x1
#endif
#ifndef IFF_BROADCAST
#define IFF_BROADCAST 0x2
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x4
#endif
struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr, *ifa_netmask, *ifa_broadaddr;
    void *ifa_data;
};
typedef struct { struct ifaddrs ifa; struct sockaddr_in addr, mask, bcast; char name[64]; } net_ifa_node;
static inline void net_freeifaddrs(struct ifaddrs *i)
{
    while (i) { struct ifaddrs *n = i->ifa_next; free(i); i = n; }   /* each node is one net_ifa_node */
}
static inline int net_getifaddrs(struct ifaddrs **out)
{
    *out = 0;
    ULONG len = 15000, r = ERROR_BUFFER_OVERFLOW; IP_ADAPTER_ADDRESSES *aa = 0;
    for (int tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(aa); aa = malloc(len); if (!aa) { errno = ENOMEM; return -1; }
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
                                 0, aa, &len);
    }
    if (r != NO_ERROR) { free(aa); if (r == ERROR_NO_DATA) return 0; errno = EIO; return -1; }
    struct ifaddrs **tail = out;
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next)
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            net_ifa_node *n = calloc(1, sizeof *n); if (!n) break;
            memcpy(&n->addr, u->Address.lpSockaddr, sizeof n->addr);
            unsigned pl = u->OnLinkPrefixLength > 32 ? 32 : u->OnLinkPrefixLength;
            uint32_t m = pl ? 0xffffffffu << (32 - pl) : 0;
            n->mask.sin_family = AF_INET; n->mask.sin_addr.s_addr = htonl(m);
            n->bcast.sin_family = AF_INET; n->bcast.sin_addr.s_addr = n->addr.sin_addr.s_addr | htonl(~m);
            snprintf(n->name, sizeof n->name, "%s", a->AdapterName ? a->AdapterName : "");
            n->ifa.ifa_name = n->name; n->ifa.ifa_addr = (struct sockaddr *)&n->addr; n->ifa.ifa_netmask = (struct sockaddr *)&n->mask;
            if (a->OperStatus == IfOperStatusUp) n->ifa.ifa_flags |= IFF_UP;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) n->ifa.ifa_flags |= IFF_LOOPBACK;
            else if (a->IfType != IF_TYPE_PPP && a->IfType != IF_TYPE_TUNNEL && pl < 31) {
                n->ifa.ifa_flags |= IFF_BROADCAST; n->ifa.ifa_broadaddr = (struct sockaddr *)&n->bcast;
            }
            *tail = &n->ifa; tail = &n->ifa.ifa_next;
        }
    free(aa);
    return 0;
}
#define getifaddrs(p)  net_getifaddrs(p)
#define freeifaddrs(p) net_freeifaddrs(p)

#else /* Linux, macOS */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int net_sock;
#define NET_BAD (-1)
#define NET_EWOULDBLOCK EWOULDBLOCK
#define NET_EINPROGRESS EINPROGRESS
static inline void net_startup(void) {}
static inline int net_errno(void) { return errno; }
static inline void net_set_errno(int e) { errno = e; }
static inline int net_close(net_sock s) { return close(s); }
static inline void net_set_nonblock(net_sock s, int on) { int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK); }
static inline void net_cloexec(net_sock s) { fcntl(s, F_SETFD, FD_CLOEXEC); }
static inline net_sock net_socket(int af, int type, int proto) { return socket(af, type, proto); }
static inline int net_poll(struct pollfd *pf, unsigned n, int ms) { return poll(pf, (nfds_t)n, ms); }
#endif
#endif
