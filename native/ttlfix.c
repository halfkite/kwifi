/*
 * ttlfix - RedianWiFi data plane helper (WinDivert 2.2).
 *
 * Everything that has to look at individual packets happens here:
 *   1. TTL normalisation   - rewrite outbound IPv4 TTL so tethered clients look
 *                            like the host (Connectify's SMSetTtlUp/DownEnabled).
 *   2. Per-client accounting - byte counters, exposed as JSON every second.
 *   3. Per-client shaping  - token bucket per client and direction.
 *   4. Priority gating     - when the aggregate budget is exhausted, only the
 *                            best priority class keeps flowing (smaller = better).
 *   5. Blocking            - drop all traffic to/from a client.
 *
 * Config is a line based file rewritten by the Node server:
 *     total=120000                 aggregate budget, kbit/s, 0 = no gating
 *     ttl=128                      TTL value to write
 *     enabled=1                    TTL rewriting on/off (helper still runs)
 *     hostprio=3                   priority class of the host's own traffic
 *     hostip=10.74.139.5           the host's uplink address
 *     limit 192.168.137.5 up=512 down=2048 prio=2
 *     block 192.168.137.9
 *
 * Build:
 *   zig cc -target x86_64-windows-gnu -O2 -I include -o ttlfix.exe ttlfix.c WinDivert.lib
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "windivert.h"

#define MAXBUF        0xFFFF
#define MAX_CLIENTS   64
#define RELOAD_MS     1000
#define STATS_MS      1000
#define MAX_PRIO      5
#define DEFAULT_PRIO  3

typedef struct {
    uint32_t ip;          /* host byte order */
    uint64_t up_bytes;
    uint64_t down_bytes;
    double   up_tokens;
    double   down_tokens;
    double   up_rate;     /* bytes per second, 0 = unlimited */
    double   down_rate;
    int      prio;        /* 1 = highest ... 5 = lowest */
    int      blocked;
    uint64_t drops;       /* packets dropped for this client (block / shaping) */
    DWORD    last_ms;
    int      used;
} client_t;

static client_t g_clients[MAX_CLIENTS];
static volatile LONG g_stop = 0;

static unsigned long long g_packets = 0;
static unsigned long long g_rewritten = 0;
static unsigned long long g_dropped = 0;
static unsigned long long g_prio_drops = 0;

static int    g_enabled = 0;
static int    g_ttl = 128;
static int    g_host_prio = DEFAULT_PRIO;
static uint32_t g_host_ip = 0;

/* aggregate budget: bytes per second, 0 = unlimited */
static double g_total_bps = 0;
static double g_up_tokens = 0;
static double g_down_tokens = 0;
static DWORD  g_budget_ms = 0;
static int    g_best_up = MAX_PRIO;
static int    g_best_down = MAX_PRIO;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}

static DWORD now_ms(void) { return (DWORD)GetTickCount64(); }

/* the hotspot subnet is not always 192.168.137.0/24 - the server sends the real
   one, otherwise a rule on another subnet would silently match nothing */
#define MAX_SUBNET 8
static uint32_t g_subnet[MAX_SUBNET];
static uint32_t g_subnet_mask[MAX_SUBNET];
static int      g_subnet_n = 0;

static void subnet_reset(void)
{
    g_subnet_n = 0;
    g_subnet[0] = 0xC0A88900u;      /* 192.168.137.0/24 */
    g_subnet_mask[0] = 0xFFFFFF00u;
    g_subnet_n = 1;
}

static void subnet_add(uint32_t base, uint32_t mask)
{
    for (int i = 0; i < g_subnet_n; i++) {
        if (g_subnet[i] == (base & mask) && g_subnet_mask[i] == mask) return;
    }
    if (g_subnet_n < MAX_SUBNET) {
        g_subnet[g_subnet_n] = base & mask;
        g_subnet_mask[g_subnet_n] = mask;
        g_subnet_n++;
    }
}

/* Blocking by address only covers IPv4. A tethered phone browses over IPv6
   just as happily, and that traffic used to sail straight through. The Ethernet
   header carries the client MAC for every protocol, so matching on it blocks
   everything the client sends or receives. */
