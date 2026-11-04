/* Minimal IPv4 network stack: Ethernet, ARP, ICMP echo, UDP and DNS.
 *
 * The e1000 driver is polled, so every entry point drains the RX ring before
 * looking at protocol state. No interrupts, no fragments, no retransmission:
 * this is the M6 baseline that later milestones extend with TCP and sockets.
 */

#include <mcube/net.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/timer.h>
#include <mcube/syscall.h>
#include <mcube/sched.h>
#include <mcube/atomic.h>

#define ARP_TABLE_SIZE 16
#define DNS_SERVER     IP4(10, 0, 2, 3)
#define DNS_LOCAL_PORT 12345

typedef struct PACKED eth_hdr {
    u8  dst[ETH_ALEN];
    u8  src[ETH_ALEN];
    u16 type;
} eth_hdr_t;

typedef struct PACKED arp_pkt {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 op;
    u8  sha[ETH_ALEN];
    u32 spa;
    u8  tha[ETH_ALEN];
    u32 tpa;
} arp_pkt_t;

typedef struct PACKED ipv4_hdr {
    u8  version_ihl;
    u8  tos;
    u16 total_len;
    u16 id;
    u16 frag;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u32 src;
    u32 dst;
} ipv4_hdr_t;

typedef struct PACKED icmp_hdr {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} icmp_hdr_t;

typedef struct PACKED udp_hdr {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} udp_hdr_t;

typedef struct PACKED tcp_hdr {
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  off;        /* data offset in 4-byte words, high nibble */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_CLOSE_WAIT  3
#define TCP_LISTEN      4
#define TCP_SYN_RCVD    5

#define TCP_RXBUF_SIZE  8192

struct net_socket {
    int used;
    int refs;
    int type;
    u32 laddr;
    u32 raddr;
    u16 lport;
    u16 rport;
    int connected;
    int rx_ready;
    u32 rxlen;
    u32 rxaddr;
    u16 rxport;
    u8 rxbuf[2048];

    /* TCP state. Only one direction is tracked: enough for an HTTP client. */
    u8  state;
    u8  eof;
    u32 snd_nxt;
    u32 rcv_nxt;
    u32 snd_una;      /* highest sequence byte the peer has acknowledged */
    u32 peer_wnd;     /* peer's last advertised receive window */
    u8  listen;       /* 1 = passive socket waiting for SYNs */
    u8  accepted;     /* handshake finished, an accept() has not taken it yet */
    struct net_socket *parent;   /* listener that spawned this connection */
    u32 tcp_len;
    u8  tcpbuf[TCP_RXBUF_SIZE];
};

static net_device_t *g_dev;
static u32 g_ip;
static u32 g_gateway;
static u32 g_netmask;
static u32 g_dns;
static u8 g_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct {
    u32 ip;
    u8 mac[ETH_ALEN];
    int valid;
} g_arp[ARP_TABLE_SIZE];

static struct net_socket g_socks[NET_MAX_SOCKETS];

static volatile int g_ping_done;
static u16 g_ping_id;
static u16 g_ping_seq;

/* ---- byte order and checksums -------------------------------------- */

static inline u16 htons(u16 v) {
    return (u16)((v >> 8) | (v << 8));
}

static inline u32 htonl(u32 v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 ntohl(u32 v) { return htonl(v); }

static u16 net_checksum(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

/* Unfolded sum, so a pseudo-header contribution can be added before the final
 * one's complement. */
static u32 net_sum(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (u32)p[0] << 8;
    return sum;
}

/* ---- ARP ----------------------------------------------------------- */

static int arp_lookup(u32 ip, u8 *mac) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(mac, g_arp[i].mac, ETH_ALEN);
            return 0;
        }
    }
    return -1;
}

static void arp_learn(u32 ip, const u8 *mac) {
    int free_slot = -1;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, ETH_ALEN);
            return;
        }
        if (!g_arp[i].valid && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    g_arp[free_slot].ip = ip;
    memcpy(g_arp[free_slot].mac, mac, ETH_ALEN);
    g_arp[free_slot].valid = 1;
}

static int arp_send(u16 op, const u8 *dst_mac, u32 dst_ip) {
    u8 frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));

    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, g_dev->mac, ETH_ALEN);
    eth->type = htons(ETH_TYPE_ARP);

    arp->htype = htons(1);
    arp->ptype = htons(ETH_TYPE_IPV4);
    arp->hlen = ETH_ALEN;
    arp->plen = 4;
    arp->op = htons(op);
    memcpy(arp->sha, g_dev->mac, ETH_ALEN);
    arp->spa = htonl(g_ip);
    memcpy(arp->tha, dst_mac, ETH_ALEN);
    arp->tpa = htonl(dst_ip);

    return g_dev->send(frame, sizeof(frame));
}

