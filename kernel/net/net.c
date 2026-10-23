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

struct net_socket {
    int used;
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
        timer_sleep_ms(1);
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
            g_socks[i].type = type;
            return &g_socks[i];
        }
    }
    return NULL;
}

void net_socket_close(struct net_socket *s) {
    if (s) memset(s, 0, sizeof(*s));
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

int net_socket_connect(struct net_socket *s, u32 ip, u16 port) {
    if (!s) return -EBADF;
    if (s->type == SOCK_STREAM) return -ENOSYS; /* TCP arrives in M6.1 */
    s->raddr = ip;
    s->rport = port;
    s->connected = 1;
    if (!s->lport) s->lport = ephemeral_port();
    return 0;
}

int net_socket_sendto(struct net_socket *s, const void *buf, u64 len,
                      u32 ip, u16 port) {
    if (!s) return -EBADF;
    if (s->type != SOCK_DGRAM) return -ENOSYS;
    if (!s->lport) s->lport = ephemeral_port();

    int rc = udp_send(ip, s->lport, port, buf, (u32)len);
    return rc == 0 ? (int)len : rc;
}

int net_socket_recvfrom(struct net_socket *s, void *buf, u64 len,
                        u32 *ip, u16 *port) {
    if (!s) return -EBADF;

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
    if (!s->connected) return -ENOTCONN;
    return net_socket_sendto(s, buf, len, s->raddr, s->rport);
}

int net_socket_recv(struct net_socket *s, void *buf, u64 len) {
    return net_socket_recvfrom(s, buf, len, NULL, NULL);
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