#define MAX_BLOCKMAC 64
static unsigned char g_blockmac[MAX_BLOCKMAC][6];
static int g_blockmac_n = 0;

static void blockmac_reset(void) { g_blockmac_n = 0; }

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int mac_parse(const char *s, unsigned char out[6])
{
    for (int i = 0; i < 6; i++) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
        s += 2;
        if (i < 5) {
            if (*s == ':' || *s == '-') s++;
            else return 0;
        }
    }
    return 1;
}

static void blockmac_add(const unsigned char mac[6])
{
    for (int i = 0; i < g_blockmac_n; i++) {
        if (memcmp(g_blockmac[i], mac, 6) == 0) return;
    }
    if (g_blockmac_n < MAX_BLOCKMAC) {
        memcpy(g_blockmac[g_blockmac_n], mac, 6);
        g_blockmac_n++;
    }
}

static int mac_blocked(const unsigned char mac[6])
{
    for (int i = 0; i < g_blockmac_n; i++) {
        if (memcmp(g_blockmac[i], mac, 6) == 0) return 1;
    }
    return 0;
}

/* Ethernet II / VLAN tagged / ARP / IPv4 / IPv6 all start with the MAC pair */
static int eth_macs(const unsigned char *p, UINT len,
                    const unsigned char **dst, const unsigned char **src)
{
    if (len < 14) return 0;
    unsigned int et = ((unsigned int)p[12] << 8) | p[13];
    if (et == 0x0800 || et == 0x86DD || et == 0x0806 ||
        et == 0x8100 || et == 0x88A8) {
        *dst = p;
        *src = p + 6;
        return 1;
    }
    return 0;
}

/* one place decides whether a frame belongs to a blocked client; the run loop
   and --selftest both call this so the test cannot drift from the real path */
static int blocked_frame(const unsigned char *p, UINT len)
{
    if (g_blockmac_n == 0) return 0;
    const unsigned char *sm = NULL, *dm = NULL;
    if (!eth_macs(p, len, &dm, &sm)) return 0;
    return (sm && mac_blocked(sm)) || (dm && mac_blocked(dm));
}

static int is_client_ip(uint32_t ip)
{
    for (int i = 0; i < g_subnet_n; i++) {
        if ((ip & g_subnet_mask[i]) == g_subnet[i]) return 1;
    }
    return 0;
}

/* WinDivert hands over the whole frame: Ethernet (possibly 802.1Q tagged) on
   real adapters, or a bare IP packet on some virtual ones. Every offset below
   used to assume the IP header starts at packet[0], which is wrong for Ethernet
   - so nothing matched and no rule was ever enforced. */
static int ipv4_offset(const unsigned char *p, UINT len)
{
    if (len >= 14) {
        unsigned int et = ((unsigned int)p[12] << 8) | p[13];
        if (et == 0x0800) return 14;                    /* Ethernet II */
        if (et == 0x8100 || et == 0x88A8) {             /* 802.1Q / QinQ */
            if (len >= 18 && ((((unsigned int)p[16] << 8) | p[17]) == 0x0800)) return 18;
        }
    }
    if (len >= 20 && (p[0] >> 4) == 4) return 0;        /* bare IPv4 */
    return -1;
}
static client_t *client_for(uint32_t ip, int create)
{
    client_t *free_slot = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].ip == ip) return &g_clients[i];
        if (!g_clients[i].used && free_slot == NULL) free_slot = &g_clients[i];
    }
    if (!create) return NULL;
    if (free_slot == NULL) {
        client_t *oldest = &g_clients[0];
        for (int i = 1; i < MAX_CLIENTS; i++) {
            if (g_clients[i].last_ms < oldest->last_ms) oldest = &g_clients[i];
        }
        free_slot = oldest;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->ip = ip;
    free_slot->used = 1;
    free_slot->prio = DEFAULT_PRIO;
    free_slot->last_ms = now_ms();
    return free_slot;
}

