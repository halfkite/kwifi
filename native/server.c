/*
 * server.c - the kwifi backend.
 *
 * Replaces the former Node.js service: a small HTTP server on 127.0.0.1 that
 * serves the embedded UI, keeps device state and drives ps\net.ps1 (which does
 * the actual Windows work) plus the WinDivert data plane.
 *
 * Everything is embedded in kwifi.exe; only data files are written to disk.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "embedded.h"
#include "version.h"
#include "server.h"

#define MAX_DEV      256
#define MAX_LOG      200
#define POLL_MS      2500
#define MAX_SSE      8

/* ---------------- small helpers ---------------- */

typedef struct {
    char *p;
    size_t len, cap;
} sb;

static void sb_init(sb *b, size_t cap)
{
    b->cap = cap ? cap : 1024;
    b->p = (char *)malloc(b->cap);
    b->len = 0;
    if (b->p) b->p[0] = 0;
}

static void sb_free(sb *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void sb_need(sb *b, size_t extra)
{
    if (!b->p) return;
    if (b->len + extra + 1 <= b->cap) return;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    b->p = (char *)realloc(b->p, b->cap);
}

static void sb_puts(sb *b, const char *s)
{
    if (!b->p || !s) return;
    size_t n = strlen(s);
    sb_need(b, n);
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

static void sb_putc(sb *b, char c)
{
    if (!b->p) return;
    sb_need(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static void sb_putf(sb *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        sb_puts(b, tmp);
        return;
    }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_puts(b, big);
    free(big);
}

static void sb_json_str(sb *b, const char *s)
{
    if (!s) {
        sb_puts(b, "\"\"");
        return;
    }
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) sb_putf(b, "\\u%04x", *p);
            else sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

/* ---------------- tiny JSON scanning ---------------- */

static const char *j_skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* returns pointer just past the value that starts at p */
static const char *j_skip_value(const char *p)
{
    p = j_skip_ws(p);
    if (!p) return NULL;
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '\\' && p[1]) p += 2;
            else if (*p == '"') return p + 1;
            else p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        int instr = 0;
        while (*p) {
            if (instr) {
                if (*p == '\\' && p[1]) p += 2;
                else { if (*p == '"') instr = 0; p++; }
                continue;
            }
            if (*p == '"') instr = 1;
            else if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* find "key" at the top level of the object text; returns value start */
static const char *j_find(const char *obj, const char *key, const char **end)
{
    if (!obj) return NULL;
    const char *p = j_skip_ws(obj);
    if (*p != '{') return NULL;
    p++;
    size_t klen = strlen(key);
    while (*p) {
        p = j_skip_ws(p);
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *ks = p + 1;
        const char *ke = j_skip_value(p);        /* just past the closing quote */
        size_t got = (size_t)((ke - 1) - ks);
        p = j_skip_ws(ke);
        if (*p == ':') p++;
        const char *vs = j_skip_ws(p);
        const char *ve = j_skip_value(vs);
        if (got == klen && _strnicmp(ks, key, klen) == 0) {
            if (end) *end = ve;
            return vs;
        }
        p = j_skip_ws(ve);
        if (*p == ',') p++;
    }
    return NULL;
}

static void j_get_str(const char *obj, const char *key, char *out, size_t cch)
{
    out[0] = 0;
    const char *end = NULL;
    const char *v = j_find(obj, key, &end);
    if (!v || *v != '"') return;
    v++;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < cch) {
        if (*v == '\\' && v[1]) {
            v++;
            if (*v == 'n') out[i++] = '\n';
            else if (*v == 't') out[i++] = '\t';
            else if (*v == 'r') out[i++] = '\r';
            else out[i++] = *v;
            v++;
        } else {
            out[i++] = *v++;
        }
    }
    out[i] = 0;
}

static int j_get_int(const char *obj, const char *key, int dflt)
{
    const char *v = j_find(obj, key, NULL);
    if (!v) return dflt;
    return atoi(v);
}

static double j_get_num(const char *obj, const char *key, double dflt)
{
    const char *v = j_find(obj, key, NULL);
    if (!v) return dflt;
    return atof(v);
}

static int j_get_bool(const char *obj, const char *key, int dflt)
{
    const char *v = j_find(obj, key, NULL);
    if (!v) return dflt;
    if (!_strnicmp(v, "true", 4)) return 1;
    if (!_strnicmp(v, "false", 5)) return 0;
    return dflt;
}

/* copy the raw JSON value of a key (object/array/number) */
static void j_get_raw(const char *obj, const char *key, char *out, size_t cch)
{
    out[0] = 0;
    const char *end = NULL;
    const char *v = j_find(obj, key, &end);
    if (!v || !end) return;
    size_t n = (size_t)(end - v);
    if (n + 1 > cch) n = cch - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

/* ---------------- state ---------------- */

typedef struct {
    char ssid[64];
    char password[64];
    char band[8];
    int autoStart;
    int autoHotspot;      /* start the hotspot automatically after boot */
    int totalMbps;
    int hostPrio;
    int st_ipv6, st_mcast, st_ttl;
} app_config;

typedef struct {
    char mac[32];
    char ip[40];
    char hostname[128];
    char alias[64];
    char note[128];
    char aliasTo[32];
    int prio;
    int blocked;
    int upMbps, downMbps;
    long long firstSeen, lastSeen;
    long long drops;      /* packets the data plane dropped for this client */
    int used;
} device_t;

typedef struct {
    long long t;
    int level;              /* 0 info, 1 warn, 2 error */
    char msg[256];
} logrec;

static struct {
    CRITICAL_SECTION cs;
    app_config cfg;
    device_t dev[MAX_DEV];
    int devCount;
    logrec logs[MAX_LOG];
    int logHead, logCount;

    char psData[8192];      /* raw JSON object from net.ps1 data */
    int admin, active;
    char method[32], state[64], ssid[64], ttlState[24];
    char hnic[1024], unic[1024];
    char clients[4096];     /* raw clients array (ip/mac only) */
    long long inBps, outBps;
    struct { long long t; int in, out; } hist[60];
    int histCount;

    int helperAlive;
    int quit;
} S;

static wchar_t g_rootDir[MAX_PATH];
static wchar_t g_dataDir[MAX_PATH];
static int g_port = 17899;

static void log_add(int level, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&S.cs);
    logrec *r = &S.logs[S.logHead];
    r->t = (long long)time(NULL) * 1000;
    r->level = level;
    lstrcpynA(r->msg, msg, sizeof(r->msg));
    S.logHead = (S.logHead + 1) % MAX_LOG;
    if (S.logCount < MAX_LOG) S.logCount++;
    LeaveCriticalSection(&S.cs);

    /* append to data\server.log */
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\server.log", g_dataDir);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char line[512];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = _snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s: %s\r\n",
                          st.wHour, st.wMinute, st.wSecond,
                          level == 2 ? "error" : level == 1 ? "warn" : "info", msg);
        DWORD w = 0;
        WriteFile(h, line, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
}

/* ---------------- device store ---------------- */

static device_t *dev_find(const char *mac, int create)
{
    if (!mac || !*mac) return NULL;
    for (int i = 0; i < S.devCount; i++) {
        if (_stricmp(S.dev[i].mac, mac) == 0) return &S.dev[i];
    }
    if (!create || S.devCount >= MAX_DEV) return NULL;
    device_t *d = &S.dev[S.devCount++];
    ZeroMemory(d, sizeof(*d));
    lstrcpynA(d->mac, mac, sizeof(d->mac));
    d->prio = 3;
    d->firstSeen = d->lastSeen = (long long)time(NULL) * 1000;
    d->used = 1;
    return d;
}

static void dev_load(void)
{
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\devices.json", g_dataDir);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(h, NULL);
    if (size == 0 || size > 1024 * 1024) { CloseHandle(h); return; }
    char *buf = (char *)malloc(size + 1);
    DWORD read = 0;
    if (buf && ReadFile(h, buf, size, &read, NULL)) {
        buf[read] = 0;
        const char *p = j_skip_ws(buf);
        if (*p == '{') {
            p++;
            while (*p) {
                p = j_skip_ws(p);
                if (*p != '"') break;
                const char *ks = p + 1;
                const char *ke = j_skip_value(p);
                char mac[32];
                size_t n = (size_t)((ke - 1) - ks);
                if (n >= sizeof(mac)) n = sizeof(mac) - 1;
                memcpy(mac, ks, n);
                mac[n] = 0;
                p = j_skip_ws(ke);
                if (*p == ':') p++;
                const char *vs = j_skip_ws(p);
                const char *ve = j_skip_value(vs);

                char obj[2048];
                size_t m = (size_t)(ve - vs);
                if (m >= sizeof(obj)) m = sizeof(obj) - 1;
                memcpy(obj, vs, m);
                obj[m] = 0;

                device_t *d = dev_find(mac, 1);
                if (d) {
                    j_get_str(obj, "alias", d->alias, sizeof(d->alias));
                    j_get_str(obj, "note", d->note, sizeof(d->note));
                    j_get_str(obj, "hostname", d->hostname, sizeof(d->hostname));
                    j_get_str(obj, "lastIp", d->ip, sizeof(d->ip));
                    j_get_str(obj, "aliasTo", d->aliasTo, sizeof(d->aliasTo));
                    d->prio = j_get_int(obj, "prio", 3);
                    d->blocked = j_get_bool(obj, "blocked", 0);
                    d->upMbps = j_get_int(obj, "upMbps", 0);
                    d->downMbps = j_get_int(obj, "downMbps", 0);
                    d->lastSeen = (long long)j_get_num(obj, "lastSeen", 0);
                    d->firstSeen = (long long)j_get_num(obj, "firstSeen", 0);
                }
                p = j_skip_ws(ve);
                if (*p == ',') p++;
            }
        }
    }
    free(buf);
    CloseHandle(h);
}

static void dev_save(void)
{
    sb b;
    sb_init(&b, 2048);
    sb_puts(&b, "{");
    for (int i = 0; i < S.devCount; i++) {
        device_t *d = &S.dev[i];
        if (i) sb_puts(&b, ",");
        sb_json_str(&b, d->mac);
        sb_puts(&b, ":{");
        sb_puts(&b, "\"alias\":"); sb_json_str(&b, d->alias);
        sb_puts(&b, ",\"note\":"); sb_json_str(&b, d->note);
        sb_puts(&b, ",\"hostname\":"); sb_json_str(&b, d->hostname);
        sb_puts(&b, ",\"lastIp\":"); sb_json_str(&b, d->ip);
        if (d->aliasTo[0]) { sb_puts(&b, ",\"aliasTo\":"); sb_json_str(&b, d->aliasTo); }
        sb_putf(&b, ",\"prio\":%d,\"blocked\":%s,\"upMbps\":%d,\"downMbps\":%d",
                d->prio, d->blocked ? "true" : "false", d->upMbps, d->downMbps);
        sb_putf(&b, ",\"lastSeen\":%lld,\"firstSeen\":%lld", d->lastSeen, d->firstSeen);
        sb_puts(&b, "}");
    }
    sb_puts(&b, "}");

    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\devices.json", g_dataDir);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, b.p, (DWORD)b.len, &w, NULL);
        CloseHandle(h);
    }
    sb_free(&b);
}

static void cfg_load(void)
{
    ZeroMemory(&S.cfg, sizeof(S.cfg));
    lstrcpynA(S.cfg.ssid, "Kwifi", sizeof(S.cfg.ssid));
    lstrcpynA(S.cfg.password, "redianwifi", sizeof(S.cfg.password));
    lstrcpynA(S.cfg.band, "auto", sizeof(S.cfg.band));
    S.cfg.hostPrio = 3;
    S.cfg.st_ipv6 = 1;
    S.cfg.st_mcast = 1;

    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\config.json", g_dataDir);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(h, NULL);
    if (size == 0 || size > 256 * 1024) { CloseHandle(h); return; }
    char *buf = (char *)malloc(size + 1);
    DWORD read = 0;
    if (buf && ReadFile(h, buf, size, &read, NULL)) {
        buf[read] = 0;
        j_get_str(buf, "ssid", S.cfg.ssid, sizeof(S.cfg.ssid));
        j_get_str(buf, "password", S.cfg.password, sizeof(S.cfg.password));
        j_get_str(buf, "band", S.cfg.band, sizeof(S.cfg.band));
        S.cfg.autoStart = j_get_bool(buf, "autoStart", 0);
        S.cfg.autoHotspot = j_get_bool(buf, "autoHotspot", 0);
        S.cfg.totalMbps = j_get_int(buf, "totalMbps", 0);
        S.cfg.hostPrio = j_get_int(buf, "hostPrio", 3);
        char stealth[512];
        j_get_raw(buf, "stealth", stealth, sizeof(stealth));
        if (stealth[0] == '{') {
            S.cfg.st_ipv6 = j_get_bool(stealth, "ipv6", 1);
            S.cfg.st_mcast = j_get_bool(stealth, "multicast", 1);
            S.cfg.st_ttl = j_get_bool(stealth, "ttl", 0);
        }
    }
    free(buf);
    CloseHandle(h);
}

static void cfg_save(void)
{
    sb b;
    sb_init(&b, 512);
    sb_puts(&b, "{\"ssid\":"); sb_json_str(&b, S.cfg.ssid);
    sb_puts(&b, ",\"password\":"); sb_json_str(&b, S.cfg.password);
    sb_puts(&b, ",\"band\":"); sb_json_str(&b, S.cfg.band);
    sb_putf(&b, ",\"autoStart\":%s,\"autoHotspot\":%s,\"totalMbps\":%d,\"hostPrio\":%d",
            S.cfg.autoStart ? "true" : "false",
            S.cfg.autoHotspot ? "true" : "false", S.cfg.totalMbps, S.cfg.hostPrio);
    sb_putf(&b, ",\"stealth\":{\"ipv6\":%s,\"multicast\":%s,\"ttl\":%s}}",
            S.cfg.st_ipv6 ? "true" : "false",
            S.cfg.st_mcast ? "true" : "false",
            S.cfg.st_ttl ? "true" : "false");

    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\config.json", g_dataDir);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, b.p, (DWORD)b.len, &w, NULL);
        CloseHandle(h);
    }
    sb_free(&b);
}

/* ---------------- PowerShell worker ---------------- */

typedef struct job {
    struct job *next;
    char args[1024];
    int prio;              /* 0 = user action, 1 = polling */
    char *out;
    int ok;
    HANDLE done;
} job_t;

static struct {
    CRITICAL_SECTION cs;
    job_t *head;
    HANDLE wake;
    int running;
} Q;

static HANDLE g_psThread = NULL;

static DWORD WINAPI ps_worker(LPVOID param)
{
    for (;;) {
        WaitForSingleObject(Q.wake, INFINITE);
        for (;;) {
            job_t *j = NULL;
            EnterCriticalSection(&Q.cs);
            for (job_t **pp = &Q.head; *pp; pp = &(*pp)->next) {
                if (!j || (*pp)->prio < j->prio) j = *pp;
            }
            if (j) {
                for (job_t **pp = &Q.head; *pp; pp = &(*pp)->next) {
                    if (*pp == j) { *pp = j->next; break; }
                }
            }
            LeaveCriticalSection(&Q.cs);
            if (!j) break;

            wchar_t exePath[MAX_PATH * 2];
            _snwprintf(exePath, MAX_PATH * 2, L"%s\\net.ps1", g_dataDir);
            wchar_t cmd[4096];
            _snwprintf(cmd, 4096,
                       L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" %hs",
                       exePath, j->args);

            SECURITY_ATTRIBUTES sa;
            ZeroMemory(&sa, sizeof(sa));
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            HANDLE rd = NULL, wr = NULL;
            if (!CreatePipe(&rd, &wr, &sa, 0)) {
                j->ok = 0;
                SetEvent(j->done);
                continue;
            }
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = wr;
            si.hStdError = wr;
            si.hStdInput = NULL;
            ZeroMemory(&pi, sizeof(pi));

            BOOL started = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                          NULL, NULL, &si, &pi);
            CloseHandle(wr);
            if (!started) {
                CloseHandle(rd);
                j->ok = 0;
                SetEvent(j->done);
                continue;
            }

            sb out;
            sb_init(&out, 4096);
            char chunk[4096];
            DWORD got = 0, total = 0;
            while (ReadFile(rd, chunk, sizeof(chunk), &got, NULL) && got > 0) {
                if (total < 512 * 1024) {
                    char *tmp = (char *)realloc(out.p, out.len + got + 1);
                    if (tmp) {
                        out.p = tmp;
                        memcpy(out.p + out.len, chunk, got);
                        out.len += got;
                        out.p[out.len] = 0;
                        total += got;
                    }
                }
            }
            CloseHandle(rd);
            WaitForSingleObject(pi.hProcess, 120000);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            j->out = out.p;
            j->ok = (code == 0);
            SetEvent(j->done);
        }
    }
    return 0;
}

