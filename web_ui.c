/*
 * Web dashboard for the ESP WiFi repeater / router.
 *
 * Fixes compared to the old web config:
 *  - URL decoding of all values (+, %xx): SSIDs/passwords with special
 *    characters or spaces are now stored correctly
 *  - settings are applied directly to the config (no console round trip,
 *    no multiple espconn_send() calls in one callback)
 *  - the reply is sent BEFORE a restart is triggered (deferred by a timer)
 *  - replies are sent in chunks, each chunk from the "sent" callback
 *  - status page: link state, last disconnect reason, RSSI, channel, IP, ...
 *  - scan for networks, Save / Save&Connect / Restart / Factory reset
 */
/* include order identical to user_main.c: lwip headers need err_t/ip_addr_t first */
#include "stdint.h"
#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip_route.h"
#include "lwip/app/dhcpserver.h"
#include "lwip/app/espconn.h"
#include "lwip/app/espconn_tcp.h"
#include "user_interface.h"
#include "string.h"
#include "user_config.h"
#include "config_flash.h"
#include "sys_time.h"
#include "web_ui.h"
#include "web_page.h"

#if WEB_CONFIG

extern sysconfig_t config;
extern uint8_t web_last_disc_reason;
extern uint64_t Bytes_in, Bytes_out;
extern void user_set_station_config(void);

#define WEB_SLOTS 4
#define WEB_CHUNK 1000
#define WEB_SCAN_MAX 12

#define JSON_HDR "HTTP/1.0 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"

typedef struct
{
    struct espconn *conn;
    char *buf;            /* RAM reply, or RAM chunk buffer when fl != NULL */
    const uint8_t *fl;    /* optional: 4-byte aligned source in flash */
    uint16_t len;
    uint16_t off;
} web_tx_t;

typedef struct
{
    char ssid[33];
    sint8 rssi;
    uint8 ch;
    uint8 auth;
} web_scan_t;

/* the dashboard page stays in flash and is streamed in chunks (saves ~7 KB RAM) */
static const uint8_t web_page_str[] ICACHE_RODATA_ATTR STORE_ATTR = WEB_PAGE;

static web_tx_t web_tx[WEB_SLOTS];
static web_scan_t web_scan[WEB_SCAN_MAX];
static uint8_t web_scan_cnt;
static bool web_scan_busy;
static uint32_t web_scan_start;
static os_timer_t web_timer;
static uint8_t web_action;

/* ------------------------------------------------------------------ */
/* Chunked transmit                                                   */
/* ------------------------------------------------------------------ */

static void ICACHE_FLASH_ATTR web_tx_free(struct espconn *c)
{
    int i;
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (web_tx[i].conn == c && web_tx[i].buf != NULL)
        {
            os_free(web_tx[i].buf);
            web_tx[i].buf = NULL;
            web_tx[i].conn = NULL;
        }
    }
}

static void ICACHE_FLASH_ATTR web_tx_next(struct espconn *c)
{
    int i;
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (web_tx[i].conn == c && web_tx[i].buf != NULL)
        {
            uint16_t left = web_tx[i].len - web_tx[i].off;
            uint16_t n, off;
            if (left == 0)
            {
                web_tx_free(c);
                espconn_disconnect(c);
                return;
            }
            n = left > WEB_CHUNK ? WEB_CHUNK : left;
            off = web_tx[i].off;
            web_tx[i].off += n;
            if (web_tx[i].fl != NULL)
            {
                /* WEB_CHUNK is a multiple of 4: flash reads stay word aligned */
                {
                    /* flash must be read with aligned 32-bit accesses */
                    const uint32_t *src = (const uint32_t *)(web_tx[i].fl + off);
                    uint32_t *dst = (uint32_t *)web_tx[i].buf;
                    uint16_t w, words = (n + 3) / 4;
                    for (w = 0; w < words; w++)
                        dst[w] = src[w];
                }
                espconn_send(c, (uint8_t *)web_tx[i].buf, n);
            }
            else
            {
                espconn_send(c, (uint8_t *)(web_tx[i].buf + off), n);
            }
            return;
        }
    }
    espconn_disconnect(c);
}