static int arp_resolve(u32 ip, u8 *mac, u32 timeout_ms) {
    if (arp_lookup(ip, mac) == 0) return 0;

    kprintf("[net] arp who-has %u.%u.%u.%u\n",
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    arp_send(1, g_broadcast, ip);
    u64 deadline = timer_uptime_ms() + timeout_ms;
    while (timer_uptime_ms() < deadline) {
        net_poll();
        if (arp_lookup(ip, mac) == 0) {
            kprintf("[net] arp reply received\n");
            return 0;
        }
        /* task_sleep_ms() yields to the scheduler; timer_sleep_ms() would halt
         * the CPU with interrupts still masked by the syscall gate, freezing
         * every task on the machine. */
        task_sleep_ms(1);
    }
    kprintf("[net] arp timeout\n");
    return -1;
}

/* ---- transmit path ------------------------------------------------- */

static int ip_send(u32 dst_ip, u8 proto, const void *payload, u32 len) {
    if (!g_dev) return -ENODEV;
    if (len + sizeof(ipv4_hdr_t) > NET_MTU) return -EINVAL;

    u8 dst_mac[ETH_ALEN];
    if ((dst_ip & g_netmask) == (g_ip & g_netmask)) {
        if (arp_resolve(dst_ip, dst_mac, 1000) != 0) return -EHOSTUNREACH;
    } else {
        if (arp_resolve(g_gateway, dst_mac, 1000) != 0) return -EHOSTUNREACH;
    }

    u8 frame[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + NET_MTU];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    u8 *data = frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t);

    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, g_dev->mac, ETH_ALEN);
    eth->type = htons(ETH_TYPE_IPV4);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((u16)(sizeof(ipv4_hdr_t) + len));
    ip->id = htons((u16)timer_ticks());
    ip->frag = htons(0x4000); /* don't fragment */
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src = htonl(g_ip);
    ip->dst = htonl(dst_ip);
    ip->checksum = htons(net_checksum(ip, sizeof(*ip)));

    if (len) memcpy(data, payload, len);

    return g_dev->send(frame, (u32)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + len));
}

static int udp_send(u32 dst_ip, u16 src_port, u16 dst_port,
                    const void *payload, u32 len) {
    u8 buf[sizeof(udp_hdr_t) + NET_MTU];
    if (len + sizeof(udp_hdr_t) > sizeof(buf)) return -EINVAL;

    udp_hdr_t *udp = (udp_hdr_t *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons((u16)(sizeof(udp_hdr_t) + len));
    udp->checksum = 0;
    if (len) memcpy(buf + sizeof(udp_hdr_t), payload, len);

    return ip_send(dst_ip, IP_PROTO_UDP, buf, (u32)(sizeof(udp_hdr_t) + len));
}

/* ---- receive path -------------------------------------------------- */

/* Locate the socket a segment belongs to. Connected sockets win over a
 * listener on the same port; a bare SYN with nothing established is what
 * opens a new connection on the listener. */
static struct net_socket *tcp_find(u16 dport, u16 sport, u32 src, u32 dst,
                                   u8 flags) {
    struct net_socket *listener = NULL;

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct net_socket *s = &g_socks[i];
        if (!s->used || s->type != SOCK_STREAM) continue;
        if (s->lport != dport) continue;
        if (s->laddr && s->laddr != dst) continue;
        if (s->listen) {
            if (!listener) listener = s;
            continue;
        }
        if (s->state == TCP_SYN_SENT) {
            if (s->raddr == src && s->rport == sport) return s;
            continue;
        }
        if (s->raddr == src && s->rport == sport) return s;
    }

    if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) return listener;
    return NULL;
}

/* Pseudo-header sum shared by the TCP checksum. */
static u32 tcp_pseudo_sum(u32 src, u32 dst, u32 tcp_len) {
    u32 sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += tcp_len & 0xFFFF;
    return sum;
}

/* Raw segment transmit. Every TCP packet goes through here so the checksum
 * arithmetic lives in exactly one place. */