static job_t *ps_run(const char *args, int prio, int timeoutMs)
{
    job_t *j = (job_t *)calloc(1, sizeof(job_t));
    if (!j) return NULL;
    lstrcpynA(j->args, args, sizeof(j->args));
    j->prio = prio;
    j->done = CreateEventW(NULL, TRUE, FALSE, NULL);

    EnterCriticalSection(&Q.cs);
    j->next = Q.head;
    Q.head = j;
    LeaveCriticalSection(&Q.cs);
    SetEvent(Q.wake);

    WaitForSingleObject(j->done, timeoutMs > 0 ? timeoutMs : 120000);
    return j;
}

static void ps_free(job_t *j)
{
    if (!j) return;
    if (j->done) CloseHandle(j->done);
    free(j->out);
    free(j);
}

/* run and keep only the data object text */
static int ps_json(const char *args, int prio, char *out, size_t cch, int timeoutMs)
{
    job_t *j = ps_run(args, prio, timeoutMs);
    if (!j) return 0;
    int ok = 0;
    out[0] = 0;
    if (j->out) {
        const char *p = strstr(j->out, "\"data\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p = j_skip_ws(p + 1);
                if (*p == '{') {
                    const char *e = j_skip_value(p);
                    size_t n = (size_t)(e - p);
                    if (n + 1 > cch) n = cch - 1;
                    memcpy(out, p, n);
                    out[n] = 0;
                    ok = 1;
                }
            }
        }
        if (!ok && strstr(j->out, "\"ok\":true")) ok = 1;
    }
    ps_free(j);
    return ok;
}