/* takes ownership of buf (must be os_malloc'ed) */
static void ICACHE_FLASH_ATTR web_send(struct espconn *c, char *buf, uint16_t len)
{
    int i;
    web_tx_free(c);
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (web_tx[i].buf == NULL)
        {
            web_tx[i].conn = c;
            web_tx[i].buf = buf;
            web_tx[i].fl = NULL;
            web_tx[i].len = len;
            web_tx[i].off = 0;
            web_tx_next(c);
            return;
        }
    }
    os_free(buf);
    espconn_disconnect(c);
}

static void ICACHE_FLASH_ATTR web_send_flash(struct espconn *c, const uint8_t *fl, uint16_t len)
{
    int i;
    char *chunk = (char *)os_malloc(WEB_CHUNK + 4);
    if (chunk == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    web_tx_free(c);
    for (i = 0; i < WEB_SLOTS; i++)
    {
        if (web_tx[i].buf == NULL)
        {
            web_tx[i].conn = c;
            web_tx[i].buf = chunk;
            web_tx[i].fl = fl;
            web_tx[i].len = len;
            web_tx[i].off = 0;
            web_tx_next(c);
            return;
        }
    }
    os_free(chunk);
    espconn_disconnect(c);
}

static char *ICACHE_FLASH_ATTR web_json_alloc(uint16_t size)
{
    char *b = (char *)os_malloc(size);
    if (b != NULL)
        os_sprintf(b, JSON_HDR);
    return b;
}

static void ICACHE_FLASH_ATTR web_reply_text(struct espconn *c, const char *status)
{
    char *b = (char *)os_malloc(120);
    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    os_sprintf(b, "HTTP/1.0 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    web_send(c, b, (uint16_t)os_strlen(b));
}

static void ICACHE_FLASH_ATTR web_reply_result(struct espconn *c, int ok, int reboot, const char *msg)
{
    int n;
    char *b = web_json_alloc(320);
    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    n = os_strlen(b);
    os_sprintf(b + n, "{\"ok\":%d,\"reboot\":%d,\"msg\":\"%s\"}", ok, reboot, msg);
    web_send(c, b, (uint16_t)os_strlen(b));
}

/* JSON-escape up to max bytes of src (NUL terminated or max long) into dst */
static int ICACHE_FLASH_ATTR web_jesc(char *dst, const uint8_t *src, int max)
{
    int n = 0, i;
    for (i = 0; i < max && src[i] != 0; i++)
    {
        if (src[i] < 0x20)
            continue;
        if (src[i] == '"' || src[i] == '\\')
            dst[n++] = '\\';
        dst[n++] = (char)src[i];
    }
    dst[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* Query string helpers                                               */
/* ------------------------------------------------------------------ */

static int ICACHE_FLASH_ATTR web_hex(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* Returns 0 if key is absent, otherwise (decoded length + 1).
 * The decoded value is copied to out (truncated to outsz-1 chars). */
static int ICACHE_FLASH_ATTR web_qget(const char *q, const char *key, char *out, int outsz)
{
    int klen = os_strlen(key);
    const char *p = q;

    while (*p)
    {
        if (os_strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            const char *v = p + klen + 1;
            int n = 0, stored = 0;
            while (*v && *v != '&')
            {
                char ch = *v++;
                if (ch == '+')
                {
                    ch = ' ';
                }
                else if (ch == '%' && web_hex(v[0]) >= 0 && web_hex(v[1]) >= 0)
                {
                    ch = (char)(web_hex(v[0]) * 16 + web_hex(v[1]));
                    v += 2;
                }
                if (ch == 0)
                    continue;
                n++;
                if (stored < outsz - 1)
                    out[stored++] = ch;
            }
            out[stored] = 0;
            return n + 1;
        }
        while (*p && *p != '&')
            p++;
        if (*p == '&')
            p++;
    }
    out[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Deferred actions (so the HTTP reply leaves before we restart)      */
/* ------------------------------------------------------------------ */

static void ICACHE_FLASH_ATTR web_timer_cb(void *arg)
{
    uint8_t a = web_action;
    web_action = 0;
    if (a == 1)
    {
        system_restart();
    }
    else if (a == 2)
    {
        user_set_station_config();
        wifi_station_disconnect();
        wifi_station_connect();
    }
}

static void ICACHE_FLASH_ATTR web_defer(uint8_t action, uint32_t ms)
{
    web_action = action;
    os_timer_disarm(&web_timer);
    os_timer_setfn(&web_timer, (os_timer_func_t *)web_timer_cb, NULL);
    os_timer_arm(&web_timer, ms, 0);
}

/* ------------------------------------------------------------------ */
/* Handlers                                                           */
/* ------------------------------------------------------------------ */

static void ICACHE_FLASH_ATTR web_page_send(struct espconn *c)
{
    web_send_flash(c, web_page_str, (uint16_t)(sizeof(web_page_str) - 1));
}

static void ICACHE_FLASH_ATTR web_status(struct espconn *c)
{
    int n, up, rssi = 0;
    uint32_t ip, gw;
    struct ip_info ipi;
    char *b = web_json_alloc(1200);

    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    if (config.locked)
    {
        n = os_strlen(b);
        os_sprintf(b + n, "{\"locked\":1}");
        web_send(c, b, (uint16_t)os_strlen(b));
        return;
    }

    up = (wifi_station_get_connect_status() == STATION_GOT_IP) ? 1 : 0;
    if (up)
    {
        rssi = wifi_station_get_rssi();
        if (rssi >= 0)
            rssi = -100; /* 31 = "unknown" */
    }
    os_memset(&ipi, 0, sizeof(ipi));
    wifi_get_ip_info(STATION_IF, &ipi);
    ip = ipi.ip.addr;
    gw = ipi.gw.addr;

    n = os_strlen(b);
    os_sprintf(b + n, "{\"locked\":0,\"conn\":%d,\"dr\":%d,\"rssi\":%d,\"ch\":%d,\"ssid\":\"", up, up ? 0 : web_last_disc_reason, rssi, wifi_get_channel());
    n = os_strlen(b);
    n += web_jesc(b + n, config.ssid, 32);
    os_sprintf(b + n, "\",\"pw\":\"");
    n = os_strlen(b);
    n += web_jesc(b + n, config.password, 64);
    os_sprintf(b + n, "\",\"ap_ssid\":\"");
    n = os_strlen(b);
    n += web_jesc(b + n, config.ap_ssid, 32);
    os_sprintf(b + n, "\",\"ap_pw\":\"");
    n = os_strlen(b);
    n += web_jesc(b + n, config.ap_password, 64);
    os_sprintf(b + n, "\",\"ap_open\":%d,\"ip\":\"%d.%d.%d.%d\",\"gw\":\"%d.%d.%d.%d\",\"clients\":%d,",
               config.ap_open ? 1 : 0,
               (int)(ip & 0xff), (int)((ip >> 8) & 0xff), (int)((ip >> 16) & 0xff), (int)((ip >> 24) & 0xff),
               (int)(gw & 0xff), (int)((gw >> 8) & 0xff), (int)((gw >> 16) & 0xff), (int)((gw >> 24) & 0xff),
               (int)wifi_softap_get_station_num());
    n = os_strlen(b);
    os_sprintf(b + n, "\"up\":%d,\"heap\":%d,\"rx\":%d,\"tx\":%d,\"ver\":\"%s\"}",
               (int)(get_long_systime() / 1000000), (int)system_get_free_heap_size(),
               (int)(Bytes_in / 1024), (int)(Bytes_out / 1024), ESP_REPEATER_VERSION);
    web_send(c, b, (uint16_t)os_strlen(b));
}

static void ICACHE_FLASH_ATTR web_scan_done(void *arg, STATUS status)
{
    struct bss_info *bss = (struct bss_info *)arg;
    int i, j, len;

    web_scan_cnt = 0;
    if (status == OK)
    {
        while (bss != NULL)
        {
            len = bss->ssid_len;
            if (len > 32)
                len = 32;
            if (len > 0 && bss->ssid[0] != 0)
            {
                int found = -1;
                char name[33];
                os_memcpy(name, bss->ssid, len);
                name[len] = 0;
                for (i = 0; i < web_scan_cnt; i++)
                {
                    if (os_strcmp(web_scan[i].ssid, name) == 0)
                    {
                        found = i;
                        break;
                    }
                }
                if (found >= 0)
                {
                    if (bss->rssi > web_scan[found].rssi)
                    {
                        web_scan[found].rssi = bss->rssi;
                        web_scan[found].ch = bss->channel;
                        web_scan[found].auth = (bss->authmode != AUTH_OPEN) ? 1 : 0;
                    }
                }
                else if (web_scan_cnt < WEB_SCAN_MAX)
                {
                    os_strcpy(web_scan[web_scan_cnt].ssid, name);
                    web_scan[web_scan_cnt].rssi = bss->rssi;
                    web_scan[web_scan_cnt].ch = bss->channel;
                    web_scan[web_scan_cnt].auth = (bss->authmode != AUTH_OPEN) ? 1 : 0;
                    web_scan_cnt++;
                }
            }
            bss = bss->next.stqe_next;
        }
        /* strongest first */
        for (i = 0; i < web_scan_cnt; i++)
        {
            for (j = i + 1; j < web_scan_cnt; j++)
            {
                if (web_scan[j].rssi > web_scan[i].rssi)
                {
                    web_scan_t t;
                    os_memcpy(&t, &web_scan[i], sizeof(t));
                    os_memcpy(&web_scan[i], &web_scan[j], sizeof(t));
                    os_memcpy(&web_scan[j], &t, sizeof(t));
                }
            }
        }
    }
    web_scan_busy = false;
}

static void ICACHE_FLASH_ATTR web_scan_reply(struct espconn *c, const char *q)
{
    int i, n;
    char tmp[4];
    char *b;

    if (web_scan_busy && (uint32_t)(system_get_time() - web_scan_start) > 15000000UL)
        web_scan_busy = false; /* callback never came: allow a new scan */

    if (web_qget(q, "go", tmp, sizeof(tmp)) && !web_scan_busy)
    {
        web_scan_busy = true;
        web_scan_start = system_get_time();
        if (!wifi_station_scan(NULL, web_scan_done))
            web_scan_busy = false;
    }

    b = web_json_alloc(1900);
    if (b == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    n = os_strlen(b);
    os_sprintf(b + n, "{\"busy\":%d,\"list\":[", web_scan_busy ? 1 : 0);
    for (i = 0; i < web_scan_cnt; i++)
    {
        n = os_strlen(b);
        if (i > 0)
        {
            os_sprintf(b + n, ",");
            n++;
        }
        os_sprintf(b + n, "{\"s\":\"");
        n = os_strlen(b);
        n += web_jesc(b + n, (const uint8_t *)web_scan[i].ssid, 32);
        os_sprintf(b + n, "\",\"r\":%d,\"c\":%d,\"a\":%d}", (int)web_scan[i].rssi, (int)web_scan[i].ch, (int)web_scan[i].auth);
    }
    n = os_strlen(b);
    os_sprintf(b + n, "]}");
    web_send(c, b, (uint16_t)os_strlen(b));
}

static void ICACHE_FLASH_ATTR web_do_save(struct espconn *c, const char *q)
{
    char sta_ssid[40], sta_pw[80], ap_ssid[40], ap_pw[80], sec[16], act[16], lk[4];
    int l_ss, l_sp, l_as, l_ap, l_sec, want_open, eff_len;
    int sta_changed = 0, ap_changed = 0, do_lock = 0;

    l_ss = web_qget(q, "sta_ssid", sta_ssid, sizeof(sta_ssid));
    l_sp = web_qget(q, "sta_pw", sta_pw, sizeof(sta_pw));
    l_as = web_qget(q, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    l_ap = web_qget(q, "ap_pw", ap_pw, sizeof(ap_pw));
    l_sec = web_qget(q, "ap_sec", sec, sizeof(sec));
    web_qget(q, "act", act, sizeof(act));
    do_lock = web_qget(q, "lock", lk, sizeof(lk)) ? 1 : 0;

    /* ---- validate everything first, change nothing on error ---- */
    if (l_ss && (l_ss - 1 < 1 || l_ss - 1 > 31))
    {
        web_reply_result(c, 0, 0, "Uplink SSID must be 1-31 characters");
        return;
    }
    if (l_sp && l_sp - 1 != 0 && (l_sp - 1 < 8 || l_sp - 1 > 63))
    {
        web_reply_result(c, 0, 0, "Uplink password must be 8-63 characters (empty = open network)");
        return;
    }
    if (l_as && (l_as - 1 < 1 || l_as - 1 > 31))
    {
        web_reply_result(c, 0, 0, "AP SSID must be 1-31 characters");
        return;
    }
    if (l_ap && l_ap - 1 > 63)
    {
        web_reply_result(c, 0, 0, "AP password is too long (max 63)");
        return;
    }
    want_open = config.ap_open ? 1 : 0;
    if (l_sec)
        want_open = (os_strcmp(sec, "open") == 0) ? 1 : 0;
    eff_len = l_ap ? (l_ap - 1) : (int)os_strlen((char *)config.ap_password);
    if (!want_open && eff_len < 8)
    {
        web_reply_result(c, 0, 0, "AP password must be 8-63 characters (or choose Open)");
        return;
    }

    /* ---- apply ---- */
    if (l_ss && os_strncmp(sta_ssid, (char *)config.ssid, sizeof(config.ssid)) != 0)
    {
        os_memset(config.ssid, 0, sizeof(config.ssid));
        os_memcpy(config.ssid, sta_ssid, l_ss - 1);
        os_memset(config.bssid, 0, sizeof(config.bssid)); /* a stale BSSID lock would block the new SSID */
        config.automesh_mode = AUTOMESH_OFF;
        sta_changed = 1;
    }
    if (l_sp && os_strncmp(sta_pw, (char *)config.password, sizeof(config.password)) != 0)
    {
        os_memset(config.password, 0, sizeof(config.password));
        os_memcpy(config.password, sta_pw, l_sp - 1);
        if (l_sp - 1 > 0)
        {
            os_memset(config.lock_password, 0, sizeof(config.lock_password));
            os_memcpy(config.lock_password, sta_pw, l_sp - 1);
        }
        sta_changed = 1;
    }
    if (l_as && os_strncmp(ap_ssid, (char *)config.ap_ssid, sizeof(config.ap_ssid)) != 0)
    {
        os_memset(config.ap_ssid, 0, sizeof(config.ap_ssid));
        os_memcpy(config.ap_ssid, ap_ssid, l_as - 1);
        ap_changed = 1;
    }
    if (l_ap && os_strncmp(ap_pw, (char *)config.ap_password, sizeof(config.ap_password)) != 0)
    {
        os_memset(config.ap_password, 0, sizeof(config.ap_password));
        os_memcpy(config.ap_password, ap_pw, l_ap - 1);
        ap_changed = 1;
    }
    if ((config.ap_open ? 1 : 0) != want_open)
    {
        config.ap_open = want_open;
        ap_changed = 1;
    }
    if (do_lock)
    {
        os_memcpy(config.lock_password, config.password, sizeof(config.lock_password));
        config.locked = 1;
    }
    config_save(&config);

    if (os_strcmp(act, "reboot") == 0)
    {
        web_reply_result(c, 1, 1, "Saved. Restarting...");
        web_defer(1, 1200);
    }
    else if (os_strcmp(act, "connect") == 0)
    {
        web_reply_result(c, 1, 0, ap_changed ? "Saved. Connecting... (AP changes need a restart)" : "Saved. Connecting to uplink...");
        web_defer(2, 800);
    }
    else
    {
        web_reply_result(c, 1, 0, ap_changed ? "Saved. Restart to apply AP changes" : (sta_changed ? "Saved. Use Save & Connect to apply" : "Saved"));
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                       */
/* ------------------------------------------------------------------ */

void ICACHE_FLASH_ATTR web_ui_recv(void *arg, char *data, unsigned short length)
{
    struct espconn *c = (struct espconn *)arg;
    char *req, *q;
    int i = 4, n = 0;

    if (length < 6 || os_strncmp(data, "GET ", 4) != 0)
    {
        espconn_disconnect(c);
        return;
    }
    req = (char *)os_malloc(length + 1);
    if (req == NULL)
    {
        espconn_disconnect(c);
        return;
    }
    while (i < length && data[i] != ' ' && data[i] != '\r' && data[i] != '\n')
        req[n++] = data[i++];
    req[n] = 0;
    if (i >= length || data[i] != ' ')
    {
        os_free(req);
        web_reply_text(c, "400 Bad Request");
        return;
    }

    q = req;
    while (*q && *q != '?')
        q++;
    if (*q == '?')
    {
        *q = 0;
        q++;
    }

    if (os_strcmp(req, "/") == 0 || os_strcmp(req, "/index.html") == 0)
    {
        web_page_send(c);
    }
    else if (os_strcmp(req, "/status") == 0)
    {
        web_status(c);
    }
    else if (os_strcmp(req, "/unlock") == 0)
    {
        char pw[72];
        int l = web_qget(q, "pw", pw, sizeof(pw));
        if (l && os_strcmp(pw, (char *)config.lock_password) == 0)
        {
            config.locked = 0;
            config_save(&config);
            web_reply_result(c, 1, 0, "Unlocked");
        }
        else
        {
            web_reply_result(c, 0, 0, "Wrong password");
        }
    }
    else if (config.locked && (os_strcmp(req, "/save") == 0 || os_strcmp(req, "/scan") == 0 ||
                               os_strcmp(req, "/reboot") == 0 || os_strcmp(req, "/factory") == 0))
    {
        web_reply_result(c, 0, 0, "Config is locked");
    }
    else if (os_strcmp(req, "/save") == 0)
    {
        web_do_save(c, q);
    }
    else if (os_strcmp(req, "/scan") == 0)
    {
        web_scan_reply(c, q);
    }
    else if (os_strcmp(req, "/reboot") == 0)
    {
        web_reply_result(c, 1, 1, "Restarting...");
        web_defer(1, 1200);
    }
    else if (os_strcmp(req, "/factory") == 0)
    {
        config_load_default(&config);
        config_save(&config);
        web_reply_result(c, 1, 1, "Factory reset done. Restarting...");
        web_defer(1, 1200);
    }
    else
    {
        web_reply_text(c, "404 Not Found");
    }
    os_free(req);
}

void ICACHE_FLASH_ATTR web_ui_sent(void *arg)
{
    web_tx_next((struct espconn *)arg);
}

void ICACHE_FLASH_ATTR web_ui_discon(void *arg)
{
    web_tx_free((struct espconn *)arg);
}

#endif /* WEB_CONFIG */