static int tcp_xmit(u32 dst, u16 sport, u16 dport, u32 seq, u32 ack,
                    u8 flags, u16 window, const void *data, u32 len) {
    u8 buf[sizeof(tcp_hdr_t) + NET_MTU];
    if (len + sizeof(tcp_hdr_t) > sizeof(buf)) return -EINVAL;

    tcp_hdr_t *t = (tcp_hdr_t *)buf;
    t->src_port = htons(sport);
    t->dst_port = htons(dport);
    t->seq = htonl(seq);
    t->ack = htonl(ack);
    t->off = (u8)((sizeof(tcp_hdr_t) / 4) << 4);
    t->flags = flags;
    t->window = htons(window);
    t->checksum = 0;
    t->urgent = 0;
    if (len) memcpy(buf + sizeof(tcp_hdr_t), data, len);

    u32 seg = (u32)(sizeof(tcp_hdr_t) + len);
    u32 sum = net_sum(buf, seg) + tcp_pseudo_sum(g_ip, dst, seg);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    t->checksum = htons((u16)~sum);

    return ip_send(dst, IP_PROTO_TCP, buf, seg);
}

static int tcp_send(struct net_socket *s, u8 flags, const void *data, u32 len) {
    u32 window = s->tcp_len < TCP_RXBUF_SIZE
                     ? (u32)(TCP_RXBUF_SIZE - s->tcp_len) : 0;
    int rc = tcp_xmit(s->raddr, s->lport, s->rport, s->snd_nxt, s->rcv_nxt,
                      flags, (u16)window, data, len);
    if (rc != 0) return rc;

    if (flags & (TCP_SYN | TCP_FIN)) s->snd_nxt++;
    s->snd_nxt += len;
    if (flags & TCP_FIN) s->state = TCP_CLOSE_WAIT;
    return (int)len;
}

/* Nothing is listening on that port: answer the SYN with RST+ACK so the peer
 * gives up immediately instead of retransmitting for a minute. Best effort --
 * without an ARP entry there is nowhere to send the refusal. */
static void tcp_refuse(u16 dport, u16 sport, u32 src, u32 seq) {
    u8 mac[ETH_ALEN];
    if (arp_lookup(src, mac) != 0) return;
    tcp_xmit(src, dport, sport, 0, seq + 1, TCP_RST | TCP_ACK, 0, NULL, 0);
}

/* Turn an incoming SYN into a half-open connection chained to the listener. */
static struct net_socket *tcp_passive_open(struct net_socket *ls, u16 sport,
                                           u32 src, u32 dst, u32 seq,
                                           u16 peer_window) {
    struct net_socket *s = NULL;

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_socks[i].used) { s = &g_socks[i]; break; }
    }
    if (!s) return NULL;

    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->refs = 1;
    s->type = SOCK_STREAM;
    s->laddr = ls->laddr ? ls->laddr : dst;
    s->lport = ls->lport;
    s->raddr = src;
    s->rport = sport;
    s->parent = ls;
    s->rcv_nxt = seq + 1;
    /* Any ISN works as long as it is not repeated on this 4-tuple. */
    s->snd_nxt = (u32)(timer_ticks() * 2654435761u) ^ src;
    s->snd_una = s->snd_nxt;
    s->peer_wnd = peer_window ? peer_window : 2048;
    s->state = TCP_SYN_RCVD;

    if (tcp_send(s, TCP_SYN | TCP_ACK, NULL, 0) < 0) {
        memset(s, 0, sizeof(*s));
        return NULL;
    }
    return s;
}