/* ---------------- data plane helper (ttlfix) ---------------- */

static HANDLE g_helper = NULL;
static DWORD g_helperPid = 0;

/* ttlfix links WinDivert.dll, and the driver is loaded from WinDivert64.sys next
   to it, so all three have to be unpacked (plus the LGPL licence text) */
static void write_beside_data(const wchar_t *name, const unsigned char *data, DWORD len)
{
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\%s", g_dataDir, name);

    /* rewrite only when the size differs: a running ttlfix holds the dll */
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &info) &&
        info.nFileSizeLow == len) {
        return;
    }

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, len, &w, NULL);
    CloseHandle(h);
}

static void helper_extract(void)
{
    write_beside_data(L"ttlfix.exe", res_ttlfix_exe, res_ttlfix_exe_len);
    write_beside_data(L"WinDivert.dll", res_windivert_dll, res_windivert_dll_len);
    write_beside_data(L"WinDivert64.sys", res_windivert_sys, res_windivert_sys_len);
    write_beside_data(L"WinDivert-LICENSE.txt", res_windivert_lic, res_windivert_lic_len);
}

static void helper_write_conf(const char *psData)
{
    sb b;
    sb_init(&b, 2048);
    sb_putf(&b, "enabled=%d\nttl=128\nhostprio=%d\n", S.cfg.st_ttl ? 1 : 0, S.cfg.hostPrio);

    char up[1024];
    j_get_raw(psData, "uplinkNic", up, sizeof(up));
    char ip[64];
    j_get_str(up, "ipv4", ip, sizeof(ip));
    /* ipv4 is an array; take the first string manually */
    if (!ip[0]) {
        const char *a = j_find(up, "ipv4", NULL);
        if (a && *a == '[') {
            a = j_skip_ws(a + 1);
            if (*a == '"') {
                a++;
                size_t i = 0;
                while (*a && *a != '"' && i + 1 < sizeof(ip)) ip[i++] = *a++;
                ip[i] = 0;
            }
        }
    }
    if (ip[0]) sb_putf(&b, "hostip=%s\n", ip);

    int total = S.cfg.totalMbps;
    if (total <= 0) {
        double speed = j_get_num(up, "speedMbps", 0);
        if (speed > 0) total = (int)(speed * 0.9 * 1000);
    } else {
        total *= 1000;
    }
    if (total > 0) sb_putf(&b, "total=%d\n", total);

    /* the hotspot subnet is whatever the clients actually use; tell the data
       plane instead of assuming 192.168.137.0/24 */
    char subnets[8][20];
    int subnetN = 0;

    /* clients + per-device rules */
    const char *cl = strstr(psData, "\"clients\"");
    if (cl) {
        cl = strchr(cl, '[');
        if (cl) {
            const char *p = j_skip_ws(cl + 1);
            while (*p && *p != ']') {
                if (*p == '{') {
                    const char *e = j_skip_value(p);
                    char obj[512];
                    size_t n = (size_t)(e - p);
                    if (n >= sizeof(obj)) n = sizeof(obj) - 1;
                    memcpy(obj, p, n);
                    obj[n] = 0;

                    char ip2[40] = {0}, mac[32] = {0};
                    j_get_str(obj, "ip", ip2, sizeof(ip2));
                    j_get_str(obj, "mac", mac, sizeof(mac));
                    if (ip2[0] && mac[0]) {
                        /* remember the /24 so the data plane recognises it */
                        {
                            unsigned sa = 0, sb = 0, sc = 0, sd = 0;
                            if (sscanf(ip2, "%u.%u.%u.%u", &sa, &sb, &sc, &sd) == 4) {
                                char net[20];
                                _snprintf(net, sizeof(net), "%u.%u.%u.0/24", sa, sb, sc);
                                int dup = 0;
                                for (int k = 0; k < subnetN; k++) {
                                    if (_stricmp(subnets[k], net) == 0) dup = 1;
                                }
                                if (!dup && subnetN < 8) {
                                    lstrcpynA(subnets[subnetN], net, sizeof(subnets[0]));
                                    subnetN++;
                                }
                            }
                        }
                        device_t *d = dev_find(mac, 0);
                        if (d) {
                            d->lastSeen = (long long)time(NULL) * 1000;
                            if (!d->ip[0]) lstrcpynA(d->ip, ip2, sizeof(d->ip));
                            if (d->blocked) sb_putf(&b, "block %s\n", ip2);
                            if (d->upMbps || d->downMbps || d->prio != 3) {
                                sb_putf(&b, "limit %s up=%d down=%d prio=%d\n", ip2,
                                        d->upMbps * 1000, d->downMbps * 1000, d->prio);
                            }
                        }
                    }
                    p = j_skip_ws(e);
                    if (*p == ',') p++;
                } else {
                    break;
                }
            }
        }
    }

    for (int k = 0; k < subnetN; k++) sb_putf(&b, "subnet %s\n", subnets[k]);

    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\ttlfix.conf", g_dataDir);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, b.p, (DWORD)b.len, &w, NULL);
        CloseHandle(h);
    }
    sb_free(&b);
}

static int helper_alive(void)
{
    if (g_helper) return 1;
    if (g_helperPid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_helperPid);
        if (h) { CloseHandle(h); return 1; }
        g_helperPid = 0;
    }
    return 0;
}

static void helper_start(void)
{
    if (helper_alive()) return;

    wchar_t exe[MAX_PATH * 2], conf[MAX_PATH * 2], stats[MAX_PATH * 2];
    _snwprintf(exe, MAX_PATH * 2, L"%s\\ttlfix.exe", g_dataDir);
    _snwprintf(conf, MAX_PATH * 2, L"%s\\ttlfix.conf", g_dataDir);
    _snwprintf(stats, MAX_PATH * 2, L"%s\\ttlfix-stats.json", g_dataDir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t cmd[MAX_PATH * 6];
    _snwprintf(cmd, MAX_PATH * 6, L"\"%s\" --config \"%s\" --stats \"%s\"", exe, conf, stats);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_dataDir, &si, &pi)) {
        CloseHandle(pi.hThread);

        /* opening the WinDivert handle fails within a few hundred milliseconds
           when the driver is missing or we are not elevated, so wait before
           claiming success - a rule that is not enforced must not look applied */
        DWORD wait = WaitForSingleObject(pi.hProcess, 1200);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            log_add(2, "数据面启动后立即退出 (code %lu)：缺少管理员权限，或 WinDivert 驱动被拦截",
                    (unsigned long)code);
            return;
        }

        g_helper = pi.hProcess;
        g_helperPid = pi.dwProcessId;
        log_add(0, "数据面已启动 (PID %lu)", (unsigned long)g_helperPid);
    } else {
        log_add(1, "数据面启动失败 (%lu)", (unsigned long)GetLastError());
    }
}

static void helper_stop(void)
{
    if (g_helper) {
        TerminateProcess(g_helper, 0);
        CloseHandle(g_helper);
        g_helper = NULL;
    }
    if (g_helperPid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, g_helperPid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
        g_helperPid = 0;
    }
}

/* the data plane enforces blocking, shaping and priority; TTL rewriting is only
   one of its jobs, so it has to run whenever any rule exists */
static int any_rules(void)
{
    for (int i = 0; i < S.devCount; i++) {
        device_t *d = &S.dev[i];
        if (d->blocked || d->upMbps || d->downMbps || d->prio != 3) return 1;
    }
    return 0;
}

