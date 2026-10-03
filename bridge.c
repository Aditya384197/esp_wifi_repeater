#include "user_config.h"

#ifdef REPEATER_MODE

#include "bridge.h"
#include "c_types.h"
#include "lwip/def.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "osapi.h"
#include "user_interface.h"
#include "sys_time.h"
#include "config_flash.h"
#include "easygpio.h"

extern sysconfig_t config;

/* my_channel is set by wifi event handler in user_main.c on STAMODE_CONNECTED */
extern uint8_t my_channel;
extern uint64_t Bytes_in, Bytes_out;
extern uint32_t Packets_in, Packets_out;
extern uint64_t Bytes_per_day;

/* -------------------------------------------------------------------------
 * Netif state
 * ------------------------------------------------------------------------- */

static struct netif        *s_sta_nif;
static struct netif        *s_ap_nif;
static netif_input_fn       s_orig_input_sta;
static netif_input_fn       s_orig_input_ap;
static netif_output_fn      s_orig_output_sta;
static netif_output_fn      s_orig_output_ap;
static netif_linkoutput_fn  s_orig_lo_sta;
static netif_linkoutput_fn  s_orig_lo_ap;
static bool                  s_bridge_hooks_installed;
static bool                  s_bridge_active;

/* -------------------------------------------------------------------------
 * Compact packed header types
 * ------------------------------------------------------------------------- */

#define ETHTYPE_IP  0x0800
#define ETHTYPE_ARP 0x0806

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t hwtype;
    uint16_t prtype;
    uint8_t  hwlen;
    uint8_t  prlen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_hdr_t;

typedef struct {
    uint8_t  vhl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t chksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t chksum;
} __attribute__((packed)) udp_hdr_t;

#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MAGIC_COOKIE 0x63825363UL

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[0];
} __attribute__((packed)) dhcp_msg_t;

/* -------------------------------------------------------------------------
 * Time helper
 * ------------------------------------------------------------------------- */

static uint32_t ICACHE_FLASH_ATTR now_secs(void)
{
    return (uint32_t)(get_long_systime() / 1000000ULL);
}

/* -------------------------------------------------------------------------
 * Forwarding Database (FDB)
 * ------------------------------------------------------------------------- */

#define FDB_SIZE   16
#define FDB_TTL_S 600

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t expires_s;
} fdb_entry_t;

static fdb_entry_t s_fdb[FDB_SIZE];

static void ICACHE_FLASH_ATTR fdb_insert_ttl(uint32_t ip, const uint8_t *mac, uint32_t ttl_s)
{
    uint32_t now;
    int free_idx = -1, oldest_idx = 0;
    uint32_t oldest_exp = 0xFFFFFFFFUL;
    int i;

    if (ip == 0 || mac == NULL) return;
    if (s_sta_nif && ip == s_sta_nif->ip_addr.addr) return;
    if (s_ap_nif && ip == s_ap_nif->ip_addr.addr) return;
    if (ttl_s == 0) ttl_s = FDB_TTL_S;
    if (ttl_s > 86400UL * 7UL) ttl_s = 86400UL * 7UL;

    now = now_secs();
    for (i = 0; i < FDB_SIZE; i++)
    {
        if (s_fdb[i].ip == ip)
        {
            os_memcpy(s_fdb[i].mac, mac, 6);
            s_fdb[i].expires_s = now + ttl_s;
            return;
        }
        if (s_fdb[i].ip == 0 || s_fdb[i].expires_s <= now)
        {
            if (free_idx < 0) free_idx = i;
        }
        if (s_fdb[i].expires_s < oldest_exp)
        {
            oldest_exp = s_fdb[i].expires_s;
            oldest_idx = i;
        }
    }

    {
        int idx = (free_idx >= 0) ? free_idx : oldest_idx;
        s_fdb[idx].ip = ip;
        os_memcpy(s_fdb[idx].mac, mac, 6);
        s_fdb[idx].expires_s = now + ttl_s;
    }
}

static void ICACHE_FLASH_ATTR fdb_insert(uint32_t ip, const uint8_t *mac)
{
    fdb_insert_ttl(ip, mac, FDB_TTL_S);
}

static const uint8_t * ICACHE_FLASH_ATTR fdb_lookup(uint32_t ip)
{
    uint32_t now = now_secs();
    int i;
    for (i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].ip == ip && s_fdb[i].expires_s > now) return s_fdb[i].mac;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * DHCP XID map
 * ------------------------------------------------------------------------- */

#define XID_MAP_SIZE  8
#define XID_TTL_S    30

typedef struct {
    uint32_t xid;
    uint8_t  chaddr[6];
    uint32_t expires_s;
} xid_entry_t;