static void refill_client(client_t *c, DWORD now)
{
    if (c->last_ms == 0) c->last_ms = now;
    double dt = (double)(now - c->last_ms) / 1000.0;
    if (dt <= 0) return;
    c->last_ms = now;

    double cap;
    if (c->up_rate > 0) {
        cap = c->up_rate * 0.25;
        if (cap < 4096) cap = 4096;
        c->up_tokens += c->up_rate * dt;
        if (c->up_tokens > cap) c->up_tokens = cap;
    }
    if (c->down_rate > 0) {
        cap = c->down_rate * 0.25;
        if (cap < 4096) cap = 4096;
        c->down_tokens += c->down_rate * dt;
        if (c->down_tokens > cap) c->down_tokens = cap;
    }
}

static void refill_budget(DWORD now)
{
    if (g_total_bps <= 0) return;
    if (g_budget_ms == 0) g_budget_ms = now;
    double dt = (double)(now - g_budget_ms) / 1000.0;
    if (dt <= 0) return;
    g_budget_ms = now;

    double cap = g_total_bps * 0.1;      /* 100 ms burst */
    if (cap < 16384) cap = 16384;

    g_up_tokens += g_total_bps * dt;
    if (g_up_tokens > cap) g_up_tokens = cap;
    g_down_tokens += g_total_bps * dt;
    if (g_down_tokens > cap) g_down_tokens = cap;

    if (g_up_tokens > 0) g_best_up = MAX_PRIO;
    if (g_down_tokens > 0) g_best_down = MAX_PRIO;
}

/*
 * Strict priority over the aggregate budget: once the shared budget runs out,
 * packets from the best class seen in this window still pass, worse classes are
 * dropped. With an idle link every class passes.
 */
static int budget_allows(int upload, int prio, size_t bytes)
{
    if (g_total_bps <= 0) return 1;

    double *tokens = upload ? &g_up_tokens : &g_down_tokens;
    int *best = upload ? &g_best_up : &g_best_down;

    *tokens -= (double)bytes;
    if (*tokens >= 0) {
        *best = MAX_PRIO;
        return 1;
    }
    if (prio < *best) *best = prio;
    return (prio <= *best);
}

/* returns 1 to keep the packet, 0 to drop it */
static int account(client_t *c, int upload, size_t bytes)
{
    DWORD now = now_ms();
    refill_client(c, now);

    if (upload) {
        c->up_bytes += bytes;
        if (c->up_rate > 0) {
            c->up_tokens -= (double)bytes;
            if (c->up_tokens < 0) { c->up_tokens = 0; return 0; }
        }
    } else {
        c->down_bytes += bytes;
        if (c->down_rate > 0) {
            c->down_tokens -= (double)bytes;
            if (c->down_tokens < 0) { c->down_tokens = 0; return 0; }
        }
    }
    return 1;
}