static int dataplane_needed(void)
{
    return (S.cfg.st_ttl || any_rules()) ? 1 : 0;
}
static void helper_read_stats(void)
{
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\ttlfix-stats.json", g_dataDir);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[4096];
    DWORD read = 0;
    if (ReadFile(h, buf, sizeof(buf) - 1, &read, NULL)) {
        buf[read] = 0;
        const char *cl = strstr(buf, "\"clients\"");
        if (cl) {
            const char *p = strchr(cl, '{');
            if (p) {
                p++;
                while (*p) {
                    p = j_skip_ws(p);
                    if (*p != '"') break;
                    char ip[40];
                    size_t i = 0;
                    p++;
                    while (*p && *p != '"' && i + 1 < sizeof(ip)) ip[i++] = *p++;
                    ip[i] = 0;
                    if (*p == '"') p++;
                    p = j_skip_ws(p);
                    if (*p == ':') p++;
                    const char *vs = j_skip_ws(p);
                    const char *ve = j_skip_value(vs);
                    char obj[256];
                    size_t n = (size_t)(ve - vs);
                    if (n >= sizeof(obj)) n = sizeof(obj) - 1;
                    memcpy(obj, vs, n);
                    obj[n] = 0;
                    int blocked = j_get_int(obj, "blocked", 0);
                    double drops = j_get_num(obj, "drops", 0);
                    for (int k = 0; k < S.devCount; k++) {
                        if (_stricmp(S.dev[k].ip, ip) == 0) {
                            if (blocked) S.dev[k].blocked = 1;
                            S.dev[k].drops = (long long)drops;
                        }
                    }
                    p = j_skip_ws(ve);
                    if (*p == ',') p++;
                }
            }
        }
    }
    CloseHandle(h);
}

/* ---------------- status snapshot ---------------- */

static void poll_once(int full)
{
    char data[8192];
    char args[128];
    _snprintf(args, sizeof(args), "-Action status -Detail %s", full ? "full" : "quick");
    if (!ps_json(args, 1, data, sizeof(data), 60000)) {
        char err[64] = {0};
        _snprintf(err, sizeof(err), "status 读取失败");
        log_add(2, "%s", err);
        return;
    }

    EnterCriticalSection(&S.cs);
    lstrcpynA(S.psData, data, sizeof(S.psData));
    S.admin = j_get_bool(data, "admin", 0);
    S.active = j_get_bool(data, "active", 0);
    j_get_str(data, "method", S.method, sizeof(S.method));
    j_get_str(data, "state", S.state, sizeof(S.state));
    j_get_str(data, "ssid", S.ssid, sizeof(S.ssid));
    j_get_str(data, "ttlState", S.ttlState, sizeof(S.ttlState));
    j_get_raw(data, "hotspotNic", S.hnic, sizeof(S.hnic));
    j_get_raw(data, "uplinkNic", S.unic, sizeof(S.unic));
    j_get_raw(data, "clients", S.clients, sizeof(S.clients));

    /* traffic from uplink counters */
    double bIn = j_get_num(S.unic, "bytesIn", 0);
    double bOut = j_get_num(S.unic, "bytesOut", 0);
    static double lastIn = -1, lastOut = -1;
    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (lastIn >= 0 && now > lastTick) {
        double dt = (now - lastTick) / 1000.0;
        S.inBps = (long long)((bIn - lastIn) / dt);
        S.outBps = (long long)((bOut - lastOut) / dt);
        if (S.inBps < 0) S.inBps = 0;
        if (S.outBps < 0) S.outBps = 0;
        if (S.histCount < 60) S.histCount++;
        for (int i = S.histCount - 1; i > 0; i--) S.hist[i] = S.hist[i - 1];
        S.hist[0].t = (long long)time(NULL) * 1000;
        S.hist[0].in = (int)S.inBps;
        S.hist[0].out = (int)S.outBps;
    }
    lastIn = bIn;
    lastOut = bOut;
    lastTick = now;
    LeaveCriticalSection(&S.cs);

    helper_write_conf(data);
    helper_extract();
    if (dataplane_needed()) {
        if (!helper_alive()) helper_start();
    } else if (helper_alive()) {
        helper_stop();
    }
    if (helper_alive()) helper_read_stats();
}

/* ---------------- SSE broadcast ---------------- */

static SOCKET g_sse[MAX_SSE];
static CRITICAL_SECTION g_sseCs;

static void sse_send_all(const char *payload, size_t len)
{
    EnterCriticalSection(&g_sseCs);
    for (int i = 0; i < MAX_SSE; i++) {
        if (g_sse[i] == INVALID_SOCKET) continue;
        if (send(g_sse[i], "data: ", 6, 0) <= 0 ||
            send(g_sse[i], payload, (int)len, 0) <= 0 ||
            send(g_sse[i], "\n\n", 2, 0) <= 0) {
            closesocket(g_sse[i]);
            g_sse[i] = INVALID_SOCKET;
        }
    }
    LeaveCriticalSection(&g_sseCs);
}

static int sse_add(SOCKET s)
{
    int slot = -1;
    EnterCriticalSection(&g_sseCs);
    for (int i = 0; i < MAX_SSE; i++) {
        if (g_sse[i] == INVALID_SOCKET) { g_sse[i] = s; slot = i; break; }
    }
    LeaveCriticalSection(&g_sseCs);
    return slot;
}

/* ---------------- status JSON for the UI ---------------- */

