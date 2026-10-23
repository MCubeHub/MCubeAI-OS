#pragma once
/* Ethernet, ARP, IPv4/ICMP/UDP and a small socket layer. */

#include <mcube/types.h>

#define ETH_ALEN        6
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

#define NET_MTU         1500
#define NET_MAX_SOCKETS 16

#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

#define IP4(a, b, c, d) \
    (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

struct task;

typedef struct net_device {
    char name[16];
    u8 mac[ETH_ALEN];
    int (*send)(const void *frame, u32 len);
    void (*poll)(void);
    void *priv;
    struct net_device *next;
} net_device_t;

struct net_socket;

int  net_init(void);
void net_poll(void);
void net_receive(const void *frame, u32 len);
net_device_t *e1000_init(void);
int  net_ping(u32 ip, u32 timeout_ms);
int  net_dns_lookup(const char *name, u32 *ip_out, u32 timeout_ms);
u32  net_my_ip(void);
u32  net_gateway(void);
const u8 *net_my_mac(void);
void net_dump(void);
int  net_selftest(void);

/* Socket objects. The descriptor table lives in the task (task->socks[]) so
 * that sockets share the fd space with files; these helpers only own the
 * socket state itself. Addresses are host-order u32. */
struct net_socket *net_socket_open(int type);
void net_socket_close(struct net_socket *s);
int  net_socket_bind(struct net_socket *s, u32 ip, u16 port);
int  net_socket_connect(struct net_socket *s, u32 ip, u16 port);
int  net_socket_send(struct net_socket *s, const void *buf, u64 len);
int  net_socket_sendto(struct net_socket *s, const void *buf, u64 len,
                       u32 ip, u16 port);
int  net_socket_recv(struct net_socket *s, void *buf, u64 len);
int  net_socket_recvfrom(struct net_socket *s, void *buf, u64 len,
                         u32 *ip, u16 *port);
void net_task_cleanup(struct task *t);
