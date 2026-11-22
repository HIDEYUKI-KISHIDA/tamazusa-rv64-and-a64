/* net_ipv4.h — IPv4 の層（kernel/net/net_ip.c）。アドレスは「線の順」の uint32_t
 *   （10.0.2.15 なら 0x0F02000A: 最初の数が下位のバイト）。net_tcp.c と同じ約束 */
#ifndef NET_IPV4_H
#define NET_IPV4_H
#include <stdint.h>
#include <stddef.h>

#define NET_IP(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

int      ipv4_send(uint32_t dst, uint8_t proto, const uint8_t *data, size_t len);
uint32_t ipv4_get_local_ip(void);
uint64_t knl_get_current_time(void);    /* 起動からのマイクロ秒 */

/* ネットワーク全体（net_ip.c） */
void net_init(void);
int  net_has_nic(void);
void net_lock(void);
void net_unlock(void);
void net_wait(int ms);                  /* 何か届くか ms たつまで眠る */
uint32_t net_map_loopback(uint32_t ip); /* 127.x.x.x を自分のアドレスに */

/* UDP（net_ip.c） */
#define NET_UDP_MAX 16
int  udp_open(void);
int  udp_bind(int u, uint32_t ip, uint16_t port);     /* port 0 なら空いている番号 */
int  udp_sendto(int u, uint32_t dip, uint16_t dport, const uint8_t *buf, size_t len);
int  udp_recv(int u, uint8_t *buf, size_t len, uint32_t *sip, uint16_t *sport, int peek); /* -11 なし */
int  udp_readable(int u);
void udp_close(int u);
int  udp_local(int u, uint32_t *ip, uint16_t *port);

/* ICMP の echo（ping の SOCK_DGRAM, IPPROTO_ICMP 用） */
int  icmp_open(int raw);                 /* raw: 受け取るときに IP の見出しも付ける（SOCK_RAW） */
int  icmp_sendto(int s, uint32_t dip, const uint8_t *buf, size_t len);
int  icmp_recv(int s, uint8_t *buf, size_t len, uint32_t *sip, int peek);
int  icmp_readable(int s);
void icmp_close(int s);
#endif