static void build_status(sb *b)
{
    EnterCriticalSection(&S.cs);

    sb_puts(b, "{\"ok\":true,\"data\":{");
    sb_putf(b, "\"admin\":%s,\"method\":", S.admin ? "true" : "false");
    sb_json_str(b, S.method);
    sb_putf(b, ",\"active\":%s", S.active ? "true" : "false");
    sb_puts(b, ",\"state\":"); sb_json_str(b, S.state);
    sb_puts(b, ",\"ssid\":"); sb_json_str(b, S.ssid[0] ? S.ssid : S.cfg.ssid);
    sb_puts(b, ",\"hotspotNic\":");
    sb_puts(b, S.hnic[0] ? S.hnic : "{}");
    sb_puts(b, ",\"uplinkNic\":");
    sb_puts(b, S.unic[0] ? S.unic : "{}");

    /* clients: the PS list enriched with what we store */
    sb_puts(b, ",\"clients\":[");
    int first = 1;
    const char *p = strchr(S.clients, '[');
    if (p) p++;
    while (p && *p) {
        p = j_skip_ws(p);
        if (*p != '{') break;
        const char *e = j_skip_value(p);
        char obj[512];
        size_t n = (size_t)(e - p);
        if (n >= sizeof(obj)) n = sizeof(obj) - 1;
        memcpy(obj, p, n);
        obj[n] = 0;

        char ip[40] = {0}, mac[32] = {0};
        j_get_str(obj, "ip", ip, sizeof(ip));
        j_get_str(obj, "mac", mac, sizeof(mac));
        if (ip[0] && mac[0]) {
            device_t *d = dev_find(mac, 1);
            if (d) {
                lstrcpynA(d->ip, ip, sizeof(d->ip));
                d->lastSeen = (long long)time(NULL) * 1000;
                if (!d->hostname[0]) {
                    char hn[128] = {0};
                    j_get_str(obj, "hostname", hn, sizeof(hn));
                    if (hn[0]) lstrcpynA(d->hostname, hn, sizeof(d->hostname));
                }
            }
            if (!first) sb_puts(b, ",");
            first = 0;
            sb_puts(b, "{\"ip\":"); sb_json_str(b, ip);
            sb_puts(b, ",\"mac\":"); sb_json_str(b, mac);
            sb_puts(b, ",\"hostname\":");
            sb_json_str(b, d && d->hostname[0] ? d->hostname : NULL);
            sb_puts(b, ",\"alias\":"); sb_json_str(b, d ? d->alias : "");
            sb_puts(b, ",\"note\":"); sb_json_str(b, d ? d->note : "");
            sb_puts(b, ",\"name\":");
            sb_json_str(b, d && d->alias[0] ? d->alias : (d && d->hostname[0] ? d->hostname : "未知设备"));
            sb_puts(b, ",\"kind\":\"设备\",\"model\":\"\",\"icon\":\"device\"");
            sb_putf(b, ",\"randomMac\":%s", (mac[1] == '2' || mac[1] == '6' || mac[1] == 'A' || mac[1] == 'E' ||
                                              mac[1] == 'a' || mac[1] == 'e') ? "true" : "false");
            sb_putf(b, ",\"prio\":%d,\"blocked\":%s", d ? d->prio : 3,
                    d && d->blocked ? "true" : "false");
            sb_putf(b, ",\"upMbps\":%d,\"downMbps\":%d", d ? d->upMbps : 0, d ? d->downMbps : 0);
            sb_putf(b, ",\"upKbps\":%d,\"downKbps\":%d", d ? d->upMbps * 1000 : 0, d ? d->downMbps * 1000 : 0);
            sb_puts(b, ",\"online\":true");
            sb_putf(b, ",\"drops\":%lld", d ? d->drops : 0);
            sb_putf(b, ",\"lastSeen\":%lld", d ? d->lastSeen : 0);
            if (d && d->aliasTo[0]) { sb_puts(b, ",\"aliasTo\":"); sb_json_str(b, d->aliasTo); }
            else sb_puts(b, ",\"aliasTo\":null");
            sb_puts(b, "}");
        }
        p = j_skip_ws(e);
        if (*p == ',') p++;
    }

    /* offline history */
    for (int i = 0; i < S.devCount; i++) {
        device_t *d = &S.dev[i];
        int online = 0;
        const char *q = strchr(S.clients, '[');
        if (q && d->ip[0]) {
            char needle[64];
            _snprintf(needle, sizeof(needle), "\"%s\"", d->ip);
            if (strstr(S.clients, needle)) online = 1;
        }
        if (online) continue;
        if (!first) sb_puts(b, ",");
        first = 0;
        sb_puts(b, "{\"ip\":"); sb_json_str(b, d->ip);
        sb_puts(b, ",\"mac\":"); sb_json_str(b, d->mac);
        sb_puts(b, ",\"hostname\":"); sb_json_str(b, d->hostname[0] ? d->hostname : NULL);
        sb_puts(b, ",\"alias\":"); sb_json_str(b, d->alias);
        sb_puts(b, ",\"note\":"); sb_json_str(b, d->note);
        sb_puts(b, ",\"name\":");
        sb_json_str(b, d->alias[0] ? d->alias : (d->hostname[0] ? d->hostname : "未知设备"));
        sb_puts(b, ",\"kind\":\"设备\",\"model\":\"\",\"icon\":\"device\"");
        sb_puts(b, ",\"randomMac\":false");
        sb_putf(b, ",\"prio\":%d,\"blocked\":%s,\"upMbps\":%d,\"downMbps\":%d",
                d->prio, d->blocked ? "true" : "false", d->upMbps, d->downMbps);
        sb_putf(b, ",\"upKbps\":%d,\"downKbps\":%d", d->upMbps * 1000, d->downMbps * 1000);
        sb_puts(b, ",\"online\":false");
        sb_putf(b, ",\"lastSeen\":%lld", d->lastSeen);
        if (d->aliasTo[0]) { sb_puts(b, ",\"aliasTo\":"); sb_json_str(b, d->aliasTo); }
        else sb_puts(b, ",\"aliasTo\":null");
        sb_puts(b, "}");
    }
    sb_puts(b, "]");

    sb_puts(b, ",\"stealth\":{\"ipv6\":");
    sb_puts(b, S.cfg.st_ipv6 ? "true" : "false");
    sb_puts(b, ",\"multicast\":");
    sb_puts(b, S.cfg.st_mcast ? "true" : "false");
    sb_puts(b, ",\"ttl\":");
    sb_json_str(b, helper_alive() ? "active" : "ready");
    sb_puts(b, "}");

    sb_putf(b, ",\"ttlState\":\"%s\"", helper_alive() ? "active" : S.ttlState[0] ? S.ttlState : "ready");
    sb_putf(b, ",\"network\":{\"totalMbps\":%d,\"auto\":%s,\"hostPrio\":%d}",
            S.cfg.totalMbps, S.cfg.totalMbps <= 0 ? "true" : "false", S.cfg.hostPrio);

    sb_puts(b, ",\"config\":{\"ssid\":"); sb_json_str(b, S.cfg.ssid);
    sb_puts(b, ",\"password\":"); sb_json_str(b, S.cfg.password);
    sb_puts(b, ",\"band\":"); sb_json_str(b, S.cfg.band);
    sb_putf(b, ",\"autoStart\":%s,\"autoHotspot\":%s,\"totalMbps\":%d,\"hostPrio\":%d",
            S.cfg.autoStart ? "true" : "false",
            S.cfg.autoHotspot ? "true" : "false", S.cfg.totalMbps, S.cfg.hostPrio);
    sb_puts(b, ",\"stealth\":{\"ipv6\":");
    sb_puts(b, S.cfg.st_ipv6 ? "true" : "false");
    sb_puts(b, ",\"multicast\":");
    sb_puts(b, S.cfg.st_mcast ? "true" : "false");
    sb_puts(b, ",\"ttl\":");
    sb_puts(b, S.cfg.st_ttl ? "true" : "false");
    sb_puts(b, "}}");

    sb_putf(b, ",\"traffic\":{\"inBps\":%lld,\"outBps\":%lld,\"history\":[",
            S.inBps, S.outBps);
    for (int i = S.histCount - 1; i >= 0; i--) {
        if (i != S.histCount - 1) sb_puts(b, ",");
        sb_putf(b, "{\"t\":%lld,\"in\":%d,\"out\":%d}", S.hist[i].t, S.hist[i].in, S.hist[i].out);
    }
    sb_puts(b, "]}");

    sb_puts(b, ",\"logs\":[");
    for (int i = 0; i < S.logCount; i++) {
        int idx = (S.logHead - S.logCount + i + MAX_LOG) % MAX_LOG;
        if (i) sb_puts(b, ",");
        sb_putf(b, "{\"t\":%lld,\"level\":\"%s\",\"message\":",
                S.logs[idx].t,
                S.logs[idx].level == 2 ? "error" : S.logs[idx].level == 1 ? "warn" : "info");
        sb_json_str(b, S.logs[idx].msg);
        sb_puts(b, "}");
    }
    sb_puts(b, "]");
    sb_putf(b, ",\"dataplane\":%s", helper_alive() ? "true" : "false");
    sb_puts(b, ",\"appVersion\":\"" APP_VERSION "\"");
    sb_putf(b, ",\"ts\":%lld}}", (long long)time(NULL) * 1000);

    LeaveCriticalSection(&S.cs);
}

/* ---------------- HTTP ---------------- */

static const unsigned char *res_lookup(const char *path, DWORD *len, const char **type)
{
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        *len = res_index_html_len; *type = "text/html; charset=utf-8";
        return res_index_html;
    }
    if (!strcmp(path, "/app.js")) {
        *len = res_app_js_len; *type = "application/javascript; charset=utf-8";
        return res_app_js;
    }
    if (!strcmp(path, "/styles.css")) {
        *len = res_styles_css_len; *type = "text/css; charset=utf-8";
        return res_styles_css;
    }
    return NULL;
}

static void http_send(SOCKET s, int code, const char *type, const void *body, size_t len)
{
    char head[512];
    const char *reason = code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request";
    int n = _snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      code, reason, type, (int)len);
    send(s, head, n, 0);
    if (len) send(s, (const char *)body, (int)len, 0);
}

static void http_json(SOCKET s, int code, const char *json)
{
    http_send(s, code, "application/json; charset=utf-8", json, json ? strlen(json) : 0);
}

/* forward */
static void build_status(sb *b);

/* ---------------- route handlers ---------------- */

static void api_status(SOCKET s)
{
    sb b;
    sb_init(&b, 8192);
    build_status(&b);
    http_json(s, 200, b.p);
    sb_free(&b);
}

static void api_events(SOCKET s)
{
    const char *head =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
    send(s, head, (int)strlen(head), 0);

    sb b;
    sb_init(&b, 8192);
    build_status(&b);
    send(s, "data: ", 6, 0);
    send(s, b.p, (int)b.len, 0);
    send(s, "\n\n", 2, 0);
    sb_free(&b);

    sse_add(s);   /* socket stays open, do not close */
}

static void api_config(SOCKET s, const char *body)
{
    if (body && *body == '{') {
        char v[64];
        j_get_str(body, "ssid", v, sizeof(v));
        if (v[0]) lstrcpynA(S.cfg.ssid, v, sizeof(S.cfg.ssid));
        j_get_str(body, "password", v, sizeof(v));
        if (v[0]) lstrcpynA(S.cfg.password, v, sizeof(S.cfg.password));
        j_get_str(body, "band", v, sizeof(v));
        if (v[0]) lstrcpynA(S.cfg.band, v, sizeof(S.cfg.band));
        if (j_find(body, "autoHotspot", NULL)) S.cfg.autoHotspot = j_get_bool(body, "autoHotspot", 0);
        cfg_save();
    }
    sb b;
    sb_init(&b, 512);
    sb_puts(&b, "{\"ok\":true,\"data\":{\"ssid\":"); sb_json_str(&b, S.cfg.ssid);
    sb_puts(&b, ",\"password\":"); sb_json_str(&b, S.cfg.password);
    sb_puts(&b, ",\"band\":"); sb_json_str(&b, S.cfg.band);
    sb_putf(&b, ",\"autoStart\":%s,\"autoHotspot\":%s,\"totalMbps\":%d,\"hostPrio\":%d,\"stealth\":{\"ipv6\":%s,\"multicast\":%s,\"ttl\":%s}}}",
            S.cfg.autoStart ? "true" : "false",
            S.cfg.autoHotspot ? "true" : "false", S.cfg.totalMbps, S.cfg.hostPrio,
            S.cfg.st_ipv6 ? "true" : "false", S.cfg.st_mcast ? "true" : "false",
            S.cfg.st_ttl ? "true" : "false");
    http_json(s, 200, b.p);
    sb_free(&b);
}

