#ifndef BFREE_NET_TCP_H
#define BFREE_NET_TCP_H

/*
 * B-Free TCP for the QEMU slirp / e1000 LAN (replaces net_tcp_min.c).
 *
 * Everything here is non-blocking and polled: there is no interrupt path from the NIC.
 * Callers (the guest socket layer in sysmain/syscall.c) loop over net_runtime_poll()
 * and block/yield themselves.  IPv4 addresses are in the kernel's "wire" format (the
 * bytes in network order stored in a little-endian uint32_t, like ipv4_send()).
 *
 * Implemented: 3-way handshake (active + passive), MSS option, byte-stream send/receive
 * rings with window advertisement, retransmission timeout with exponential back-off
 * (go-back-N), zero-window probing, FIN/half-close (shutdown), RST handling, TIME_WAIT.
 * Not implemented: out-of-order queueing, SACK, window scaling, congestion control,
 * urgent data, timestamps (slirp is a loss-free, low-latency link).
 */

#include <stddef.h>
#include <stdint.h>

#define BFREE_TCP_MAX 40          /* connections + listeners (was 12: browser pages ran out) */

/* tcp_poll_flags() bits */
#define TCPF_IN   0x01  /* readable: data, EOF, error, or (listener) a connection to accept */
#define TCPF_OUT  0x02  /* writable: established with free send buffer */
#define TCPF_ERR  0x04  /* pending error (tcp_take_error) */
#define TCPF_HUP  0x08  /* connection fully closed / reset */
#define TCPF_RDHUP 0x10 /* peer sent FIN */
#define TCPF_CONNECTING 0x20

void tcp_init(void);
void tcp_tick(void);                                  /* timers; called from net_runtime_poll */
void tcp_min_input(uint32_t src_ip, const uint8_t *pkt, size_t len); /* from ipv4_input */

int  tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port); /* pcb id or -errno */
int  tcp_connect_status(int id);          /* 0 established, -115 in progress, else -errno */
int  tcp_listen(uint32_t addr, uint16_t port, int backlog);              /* listener id or -errno */
int  tcp_accept(int lid, uint32_t *rip, uint16_t *rport);                /* new id, -11 none */
int  tcp_send(int id, const uint8_t *data, size_t len);   /* >0 queued, -11 full, -errno */
int  tcp_recv(int id, uint8_t *buf, size_t len, int peek); /* >0, 0 EOF, -11, -errno */
int  tcp_shutdown(int id, int how);       /* 0 SHUT_RD, 1 SHUT_WR, 2 both */
void tcp_close(int id);                   /* graceful close; RST if unread data */
int  tcp_poll_flags(int id);
int  tcp_take_error(int id);              /* SO_ERROR: returns errno (positive) and clears it */
int  tcp_get_addrs(int id, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport);
int  tcp_rx_avail(int id);                /* FIONREAD */
int  tcp_tx_free(int id);
int  tcp_active(void);                    /* number of pcbs in use (poll gating) */

#endif
