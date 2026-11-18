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

static int is_client_ip(uint32_t ip)
{
    /* 192.168.137.0/24 - the ICS / hosted-network subnet we serve */
    return (ip & 0xFFFFFF00u) == 0xC0A88900u;
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

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *stats_path = NULL;
    const char *filter = "ip and not loopback";
    int probe = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) stats_path = argv[++i];
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (strcmp(argv[i], "--probe") == 0) probe = 1;
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

        if (!addr.IPv6 && packet_len >= 20 && (packet[0] >> 4) == 4) {
            unsigned char *ttl = &packet[8];
            uint32_t src = ((uint32_t)packet[12] << 24) | ((uint32_t)packet[13] << 16) |
                           ((uint32_t)packet[14] << 8) | packet[15];
            uint32_t dst = ((uint32_t)packet[16] << 24) | ((uint32_t)packet[17] << 16) |
                           ((uint32_t)packet[18] << 8) | packet[19];

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