/* pass the PowerShell JSON envelope through unchanged */
static void ps_passthrough(SOCKET s, const char *args, int prio, int timeoutMs)
{
    job_t *j = ps_run(args, prio, timeoutMs);
    if (!j || !j->out) {
        http_json(s, 200, "{\"ok\":false,\"error\":\"PowerShell 调用失败\"}");
        ps_free(j);
        return;
    }
    http_json(s, 200, j->out);
    ps_free(j);
}

static void api_hotspot(SOCKET s, const char *body)
{
    char action[16] = {0};
    j_get_str(body, "action", action, sizeof(action));
    if (!action[0]) lstrcpynA(action, "start", sizeof(action));

    char args[512];
    if (!strcmp(action, "stop")) {
        _snprintf(args, sizeof(args), "-Action stop");
        ps_passthrough(s, args, 0, 90000);
        if (S.cfg.st_ttl) helper_stop();
        log_add(0, "热点已关闭");
        return;
    }
    char ssid[64], pass[64], band[8];
    lstrcpynA(ssid, S.cfg.ssid, sizeof(ssid));
    lstrcpynA(pass, S.cfg.password, sizeof(pass));
    lstrcpynA(band, S.cfg.band, sizeof(band));
    char v[64];
    j_get_str(body, "ssid", v, sizeof(v)); if (v[0]) lstrcpynA(ssid, v, sizeof(ssid));
    j_get_str(body, "password", v, sizeof(v)); if (v[0]) lstrcpynA(pass, v, sizeof(pass));
    if (v[0]) { lstrcpynA(S.cfg.ssid, ssid, sizeof(S.cfg.ssid)); cfg_save(); }

    _snprintf(args, sizeof(args), "-Action start -Ssid \"%s\" -Password \"%s\" -Band \"%s\"",
              ssid, pass, band);
    ps_passthrough(s, args, 0, 120000);
    log_add(0, "热点已启动");

    Sleep(300);
    poll_once(1);
}

static void api_network(SOCKET s, const char *body)
{
    if (body && *body == '{') {
        if (j_find(body, "totalMbps", NULL)) S.cfg.totalMbps = j_get_int(body, "totalMbps", 0);
        if (j_find(body, "hostPrio", NULL)) S.cfg.hostPrio = j_get_int(body, "hostPrio", 3);
        cfg_save();
    }
    sb b;
    sb_init(&b, 128);
    sb_putf(&b, "{\"ok\":true,\"data\":{\"totalMbps\":%d,\"hostPrio\":%d}}",
            S.cfg.totalMbps, S.cfg.hostPrio);
    http_json(s, 200, b.p);
    sb_free(&b);
}

static void api_stealth(SOCKET s, const char *body)
{
    if (body && *body == '{') {
        if (j_find(body, "ipv6", NULL)) S.cfg.st_ipv6 = j_get_bool(body, "ipv6", 1);
        if (j_find(body, "multicast", NULL)) S.cfg.st_mcast = j_get_bool(body, "multicast", 1);
        if (j_find(body, "ttl", NULL)) S.cfg.st_ttl = j_get_bool(body, "ttl", 0);
        cfg_save();
    }

    char args[256];
    _snprintf(args, sizeof(args), "-Action stealth -Ipv6 %s -Multicast %s",
              S.cfg.st_ipv6 ? "off" : "on", S.cfg.st_mcast ? "on" : "off");
    job_t *j = ps_run(args, 0, 90000);
    sb b;
    sb_init(&b, 512);
    sb_puts(&b, "{\"ok\":true,\"action\":\"stealth\",\"data\":{\"ttl\":");
    sb_json_str(&b, helper_alive() ? "active" : "ready");
    sb_puts(&b, "},\"warnings\":[],\"error\":null,\"ttl\":{\"ok\":true}}");
    http_json(s, 200, b.p);
    sb_free(&b);
    ps_free(j);

    helper_write_conf(S.psData);
    if (dataplane_needed()) helper_start(); else helper_stop();
}

/* per-device settings: /api/device, /api/limit, /api/block */
static void api_device(SOCKET s, const char *body, int blockAlias)
{
    char mac[32] = {0}, ip[40] = {0};
    j_get_str(body, "mac", mac, sizeof(mac));
    j_get_str(body, "ip", ip, sizeof(ip));

    device_t *d = NULL;
    if (mac[0]) d = dev_find(mac, 0);
    if (!d && ip[0]) {
        for (int i = 0; i < S.devCount; i++) {
            if (!_stricmp(S.dev[i].ip, ip)) { d = &S.dev[i]; break; }
        }
    }
    if (!d && mac[0]) d = dev_find(mac, 1);
    if (!d) {
        http_json(s, 200, "{\"ok\":false,\"error\":\"这台设备当前不在线，请刷新后重试\"}");
        return;
    }

    int wantBlock = -1;
    char v[160];
    j_get_str(body, "alias", v, sizeof(v));
    if (j_find(body, "alias", NULL)) lstrcpynA(d->alias, v, sizeof(d->alias));
    j_get_str(body, "note", v, sizeof(v));
    if (j_find(body, "note", NULL)) lstrcpynA(d->note, v, sizeof(d->note));
    if (j_find(body, "prio", NULL)) {
        int p = j_get_int(body, "prio", 3);
        if (p < 1) p = 1;
        if (p > 5) p = 5;
        d->prio = p;
    }
    if (j_find(body, "upMbps", NULL)) d->upMbps = j_get_int(body, "upMbps", 0);
    if (j_find(body, "downMbps", NULL)) d->downMbps = j_get_int(body, "downMbps", 0);
    if (blockAlias && j_find(body, "block", NULL)) wantBlock = j_get_bool(body, "block", 0);
    if (j_find(body, "blocked", NULL)) wantBlock = j_get_bool(body, "blocked", 0);

    if (wantBlock >= 0 && wantBlock != d->blocked) {
        d->blocked = wantBlock;
        if (d->ip[0]) {
            char args[160];
            _snprintf(args, sizeof(args), "-Action %s -Ip %s",
                      d->blocked ? "block" : "unblock", d->ip);
            job_t *j = ps_run(args, 0, 60000);
            ps_free(j);
        }
        log_add(0, "%s %s", d->blocked ? "已封禁" : "已解封",
                d->alias[0] ? d->alias : d->mac);
    }
    if (d->ip[0] && (d->upMbps || d->downMbps)) {
        char args[200];
        _snprintf(args, sizeof(args), "-Action limit -Ip %s -UpKbps %d -DownKbps %d",
                  d->ip, d->upMbps * 1000, d->downMbps * 1000);
        job_t *j = ps_run(args, 0, 60000);
        ps_free(j);
    }

    dev_save();

    /* apply the rule now: rewriting the config and starting the data plane here
       makes blocking take effect immediately instead of on the next poll */
    helper_write_conf(S.psData);
    if (dataplane_needed()) {
        if (!helper_alive()) helper_start();
    } else if (helper_alive()) {
        helper_stop();
    }

    /* a rule nobody enforces is worse than no rule: refuse to report success and
       put the device back, otherwise the interface would show a block that is
       not being enforced */
    if (wantBlock == 1 && !helper_alive()) {
        d->blocked = 0;
        dev_save();
        helper_write_conf(S.psData);
        log_add(1, "封禁失败: 数据面未运行, 已回滚 (%s)", d->mac);

        sb e;
        sb_init(&e, 512);
        sb_puts(&e, "{\"ok\":false,\"error\":");
        sb_json_str(&e, "数据面没有运行，封禁不会生效。请用管理员权限运行，或在设置里安装后台服务。");
        sb_puts(&e, ",\"dataplane\":false}");
        http_json(s, 200, e.p);
        sb_free(&e);
        return;
    }

    sb b;
    sb_init(&b, 512);
    sb_puts(&b, "{\"ok\":true,\"data\":{");
    sb_puts(&b, "\"mac\":"); sb_json_str(&b, d->mac);
    sb_puts(&b, ",\"blocked\":"); sb_puts(&b, d->blocked ? "true" : "false");
    sb_putf(&b, ",\"prio\":%d,\"upMbps\":%d,\"downMbps\":%d", d->prio, d->upMbps, d->downMbps);
    sb_putf(&b, ",\"dataplane\":%s", helper_alive() ? "true" : "false");
    sb_puts(&b, "},\"warnings\":[]}");
    http_json(s, 200, b.p);
    sb_free(&b);
}