static void tcp_receive(const u8 *pkt, u32 plen, u32 src, u32 dst) {
    if (plen < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *t = (const tcp_hdr_t *)pkt;

    u16 sport = ntohs(t->src_port);
    u16 dport = ntohs(t->dst_port);
    u32 seq = ntohl(t->seq);
    u32 ack = ntohl(t->ack);
    u8 flags = t->flags;

    struct net_socket *s = tcp_find(dport, sport, src, dst, flags);
    if (!s) {
        if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN)
            tcp_refuse(dport, sport, src, seq);
        return;
    }

    if (s->listen) {
        /* A listener only reacts to a fresh SYN. A retransmitted SYN that
         * already produced a connection lands on that connection instead,
         * because tcp_find() prefers the established 4-tuple. */
        if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN)
            tcp_passive_open(s, sport, src, dst, seq, ntohs(t->window));
        return;
    }

    /* A refused connection answers the SYN with RST+ACK; fail immediately
     * instead of letting the caller sit out the whole connect timeout. */
    if (flags & TCP_RST) {
        s->eof = 1;
        s->state = TCP_CLOSED;
        return;
    }

    /* Our own handshake acknowledgement. */
    if (s->state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->state = TCP_ESTABLISHED;
            s->connected = 1;
            s->snd_una = ack;
            s->peer_wnd = ntohs(t->window);
            tcp_send(s, TCP_ACK, NULL, 0);
        }
        return;
    }

    /* The ACK that completes a passively opened connection. */
    if (s->state == TCP_SYN_RCVD) {
        if (!(flags & TCP_ACK) || ack != s->snd_nxt) return;
        s->state = TCP_ESTABLISHED;
        s->connected = 1;
        s->accepted = 1;
    }

    /* Track the peer's acknowledgement point and its receive window, so the
     * send path can throttle instead of blasting past the receiver. */
    if ((flags & TCP_ACK) &&
        (u32)(ack - s->snd_una) <= (u32)(s->snd_nxt - s->snd_una))
        s->snd_una = ack;
    s->peer_wnd = ntohs(t->window);

    u32 hdr = (u32)(t->off >> 4) * 4;
    if (hdr < sizeof(tcp_hdr_t) || hdr > plen) return;
    u32 dlen = plen - hdr;

    if (dlen > 0) {
        if (seq != s->rcv_nxt) {
            /* Out of order: re-acknowledge what we already hold. */
            if (seq + dlen <= s->rcv_nxt) tcp_send(s, TCP_ACK, NULL, 0);
            return;
        }
        u32 room = TCP_RXBUF_SIZE - s->tcp_len;
        if (dlen > room) {
            /* No space: advertise the closed window and drop the segment so the
             * peer retries once the application has read. */
            tcp_send(s, TCP_ACK, NULL, 0);
            return;
        }
        memcpy(s->tcpbuf + s->tcp_len, pkt + hdr, dlen);
        s->tcp_len += dlen;
        s->rcv_nxt += dlen;
        tcp_send(s, TCP_ACK, NULL, 0);
    }

    if (flags & TCP_FIN) {
        s->rcv_nxt++;
        s->eof = 1;
        tcp_send(s, TCP_ACK, NULL, 0);
    }
}

static struct net_socket *udp_find(u16 dst_port, u32 dst_ip) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct net_socket *s = &g_socks[i];
        if (!s->used || s->type != SOCK_DGRAM) continue;
        if (s->lport != dst_port) continue;
        if (s->laddr && s->laddr != dst_ip) continue;
        return s;
    }
    return NULL;
}

static void ip_receive(const u8 *pkt, u32 len) {
    if (len < sizeof(ipv4_hdr_t)) return;

    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)pkt;
    if ((ip->version_ihl >> 4) != 4) return;

    u32 ihl = (u32)(ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_hdr_t) || ihl > len) return;

    u32 total = ntohs(ip->total_len);
    if (total < ihl || total > len) return;

    u32 src = ntohl(ip->src);
    u32 dst = ntohl(ip->dst);
    const u8 *payload = pkt + ihl;
    u32 plen = total - ihl;

    switch (ip->proto) {
    case IP_PROTO_ICMP: {
        if (plen < sizeof(icmp_hdr_t)) return;
        const icmp_hdr_t *icmp = (const icmp_hdr_t *)payload;
        if (icmp->type == 0 && ntohs(icmp->id) == g_ping_id &&
            ntohs(icmp->seq) == g_ping_seq) {
            g_ping_done = 1;
        }
        break;
    }
    case IP_PROTO_UDP: {
        if (plen < sizeof(udp_hdr_t)) return;
        const udp_hdr_t *udp = (const udp_hdr_t *)payload;
        u16 sport = ntohs(udp->src_port);
        u16 dport = ntohs(udp->dst_port);
        u32 ulen = ntohs(udp->length);
        if (ulen < sizeof(udp_hdr_t) || ulen > plen) return;

        struct net_socket *s = udp_find(dport, dst);
        if (!s) return;

        u32 copy = ulen - sizeof(udp_hdr_t);
        if (copy > sizeof(s->rxbuf)) copy = sizeof(s->rxbuf);
        memcpy(s->rxbuf, payload + sizeof(udp_hdr_t), copy);
        s->rxlen = copy;
        s->rxaddr = src;
        s->rxport = sport;
        s->rx_ready = 1;
        break;
    }
    case IP_PROTO_TCP:
        tcp_receive(payload, plen, src, dst);
        break;
    default:
        break;
    }
}