static void load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    int enabled = g_enabled;
    int ttl = g_ttl;
    int host_prio = g_host_prio;
    uint32_t host_ip = g_host_ip;
    double total_kbps = g_total_bps > 0 ? g_total_bps * 8.0 / 1000.0 : 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        g_clients[i].up_rate = 0;
        g_clients[i].down_rate = 0;
        g_clients[i].blocked = 0;
        g_clients[i].prio = DEFAULT_PRIO;
    }

    subnet_reset();
    blockmac_reset();

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0) continue;

        if (strncmp(p, "enabled=", 8) == 0) {
            enabled = atoi(p + 8) ? 1 : 0;
        } else if (strncmp(p, "ttl=", 4) == 0) {
            int v = atoi(p + 4);
            if (v >= 1 && v <= 255) ttl = v;
        } else if (strncmp(p, "hostprio=", 9) == 0) {
            int v = atoi(p + 9);
            if (v >= 1 && v <= MAX_PRIO) host_prio = v;
        } else if (strncmp(p, "hostip=", 7) == 0) {
            unsigned a, b, c, d;
            if (sscanf(p + 7, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                host_ip = (a << 24) | (b << 16) | (c << 8) | d;
            }
        } else if (strncmp(p, "total=", 6) == 0) {
            double v = atof(p + 6);
            total_kbps = (v > 0) ? v : 0;
        } else if (strncmp(p, "limit ", 6) == 0) {
            unsigned a, b, c, d, up = 0, down = 0, prio = DEFAULT_PRIO;
            int n = sscanf(p + 6, "%u.%u.%u.%u up=%u down=%u prio=%u", &a, &b, &c, &d, &up, &down, &prio);
            if (n >= 6) {
                uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
                client_t *cl = client_for(ip, 1);
                if (cl) {
                    cl->up_rate = (double)up * 1000.0 / 8.0;
                    cl->down_rate = (double)down * 1000.0 / 8.0;
                    if (n >= 7 && prio >= 1 && prio <= MAX_PRIO) cl->prio = (int)prio;
                }
            }
        } else if (strncmp(p, "subnet", 6) == 0) {
            unsigned sa = 0, sb = 0, sc2 = 0, sd = 0, bits = 24;
            char *q = p + 6;
            while (*q == ' ' || *q == '\t') q++;
            int got = sscanf(q, "%u.%u.%u.%u/%u", &sa, &sb, &sc2, &sd, &bits);
            if (got < 4) {
                bits = 24;
                got = sscanf(q, "%u.%u.%u.%u", &sa, &sb, &sc2, &sd);
            }
            if (got >= 4) {
                if (bits > 32) bits = 32;
                uint32_t base = ((uint32_t)sa << 24) | ((uint32_t)sb << 16) |
                                ((uint32_t)sc2 << 8) | (uint32_t)sd;
                uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - bits));
                subnet_add(base, mask);
            }
        } else if (strncmp(p, "blockmac ", 9) == 0) {
            unsigned char mac[6];
            char *q = p + 9;
            while (*q == ' ' || *q == '\t') q++;
            if (mac_parse(q, mac)) blockmac_add(mac);
        } else if (strncmp(p, "block ", 6) == 0) {
            unsigned a, b, c, d;
            if (sscanf(p + 6, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
                client_t *cl = client_for(ip, 1);
                if (cl) cl->blocked = 1;
            }
        }
    }
    fclose(f);

    g_enabled = enabled;
    g_ttl = ttl;
    g_host_prio = host_prio;
    g_host_ip = host_ip;

    double new_bps = total_kbps * 1000.0 / 8.0;
    if (new_bps != g_total_bps) {
        g_total_bps = new_bps;
        g_up_tokens = 0;
        g_down_tokens = 0;
        g_budget_ms = 0;
    }
}

static void write_stats(const char *path)
{
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"ts\":%llu,\"packets\":%llu,\"rewritten\":%llu,\"dropped\":%llu,\"prioDrops\":%llu,\"budget\":%.0f,\"clients\":{",
            (unsigned long long)time(NULL), g_packets, g_rewritten, g_dropped, g_prio_drops, g_total_bps);
    int first = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        uint32_t ip = g_clients[i].ip;
        fprintf(f, "%s\"%u.%u.%u.%u\":{\"up\":%llu,\"down\":%llu,\"prio\":%d,\"blocked\":%d,\"drops\":%llu}",
                first ? "" : ",", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                (unsigned long long)g_clients[i].up_bytes,
                (unsigned long long)g_clients[i].down_bytes,
                g_clients[i].prio, g_clients[i].blocked,
                (unsigned long long)g_clients[i].drops);
        first = 0;
    }
    fprintf(f, "}}");
    fclose(f);
    remove(path);
    rename(tmp, path);
}

/* --selftest: prove the frame parser before trusting it with real traffic.
   Runs without administrator rights, so it can be checked anywhere. */