static void api_device_delete(SOCKET s, const char *body)
{
    char mac[32] = {0};
    j_get_str(body, "mac", mac, sizeof(mac));
    if (!mac[0]) {
        http_json(s, 200, "{\"ok\":false,\"error\":\"缺少 mac\"}");
        return;
    }
    for (int i = 0; i < S.devCount; i++) {
        if (_stricmp(S.dev[i].mac, mac) == 0) {
            for (int k = i; k < S.devCount - 1; k++) S.dev[k] = S.dev[k + 1];
            S.devCount--;
            dev_save();
            log_add(0, "已删除设备记录 %s", mac);
            http_json(s, 200, "{\"ok\":true}");
            return;
        }
    }
    http_json(s, 200, "{\"ok\":false,\"error\":\"设备记录不存在\"}");
}

static void api_device_link(SOCKET s, const char *body)
{
    char mac[32] = {0}, to[32] = {0};
    j_get_str(body, "mac", mac, sizeof(mac));
    j_get_str(body, "aliasTo", to, sizeof(to));
    device_t *d = dev_find(mac, 1);
    if (!d) {
        http_json(s, 200, "{\"ok\":false,\"error\":\"缺少 mac\"}");
        return;
    }
    if (to[0] && !_stricmp(mac, to)) {
        http_json(s, 200, "{\"ok\":false,\"error\":\"不能合并到自己\"}");
        return;
    }
    lstrcpynA(d->aliasTo, to, sizeof(d->aliasTo));
    dev_save();
    http_json(s, 200, "{\"ok\":true}");
}

static void api_autostart(SOCKET s, const char *body, int isPost)
{
    wchar_t cmd[1024] = {0};
    if (isPost) {
        int enable = j_get_bool(body, "enabled", 0);
        int rc = kwifi_autostart_set(enable, cmd, 1024);
        S.cfg.autoStart = enable ? 1 : 0;
        cfg_save();
        if (rc != 0) {
            sb b;
            sb_init(&b, 256);
            sb_puts(&b, "{\"ok\":false,\"error\":");
            sb_json_str(&b, "注册表与启动文件夹都被拒绝，请检查安全软件");
            sb_puts(&b, "}");
            http_json(s, 200, b.p);
            sb_free(&b);
            return;
        }
        sb b;
        sb_init(&b, 512);
        sb_puts(&b, "{\"ok\":true,\"data\":{\"enabled\":");
        sb_puts(&b, enable ? "true" : "false");
        sb_puts(&b, ",\"command\":");
        char u8[1024] = {0};
        WideCharToMultiByte(CP_UTF8, 0, cmd, -1, u8, sizeof(u8), NULL, NULL);
        sb_json_str(&b, enable ? u8 : "");
        sb_puts(&b, ",\"method\":\"run\"}}");
        http_json(s, 200, b.p);
        sb_free(&b);
        return;
    }

    int enabled = kwifi_autostart_get(cmd, 1024);
    sb b;
    sb_init(&b, 512);
    sb_puts(&b, "{\"ok\":true,\"data\":{\"enabled\":");
    sb_puts(&b, enabled ? "true" : "false");
    sb_puts(&b, ",\"command\":");
    char u8[1024] = {0};
    WideCharToMultiByte(CP_UTF8, 0, cmd, -1, u8, sizeof(u8), NULL, NULL);
    sb_json_str(&b, enabled ? u8 : "");
    sb_puts(&b, ",\"method\":\"run\"}}");
    http_json(s, 200, b.p);
    sb_free(&b);
}

/* turning the backend into a Windows service is what removes the UAC prompt */
static void api_service(SOCKET s, const char *body, int isPost)
{
    if (isPost) {
        char action[16] = {0};
        j_get_str(body, "action", action, sizeof(action));

        int enable = !strcmp(action, "install");
        int ok = enable ? kwifi_service_install() : kwifi_service_remove();
        if (!ok) {
            sb e;
            sb_init(&e, 256);
            sb_puts(&e, "{\"ok\":false,\"error\":\"需要管理员权限\"}");
            http_json(s, 200, e.p);
            sb_free(&e);
            return;
        }
        log_add(0, enable ? "已安装 Windows 服务" : "已移除 Windows 服务");
    }

    sb b;
    sb_init(&b, 256);
    sb_putf(&b,
            "{\"ok\":true,\"data\":{\"installed\":%s,\"running\":%s,\"backendIsService\":%s}}",
            kwifi_service_installed() ? "true" : "false",
            kwifi_service_running() ? "true" : "false",
            kwifi_is_service() ? "true" : "false");
    http_json(s, 200, b.p);
    sb_free(&b);
}
/* ask Windows for administrator rights on demand (one UAC dialog) */
static void api_elevate(SOCKET s, const char *body, int isPost)
{
    if (isPost) {
        if (kwifi_is_admin()) {
            http_json(s, 200, "{\"ok\":true,\"data\":{\"admin\":true}}");
            return;
        }
        http_json(s, 200, "{\"ok\":true,\"data\":{\"admin\":false,\"handover\":true}}");
        /* give the answer time to leave the socket, then relaunch elevated */
        Sleep(250);
        kwifi_request_elevation();
        return;
    }
    sb b;
    sb_init(&b, 128);
    sb_putf(&b, "{\"ok\":true,\"data\":{\"admin\":%s}}", kwifi_is_admin() ? "true" : "false");
    http_json(s, 200, b.p);
    sb_free(&b);
}
/* silent elevation: a scheduled task makes UAC ask only once, at install time */
static void api_task(SOCKET s, const char *body, int isPost)
{
    if (isPost) {
        char action[16] = {0};
        j_get_str(body, "action", action, sizeof(action));

        int enable = !strcmp(action, "install");
        int ok = enable ? kwifi_task_install() : kwifi_task_remove();
        if (ok && enable) {
            /* the task has an onlogon trigger; the Run key would start a second copy */
            S.cfg.autoStart = 0;
            cfg_save();
        }
        if (!ok) {
            http_json(s, 200, "{\"ok\":false,\"error\":\"需要管理员权限，且计划任务服务需可用\"}");
            return;
        }
        log_add(0, enable ? "已安装免 UAC 启动任务" : "已移除免 UAC 启动任务");
    }

    int state = kwifi_task_state();
    sb b;
    sb_init(&b, 256);
    sb_puts(&b, "{\"ok\":true,\"data\":{\"installed\":");
    sb_puts(&b, state ? "true" : "false");
    sb_puts(&b, ",\"admin\":");
    sb_puts(&b, S.admin ? "true" : "false");
    sb_puts(&b, "}}");
    http_json(s, 200, b.p);
    sb_free(&b);
}
static void api_quit(SOCKET s, const char *body)
{
    int stop = j_get_bool(body, "stopHotspot", 1);

    /* as a service the backend outlives the window: closing the interface must
       not tear down the hotspot or stop the service */
    if (kwifi_is_service()) {
        log_add(0, "界面已关闭（服务继续运行）");
        http_json(s, 200, "{\"ok\":true,\"data\":{\"service\":true}}");
        return;
    }

    http_json(s, 200, "{\"ok\":true}");
    helper_stop();
    if (stop) {
        job_t *j = ps_run("-Action stop", 0, 45000);
        ps_free(j);
    }
    S.quit = 1;
    kwifi_request_exit();
}

/* ---------------- request dispatch ---------------- */