void net_receive(const void *frame, u32 len) {
    if (len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    u16 type = ntohs(eth->type);
    const u8 *payload = (const u8 *)frame + sizeof(eth_hdr_t);
    u32 plen = len - sizeof(eth_hdr_t);

    if (type == ETH_TYPE_ARP && plen >= sizeof(arp_pkt_t)) {
        const arp_pkt_t *arp = (const arp_pkt_t *)payload;
        u32 spa = ntohl(arp->spa);
        arp_learn(spa, arp->sha);

        if (ntohs(arp->op) == 1 && ntohl(arp->tpa) == g_ip) {
            arp_send(2, arp->sha, spa);
        }
        return;
    }

    if (type == ETH_TYPE_IPV4) {
        ip_receive(payload, plen);
    }
}

void net_poll(void) {
    if (g_dev && g_dev->poll) g_dev->poll();
}

/* ---- ping and DNS -------------------------------------------------- */

int net_ping(u32 ip, u32 timeout_ms) {
    if (!g_dev) return -ENODEV;

    u8 mac[ETH_ALEN];
    if (arp_resolve(ip, mac, timeout_ms) != 0) return -EHOSTUNREACH;

    g_ping_id = (u16)(timer_ticks() & 0xFFFF);
    g_ping_seq = 1;
    g_ping_done = 0;

    u8 pkt[sizeof(icmp_hdr_t) + 32];
    icmp_hdr_t *icmp = (icmp_hdr_t *)pkt;
    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(g_ping_id);
    icmp->seq = htons(g_ping_seq);
    const char *tag = "MCubeAIOS";
    memcpy(pkt + sizeof(icmp_hdr_t), tag, 10);
    icmp->checksum = htons(net_checksum(pkt, sizeof(pkt)));

    if (ip_send(ip, IP_PROTO_ICMP, pkt, sizeof(pkt)) != 0) return -EIO;

    u64 deadline = timer_uptime_ms() + timeout_ms;
    while (timer_uptime_ms() < deadline) {
        net_poll();
        if (g_ping_done) return 0;
        timer_sleep_ms(1);
    }
    return -ETIMEDOUT;
}

static int dns_build_query(u8 *buf, usize cap, const char *name, u16 id) {
    if (cap < 12 + 1 + 4) return -EINVAL;

    memset(buf, 0, 12);
    buf[0] = (u8)(id >> 8);
    buf[1] = (u8)id;
    buf[2] = 0x01; /* recursion desired */
    buf[3] = 0x00;
    buf[5] = 1;    /* QDCOUNT = 1 */

    usize pos = 12;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        usize n = (usize)(dot - p);
        if (n == 0 || n > 63 || pos + n + 1 >= cap) return -ENAMETOOLONG;
        buf[pos++] = (u8)n;
        memcpy(buf + pos, p, n);
        pos += n;
        if (*dot == '.') dot++;
        p = dot;
    }
    if (pos + 1 + 4 > cap) return -ENAMETOOLONG;
    buf[pos++] = 0;
    buf[pos++] = 0; buf[pos++] = 1; /* QTYPE A */
    buf[pos++] = 0; buf[pos++] = 1; /* QCLASS IN */
    return (int)pos;
}

static int dns_skip_name(const u8 *buf, u32 len, u32 *pos) {
    while (*pos < len) {
        u8 c = buf[*pos];
        if (c == 0) {
            (*pos)++;
            return 0;
        }
        if ((c & 0xC0) == 0xC0) {
            if (*pos + 1 >= len) return -1;
            *pos += 2;
            return 0;
        }
        *pos += (u32)c + 1;
    }
    return -1;
}

static int dns_parse(const u8 *buf, u32 len, u16 id, u32 *ip_out) {
    if (len < 12) return -EIO;
    if (((u16)buf[0] << 8 | buf[1]) != id) return -EIO;
    u16 flags = (u16)(buf[2] << 8 | buf[3]);
    if ((flags & 0x8000) == 0 || (flags & 0x000F) != 0) return -EIO;
    u16 qd = (u16)(buf[4] << 8 | buf[5]);
    u16 an = (u16)(buf[6] << 8 | buf[7]);
    if (an == 0) return -ENOENT;

    u32 pos = 12;
    for (u16 i = 0; i < qd; i++) {
        if (dns_skip_name(buf, len, &pos) != 0) return -EIO;
        pos += 4;
        if (pos > len) return -EIO;
    }

    for (u16 i = 0; i < an; i++) {
        if (dns_skip_name(buf, len, &pos) != 0) return -EIO;
        if (pos + 10 > len) return -EIO;
        u16 type = (u16)(buf[pos] << 8 | buf[pos + 1]);
        u16 cls = (u16)(buf[pos + 2] << 8 | buf[pos + 3]);
        u16 rdlen = (u16)(buf[pos + 8] << 8 | buf[pos + 9]);
        pos += 10;
        if (pos + rdlen > len) return -EIO;
        if (type == 1 && cls == 1 && rdlen == 4) {
            *ip_out = ((u32)buf[pos] << 24) | ((u32)buf[pos + 1] << 16) |
                      ((u32)buf[pos + 2] << 8) | buf[pos + 3];
            return 0;
        }
        pos += rdlen;
    }
    return -ENOENT;
}

