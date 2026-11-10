/* relay.c without the game: stand-ins for the game's two ends, driven by tests/relay_e2e.py against recomp/relay/r6relay.py.
 *   relay_test host <port>          a "game" listening on 127.0.0.1:<port> (relay_listen, as winsock.c calls it); each
 *                                   read on a connection it accepted is answered with [last octet of the address the
 *                                   connection came from][u16 length][the bytes read]. Runs until stdin closes.
 *   relay_test join <port> <bytes>  a "game" joining through the local listener (relay_startup, as WSAStartup calls it)
 *                                   on 127.0.0.1:<port>: sends <bytes> bytes, checks they all come back in order, prints
 *                                   "ok <octet>".
 * The relay settings come from the environment (R6_RELAY, R6_ROOM, R6_RELAY_STATUS, ...), as from the launcher.
 * Build (MSYS2 CLANG64, from recomp/):
 *   clang -O1 -g -D_GNU_SOURCE -I runtime/win_include -o relay_test.exe tests/relay_test.c -pthread -lws2_32 -liphlpapi
 * Linux/macOS: cc -O1 -g -o relay_test tests/relay_test.c -pthread */
#include "../runtime/win32/relay.c"

static void *answer(void *arg)
{
    net_sock s = (net_sock)(intptr_t)arg; struct sockaddr_in a; socklen_t l = sizeof a; uint8_t buf[3 + 4096];
    uint8_t octet = getpeername(s, (struct sockaddr *)&a, &l) ? 0 : (uint8_t)(ntohl(a.sin_addr.s_addr) & 255);
    for (;;) {
        long n = recv(s, buf + 3, 4096, 0); if (n <= 0) break;
        buf[0] = octet; buf[1] = (uint8_t)(n >> 8); buf[2] = (uint8_t)n;
        if (send_all(s, buf, (size_t)n + 3)) break;
    }
    net_close(s); return 0;
}
static uint8_t *out; static size_t out_n; static net_sock join_s;
static void *sender(void *arg) { (void)arg; for (size_t k = 0; k < out_n; k += 1000) send_all(join_s, out + k, out_n - k < 1000 ? out_n - k : 1000); return 0; }
static void *stdin_watch(void *arg) { (void)arg; char b[64]; while (fread(b, 1, sizeof b, stdin) > 0) {} exit(0); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: relay_test host <port> | join <port> <bytes>\n"); return 2; }
    net_startup(); setvbuf(stdout, 0, _IONBF, 0);
    uint16_t port = (uint16_t)atoi(argv[2]);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (!strcmp(argv[1], "host")) {
        net_sock l = net_socket(AF_INET, SOCK_STREAM, 0);
        if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 8)) { perror("listen"); return 1; }
        relay_listen(1, port);
        pthread_t w; pthread_create(&w, 0, stdin_watch, 0);
        for (;;) {
            net_sock c = accept(l, 0, 0); if (c == NET_BAD) continue;
            pthread_t th; pthread_create(&th, 0, answer, (void *)(intptr_t)c); pthread_detach(th);
        }
    }
    if (!strcmp(argv[1], "join") && argc == 4) {
        relay_startup();
        net_sock s = NET_BAD;
        for (int tries = 0; tries < 100 && s == NET_BAD; tries++) {      /* the listener starts on its own thread */
            s = net_socket(AF_INET, SOCK_STREAM, 0);
            if (connect(s, (struct sockaddr *)&a, sizeof a)) { net_close(s); s = NET_BAD; usleep(50000); }
        }
        if (s == NET_BAD) { printf("fail connect\n"); return 1; }
        out_n = (size_t)atol(argv[3]); out = malloc(out_n); join_s = s;
        for (size_t k = 0; k < out_n; k++) out[k] = (uint8_t)(k * 7 + 3);
        pthread_t th; pthread_create(&th, 0, sender, 0);
        size_t got = 0; uint8_t octet = 0, h[3], buf[4096]; int bad = 0;
        while (got < out_n && !bad) {
            if (!readable(s, 5000) || recv_all(s, h, 3)) break;
            size_t m = (size_t)h[1] << 8 | h[2]; octet = h[0];
            if (!m || m > sizeof buf || got + m > out_n || recv_all(s, buf, m) || memcmp(buf, out + got, m)) bad = 1;
            got += m;
        }
        pthread_join(th, 0);
        int ok = !bad && got == out_n; printf("%s %u\n", ok ? "ok" : "fail", octet);
        usleep(1500000);                                                 /* stay a while: joiners overlap in the test */
        net_close(s); usleep(200000);
        return !ok;
    }
    return 2;
}