static xid_entry_t s_xid_map[XID_MAP_SIZE];

static void ICACHE_FLASH_ATTR xid_map_insert(uint32_t xid, const uint8_t *chaddr)
{
    uint32_t now = now_secs();
    int free_idx = -1, oldest_idx = 0;
    uint32_t oldest_exp = 0xFFFFFFFFUL;
    int i;
    for (i = 0; i < XID_MAP_SIZE; i++) {
        if (s_xid_map[i].xid == xid) {
            os_memcpy(s_xid_map[i].chaddr, chaddr, 6); s_xid_map[i].expires_s = now + XID_TTL_S;
            return;
        }
        if (s_xid_map[i].xid == 0 || s_xid_map[i].expires_s <= now) { if (free_idx < 0) free_idx = i; }
        if (s_xid_map[i].expires_s < oldest_exp) { oldest_exp = s_xid_map[i].expires_s; oldest_idx = i; }
    }
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    s_xid_map[idx].xid = xid; os_memcpy(s_xid_map[idx].chaddr, chaddr, 6); s_xid_map[idx].expires_s = now + XID_TTL_S;
}

static const uint8_t * ICACHE_FLASH_ATTR xid_map_lookup(uint32_t xid)
{
    uint32_t now = now_secs();
    int i;
    for (i = 0; i < XID_MAP_SIZE; i++) {
        if (s_xid_map[i].xid == xid && s_xid_map[i].expires_s > now) return s_xid_map[i].chaddr;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline void * ICACHE_FLASH_ATTR pkt_at(struct pbuf *p, uint16_t off, uint16_t need)
{
    if (p->tot_len < (uint16_t)(off + need)) return NULL;
    return (uint8_t *)p->payload + off;
}

static void ICACHE_FLASH_ATTR update_ip_chksum(ip_hdr_t *ip)
{
    ip->chksum = 0;
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)ip;
    int i;
    for (i = 0; i < (ip->vhl & 0x0f) * 2; i++) sum += ntohs(p[i]);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    ip->chksum = htons(~((uint16_t)sum));
}

static uint8_t * ICACHE_FLASH_ATTR dhcp_find_option(uint8_t *opts, uint16_t opts_len, uint8_t tag, uint8_t *out_len)
{
    uint16_t i = 0;
    while (i < opts_len)
    {
        uint8_t t = opts[i];
        if (t == 255)
            return tag == 255 ? &opts[i] : NULL;
        if (t == 0)
        {
            i++;
            continue;
        }
        if (i + 1 >= opts_len) return NULL;
        uint8_t l = opts[i + 1];
        if ((uint16_t)(i + 2 + l) > opts_len) return NULL;
        if (t == tag)
        {
            if (out_len) *out_len = l;
            return &opts[i + 2];
        }
        i = (uint16_t)(i + 2 + l);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Proxy ARP & Maintenance
 * ------------------------------------------------------------------------- */

static void ICACHE_FLASH_ATTR send_proxy_arp_reply(struct netif *tx_nif, netif_linkoutput_fn lo, const arp_hdr_t *req, uint32_t target_ip)
{
    uint16_t pkt_len = sizeof(eth_hdr_t) + sizeof(arp_hdr_t);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, pkt_len, PBUF_RAM);
    if (!p) return;
    eth_hdr_t *eth = (eth_hdr_t *)p->payload;
    arp_hdr_t *arp = (arp_hdr_t *)((uint8_t *)p->payload + sizeof(eth_hdr_t));
    os_memcpy(eth->dst, req->sha, 6); os_memcpy(eth->src, tx_nif->hwaddr, 6); eth->type = htons(ETHTYPE_ARP);
    arp->hwtype = req->hwtype; arp->prtype = req->prtype; arp->hwlen = req->hwlen; arp->prlen = req->prlen;
    arp->op = htons(2); 
    os_memcpy(arp->sha, tx_nif->hwaddr, 6); arp->spa = target_ip;
    os_memcpy(arp->tha, req->sha, 6); arp->tpa = req->spa;
    lo(tx_nif, p); pbuf_free(p);
}


/* -------------------------------------------------------------------------
 * DHCP snooping
 * ------------------------------------------------------------------------- */

static void ICACHE_FLASH_ATTR snoop_dhcp_request(struct pbuf *p, uint16_t eth_ip_udp_hdr_len)
{
    dhcp_msg_t *dhcp = (dhcp_msg_t *)pkt_at(p, eth_ip_udp_hdr_len, sizeof(dhcp_msg_t));
    if (!dhcp || dhcp->op != DHCP_OP_REQUEST || dhcp->hlen != 6 || dhcp->magic != htonl(DHCP_MAGIC_COOKIE)) return;

    uint8_t client_mac[6]; os_memcpy(client_mac, dhcp->chaddr, 6);
    os_memcpy(dhcp->chaddr, s_sta_nif->hwaddr, 6); 
    dhcp->flags |= htons(0x8000); 

    uint16_t opts_len = p->len - eth_ip_udp_hdr_len - (uint16_t)sizeof(dhcp_msg_t);
    uint8_t msg_type = 0, optlen;
    uint8_t *opt = dhcp_find_option(dhcp->options, opts_len, 53, &optlen);
    if (opt && optlen >= 1) msg_type = opt[0];

    if (msg_type == DHCP_MSG_DISCOVER || msg_type == DHCP_MSG_REQUEST) {
        xid_map_insert(dhcp->xid, client_mac);
        uint8_t opt61len;
        uint8_t *opt61 = dhcp_find_option(dhcp->options, opts_len, 61, &opt61len);
        if (!opt61 && opts_len + 9 <= 308) { 
            uint8_t *end = dhcp_find_option(dhcp->options, opts_len, 255, NULL);
            if (end) {
                end[0] = 61; end[1] = 7; end[2] = 1; os_memcpy(&end[3], client_mac, 6); end[9] = 255;
                uint16_t *ip_len_ptr = (uint16_t *)pkt_at(p, sizeof(eth_hdr_t) + 2, 2);
                uint16_t *udp_len_ptr = (uint16_t *)pkt_at(p, eth_ip_udp_hdr_len - 4, 2);
                if (ip_len_ptr) *ip_len_ptr = htons(ntohs(*ip_len_ptr) + 9);
                if (udp_len_ptr) *udp_len_ptr = htons(ntohs(*udp_len_ptr) + 9);
                p->len += 9; p->tot_len += 9;
                ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
                if (ip) update_ip_chksum(ip);
            }
        }
        udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, eth_ip_udp_hdr_len - (uint16_t)sizeof(udp_hdr_t), sizeof(udp_hdr_t));
        if (udp) udp->chksum = 0;
    }
}

static bool ICACHE_FLASH_ATTR snoop_dhcp_reply(struct pbuf *p, uint16_t eth_ip_udp_hdr_len, uint8_t chaddr_out[6])
{
    dhcp_msg_t *dhcp = (dhcp_msg_t *)pkt_at(p, eth_ip_udp_hdr_len, sizeof(dhcp_msg_t));
    if (!dhcp || dhcp->op != DHCP_OP_REPLY || dhcp->hlen != 6 ||
        dhcp->magic != htonl(DHCP_MAGIC_COOKIE)) return false;

    const uint8_t *orig_mac = xid_map_lookup(dhcp->xid);
    if (orig_mac)
    {
        os_memcpy(chaddr_out, orig_mac, 6);
        os_memcpy(dhcp->chaddr, orig_mac, 6);
        udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, eth_ip_udp_hdr_len - (uint16_t)sizeof(udp_hdr_t), sizeof(udp_hdr_t));
        if (udp) udp->chksum = 0;
    }
    else
    {
        os_memcpy(chaddr_out, dhcp->chaddr, 6);
    }

    /* DHCP replies for the ESP's own station lease belong to lwIP, not to an
       AP client. Consuming them here can make the uplink lease appear flaky. */
    if (s_sta_nif && os_memcmp(chaddr_out, s_sta_nif->hwaddr, 6) == 0)
        return false;

    if (dhcp->yiaddr != 0)
    {
        uint16_t opts_len = p->len - eth_ip_udp_hdr_len - (uint16_t)sizeof(dhcp_msg_t);
        uint8_t msg_type = 0, optlen;
        uint8_t *opt = dhcp_find_option(dhcp->options, opts_len, 53, &optlen);
        if (opt && optlen >= 1 && opt[0] == DHCP_MSG_ACK)
        {
            uint32_t ttl = FDB_TTL_S;
            opt = dhcp_find_option(dhcp->options, opts_len, 51, &optlen);
            if (opt && optlen >= 4)
                ttl = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                      ((uint32_t)opt[2] << 8) | (uint32_t)opt[3];
            fdb_insert_ttl(dhcp->yiaddr, chaddr_out, ttl);
        }
        (void)msg_type;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Minimal mDNS responder for AP-side queries
 *
 * The SDK's espconn_mdns_init only handles queries arriving on the STA netif
 * (it filters on a WiFi-driver interface tag below lwip), so AP-side clients
 * cannot resolve esp-wifi-repeater.local through it.  We answer A queries for
 * the configured hostname directly here, sending a unicast reply to the
 * querier's MAC via the AP linkoutput.
 * ------------------------------------------------------------------------- */

#define MDNS_HOSTNAME      "esp-wifi-repeater"
#define MDNS_HOSTNAME_LEN  17

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_hdr_t;

static inline uint8_t ICACHE_FLASH_ATTR ascii_lower(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

static bool ICACHE_FLASH_ATTR label_iequal(const uint8_t *a, const char *b, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        if (ascii_lower(a[i]) != ascii_lower((uint8_t)b[i])) return false;
    return true;
}

/* Walk a single question name starting at body[*pos].  Returns true if it
   matches "<hostname>.local" (case-insensitive).  On return *pos points past
   the name and qtype/qclass are filled in.  Compression pointers in questions
   are rare and not handled. */
static bool ICACHE_FLASH_ATTR mdns_match_hostname(const uint8_t *body, uint16_t body_len,
                                                  uint16_t *pos, uint16_t *qtype, uint16_t *qclass)
{
    uint16_t p = *pos;
    bool match = true;
    uint8_t label_idx = 0;
    while (p < body_len) {
        uint8_t len = body[p++];
        if (len == 0) break;
        if ((len & 0xC0) != 0) return false;
        if (p + len > body_len) return false;
        if (label_idx == 0) {
            if (len != MDNS_HOSTNAME_LEN || !label_iequal(body + p, MDNS_HOSTNAME, len)) match = false;
        } else if (label_idx == 1) {
            if (len != 5 || !label_iequal(body + p, "local", len)) match = false;
        } else {
            match = false;
        }
        p += len;
        label_idx++;
    }
    if (label_idx != 2) match = false;
    if (p + 4 > body_len) return false;
    *qtype  = ((uint16_t)body[p] << 8) | body[p + 1];
    *qclass = ((uint16_t)body[p + 2] << 8) | body[p + 3];
    *pos = p + 4;
    return match;
}

static void ICACHE_FLASH_ATTR send_mdns_a_reply(const uint8_t *client_mac, uint32_t client_ip,
                                                uint16_t client_udp_port, uint16_t query_id)
{
    if (s_sta_nif->ip_addr.addr == 0) return;

    const uint16_t name_len   = 1 + MDNS_HOSTNAME_LEN + 1 + 5 + 1;     /* 25 */
    const uint16_t answer_len = name_len + 2 + 2 + 4 + 2 + 4;          /* +type,class,ttl,rdlen,rdata */
    const uint16_t mdns_len   = sizeof(dns_hdr_t) + answer_len;
    const uint16_t udp_len    = sizeof(udp_hdr_t) + mdns_len;
    const uint16_t ip_len     = sizeof(ip_hdr_t) + udp_len;
    const uint16_t pkt_len    = sizeof(eth_hdr_t) + ip_len;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, pkt_len, PBUF_RAM);
    if (!p) return;

    eth_hdr_t *eth = (eth_hdr_t *)p->payload;
    os_memcpy(eth->dst, client_mac, 6);
    os_memcpy(eth->src, s_ap_nif->hwaddr, 6);
    eth->type = htons(ETHTYPE_IP);

    ip_hdr_t *ip = (ip_hdr_t *)((uint8_t *)p->payload + sizeof(eth_hdr_t));
    ip->vhl = 0x45; ip->tos = 0; ip->len = htons(ip_len);
    ip->id = 0; ip->off = 0; ip->ttl = 255; ip->proto = 17; ip->chksum = 0;
    ip->src = s_sta_nif->ip_addr.addr;
    ip->dst = client_ip;
    update_ip_chksum(ip);

    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    udp->src_port = htons(5353);
    udp->dst_port = htons(client_udp_port);
    udp->len = htons(udp_len);
    udp->chksum = 0;

    dns_hdr_t *dns = (dns_hdr_t *)((uint8_t *)udp + sizeof(udp_hdr_t));
    dns->id = htons(query_id);
    dns->flags = htons(0x8400);   /* QR=1, AA=1 */
    dns->qdcount = 0; dns->ancount = htons(1); dns->nscount = 0; dns->arcount = 0;

    uint8_t *a = (uint8_t *)dns + sizeof(dns_hdr_t);
    *a++ = MDNS_HOSTNAME_LEN; os_memcpy(a, MDNS_HOSTNAME, MDNS_HOSTNAME_LEN); a += MDNS_HOSTNAME_LEN;
    *a++ = 5;                  os_memcpy(a, "local", 5);                       a += 5;
    *a++ = 0;
    *a++ = 0x00; *a++ = 0x01;        /* TYPE  = A  */
    *a++ = 0x80; *a++ = 0x01;        /* CLASS = IN with cache-flush bit */
    *a++ = 0;    *a++ = 0;    *a++ = 0; *a++ = 120;  /* TTL = 120s */
    *a++ = 0;    *a++ = 4;            /* RDLENGTH = 4 */
    os_memcpy(a, &s_sta_nif->ip_addr.addr, 4);

    s_orig_lo_ap(s_ap_nif, p);
    pbuf_free(p);
}

/* Called for an mDNS query (UDP dst port 5353) arriving on the AP side.
   q is the bridge's working copy of the frame; udp_off is the offset of the
   UDP header from the start of the Ethernet frame.  client_mac is the
   original src MAC from the unrewritten Ethernet header. */
static void ICACHE_FLASH_ATTR handle_ap_mdns_query(struct pbuf *q, uint16_t udp_off, const uint8_t *client_mac)
{
    udp_hdr_t *udp = (udp_hdr_t *)pkt_at(q, udp_off, sizeof(udp_hdr_t));
    if (!udp) return;
    uint16_t mdns_off = udp_off + sizeof(udp_hdr_t);
    dns_hdr_t *dns = (dns_hdr_t *)pkt_at(q, mdns_off, sizeof(dns_hdr_t));
    if (!dns) return;

    if (ntohs(dns->flags) & 0x8000) return;     /* not a query */
    uint16_t qdcount = ntohs(dns->qdcount);
    if (qdcount == 0 || qdcount > 16) return;

    uint16_t udp_payload_len = ntohs(udp->len);
    if (udp_payload_len < sizeof(udp_hdr_t) + sizeof(dns_hdr_t)) return;
    uint16_t body_len = udp_payload_len - sizeof(udp_hdr_t);
    uint8_t *body = (uint8_t *)dns;
    if (mdns_off + body_len > q->len) return;   /* stay within first pbuf chunk */

    ip_hdr_t *ip = (ip_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
    if (!ip) return;

    uint16_t pos = sizeof(dns_hdr_t);
    uint16_t i;
    for (i = 0; i < qdcount; i++) {
        uint16_t qtype = 0, qclass = 0;
        bool match = mdns_match_hostname(body, body_len, &pos, &qtype, &qclass);
        if (match && (qtype == 1 /*A*/ || qtype == 0xFF /*ANY*/) && (qclass & 0x7FFF) == 1) {
            send_mdns_a_reply(client_mac, ip->src, ntohs(udp->src_port), ntohs(dns->id));
            return;
        }
        if (pos == 0 || pos > body_len) return;
    }
}

/* -------------------------------------------------------------------------
 * Hooks
 * ------------------------------------------------------------------------- */

static inline bool ICACHE_FLASH_ATTR bridge_is_multicast(const eth_hdr_t *eth)
{
    return (eth->dst[0] & 0x01) != 0;
}

static void ICACHE_FLASH_ATTR bridge_count_out(uint16_t len)
{
    Bytes_out += len;
    Packets_out++;
#if DAILY_LIMIT
    Bytes_per_day += len;
#endif
}

static void ICACHE_FLASH_ATTR bridge_count_in(uint16_t len)
{
    Bytes_in += len;
    Packets_in++;
#if DAILY_LIMIT
    Bytes_per_day += len;
#endif
}

static err_t ICACHE_FLASH_ATTR bridge_output_sta(struct netif *netif, struct pbuf *p, ip_addr_t *ipaddr)
{
    if (!s_bridge_active)
        return s_orig_output_sta(netif, p, ipaddr);

    if (ipaddr != NULL)
    {
        const uint8_t *client_mac = fdb_lookup(ipaddr->addr);
        if (client_mac && pbuf_header(p, sizeof(eth_hdr_t)) == 0)
        {
            eth_hdr_t *eth = (eth_hdr_t *)p->payload;
            os_memcpy(eth->dst, client_mac, 6);
            os_memcpy(eth->src, s_ap_nif->hwaddr, 6);
            eth->type = htons(ETHTYPE_IP);
            bridge_count_out(p->tot_len);
            err_t err = s_orig_lo_ap(s_ap_nif, p);
            pbuf_header(p, -(s16_t)sizeof(eth_hdr_t));
            return err;
        }

        /* Locally generated IPv4 multicast must reach AP clients as well as
           the upstream network. Copy only this exceptional traffic; ordinary
           unicast stays allocation-free. */
        uint32_t hip = ntohl(ipaddr->addr);
        if ((hip >> 28) == 0xE && pbuf_header(p, sizeof(eth_hdr_t)) == 0)
        {
            struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
            if (q)
            {
                pbuf_copy(q, p);
                eth_hdr_t *ethq = (eth_hdr_t *)q->payload;
                ethq->dst[0] = 0x01; ethq->dst[1] = 0x00; ethq->dst[2] = 0x5e;
                ethq->dst[3] = (uint8_t)((hip >> 16) & 0x7f);
                ethq->dst[4] = (uint8_t)((hip >> 8) & 0xff);
                ethq->dst[5] = (uint8_t)(hip & 0xff);
                os_memcpy(ethq->src, s_ap_nif->hwaddr, 6);
                ethq->type = htons(ETHTYPE_IP);
                s_orig_lo_ap(s_ap_nif, q);
                pbuf_free(q);
            }
            pbuf_header(p, -(s16_t)sizeof(eth_hdr_t));
        }
    }

    return s_orig_output_sta(netif, p, ipaddr);
}

static err_t ICACHE_FLASH_ATTR bridge_output_ap(struct netif *netif, struct pbuf *p, ip_addr_t *ipaddr)
{
    if (!s_bridge_active)
        return s_orig_output_ap(netif, p, ipaddr);

    if (ipaddr != NULL)
    {
        const uint8_t *client_mac = fdb_lookup(ipaddr->addr);
        if (client_mac && pbuf_header(p, sizeof(eth_hdr_t)) == 0)
        {
            eth_hdr_t *eth = (eth_hdr_t *)p->payload;
            os_memcpy(eth->dst, client_mac, 6);
            os_memcpy(eth->src, s_ap_nif->hwaddr, 6);
            eth->type = htons(ETHTYPE_IP);
            err_t err = s_orig_lo_ap(s_ap_nif, p);
            pbuf_header(p, -(s16_t)sizeof(eth_hdr_t));
            return err;
        }
    }

    return s_orig_output_sta(s_sta_nif, p, ipaddr);
}

static err_t ICACHE_FLASH_ATTR bridge_input_ap(struct pbuf *p, struct netif *inp)
{
    eth_hdr_t *eth;
    bool is_bcast;
    bool is_to_ap_mac;
    uint16_t eth_type;
    const uint8_t *src_mac;

    if (!s_bridge_active || p == NULL || p->tot_len < sizeof(eth_hdr_t))
        return s_orig_input_ap(p, inp);

    eth = (eth_hdr_t *)p->payload;
    is_bcast = bridge_is_multicast(eth);
    is_to_ap_mac = (os_memcmp(eth->dst, s_ap_nif->hwaddr, 6) == 0);
    eth_type = ntohs(eth->type);
    src_mac = eth->src;

    bridge_count_out(p->tot_len);

    /* Learn the AP client's IP before any forwarding/local-delivery decision. */
    if (eth_type == ETHTYPE_ARP)
    {
        arp_hdr_t *arp = (arp_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
        if (arp) fdb_insert(arp->spa, src_mac);
    }
    else if (eth_type == ETHTYPE_IP)
    {
        ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
        if (ip) fdb_insert(ip->src, src_mac);
    }

    /* Frames explicitly addressed to the ESP AP interface are local. */
    if (is_to_ap_mac && !is_bcast)
        return s_orig_input_ap(p, inp);

    if (is_bcast)
    {
        /* Broadcast/multicast has two consumers: upstream and local lwIP.
           Only this path allocates a second pbuf. */
        struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len + 16, PBUF_RAM);
        if (q == NULL)
            return s_orig_input_ap(p, inp);

        pbuf_copy(q, p);
        eth_hdr_t *qeth = (eth_hdr_t *)q->payload;
        os_memcpy(qeth->src, s_sta_nif->hwaddr, 6);

        if (eth_type == ETHTYPE_ARP)
        {
            arp_hdr_t *arp = (arp_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
            if (arp)
            {
                if (ntohs(arp->op) == 1 && s_sta_nif->ip_addr.addr != 0 && arp->tpa == s_sta_nif->ip_addr.addr)
                {
                    /* send_proxy_arp_reply() reads the request, so q must stay
                       alive until after the reply has been constructed/sent. */
                    send_proxy_arp_reply(s_ap_nif, s_orig_lo_ap, arp, s_sta_nif->ip_addr.addr);
                    pbuf_free(q);
                    pbuf_free(p);
                    return ERR_OK;
                }
                os_memcpy(arp->sha, s_sta_nif->hwaddr, 6);
                s_orig_lo_sta(s_sta_nif, q);
            }
            else
                pbuf_free(q);
        }
        else if (eth_type == ETHTYPE_IP)
        {
            ip_hdr_t *ip = (ip_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
            if (ip)
            {
                uint16_t udp_off = sizeof(eth_hdr_t) + (uint16_t)((ip->vhl & 0x0f) * 4);
                if (ip->proto == 17)
                {
                    udp_hdr_t *udp = (udp_hdr_t *)pkt_at(q, udp_off, sizeof(udp_hdr_t));
                    if (udp && ntohs(udp->dst_port) == 67)
                        snoop_dhcp_request(q, udp_off + sizeof(udp_hdr_t));
                    if (udp && ntohs(udp->dst_port) == 5353)
                        handle_ap_mdns_query(q, udp_off, src_mac);
                }
                s_orig_lo_sta(s_sta_nif, q);
            }
            else
                pbuf_free(q);
        }
        else
            s_orig_lo_sta(s_sta_nif, q);

        pbuf_free(q);

        return s_orig_input_ap(p, inp);
    }

    /* Ordinary unicast forwarding: mutate the original pbuf in place. This is
       the high-rate path and avoids a heap allocation/copy per packet. */
    {
        uint8_t old_src[6];
        os_memcpy(old_src, eth->src, 6);
        os_memcpy(eth->src, s_sta_nif->hwaddr, 6);

        if (eth_type == ETHTYPE_ARP)
        {
            arp_hdr_t *arp = (arp_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
            if (arp)
            {
                if (ntohs(arp->op) == 1 && s_sta_nif->ip_addr.addr != 0 && arp->tpa == s_sta_nif->ip_addr.addr)
                {
                    os_memcpy(eth->src, old_src, 6);
                    send_proxy_arp_reply(s_ap_nif, s_orig_lo_ap, arp, s_sta_nif->ip_addr.addr);
                    pbuf_free(p);
                    return ERR_OK;
                }
                os_memcpy(arp->sha, s_sta_nif->hwaddr, 6);
            }
        }
        else if (eth_type == ETHTYPE_IP)
        {
            ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
            if (ip && ip->proto == 17)
            {
                uint16_t udp_off = sizeof(eth_hdr_t) + (uint16_t)((ip->vhl & 0x0f) * 4);
                udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, udp_off, sizeof(udp_hdr_t));
                if (udp && ntohs(udp->dst_port) == 67)
                    snoop_dhcp_request(p, udp_off + sizeof(udp_hdr_t));
            }
        }

        s_orig_lo_sta(s_sta_nif, p);
        os_memcpy(eth->src, old_src, 6);
    }

    pbuf_free(p);
    return ERR_OK;
}

static err_t ICACHE_FLASH_ATTR bridge_input_sta(struct pbuf *p, struct netif *inp)
{
    eth_hdr_t *eth;
    bool is_bcast;
    bool is_to_sta_mac;
    uint16_t eth_type;
    const uint8_t *src_mac;

    if (!s_bridge_active || p == NULL || p->tot_len < sizeof(eth_hdr_t))
        return s_orig_input_sta(p, inp);

    eth = (eth_hdr_t *)p->payload;
    is_bcast = bridge_is_multicast(eth);
    is_to_sta_mac = (os_memcmp(eth->dst, s_sta_nif->hwaddr, 6) == 0);
    eth_type = ntohs(eth->type);
    src_mac = eth->src;

    bridge_count_in(p->tot_len);

    if (eth_type == ETHTYPE_IP)
    {
        ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
        if (ip) fdb_insert(ip->src, src_mac);
    }
    else if (eth_type == ETHTYPE_ARP)
    {
        arp_hdr_t *arp = (arp_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
        if (arp) fdb_insert(arp->spa, src_mac);
    }

    if (is_bcast)
    {
        struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
        if (q)
        {
            pbuf_copy(q, p);
            eth_hdr_t *qeth = (eth_hdr_t *)q->payload;
            os_memcpy(qeth->src, s_ap_nif->hwaddr, 6);

            if (eth_type == ETHTYPE_IP)
            {
                ip_hdr_t *ip = (ip_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
                if (ip && ip->proto == 17)
                {
                    uint16_t udp_off = sizeof(eth_hdr_t) + (uint16_t)((ip->vhl & 0x0f) * 4);
                    udp_hdr_t *udp = (udp_hdr_t *)pkt_at(q, udp_off, sizeof(udp_hdr_t));
                    if (udp && ntohs(udp->src_port) == 67 && ntohs(udp->dst_port) == 68)
                    {
                        uint8_t client_mac[6];
                        if (snoop_dhcp_reply(q, udp_off + sizeof(udp_hdr_t), client_mac))
                        {
                            os_memcpy(qeth->dst, client_mac, 6);
                        }
                    }
                }
            }

            s_orig_lo_ap(s_ap_nif, q);
            pbuf_free(q);
        }
        return s_orig_input_sta(p, inp);
    }

    if (eth_type == ETHTYPE_ARP)
    {
        arp_hdr_t *arp = (arp_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
        if (arp)
        {
            const uint8_t *client_mac = fdb_lookup(arp->tpa);
            if (ntohs(arp->op) == 1 && client_mac)
            {
                send_proxy_arp_reply(s_sta_nif, s_orig_lo_sta, arp, arp->tpa);
                pbuf_free(p);
                return ERR_OK;
            }

            if (is_to_sta_mac)
                return s_orig_input_sta(p, inp);

            if (client_mac)
            {
                uint8_t old_src[6];
                os_memcpy(old_src, eth->src, 6);
                os_memcpy(eth->dst, client_mac, 6);
                os_memcpy(eth->src, s_ap_nif->hwaddr, 6);
                os_memcpy(arp->sha, s_ap_nif->hwaddr, 6);
                os_memcpy(arp->tha, client_mac, 6);
                s_orig_lo_ap(s_ap_nif, p);
                os_memcpy(eth->src, old_src, 6);
                pbuf_free(p);
                return ERR_OK;
            }
        }
    }
    else if (eth_type == ETHTYPE_IP)
    {
        ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
        if (ip)
        {
            uint8_t client_mac[6];
            bool have_dhcp = false;
            if (ip->proto == 17)
            {
                uint16_t udp_off = sizeof(eth_hdr_t) + (uint16_t)((ip->vhl & 0x0f) * 4);
                udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, udp_off, sizeof(udp_hdr_t));
                if (udp && ntohs(udp->src_port) == 67 && ntohs(udp->dst_port) == 68)
                    have_dhcp = snoop_dhcp_reply(p, udp_off + sizeof(udp_hdr_t), client_mac);
            }

            const uint8_t *dest_mac = have_dhcp ? client_mac : fdb_lookup(ip->dst);
            if (dest_mac && ip->dst != s_sta_nif->ip_addr.addr)
            {
                uint8_t old_src[6];
                os_memcpy(old_src, eth->src, 6);
                os_memcpy(eth->dst, dest_mac, 6);
                os_memcpy(eth->src, s_ap_nif->hwaddr, 6);
                s_orig_lo_ap(s_ap_nif, p);
                os_memcpy(eth->src, old_src, 6);
                pbuf_free(p);
                return ERR_OK;
            }
        }
    }

    if (is_to_sta_mac)
        return s_orig_input_sta(p, inp);

    pbuf_free(p);
    return ERR_OK;
}

void ICACHE_FLASH_ATTR bridge_init(struct netif *sta_nif, struct netif *ap_nif)
{
    if (sta_nif == NULL || ap_nif == NULL)
        return;

    s_sta_nif = sta_nif;
    s_ap_nif = ap_nif;

    if (!s_bridge_hooks_installed)
    {
        s_orig_input_sta = sta_nif->input;
        s_orig_input_ap = ap_nif->input;
        s_orig_output_sta = sta_nif->output;
        s_orig_output_ap = ap_nif->output;
        s_orig_lo_sta = sta_nif->linkoutput;
        s_orig_lo_ap = ap_nif->linkoutput;

        sta_nif->input = bridge_input_sta;
        ap_nif->input = bridge_input_ap;
        sta_nif->output = bridge_output_sta;
        ap_nif->output = bridge_output_ap;
        s_bridge_hooks_installed = true;
    }

    s_bridge_active = true;
    netif_set_default(sta_nif);
    sta_nif->napt = 0;
    ap_nif->napt = 0;
    os_memset(s_fdb, 0, sizeof(s_fdb));
    os_memset(s_xid_map, 0, sizeof(s_xid_map));

    {
        struct softap_config ap_cfg;
        wifi_softap_get_config(&ap_cfg);
        ap_cfg.channel = my_channel;
        wifi_softap_set_config(&ap_cfg);
    }
    wifi_set_sleep_type(NONE_SLEEP_T);
    os_printf("bridge: init done on channel %d\n", my_channel);
}

void ICACHE_FLASH_ATTR bridge_uplink_down(void)
{
    s_bridge_active = false;
    os_memset(s_fdb, 0, sizeof(s_fdb));
    os_memset(s_xid_map, 0, sizeof(s_xid_map));
    os_printf("bridge: uplink down, AP returned to local management path\n");
}

void ICACHE_FLASH_ATTR bridge_show_fdb(void)
{
    uint32_t now = now_secs();
    os_printf("Repeater FDB (IP -> Client MAC):\n");
    int i, count = 0;
    for (i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].ip != 0 && s_fdb[i].expires_s > now) {
            os_printf("  " IPSTR " -> %02x:%02x:%02x:%02x:%02x:%02x (expires in %d s)\n",
                      IP2STR(&s_fdb[i].ip),
                      s_fdb[i].mac[0], s_fdb[i].mac[1], s_fdb[i].mac[2],
                      s_fdb[i].mac[3], s_fdb[i].mac[4], s_fdb[i].mac[5],
                      (int)(s_fdb[i].expires_s - now));
            count++;
        }
    }
    if (count == 0) os_printf("  (empty)\n");
}

#endif /* REPEATER_MODE */