int net_dns_lookup(const char *name, u32 *ip_out, u32 timeout_ms) {
    if (!g_dev || !name || !ip_out) return -EINVAL;

    struct net_socket *s = net_socket_open(SOCK_DGRAM);
    if (!s) return -ENFILE;
    if (net_socket_bind(s, 0, DNS_LOCAL_PORT) != 0) {
        net_socket_close(s);
        return -EIO;
    }

    u8 query[512];
    u16 id = (u16)(timer_ticks() ^ 0x4D43);
    int qlen = dns_build_query(query, sizeof(query), name, id);
    if (qlen < 0) {
        net_socket_close(s);
        return qlen;
    }

    int rc = net_socket_sendto(s, query, (u64)qlen, DNS_SERVER, 53);
    if (rc < 0) {
        net_socket_close(s);
        return rc;
    }

    u64 deadline = timer_uptime_ms() + timeout_ms;
    u8 reply[512];
    while (timer_uptime_ms() < deadline) {
        net_poll();
        rc = net_socket_recvfrom(s, reply, sizeof(reply), NULL, NULL);
        if (rc > 0) {
            rc = dns_parse(reply, (u32)rc, id, ip_out);
            net_socket_close(s);
            return rc;
        }
        timer_sleep_ms(1);
    }

    net_socket_close(s);
    return -ETIMEDOUT;
}

/* ---- sockets ------------------------------------------------------- */

struct net_socket *net_socket_open(int type) {
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return NULL;

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_socks[i].used) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].used = 1;
            g_socks[i].refs = 1;
            g_socks[i].type = type;
            return &g_socks[i];
        }
    }
    return NULL;
}

void net_socket_close(struct net_socket *s) {
    if (!s) return;
    if (s->refs > 1) {
        s->refs--;
        return;
    }

    if (s->type == SOCK_STREAM && s->listen) {
        /* Tear down everything this listener spawned: without the port there
         * is nobody left to serve those connections. */
        for (int i = 0; i < NET_MAX_SOCKETS; i++) {
            struct net_socket *c = &g_socks[i];
            if (!c->used || c->parent != s) continue;
            if (c->state == TCP_ESTABLISHED)
                tcp_send(c, TCP_FIN | TCP_ACK, NULL, 0);
            memset(c, 0, sizeof(*c));
        }
    } else if (s->type == SOCK_STREAM && s->state == TCP_ESTABLISHED) {
        /* Tell the peer before the state disappears; best effort only. */
        tcp_send(s, TCP_FIN | TCP_ACK, NULL, 0);
    }
    memset(s, 0, sizeof(*s));
}

void net_socket_ref(struct net_socket *s) {
    if (s) s->refs++;
}

static u16 ephemeral_port(void) {
    static u16 next = 40000;
    return next++;
}

int net_socket_bind(struct net_socket *s, u32 ip, u16 port) {
    if (!s) return -EBADF;
    s->laddr = ip;
    s->lport = port ? port : ephemeral_port();
    return 0;
}

/* Passive open: reserve a port and let inbound SYNs create connections.
 * accept() then hands them to the server loop one at a time. */
int net_socket_listen(struct net_socket *s, u16 port) {
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EINVAL;
    if (!port) return -EINVAL;

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct net_socket *o = &g_socks[i];
        if (o == s || !o->used || o->type != SOCK_STREAM) continue;
        if (o->lport == port) return -EADDRINUSE;
    }

    s->lport = port;
    s->listen = 1;
    s->state = TCP_LISTEN;
    return 0;
}

/* Returns a connection whose handshake finished, or NULL. Never blocks; the
 * caller polls this from its own loop. */
struct net_socket *net_socket_accept(struct net_socket *ls, u32 *ip, u16 *port) {
    if (!ls || !ls->listen) return NULL;
    net_poll();

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct net_socket *s = &g_socks[i];
        if (!s->used || s->parent != ls || !s->accepted) continue;
        s->accepted = 0;
        if (ip) *ip = s->raddr;
        if (port) *port = s->rport;
        return s;
    }
    return NULL;
}

