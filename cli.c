/* Console front end: Linux until it gets a GUI, and the self-test on every OS. */
#include "net.h"
#ifndef _WIN32
#include <sys/select.h>
#endif
#undef NDEBUG /* the self-test relies on assert even in release builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int inject(SOCKET tx, SOCKET rx, const char *msg, const adapter *me, peer *peers) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);
    to.sin_addr.s_addr = inet_addr("127.0.0.1");
    sendto(tx, msg, (int)strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
    return handle_packet(rx, me, "LANSHARER1 me", peers);
}

/* Real sockets over loopback: we pretend to be 127.0.0.2, a "peer" at 127.0.0.1 announces. */
static void loopback_test(void) {
    adapter me = {0};
    peer peers[MAX_PEERS] = {0};
    SOCKET rx = open_socket(), tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(rx != INVALID_SOCKET && "port busy: close lansharer before running --test");
    me.ip.s_addr = inet_addr("127.0.0.2");
    me.mask.s_addr = inet_addr("255.0.0.0");
    assert(inject(tx, rx, "LANSHARER1 peer1", &me, peers) == 1); /* new peer */
    assert(strcmp(peers[0].name, "peer1") == 0 && peers[0].ip.s_addr == inet_addr("127.0.0.1"));
    assert(inject(tx, rx, "LANSHARER1 peer1", &me, peers) == 0); /* same peer again: no change */
    assert(inject(tx, rx, "LANSHARER1 peer2", &me, peers) == 1); /* renamed */
    assert(inject(tx, rx, "garbage", &me, peers) == 0);
    assert(prune_peers(peers) == 0);
    peers[0].seen -= TIMEOUT_MS + 1;
    assert(prune_peers(peers) == 1 && !peers[0].ip.s_addr); /* silent too long: gone */
    assert(inject(tx, rx, "LANSHARER1 me", &me, peers) == 0); /* own echo, even from an IP that isn't me.ip */
}

static int self_test(void) {
    char name[NAME_LEN], big[200];
    if (net_init() != 0) return 1;
    loopback_test();
    adapter lan = {0};
    assert(parse_beacon("LANSHARER1 box", 14, name) && strcmp(name, "box") == 0);
    assert(!parse_beacon("LANSHARER1 ", 11, name));     /* empty name */
    assert(!parse_beacon("LANSHARER", 9, name));        /* shorter than magic */
    assert(!parse_beacon("HELLOWORLD box", 14, name));  /* wrong magic */
    assert(!parse_beacon("LANSHARER1 b\nx", 14, name)); /* control char */
    memcpy(big, MAGIC, 11);
    memset(big + 11, 'a', 100);
    assert(parse_beacon(big, 11 + NAME_LEN - 1, name)); /* 63 chars: longest allowed */
    assert(!parse_beacon(big, 11 + NAME_LEN, name));    /* 64 chars: too long */
    lan.ip.s_addr = inet_addr("192.168.88.21");
    lan.mask.s_addr = inet_addr("255.255.255.0");
    assert(broadcast_addr(&lan).s_addr == inet_addr("192.168.88.255"));
    puts("self-test ok");
    return 0;
}

static void print_peers(const peer *peers) {
    int n = 0;
    puts("--- peers ---");
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].ip.s_addr) printf("%-24s %s\n", peers[i].name, inet_ntoa(peers[i].ip)), n++;
    if (!n) puts("(none)");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* peer changes must show up immediately even when piped */
    if (argc > 1 && strcmp(argv[1], "--test") == 0) return self_test();
    if (net_init() != 0) { fprintf(stderr, "Network init failed\n"); return 1; }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    adapter ads[MAX_ADAPTERS];
    int n = list_adapters(ads, MAX_ADAPTERS), pick = 1;
    if (n == 0) { fprintf(stderr, "No active IPv4 adapters found.\n"); return 1; }
    for (int i = 0; i < n; i++) printf("%d) %s  %s\n", i + 1, ads[i].name, inet_ntoa(ads[i].ip));
    if (n > 1) {
        printf("Pick adapter [1-%d]: ", n);
        if (scanf("%d", &pick) != 1 || pick < 1 || pick > n) { fprintf(stderr, "Invalid choice.\n"); return 1; }
    }
    adapter *me = &ads[pick - 1];

    char beacon[sizeof MAGIC + NAME_LEN];
    SOCKET s = open_socket();
    if (s == INVALID_SOCKET) { fprintf(stderr, "Could not open UDP port %d (another copy running?)\n", PORT); return 1; }
    make_beacon(beacon, sizeof beacon);
    printf("Announcing as \"%s\" to %s:%d. Ctrl+C to quit.\n", beacon + strlen(MAGIC),
           inet_ntoa(broadcast_addr(me)), PORT);

    peer peers[MAX_PEERS] = {0};
    long long next_send = 0;
    for (;;) {
        long long wait = next_send - now_ms();
        if (wait <= 0) {
            send_beacon(s, me, beacon);
            if (prune_peers(peers)) print_peers(peers);
            next_send = now_ms() + INTERVAL_MS;
            continue;
        }
        fd_set fds;
        struct timeval tv = {(long)(wait / 1000), (long)(wait % 1000) * 1000};
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        if (select((int)s + 1, &fds, NULL, NULL, &tv) > 0 && handle_packet(s, me, beacon, peers)) print_peers(peers);
    }
}