static void handle_request(SOCKET s)
{
    char buf[16384];
    int len = 0;
    /* headers */
    while (len < (int)sizeof(buf) - 1) {
        int n = recv(s, buf + len, (int)sizeof(buf) - 1 - len, 0);
        if (n <= 0) break;
        len += n;
        buf[len] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[len] = 0;

    char method[8] = {0}, path[256] = {0};
    if (sscanf(buf, "%7s %255s", method, path) != 2) {
        closesocket(s);
        return;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;

    /* body */
    char body[8192] = {0};
    char *hdrEnd = strstr(buf, "\r\n\r\n");
    int contentLen = 0;
    char *cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    if (cl) contentLen = atoi(cl + 15);
    int have = hdrEnd ? (int)(len - (hdrEnd + 4 - buf)) : 0;
    if (hdrEnd && have > 0) {
        int copy = have < (int)sizeof(body) - 1 ? have : (int)sizeof(body) - 1;
        memcpy(body, hdrEnd + 4, copy);
        body[copy] = 0;
    }
    while (hdrEnd && have < contentLen && have < (int)sizeof(body) - 1) {
        int n = recv(s, body + have, (int)sizeof(body) - 1 - have, 0);
        if (n <= 0) break;
        have += n;
        body[have] = 0;
    }

    int isPost = !strcmp(method, "POST");


    if (!strcmp(path, "/api/health")) {
        char out[128];
        _snprintf(out, sizeof(out),
                  "{\"ok\":true,\"pid\":%lu,\"admin\":%s,\"api\":\"4\",\"app\":\"" APP_VERSION "\"}",
                  (unsigned long)GetCurrentProcessId(), S.admin ? "true" : "false");
        http_json(s, 200, out);
        return;
    }
    if (!strcmp(path, "/api/status")) { api_status(s); return; }
    if (!strcmp(path, "/api/events")) { api_events(s); return; }
    if (!strcmp(path, "/api/config")) { api_config(s, isPost ? body : NULL); return; }
    if (!strcmp(path, "/api/doctor")) {
        ps_passthrough(s, "-Action doctor", 0, 60000);
        return;
    }
    if (!strcmp(path, "/api/hotspot")) {
        if (isPost) { api_hotspot(s, body); return; }
        http_json(s, 200, "{\"ok\":false,\"error\":\"POST required\"}");
        return;
    }
    if (!strcmp(path, "/api/hotspot/restart")) {
        job_t *j = ps_run("-Action stop", 0, 90000);
        ps_free(j);
        helper_stop();
        Sleep(1500);
        char args[512];
        _snprintf(args, sizeof(args), "-Action start -Ssid \"%s\" -Password \"%s\" -Band \"%s\"",
                  S.cfg.ssid, S.cfg.password, S.cfg.band);
        ps_passthrough(s, args, 0, 120000);
        log_add(0, "热点已重启");
        poll_once(1);
        return;
    }
    if (!strcmp(path, "/api/network")) { api_network(s, body); return; }
    if (!strcmp(path, "/api/stealth")) { api_stealth(s, body); return; }
    if (!strcmp(path, "/api/autostart")) { api_autostart(s, body, isPost); return; }
    if (!strcmp(path, "/api/task")) { api_task(s, body, isPost); return; }
    if (!strcmp(path, "/api/elevate")) { api_elevate(s, body, isPost); return; }
    if (!strcmp(path, "/api/service")) { api_service(s, body, isPost); return; }
    if (!strcmp(path, "/api/quit")) { api_quit(s, body); return; }
    if (!strcmp(path, "/api/device") || !strcmp(path, "/api/limit")) {
        api_device(s, body, 0);
        return;
    }
    if (!strcmp(path, "/api/block")) { api_device(s, body, 1); return; }
    if (!strcmp(path, "/api/device/delete")) { api_device_delete(s, body); return; }
    if (!strcmp(path, "/api/device/link")) { api_device_link(s, body); return; }

    DWORD rlen = 0;
    const char *rtype = NULL;
    const unsigned char *res = res_lookup(path, &rlen, &rtype);
    if (res) {
        http_send(s, 200, rtype, res, rlen);
        return;
    }
    if (!strncmp(path, "/api/", 5)) {
        http_json(s, 404, "{\"ok\":false,\"error\":\"unknown endpoint\"}");
        return;
    }
    http_json(s, 404, "{\"ok\":false,\"error\":\"not found\"}");
}

static DWORD WINAPI conn_thread(LPVOID param)
{
    SOCKET s = (SOCKET)(ULONG_PTR)param;
    handle_request(s);
    /* SSE sockets must stay open */
    int stillOpen = 0;
    EnterCriticalSection(&g_sseCs);
    for (int i = 0; i < MAX_SSE; i++) if (g_sse[i] == s) stillOpen = 1;
    LeaveCriticalSection(&g_sseCs);
    if (!stillOpen) closesocket(s);
    return 0;
}

static DWORD WINAPI accept_thread(LPVOID param)
{
    SOCKET srv = (SOCKET)(ULONG_PTR)param;
    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (S.quit) break;
            Sleep(50);
            continue;
        }
        DWORD tid = 0;
        HANDLE h = CreateThread(NULL, 0, conn_thread, (LPVOID)(ULONG_PTR)c, 0, &tid);
        if (h) CloseHandle(h);
        else closesocket(c);
    }
    return 0;
}

static DWORD WINAPI poll_thread(LPVOID param)
{
    int tick = 0;
    for (;;) {
        if (S.quit) break;
        poll_once((tick % 4) == 0);
        tick++;

        sb b;
        sb_init(&b, 8192);
        build_status(&b);
        sse_send_all(b.p, b.len);
        sb_free(&b);

        Sleep(POLL_MS);
    }
    return 0;
}

/* boot-time hotspot：等网络栈就绪后再开热点，避免开机瞬间失败 */
static DWORD WINAPI auto_hotspot_thread(LPVOID param)
{
    (void)param;
    Sleep(6000);

    for (int attempt = 0; attempt < 6; attempt++) {
        if (S.quit) return 0;
        if (S.active) {
            log_add(0, "开机自启：热点已在线，跳过");
            return 0;
        }

        char args[512];
        _snprintf(args, sizeof(args),
                  "-Action start -Ssid \"%s\" -Password \"%s\" -Band \"%s\"",
                  S.cfg.ssid, S.cfg.password, S.cfg.band);
        job_t *j = ps_run(args, 0, 120000);
        int ok = 0;
        if (j && j->out) ok = strstr(j->out, "\"ok\":true") != NULL;
        char err[160] = {0};
        if (j && j->out) {
            const char *e = strstr(j->out, "\"error\":");
            if (e) {
                e += 8;
                if (*e == '"') {
                    e++;
                    size_t n = 0;
                    while (*e && *e != '"' && n + 1 < sizeof(err)) err[n++] = *e++;
                    err[n] = 0;
                }
            }
        }
        ps_free(j);

        if (ok) {
            log_add(0, "开机自启：热点已开启 (%s)", S.cfg.ssid);
            poll_once(1);
            return 0;
        }
        log_add(1, "开机自启：热点开启失败（第 %d 次）%s", attempt + 1, err);
        Sleep(5000);
    }
    return 0;
}
/* ---------------- embedded file extraction ---------------- */

static void write_embedded(const wchar_t *dir, const wchar_t *name,
                          const unsigned char *data, DWORD len)
{
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\%s", dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, len, &w, NULL);
    CloseHandle(h);
}

/* the WebView2 loader is embedded once here and unpacked by the shell */
DWORD srv_wv2_loader_size(void)
{
    return res_wv2_loader_len;
}

int srv_write_wv2_loader(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, res_wv2_loader, res_wv2_loader_len, &w, NULL);
    CloseHandle(h);
    return ok && w == res_wv2_loader_len;
}

void srv_start(HINSTANCE inst, const wchar_t *exeDir, const wchar_t *dataDir, int port)
{
    (void)inst;
    lstrcpynW(g_rootDir, exeDir, MAX_PATH);
    lstrcpynW(g_dataDir, dataDir, MAX_PATH);
    if (port > 0) g_port = port;

    CreateDirectoryW(g_dataDir, NULL);

    InitializeCriticalSection(&S.cs);
    InitializeCriticalSection(&Q.cs);
    InitializeCriticalSection(&g_sseCs);
    Q.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (int i = 0; i < MAX_SSE; i++) g_sse[i] = INVALID_SOCKET;

    cfg_load();
    dev_load();

    /* net.ps1 and the data plane live next to the state */
    write_embedded(g_dataDir, L"net.ps1", res_net_ps1, res_net_ps1_len);
    write_beside_data(L"ttlfix.exe", res_ttlfix_exe, res_ttlfix_exe_len);
    write_beside_data(L"WinDivert.dll", res_windivert_dll, res_windivert_dll_len);
    write_beside_data(L"WinDivert64.sys", res_windivert_sys, res_windivert_sys_len);
    write_beside_data(L"WinDivert-LICENSE.txt", res_windivert_lic, res_windivert_lic_len);

    log_add(0, "Kwifi 控制台已启动: http://127.0.0.1:%d", g_port);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_add(2, "网络初始化失败");
        return;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) return;

    /* SO_REUSEADDR (not EXCLUSIVEADDRUSE): a restart right after a kill must not
       fail on the socket lingering in TIME_WAIT */
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_add(2, "端口 %d 已被占用", g_port);
        closesocket(srv);
        return;
    }
    listen(srv, 16);

    DWORD tid = 0;
    HANDLE t1 = CreateThread(NULL, 0, accept_thread, (LPVOID)(ULONG_PTR)srv, 0, &tid);
    if (t1) CloseHandle(t1);
    HANDLE t2 = CreateThread(NULL, 0, ps_worker, NULL, 0, &tid);
    if (t2) CloseHandle(t2);
    HANDLE t3 = CreateThread(NULL, 0, poll_thread, NULL, 0, &tid);
    if (t3) CloseHandle(t3);

    /* first status right away so the window has data */
    poll_once(1);
    log_add(0, "服务已就绪");

    if (S.cfg.autoHotspot) {
        HANDLE t = CreateThread(NULL, 0, auto_hotspot_thread, NULL, 0, &tid);
        if (t) CloseHandle(t);
    }
}

void srv_stop(void)
{
    S.quit = 1;
    helper_stop();
    if (Q.wake) CloseHandle(Q.wake);
    Q.wake = NULL;
}