/* Number of connections now waiting for accept(). */
int net_socket_backlog(struct net_socket *ls) {
    int n = 0;
    if (!ls || !ls->listen) return 0;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_socks[i].used && g_socks[i].parent == ls && g_socks[i].accepted) n++;
    }
    return n;
}

int net_socket_connect(struct net_socket *s, u32 ip, u16 port) {
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) {
        s->raddr = ip;
        s->rport = port;
        s->connected = 1;
        if (!s->lport) s->lport = ephemeral_port();
        return 0;
    }

    if (!g_dev) return -ENODEV;

    s->raddr = ip;
    s->rport = port;
    if (!s->lport) s->lport = ephemeral_port();
    s->snd_nxt = (u32)(timer_ticks() * 2654435761u);
    s->rcv_nxt = 0;
    s->tcp_len = 0;
    s->eof = 0;
    s->state = TCP_SYN_SENT;

    int rc = tcp_send(s, TCP_SYN, NULL, 0);
    if (rc < 0) {
        s->state = TCP_CLOSED;
        return rc;
    }

    /* Wait for the SYN-ACK. The backlog is drained by net_poll(), so the reply
     * is handled right here inside tcp_receive(). Sleep through the scheduler:
     * this runs in syscall context, where the timer interrupt is masked and a
     * cpu_hlt() wait would wedge the whole machine. */
    u64 deadline = timer_uptime_ms() + 5000;
    while (timer_uptime_ms() < deadline) {
        net_poll();
        if (s->state == TCP_ESTABLISHED) {
            kprintf("[net] tcp connected to %u.%u.%u.%u:%u\n",
                    (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
                    ip & 0xFF, port);
            return 0;
        }
        if (s->state == TCP_CLOSED) {
            kprintf("[net] tcp connection refused\n");
            return -ECONNREFUSED;
        }
        task_sleep_ms(1);
    }

    s->state = TCP_CLOSED;
    kprintf("[net] tcp connect timeout\n");
    return -ETIMEDOUT;
}

int net_socket_sendto(struct net_socket *s, const void *buf, u64 len,
                      u32 ip, u16 port) {
    if (!s) return -EBADF;

    if (s->type == SOCK_STREAM) {
        if (!s->connected || s->state != TCP_ESTABLISHED) return -ENOTCONN;
        if (len == 0) return 0;
        /* Chunk the payload across segments once it no longer fits one frame. */
        const u8 *p = (const u8 *)buf;
        u64 left = len;
        u64 deadline = timer_uptime_ms() + 10000;
        while (left > 0) {
            /* Stay inside the peer's receive window. There is no
             * retransmission here, so a segment pushed at a full buffer is
             * simply lost -- and a framebuffer update is orders of magnitude
             * bigger than any plausible receive buffer. */
            for (;;) {
                u32 inflight = (u32)(s->snd_nxt - s->snd_una);
                if (s->peer_wnd && inflight < s->peer_wnd) break;
                net_poll();
                inflight = (u32)(s->snd_nxt - s->snd_una);
                if (s->peer_wnd && inflight < s->peer_wnd) break;
                if (s->state != TCP_ESTABLISHED) return -ENOTCONN;
                if (timer_uptime_ms() > deadline) return -ETIMEDOUT;
                task_sleep_ms(1);
            }

            u32 chunk = (u32)(left > NET_MTU - sizeof(tcp_hdr_t)
                                  ? NET_MTU - sizeof(tcp_hdr_t) : left);
            int rc = tcp_send(s, TCP_PSH | TCP_ACK, p, chunk);
            if (rc < 0) return rc;
            p += chunk;
            left -= chunk;
        }
        return (int)len;
    }

    if (!s->lport) s->lport = ephemeral_port();

    int rc = udp_send(ip, s->lport, port, buf, (u32)len);
    return rc == 0 ? (int)len : rc;
}

int net_socket_recvfrom(struct net_socket *s, void *buf, u64 len,
                        u32 *ip, u16 *port) {
    if (!s) return -EBADF;

    if (s->type == SOCK_STREAM) {
        net_poll();
        if (s->tcp_len == 0) {
            /* A cleanly closed connection reports end of stream. */
            return s->eof ? 0 : -EAGAIN;
        }
        u32 n = s->tcp_len;
        if (n > len) n = (u32)len;
        memcpy(buf, s->tcpbuf, n);
        if (n < s->tcp_len) memmove(s->tcpbuf, s->tcpbuf + n, s->tcp_len - n);
        s->tcp_len -= n;
        /* Reopen the window so the peer may send the next burst. */
        if (s->state == TCP_ESTABLISHED) tcp_send(s, TCP_ACK, NULL, 0);
        return (int)n;
    }

    net_poll();
    if (!s->rx_ready) return -EAGAIN;

    u32 n = s->rxlen;
    if (n > len) n = (u32)len;
    memcpy(buf, s->rxbuf, n);
    if (ip) *ip = s->rxaddr;
    if (port) *port = s->rxport;
    s->rx_ready = 0;
    return (int)n;
}

