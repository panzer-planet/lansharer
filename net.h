/* Portable LAN discovery: adapters, beacons, peer table. Used by the CLI and GUI front ends. */
#ifndef NET_H
#define NET_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif
#include <stddef.h>

#define PORT 53530
#define MAGIC "LANSHARER1 "
#define INTERVAL_MS 2000              /* between beacons */
#define TIMEOUT_MS (3 * INTERVAL_MS)  /* peer is gone after this much silence */
#define MAX_ADAPTERS 32
#define MAX_PEERS 64 /* shortcut: fixed table, extra peers are ignored; grow it if a LAN ever has more */
#define NAME_LEN 64

typedef struct { char name[128]; struct in_addr ip, mask; } adapter;
typedef struct { struct in_addr ip; char name[NAME_LEN]; long long seen; } peer; /* ip 0 = empty slot */

int net_init(void);                                          /* 0 on success */
long long now_ms(void);                                      /* monotonic, immune to clock changes */
int list_adapters(adapter *out, int max);                    /* up, non-loopback IPv4 with a real subnet */
int parse_beacon(const char *buf, int len, char out[NAME_LEN]);
void make_beacon(char *out, size_t size);                    /* MAGIC + sanitized hostname */
SOCKET open_socket(void);                                    /* UDP, bound to PORT, broadcast on */
struct in_addr broadcast_addr(const adapter *a);
void send_beacon(SOCKET s, const adapter *me, const char *beacon);
int handle_packet(SOCKET s, const adapter *me, const char *beacon, peer *peers); /* reads one packet; 1 if peers changed */
int prune_peers(peer *peers);                                /* 1 if peer list changed */
#endif
