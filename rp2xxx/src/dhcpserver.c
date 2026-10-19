// dhcpserver.c - a minimal DHCP server for the access point: one /24, eight leases keyed by MAC.
#include "dhcpserver.h"

#include <string.h>
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define PORT_SERVER   67
#define PORT_CLIENT   68
#define FIRST_HOST    16
#define MAX_LEASES    8
#define LEASE_SECONDS (24 * 60 * 60)

#define OP_REQUEST 1
#define OP_REPLY   2

#define OPT_PAD        0
#define OPT_MASK       1
#define OPT_ROUTER     3
#define OPT_DNS        6
#define OPT_REQ_IP     50
#define OPT_LEASE_TIME 51
#define OPT_MSG_TYPE   53
#define OPT_SERVER_ID  54
#define OPT_END        255

#define DISCOVER 1
#define OFFER    2
#define REQUEST  3
#define DECLINE  4
#define ACK      5
#define NAK      6
#define RELEASE  7

typedef struct __attribute__((packed)) {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t options[312];
} dhcp_msg_t;

static const uint8_t MAGIC[4] = { 99, 130, 83, 99 };

static struct udp_pcb *s_pcb;
static struct netif *s_netif;
static void (*s_joined)(void);
static uint8_t s_lease_mac[MAX_LEASES][6];

// Find option `code`; returns a pointer to its length byte, or NULL.
static const uint8_t *find_option(const uint8_t *opt, const uint8_t *end, uint8_t code)
{
    while (opt < end && *opt != OPT_END) {
        if (*opt == OPT_PAD) {
            opt++;
            continue;
        }
        if (opt + 1 >= end || opt + 2 + opt[1] > end) {
            return NULL;
        }
        if (*opt == code) {
            return opt + 1;
        }
        opt += 2 + opt[1];
    }
    return NULL;
}

static int lease_for(const uint8_t *mac, bool create)
{
    static const uint8_t none[6];
    for (int i = 0; i < MAX_LEASES; i++) {
        if (memcmp(s_lease_mac[i], mac, 6) == 0) {
            return i;
        }
    }
    if (!create) {
        return -1;
    }
    for (int i = 0; i < MAX_LEASES; i++) {
        if (memcmp(s_lease_mac[i], none, 6) == 0) {
            memcpy(s_lease_mac[i], mac, 6);
            return i;
        }
    }
    // full: reuse the oldest slot
    memmove(s_lease_mac[0], s_lease_mac[1], sizeof(s_lease_mac) - 6);
    memcpy(s_lease_mac[MAX_LEASES - 1], mac, 6);
    return MAX_LEASES - 1;
}

static uint8_t *put_option(uint8_t *o, uint8_t code, const void *data, uint8_t len)
{
    *o++ = code;
    *o++ = len;
    memcpy(o, data, len);
    return o + len;
}

static void reply(const dhcp_msg_t *req, uint8_t type, int lease)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_msg_t), PBUF_RAM);
    if (!p) {
        return;
    }
    dhcp_msg_t *m = p->payload;
    memset(m, 0, sizeof(*m));
    m->op = OP_REPLY;
    m->htype = req->htype;
    m->hlen = req->hlen;
    m->xid = req->xid;
    m->flags = req->flags;
    memcpy(m->giaddr, req->giaddr, 4);
    memcpy(m->chaddr, req->chaddr, sizeof(m->chaddr));
    memcpy(m->magic, MAGIC, 4);

    const ip4_addr_t *own = netif_ip4_addr(s_netif);
    const ip4_addr_t *mask = netif_ip4_netmask(s_netif);
    uint8_t server[4];
    memcpy(server, &own->addr, 4);
    memcpy(m->siaddr, server, 4);
    if (lease >= 0) {
        memcpy(m->yiaddr, server, 3);
        m->yiaddr[3] = FIRST_HOST + lease;
    }

    uint8_t *o = m->options;
    o = put_option(o, OPT_MSG_TYPE, &type, 1);
    o = put_option(o, OPT_SERVER_ID, server, 4);
    if (type != NAK) {
        uint32_t secs = PP_HTONL(LEASE_SECONDS);
        o = put_option(o, OPT_MASK, &mask->addr, 4);
        o = put_option(o, OPT_ROUTER, server, 4);
        o = put_option(o, OPT_DNS, server, 4);
        o = put_option(o, OPT_LEASE_TIME, &secs, 4);
    }
    *o++ = OPT_END;
    pbuf_realloc(p, (uint16_t)(o - (uint8_t *)m));

    ip_addr_t dest;
    IP_ADDR4(&dest, 255, 255, 255, 255);
    udp_sendto_if(s_pcb, p, &dest, PORT_CLIENT, s_netif);
    pbuf_free(p);
}

static void on_packet(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    static dhcp_msg_t req;
    memset(&req, 0, sizeof(req));
    uint16_t len = pbuf_copy_partial(p, &req, sizeof(req), 0);
    pbuf_free(p);
    if (len < offsetof(dhcp_msg_t, options) || req.op != OP_REQUEST || memcmp(req.magic, MAGIC, 4) != 0) {
        return;
    }
    const uint8_t *end = (const uint8_t *)&req + len;
    const uint8_t *type = find_option(req.options, end, OPT_MSG_TYPE);
    if (!type || type[0] != 1) {
        return;
    }
    const uint8_t *mac = req.chaddr;
    switch (type[1]) {
    case DISCOVER:
        reply(&req, OFFER, lease_for(mac, true));
        break;
    case REQUEST: {
        int lease = lease_for(mac, true);
        const uint8_t *want = find_option(req.options, end, OPT_REQ_IP);
        uint8_t ip[4];
        if (want && want[0] == 4) {
            memcpy(ip, want + 1, 4);
        } else {
            memcpy(ip, req.ciaddr, 4);
        }
        const uint8_t *own = (const uint8_t *)&netif_ip4_addr(s_netif)->addr;
        bool ours = memcmp(ip, own, 3) == 0 && ip[3] == FIRST_HOST + lease;
        if (!ours && (ip[0] | ip[1] | ip[2] | ip[3])) {
            reply(&req, NAK, -1);
            break;
        }
        reply(&req, ACK, lease);
        if (s_joined) {
            s_joined();
        }
        break;
    }
    case RELEASE:
    case DECLINE: {
        int lease = lease_for(mac, false);
        if (lease >= 0) {
            memset(s_lease_mac[lease], 0, 6);
        }
        break;
    }
    default:
        break;
    }
}

bool dhcpserver_start(struct netif *netif, void (*joined)(void))
{
    dhcpserver_stop();
    s_pcb = udp_new();
    if (!s_pcb) {
        return false;
    }
    ip_set_option(s_pcb, SOF_BROADCAST);
    if (udp_bind(s_pcb, IP_ANY_TYPE, PORT_SERVER) != ERR_OK) {
        udp_remove(s_pcb);
        s_pcb = NULL;
        return false;
    }
    udp_bind_netif(s_pcb, netif);
    s_netif = netif;
    s_joined = joined;
    udp_recv(s_pcb, on_packet, NULL);
    return true;
}

void dhcpserver_stop(void)
{
    if (s_pcb) {
        udp_remove(s_pcb);
        s_pcb = NULL;
    }
}
