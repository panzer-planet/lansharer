#include "net.h"
#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    return 0;
#endif
}

long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
#endif
}

/* /31 and /32 (VPNs, point-to-point links) have no broadcast address worth sending to. */
static int has_subnet(struct in_addr mask) { return (ntohl(mask.s_addr) & 3) == 0; }

#ifdef _WIN32
int list_adapters(adapter *out, int max) {
    ULONG size = 16 * 1024, rc;
    IP_ADAPTER_ADDRESSES *buf = NULL;
    int n = 0;
    do {
        free(buf);
        if (!(buf = malloc(size))) return 0;
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, buf, &size);
    } while (rc == ERROR_BUFFER_OVERFLOW);
    for (IP_ADAPTER_ADDRESSES *a = rc == NO_ERROR ? buf : NULL; a && n < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
            adapter *ad = &out[n];
            ULONG mask;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
            ad->mask.s_addr = mask;
            if (!has_subnet(ad->mask)) continue;
            if (!WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, ad->name, sizeof ad->name, NULL, NULL))
                strcpy(ad->name, "?");
            ad->ip = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr;
            n++;
        }
    }
    free(buf);
    return n;
}
#else
int list_adapters(adapter *out, int max) {
    struct ifaddrs *list;
    int n = 0;
    if (getifaddrs(&list) != 0) return 0;
    for (struct ifaddrs *i = list; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        adapter *ad = &out[n];
        ad->mask = ((struct sockaddr_in *)i->ifa_netmask)->sin_addr;
        if (!has_subnet(ad->mask)) continue;
        snprintf(ad->name, sizeof ad->name, "%s", i->ifa_name);
        ad->ip = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        n++;
    }
    freeifaddrs(list);
    return n;
}
#endif

/* Packets come from anyone on the LAN: accept only MAGIC + 1..63 printable ASCII chars. */
int parse_beacon(const char *buf, int len, char out[NAME_LEN]) {
    int m = (int)strlen(MAGIC), n = len - m;
    if (n < 1 || n >= NAME_LEN || memcmp(buf, MAGIC, m) != 0) return 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[m + i];
        if (c < 0x20 || c > 0x7e) return 0;
        out[i] = (char)c;
    }
    out[n] = '\0';
    return 1;
}

void make_beacon(char *out, size_t size) {
    char host[NAME_LEN];
    if (gethostname(host, sizeof host) != 0) strcpy(host, "unknown");
    host[NAME_LEN - 1] = '\0';
    for (unsigned char *p = (unsigned char *)host; *p; p++)
        if (*p < 0x20 || *p > 0x7e) *p = '?';
    snprintf(out, size, MAGIC "%s", host);
}

SOCKET open_socket(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int on = 1;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on) != 0 ||
        bind(s, (struct sockaddr *)&local, sizeof local) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return INVALID_SOCKET;
    }
    return s;
}

/* Subnet-directed broadcast reaches the chosen adapter's subnet on both OSes, unlike 255.255.255.255.
   With two adapters on that subnet the OS may send it out the other one, which is the same LAN anyway. */
struct in_addr broadcast_addr(const adapter *a) {
    struct in_addr b;
    b.s_addr = a->ip.s_addr | ~a->mask.s_addr;
    return b;
}

void send_beacon(SOCKET s, const adapter *me, const char *beacon) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(PORT);
    to.sin_addr = broadcast_addr(me);
    sendto(s, beacon, (int)strlen(beacon), 0, (struct sockaddr *)&to, sizeof to);
}

int handle_packet(SOCKET s, const adapter *me, const char *beacon, peer *peers) {
    char buf[256], name[NAME_LEN];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof from;
    int len = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
    peer *slot = NULL;
    if (len <= 0) return 0;
    /* Our echo can arrive from any of our IPs (two adapters on one LAN, new DHCP lease), so match content.
       Cost: another PC with our exact hostname is hidden too; Windows already warns about duplicate names. */
    if (len == (int)strlen(beacon) && memcmp(buf, beacon, len) == 0) return 0;
    if ((from.sin_addr.s_addr & me->mask.s_addr) != (me->ip.s_addr & me->mask.s_addr)) return 0; /* other adapter */
    if (!parse_beacon(buf, len, name)) return 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].ip.s_addr == from.sin_addr.s_addr) {
            int renamed = strcmp(peers[i].name, name) != 0;
            strcpy(peers[i].name, name);
            peers[i].seen = now_ms();
            return renamed;
        }
        if (!peers[i].ip.s_addr && !slot) slot = &peers[i];
    }
    if (!slot) return 0;
    slot->ip = from.sin_addr;
    strcpy(slot->name, name);
    slot->seen = now_ms();
    return 1;
}

int prune_peers(peer *peers) {
    long long now = now_ms();
    int changed = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].ip.s_addr && now - peers[i].seen > TIMEOUT_MS) {
            peers[i].ip.s_addr = 0;
            changed = 1;
        }
    return changed;
}