static int selftest(void)
{
    unsigned char eth[14 + 20];
    unsigned char vlan[18 + 20];
    unsigned char raw[20];
    int bad = 0;

    memset(eth, 0, sizeof(eth));
    eth[0] = 0xFC; eth[1] = 0x43; eth[2] = 0x45; eth[3] = 0xAB; eth[4] = 0x79; eth[5] = 0x48;
    eth[6] = 0x00; eth[7] = 0x11; eth[8] = 0x22; eth[9] = 0x33; eth[10] = 0x44; eth[11] = 0x55;
    eth[12] = 0x08; eth[13] = 0x00;                 /* IPv4 */
    eth[14] = 0x45; eth[22] = 64;                   /* version/IHL, ttl */
    eth[14 + 12] = 192; eth[14 + 13] = 168; eth[14 + 14] = 137; eth[14 + 15] = 50;
    eth[14 + 16] = 8;   eth[14 + 17] = 8;   eth[14 + 18] = 8;   eth[14 + 19] = 8;

    memset(vlan, 0, sizeof(vlan));
    memcpy(vlan, eth, 12);
    vlan[12] = 0x81; vlan[13] = 0x00;               /* 802.1Q */
    vlan[14] = 0x00; vlan[15] = 0x64;
    vlan[16] = 0x08; vlan[17] = 0x00;
    memcpy(vlan + 18, eth + 14, 20);

    memset(raw, 0, sizeof(raw));
    raw[0] = 0x45; raw[8] = 64;
    raw[12] = 192; raw[13] = 168; raw[14] = 137; raw[15] = 9;
    raw[16] = 1;   raw[17] = 1;   raw[18] = 1;   raw[19] = 1;

    struct { const char *name; const unsigned char *buf; UINT len; int off; UINT32 src; } cases[] = {
        { "ethernet",        eth,  sizeof(eth),  14, 0xC0A88932u },
        { "802.1q",          vlan, sizeof(vlan), 18, 0xC0A88932u },
        { "bare ip",         raw,  sizeof(raw),   0, 0xC0A88909u },
    };

    for (int i = 0; i < 3; i++) {
        int off = ipv4_offset(cases[i].buf, cases[i].len);
        UINT32 src = 0;
        if (off >= 0) {
            const unsigned char *ip = cases[i].buf + off;
            src = ((UINT32)ip[12] << 24) | ((UINT32)ip[13] << 16) |
                  ((UINT32)ip[14] << 8) | ip[15];
        }
        int ok = (off == cases[i].off) && (src == cases[i].src);
        if (!ok) bad++;
        printf("  %-9s offset=%d src=%u.%u.%u.%u  %s\n", cases[i].name, off,
               (src >> 24) & 0xFF, (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
               ok ? "ok" : "FAIL");
    }

    /* an Ethernet frame must not be mistaken for a bare IP packet */
    if (ipv4_offset(eth, sizeof(eth)) == 0) {
        printf("  ethernet misdetected as bare ip  FAIL\n");
        bad++;
    }

    /* when a config was loaded, show what the rules resolved to */
    if (g_subnet_n > 0) {
        printf("  subnets  :");
        for (int i = 0; i < g_subnet_n; i++) {
            printf(" %u.%u.%u.%u/%u",
                   (g_subnet[i] >> 24) & 0xFF, (g_subnet[i] >> 16) & 0xFF,
                   (g_subnet[i] >> 8) & 0xFF, g_subnet[i] & 0xFF,
                   g_subnet_mask[i] == 0xFFFFFFFFu ? 32 : 24);
        }
        printf("\n");
    }
    int blocked = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        if (g_clients[i].blocked) {
            printf("  blocked  : %u.%u.%u.%u\n",
                   (g_clients[i].ip >> 24) & 0xFF, (g_clients[i].ip >> 16) & 0xFF,
                   (g_clients[i].ip >> 8) & 0xFF, g_clients[i].ip & 0xFF);
            blocked++;
        }
    }
    if (!blocked) printf("  blocked  : (none)\n");

    /* prove the MAC rule actually rejects a frame from the blocked client */
    if (g_blockmac_n > 0) {
        int dropped = blocked_frame(eth, sizeof(eth));
        printf("  blockmac : %d rule(s), frame to/from client %s\n",
               g_blockmac_n, dropped ? "DROPPED" : "passed  FAIL");
        if (!dropped) bad++;
    }

    printf("%s\n", bad ? "parse self test FAILED" : "parse self test passed");
    return bad ? 1 : 0;
}
int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *stats_path = NULL;
    const char *filter = "ip and not loopback";
    int probe = 0;
    int selftest_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) stats_path = argv[++i];
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (strcmp(argv[i], "--probe") == 0) probe = 1;
        else if (strcmp(argv[i], "--selftest") == 0) selftest_only = 1;
    }

    if (selftest_only) {
        if (config_path) load_config(config_path);
        return selftest();
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (config_path) load_config(config_path);

    HANDLE wd = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
    if (wd == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr, "windivert open failed: %lu (administrator rights and the signed driver are required)\n", err);
        return 3;
    }

    if (probe) {
        printf("ok\n");
        WinDivertClose(wd);
        return 0;
    }

    WinDivertSetParam(wd, WINDIVERT_PARAM_QUEUE_LENGTH, 8192);
    WinDivertSetParam(wd, WINDIVERT_PARAM_QUEUE_TIME, 2000);

    unsigned char packet[MAXBUF];
    UINT packet_len = 0;
    WINDIVERT_ADDRESS addr;
    DWORD last_reload = now_ms();
    DWORD last_stats = now_ms();

    printf("ttlfix running: ttl=%d enabled=%d hostprio=%d total=%.0f B/s\n", g_ttl, g_enabled, g_host_prio, g_total_bps);
    fflush(stdout);

    while (!g_stop) {
        if (!WinDivertRecv(wd, packet, sizeof(packet), &packet_len, &addr)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) continue;
            fprintf(stderr, "recv failed: %lu\n", err);
            break;
        }

        g_packets++;
        int keep = 1;

        /* MAC first: covers IPv4, IPv6 and anything else the client sends or
           receives, and survives a DHCP address change */
        if (blocked_frame(packet, packet_len)) {
            g_dropped++;
            DWORD mac_now = now_ms();
            refill_budget(mac_now);
            if (config_path && mac_now - last_reload >= RELOAD_MS) {
                last_reload = mac_now;
                load_config(config_path);
            }
            continue;
        }

        int off = addr.IPv6 ? -1 : ipv4_offset(packet, packet_len);
        if (off >= 0 && packet_len >= (UINT)(off + 20)) {
            unsigned char *ip = packet + off;
            unsigned char *ttl = &ip[8];
            uint32_t src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
                           ((uint32_t)ip[14] << 8) | ip[15];
            uint32_t dst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) |
                           ((uint32_t)ip[18] << 8) | ip[19];

            int upload = addr.Outbound ? 0 : 1;   /* outbound = towards the client */
            client_t *c = NULL;
            int prio;

            if (upload) {
                if (is_client_ip(src)) c = client_for(src, 1);
            } else {
                if (is_client_ip(dst)) c = client_for(dst, 1);
            }

            if (c && c->blocked) {
                keep = 0;
                g_dropped++;
                c->drops++;
            } else if (c) {
                prio = c->prio;
                if (!account(c, upload, packet_len)) {
                    keep = 0;
                    g_dropped++;
                    c->drops++;
                } else if (!budget_allows(upload, prio, packet_len)) {
                    keep = 0;
                    g_prio_drops++;
                    c->drops++;
                }
            } else {
                /* host's own traffic does not consume a client bucket */
                if (!budget_allows(upload, g_host_prio, packet_len)) {
                    keep = 0;
                    g_prio_drops++;
                }
            }

            /* TTL rewrite only for packets leaving towards the internet */
            if (keep && !upload && addr.Outbound && g_enabled && !is_client_ip(dst) && *ttl != (unsigned char)g_ttl) {
                *ttl = (unsigned char)g_ttl;
                WinDivertHelperCalcChecksums(packet, packet_len, &addr, 0);
                g_rewritten++;
            }
        }

        if (!keep) {
            /* still keep the config fresh: a rule change must apply even while
               traffic is being dropped */
            DWORD reload_now = now_ms();
            refill_budget(reload_now);
            if (config_path && reload_now - last_reload >= RELOAD_MS) {
                last_reload = reload_now;
                load_config(config_path);
            }
            continue;          /* dropped: never re-injected */
        }

        UINT send_len = 0;
        if (!WinDivertSend(wd, packet, packet_len, &send_len, &addr)) {
            fprintf(stderr, "send failed: %lu\n", GetLastError());
        }

        DWORD now = now_ms();
        refill_budget(now);
        if (config_path && now - last_reload >= RELOAD_MS) {
            last_reload = now;
            load_config(config_path);
        }
        if (stats_path && now - last_stats >= STATS_MS) {
            last_stats = now;
            write_stats(stats_path);
        }
    }

    if (stats_path) write_stats(stats_path);
    WinDivertClose(wd);
    printf("ttlfix stopped\n");
    return 0;
}