int net_socket_send(struct net_socket *s, const void *buf, u64 len) {
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -ENOTCONN;
    return net_socket_sendto(s, buf, len, s->raddr, s->rport);
}

int net_socket_recv(struct net_socket *s, void *buf, u64 len) {
    return net_socket_recvfrom(s, buf, len, NULL, NULL);
}

int net_socket_readable(struct net_socket *s) {
    if (!s) return 0;
    net_poll();
    if (s->type == SOCK_STREAM) return s->tcp_len != 0 || s->eof;
    return s->rx_ready != 0;
}

void net_task_cleanup(struct task *t) {
    task_t *task = (task_t *)t;
    if (!task) return;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (task->socks[i]) {
            net_socket_close(task->socks[i]);
            task->socks[i] = NULL;
        }
    }
}

/* ---- init and self test -------------------------------------------- */

u32 net_my_ip(void) { return g_ip; }
u32 net_gateway(void) { return g_gateway; }
u32 net_netmask(void) { return g_netmask; }
u32 net_dns_server(void) { return g_dns; }
const u8 *net_my_mac(void) { return g_dev ? g_dev->mac : NULL; }

void net_dump(void) {
    if (!g_dev) {
        kprintf("[net] no device\n");
        return;
    }
    const u8 *m = g_dev->mac;
    kprintf("[net] %s mac %02x:%02x:%02x:%02x:%02x:%02x ip %u.%u.%u.%u\n",
            g_dev->name, m[0], m[1], m[2], m[3], m[4], m[5],
            (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF,
            (g_ip >> 8) & 0xFF, g_ip & 0xFF);
    kprintf("[net] gateway %u.%u.%u.%u dns %u.%u.%u.%u\n",
            (g_gateway >> 24) & 0xFF, (g_gateway >> 16) & 0xFF,
            (g_gateway >> 8) & 0xFF, g_gateway & 0xFF,
            (g_dns >> 24) & 0xFF, (g_dns >> 16) & 0xFF,
            (g_dns >> 8) & 0xFF, g_dns & 0xFF);
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!g_arp[i].valid) continue;
        kprintf("[net] arp %u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                (g_arp[i].ip >> 24) & 0xFF, (g_arp[i].ip >> 16) & 0xFF,
                (g_arp[i].ip >> 8) & 0xFF, g_arp[i].ip & 0xFF,
                g_arp[i].mac[0], g_arp[i].mac[1], g_arp[i].mac[2],
                g_arp[i].mac[3], g_arp[i].mac[4], g_arp[i].mac[5]);
    }
}

int net_init(void) {
    memset(g_arp, 0, sizeof(g_arp));
    memset(g_socks, 0, sizeof(g_socks));

    g_ip = IP4(10, 0, 2, 15);
    g_gateway = IP4(10, 0, 2, 2);
    g_netmask = IP4(255, 255, 255, 0);
    g_dns = DNS_SERVER;

    g_dev = e1000_init();
    if (!g_dev) return -ENODEV;

    net_dump();
    return 0;
}

int net_selftest(void) {
    if (!g_dev) {
        kprintf("[net] self test skipped: no device\n");
        return -ENODEV;
    }

    kprintf("[net] pinging gateway...\n");
    int rc = net_ping(g_gateway, 3000);
    if (rc != 0) {        kprintf("[net] ping gateway failed: %d\n", rc);
        return rc;
    }
    kprintf("[net] ICMP echo reply from gateway\n");
    kprintf("NET_PING_OK\n");

    u32 ip = 0;
    rc = net_dns_lookup("example.com", &ip, 3000);
    if (rc == 0) {
        kprintf("[net] DNS example.com = %u.%u.%u.%u\n",
                (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 8) & 0xFF, ip & 0xFF);
        kprintf("NET_DNS_OK\n");
    } else {
        kprintf("[net] DNS lookup failed: %d\n", rc);
    }

    net_dump();
    kprintf("NET_TEST_OK\n");
    return 0;
}
