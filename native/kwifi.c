/*
 * kwifi.exe - native Windows shell for kwifi.
 *
 * One small program that:
 *   - makes sure the local web service is running (starts it hidden if needed)
 *   - opens the interface in an Edge app window
 *   - lives in the notification area (tray) with a right-click menu:
 *         打开界面 / 重启热点 / 停止热点 / 打开日志目录 / 退出
 *   - detects the interface window being closed and tells the user it is still
 *     running in the tray (that is the "minimise to tray" behaviour)
 *
 * Build (no SDK needed, zig ships the C toolchain):
 *   zig cc -target x86_64-windows-gnu -O2 -o kwifi.exe kwifi.c -lws2_32 -lshell32 -luser32 -lgdi32
 *
 * The web service needs administrator rights; run this program elevated.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <sddl.h>
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <stdarg.h>
#include <winsvc.h>
#include <shlobj.h>

#define APP_TITLE   L"kwifi"

/* native/kgwebview.cpp - the interface window (WebView2, no browser) */
extern HWND kg_webview_open(HINSTANCE inst, const wchar_t *url,
                            const wchar_t *userDataDir, const wchar_t *dllPath);
extern BOOL kg_webview_alive(void);
extern BOOL kg_webview_failed(void);
extern long kg_webview_last_error(void);
extern void kg_webview_show(void);
extern void kg_webview_close(void);
#define TRAY_ID     1
#define WM_TRAYICON (WM_APP + 1)
#define TIMER_ID    1
#define BASE_PATH   L"/api/health"

static unsigned short g_port = 17899;
static HICON g_icon = NULL;
static BOOL g_uiWasOpen = FALSE;
static HWND g_hwnd = NULL;
static wchar_t g_root[MAX_PATH];
static wchar_t g_data[MAX_PATH];
static wchar_t g_exe[MAX_PATH];

static void mkdirs(wchar_t *path);
static HICON load_app_icon(int size);
static BOOL ensure_wv2_loader(wchar_t *out, size_t cch);
static void compute_data_dir(wchar_t *out, size_t cch);
static int dir_writable(const wchar_t *dir);


/* autostart helpers (defined further down) */
static BOOL sid_of_process(DWORD pid, wchar_t *out, size_t cch);
static BOOL interactive_sid(wchar_t *out, size_t cch);
static LONG run_key_write_root(HKEY root, const wchar_t *sid, const wchar_t *command, int enable);
static void startup_approved(const wchar_t *sid, int enable);
static BOOL make_startup_shortcut(void);
static BOOL has_startup_shortcut(void);
static void remove_startup_shortcut(void);
static HINSTANCE g_inst = NULL;

/* ---------------- log file (data\kwifi.log) ---------------- */

static void logf_(const char *fmt, ...)
{
    char path[MAX_PATH * 2];
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_data[0]) _snprintf(path, sizeof(path), "%ls\\kwifi.log", g_data);
    else _snprintf(path, sizeof(path), "%ls\\data\\kwifi.log", g_root);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* the app data folder may not exist yet: fall back to %TEMP% so a boot
           failure is still diagnosable */
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        strcat(tmp, "kwifi.log");
        h = CreateFileA(tmp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[64];
    _snprintf(stamp, sizeof(stamp), "[%04d-%02d-%02d %02d:%02d:%02d] ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD written = 0;
    WriteFile(h, stamp, (DWORD)strlen(stamp), &written, NULL);
    WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
    WriteFile(h, "\r\n", 2, &written, NULL);
    CloseHandle(h);
}

/* ---------------- tiny HTTP client (plain sockets, no proxy surprises) ---------------- */

static BOOL http_call(const wchar_t *verb, const wchar_t *path, const char *body,
                      char *out, DWORD out_size)
{
    if (out && out_size) out[0] = 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return FALSE;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); return FALSE; }

    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        logf_("connect failed (%d)", WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return FALSE;
    }

    char verb_a[8] = {0};
    char path_a[160] = {0};
    wcstombs(verb_a, verb, sizeof(verb_a) - 1);
    wcstombs(path_a, path, sizeof(path_a) - 1);
    DWORD body_len = body ? (DWORD)strlen(body) : 0;

    char req[400];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %lu\r\n\r\n",
                     verb_a, path_a, (unsigned long)body_len);
    if (n > 0) send(s, req, n, 0);
    if (body_len) send(s, body, (int)body_len, 0);

    char resp[1024];
    int total = 0;
    int r;
    while ((r = recv(s, resp + total, (int)sizeof(resp) - 1 - total, 0)) > 0) {
        total += r;
        if (total >= (int)sizeof(resp) - 1) break;
    }
    resp[total] = 0;

    closesocket(s);
    WSACleanup();

    BOOL ok = (strncmp(resp, "HTTP/1.1 2", 10) == 0) || (strncmp(resp, "HTTP/1.0 2", 10) == 0);
    logf_("%s %s -> %d bytes ok=%d", verb_a, path_a, total, ok);
    if (out && out_size) lstrcpynA(out, resp, (int)out_size);
    return ok;
}

static BOOL service_alive(void)
{
    return http_call(L"GET", L"/api/health", NULL, NULL, 0);
}

/* ---------------- service bootstrap ---------------- */

/* the backend now runs in this process (native/server.c) */
static int g_serviceStarted = 0;

static BOOL start_service(void)
{
    if (g_serviceStarted) return TRUE;

    wchar_t dataDir[MAX_PATH * 2];
    if (g_data[0]) lstrcpynW(dataDir, g_data, MAX_PATH * 2);
    else compute_data_dir(dataDir, MAX_PATH * 2);

    int port = 17899;
    wchar_t *penv = _wgetenv(L"KWIFI_PORT");
    if (penv && *penv) port = _wtoi(penv);

    srv_start(g_inst, g_root, dataDir, port);
    g_serviceStarted = 1;
    return TRUE;
}

static BOOL ensure_service(void)
{
    return start_service();
}

int kwifi_autostart_get(wchar_t *out, int cch)
{
    if (out && cch > 0) out[0] = 0;

    wchar_t sid[128] = {0};
    if (!interactive_sid(sid, 128)) return 0;

    wchar_t path[512];
    _snwprintf(path, 512, L"%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", sid);
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_USERS, path, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        wchar_t value[1024] = {0};
        DWORD cb = sizeof(value), type = 0;
        LONG rc = RegQueryValueExW(key, L"Kwifi", NULL, &type, (BYTE *)value, &cb);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && type == REG_SZ) {
            lstrcpynW(out, value, cch);
            return 1;
        }
    }
    if (has_startup_shortcut()) {
        _snwprintf(out, cch, L"\"%s\" --tray", g_exe);
        return 1;
    }
    return 0;
}

int kwifi_autostart_set(int enable, wchar_t *out, int cch)
{
    if (out && cch > 0) out[0] = 0;

    wchar_t sid[128] = {0};
    if (!interactive_sid(sid, 128)) return 1;

    wchar_t command[1024];
    _snwprintf(command, 1024, L"\"%s\" --tray", g_exe);

    if (!enable) {
        remove_startup_shortcut();
        wchar_t own[128] = {0};
        BOOL same = sid_of_process(GetCurrentProcessId(), own, 128) && _wcsicmp(own, sid) == 0;
        if (same) run_key_write_root(HKEY_CURRENT_USER, sid, command, 0);
        run_key_write_root(HKEY_USERS, sid, command, 0);
        return 0;
    }

    wchar_t own[128] = {0};
    BOOL same = sid_of_process(GetCurrentProcessId(), own, 128) && _wcsicmp(own, sid) == 0;
    LONG rc = same ? run_key_write_root(HKEY_CURRENT_USER, sid, command, 1)
                   : run_key_write_root(HKEY_USERS, sid, command, 1);
    if (rc != ERROR_SUCCESS) rc = run_key_write_root(HKEY_USERS, sid, command, 1);
    if (rc == ERROR_SUCCESS) {
        startup_approved(sid, 1);
        remove_startup_shortcut();
        lstrcpynW(out, command, cch);
        return 0;
    }
    if (make_startup_shortcut()) {
        lstrcpynW(out, command, cch);
        return 0;
    }
    return 1;
}

void kwifi_request_exit(void)
{
    if (g_hwnd) PostMessageW(g_hwnd, WM_COMMAND, 5, 0);
}

/* can we actually create a file here? */
static int dir_writable(const wchar_t *dir)
{
    wchar_t probe[MAX_PATH * 2];
    _snwprintf(probe, MAX_PATH * 2, L"%s\\.writable", dir);
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    DeleteFileW(probe);
    return 1;
}
/* where the backend keeps its state (must match server.c) */
static void compute_data_dir(wchar_t *out, size_t cch)
{
    wchar_t *env = _wgetenv(L"KWIFI_DATA");
    if (env && *env) {
        lstrcpynW(out, env, (int)cch);
        return;
    }
    /* settings never sit next to the exe: the program may be run from a read-only
       or portable medium, and one folder keeps upgrades and reinstalls intact.
       KWIFI_DATA overrides it for tests. */
    wchar_t appdata[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) == 0 || !appdata[0]) {
        if (GetEnvironmentVariableW(L"USERPROFILE", appdata, MAX_PATH) == 0) {
            lstrcpynW(appdata, L"C:\\", MAX_PATH);
        }
        lstrcatW(appdata, L"\\AppData\\Local");
    }
    _snwprintf(out, cch, L"%s\\Kwifi\\data", appdata);
}

/* read the "autoHotspot" flag straight from config.json (the backend owns it) */
static BOOL config_auto_hotspot(void)
{
    wchar_t dir[MAX_PATH * 2];
    wchar_t path[MAX_PATH * 2];
    compute_data_dir(dir, MAX_PATH * 2);
    if (!dir[0]) return FALSE;
    _snwprintf(path, MAX_PATH * 2, L"%s\\config.json", dir);

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    char buf[4096];
    DWORD read = 0;
    BOOL on = FALSE;
    if (ReadFile(h, buf, sizeof(buf) - 1, &read, NULL)) {
        buf[read] = 0;
        const char *p = strstr(buf, "\"autoHotspot\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                while (*p == ':' || *p == ' ' || *p == '\t') p++;
                on = (strncmp(p, "true", 4) == 0);
            }
        }
    }
    CloseHandle(h);
    return on;
}
/* ---------------- autostart (HKCU/HKU Run key, no reg.exe) ---------------- */

static BOOL sid_of_process(DWORD pid, wchar_t *out, size_t cch)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return FALSE;

    HANDLE token = NULL;
    BOOL ok = FALSE;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        DWORD need = 0;
        GetTokenInformation(token, TokenUser, NULL, 0, &need);
        if (need) {
            void *buf = malloc(need);
            if (buf && GetTokenInformation(token, TokenUser, buf, need, &need)) {
                TOKEN_USER *tu = (TOKEN_USER *)buf;
                LPWSTR str = NULL;
                if (ConvertSidToStringSidW(tu->User.Sid, &str)) {
                    lstrcpynW(out, str, (int)cch);
                    LocalFree(str);
                    ok = TRUE;
                }
            }
            free(buf);
        }
        CloseHandle(token);
    }
    CloseHandle(proc);
    return ok;
}

/* the desktop user, which is not necessarily this process's user when the app
   was started elevated from another account */
static BOOL interactive_sid(wchar_t *out, size_t cch)
{
    out[0] = 0;

    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (tray) {
        DWORD pid = 0;
        GetWindowThreadProcessId(tray, &pid);
        if (pid && sid_of_process(pid, out, cch)) return TRUE;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    BOOL ok = FALSE;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                if (sid_of_process(pe.th32ProcessID, out, cch)) { ok = TRUE; break; }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

static LONG run_key_write_root(HKEY root, const wchar_t *sid, const wchar_t *command, int enable)
{
    wchar_t path[512];
    if (root == HKEY_CURRENT_USER) {
        lstrcpynW(path, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 512);
    } else {
        _snwprintf(path, 512, L"%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", sid);
    }

    HKEY key = NULL;
    LONG rc = RegOpenKeyExW(root, path, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) return rc;

    if (enable) {
        rc = RegSetValueExW(key, L"Kwifi", 0, REG_SZ,
                            (const BYTE *)command,
                            (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, L"Kwifi");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc;
}

/* an entry can exist but be disabled in Task Manager; clear that flag */
static void startup_approved(const wchar_t *sid, int enable)
{
    wchar_t path[512];
    _snwprintf(path, 512,
               L"%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", sid);

    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_USERS, path, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;

    if (enable) {
        BYTE enabled[12] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        RegSetValueExW(key, L"Kwifi", 0, REG_BINARY, enabled, sizeof(enabled));
    } else {
        RegDeleteValueW(key, L"Kwifi");
    }
    RegCloseKey(key);
}

static wchar_t g_asResult[MAX_PATH * 2];

/* Some security suites and policies block writes to the Run key. A shortcut in
   the user's Startup folder achieves the same "start at login" without touching
   the registry, and is accepted by far more of them. */
static const GUID KWIFI_CLSID_ShellLink =
    { 0x00021401, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID KWIFI_IID_IShellLinkW =
    { 0x000214F9, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID KWIFI_IID_IPersistFile =
    { 0x0000010b, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static BOOL startup_lnk_path(wchar_t *out, DWORD cch)
{
    wchar_t dir[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, dir))) return FALSE;
    _snwprintf(out, cch, L"%s\\kwifi.lnk", dir);
    return TRUE;
}

static BOOL make_startup_shortcut(void)
{
    wchar_t lnk[MAX_PATH * 2];
    if (!startup_lnk_path(lnk, MAX_PATH * 2)) return FALSE;

    BOOL ok = FALSE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return FALSE;

    IShellLinkW *sl = NULL;
    if (SUCCEEDED(CoCreateInstance(&KWIFI_CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &KWIFI_IID_IShellLinkW, (void **)&sl))) {
        sl->lpVtbl->SetPath(sl, g_exe);
        sl->lpVtbl->SetArguments(sl, L"--tray");
        sl->lpVtbl->SetDescription(sl, L"kwifi");
        sl->lpVtbl->SetWorkingDirectory(sl, g_root);

        IPersistFile *pf = NULL;
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &KWIFI_IID_IPersistFile, (void **)&pf))) {
            ok = SUCCEEDED(pf->lpVtbl->Save(pf, lnk, TRUE));
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
}

static BOOL has_startup_shortcut(void)
{
    wchar_t lnk[MAX_PATH * 2];
    if (!startup_lnk_path(lnk, MAX_PATH * 2)) return FALSE;
    return GetFileAttributesW(lnk) != INVALID_FILE_ATTRIBUTES;
}

static void remove_startup_shortcut(void)
{
    wchar_t lnk[MAX_PATH * 2];
    if (!startup_lnk_path(lnk, MAX_PATH * 2)) return;
    DeleteFileW(lnk);
}
static void autostart_result(int ok, const wchar_t *error, const wchar_t *command, const wchar_t *hive)
{
    wchar_t dir[MAX_PATH * 2];
    if (g_asResult[0]) {
        lstrcpynW(dir, g_asResult, MAX_PATH * 2);
    } else {
        if (g_data[0]) lstrcpynW(dir, g_data, MAX_PATH * 2);
        else _snwprintf(dir, MAX_PATH * 2, L"%s\\data", g_root);
    }
    mkdirs(dir);

    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%s\\autostart.txt", dir);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[2048];
    char cmd[1024] = {0};
    char hv[256] = {0};
    char er[512] = {0};
    WideCharToMultiByte(CP_UTF8, 0, command ? command : L"", -1, cmd, sizeof(cmd), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, hive ? hive : L"", -1, hv, sizeof(hv), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, error ? error : L"", -1, er, sizeof(er), NULL, NULL);

    /* diagnostic: integrity level, elevation and restricted SID count tell us
       whether this process may write the Run key at all */
    HANDLE tok = NULL;
    DWORD integrity = 0, elevated = 0, restricted = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD need = 0, got = 0;
        BYTE *pub = NULL;
        if (GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &need) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            pub = (BYTE *)malloc(need);
            if (pub && GetTokenInformation(tok, TokenIntegrityLevel, pub, need, &got)) {
                TOKEN_MANDATORY_LABEL *ml = (TOKEN_MANDATORY_LABEL *)pub;
                DWORD *sub = GetSidSubAuthority(ml->Label.Sid,
                                                *GetSidSubAuthorityCount(ml->Label.Sid) - 1);
                if (sub) integrity = *sub;
            }
            free(pub);
        }
        if (!GetTokenInformation(tok, TokenElevation, &elevated, sizeof(elevated), &got)) elevated = 0;
        if (!GetTokenInformation(tok, TokenRestrictedSids, NULL, 0, &need)) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && need >= sizeof(DWORD)) {
                DWORD *cnt = (DWORD *)malloc(need);
                if (cnt && GetTokenInformation(tok, TokenRestrictedSids, cnt, need, &got)) restricted = *cnt;
                free(cnt);
            }
        }
        CloseHandle(tok);
    }

    int n = _snprintf(buf, sizeof(buf),
                      "ok=%d\ncommand=%s\nhive=%s\nerror=%s\nintegrity=%lu\nelevated=%lu\nrestricted=%lu\n",
                      ok, cmd, hv, er, (unsigned long)integrity, (unsigned long)elevated,
                      (unsigned long)restricted);
    DWORD written = 0;
    WriteFile(h, buf, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

static int do_autostart(const wchar_t *mode)
{
    wchar_t sid[128] = {0};
    if (!interactive_sid(sid, 128)) {
        if (!sid_of_process(GetCurrentProcessId(), sid, 128)) {
            autostart_result(0, L"无法确定当前用户", L"", L"");
            return 1;
        }
    }

    wchar_t hive[512];
    _snwprintf(hive, 512, L"HKU\\%s", sid);

    if (_wcsicmp(mode, L"status") == 0) {
        wchar_t path[512];
        _snwprintf(path, 512, L"%s\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", sid);
        HKEY key = NULL;
        wchar_t value[1024] = {0};
        DWORD cb = sizeof(value);
        DWORD type = 0;
        LONG rc = RegOpenKeyExW(HKEY_USERS, path, 0, KEY_QUERY_VALUE, &key);
        if (rc == ERROR_SUCCESS) {
            rc = RegQueryValueExW(key, L"Kwifi", NULL, &type, (BYTE *)value, &cb);
            RegCloseKey(key);
        }
        if (rc == ERROR_SUCCESS && type == REG_SZ) {
            autostart_result(1, L"", value, hive);
            return 0;
        }
        if (has_startup_shortcut()) {
            wchar_t cmd[1024];
            _snwprintf(cmd, 1024, L"\"%s\" --tray", g_exe);
            autostart_result(1, L"", cmd, L"Startup");
            return 0;
        }
        autostart_result(0, L"", L"", hive);
        return 0;
    }

    int enable = _wcsicmp(mode, L"on") == 0;

    /* diagnostic hook used by the tests: KWIFI_AS_FORCE=startup skips the Run key */
    const char *force = getenv("KWIFI_AS_FORCE");
    if (enable && force && strcmp(force, "startup") == 0) {
        wchar_t cmd2[1024];
        _snwprintf(cmd2, 1024, L"\"%s\" --tray", g_exe);
        if (make_startup_shortcut()) {
            autostart_result(1, L"", cmd2, L"Startup");
            return 0;
        }
        autostart_result(0, L"启动文件夹写入失败", cmd2, L"Startup");
        return 1;
    }
    wchar_t command[1024];
    _snwprintf(command, 1024, L"\"%s\" --tray", g_exe);

    /* disabling must clear every mechanism, not just the one that answers */
    if (!enable) remove_startup_shortcut();

    wchar_t own[128] = {0};
    BOOL sameUser = sid_of_process(GetCurrentProcessId(), own, 128) && _wcsicmp(own, sid) == 0;

    LONG rc;
    if (sameUser) {
        /* the normal case: HKEY_CURRENT_USER is the desktop user's hive */
        rc = run_key_write_root(HKEY_CURRENT_USER, sid, command, enable);
        if (rc == ERROR_SUCCESS) lstrcpynW(hive, L"HKCU", 512);
    } else {
        rc = run_key_write_root(HKEY_USERS, sid, command, enable);
    }

    if (rc != ERROR_SUCCESS) {
        /* fall back to the other hive view before giving up */
        rc = run_key_write_root(HKEY_USERS, sid, command, enable);
        if (rc == ERROR_SUCCESS) {
            _snwprintf(hive, 512, L"HKU\\%s", sid);
        }
    }

    if (rc != ERROR_SUCCESS) {
        if (!enable) {
            /* nothing in the registry: still clean up a shortcut */
            remove_startup_shortcut();
            autostart_result(1, L"", L"", hive);
            return 0;
        }
        if (make_startup_shortcut()) {
            startup_approved(sid, 0);
            autostart_result(1, L"", command, L"Startup");
            return 0;
        }
        wchar_t msg[320];
        if (rc == ERROR_ACCESS_DENIED) {
            _snwprintf(msg, 320,
                       L"拒绝访问（注册表被安全软件/策略拦截，且启动文件夹也写入失败，代码 %ld）", rc);
        } else {
            _snwprintf(msg, 320, L"注册表错误代码 %ld", rc);
        }
        autostart_result(0, msg, command, hive);
        return 1;
    }

    startup_approved(sid, enable);
    if (enable) remove_startup_shortcut();
    autostart_result(1, L"", enable ? command : L"", hive);
    return 0;
}
/* ---------------- interface window ---------------- */

static void open_ui(void)
{
    if (kg_webview_alive()) {
        kg_webview_show();
        logf_("open_ui: existing window raised");
        return;
    }

    wchar_t url[128];
    _snwprintf(url, 128, L"http://127.0.0.1:%u/", (unsigned)g_port);

    /* WebView2 needs a writable profile folder. Keep it inside the data folder,
       which is known to be writable (the backend already writes there), and only
       fall back to LOCALAPPDATA when that cannot be created. */
    wchar_t profile[MAX_PATH * 2];
    _snwprintf(profile, MAX_PATH * 2, L"%s\\webview", g_data);
    mkdirs(profile);
    if (GetFileAttributesW(profile) == INVALID_FILE_ATTRIBUTES) {
        wchar_t appdata[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0 && appdata[0]) {
            _snwprintf(profile, MAX_PATH * 2, L"%s\\Kwifi\\webview", appdata);
            mkdirs(profile);
        }
    }
    logf_("open_ui: profile %ls", profile);

    /* the loader ships inside this exe. A previous run may have left a truncated
       copy behind, so check the size rather than mere existence, and try the
       temp folder before giving up: a failed write used to surface much later as
       "cannot find WebView2Loader.dll" with the window already on screen. */
    wchar_t loader[MAX_PATH * 2];
    if (!ensure_wv2_loader(loader, MAX_PATH * 2)) {
        logf_("open_ui: webview loader unavailable in %ls", g_data);
        MessageBoxW(NULL,
                    L"无法解出 WebView2Loader.dll（界面组件）。\n\n"
                    L"可能是数据目录不可写，或被杀毒软件拦截。\n"
                    L"请确认以下目录可写：\n",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return;
    }

    logf_("open_ui: creating webview window");
    HWND ui = kg_webview_open(g_inst, url, profile, loader);

    /* WebView2 fails with E_UNEXPECTED when the profile folder is locked by a
       leftover process; a fresh profile is the fix, and it beats a blank window */
    for (int i = 0; i < 80 && !kg_webview_alive() && !kg_webview_failed(); i++) Sleep(100);

    /* the WebView2 callback never arriving looks exactly like a blank window;
       treat eight seconds of silence as a failure so the user gets a reason */
    if (!kg_webview_alive() && !kg_webview_failed()) {
        wchar_t msg[1024];
        _snwprintf(msg, 1024,
                   L"WebView2 在 8 秒内没有初始化完成。\n\n"
                   L"请确认已安装 Microsoft Edge WebView2 Runtime（Windows 11 自带）。\n"
                   L"若反复出现，删除下面的目录后重启程序：\n%ls\n",
                   profile);
        MessageBoxW(NULL, msg, APP_TITLE, MB_ICONERROR | MB_OK);
        logf_("open_ui: webview did not initialise in 8s");
        return;
    }

    if (kg_webview_failed()) {
        logf_("open_ui: webview failed (0x%08lX), retrying with a fresh profile",
              (unsigned long)kg_webview_last_error());
        kg_webview_close();

        wchar_t fresh[MAX_PATH * 2];
        _snwprintf(fresh, MAX_PATH * 2, L"%s-%lu", profile, (unsigned long)GetCurrentProcessId());
        mkdirs(fresh);
        ui = kg_webview_open(g_inst, url, fresh, loader);
        for (int i = 0; i < 25 && !kg_webview_alive() && !kg_webview_failed(); i++) Sleep(100);
    }

    if (kg_webview_failed()) {
        wchar_t msg[1024];
        _snwprintf(msg, 1024,
                   L"界面组件启动失败（0x%08lX）。\n\n"
                   L"服务本身已经运行，可以稍后重试。\n"
                   L"若反复失败，请删除下面的目录后重启程序：\n%ls\n",
                   (unsigned long)kg_webview_last_error(), profile);
        MessageBoxW(NULL, msg, APP_TITLE, MB_ICONERROR | MB_OK);
        return;
    }

    if (ui) {
        HICON big = load_app_icon(32);
        HICON small = load_app_icon(16);
        if (big) SendMessageW(ui, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (small) SendMessageW(ui, WM_SETICON, ICON_SMALL, (LPARAM)small);
    }
}
static BOOL ui_window_exists(void)
{
    return kg_webview_alive() ? TRUE : FALSE;
}

/* closing the shell must not leave the interface window behind */
static void close_ui(void)
{
    if (kg_webview_alive()) kg_webview_close();
}
/* ---------------- tray icon ---------------- */

static HICON make_icon(int size)
{
    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(NULL);
    void *bits = NULL;
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color || !bits) return LoadIconW(NULL, IDI_APPLICATION);

    unsigned char *px = (unsigned char *)bits;
    double c = (size - 1) / 2.0;
    double outer = size * 0.48;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            unsigned char *p = px + ((size_t)y * size + x) * 4;
            double d = sqrt((x - c) * (x - c) + (y - c) * (y - c));
            int alpha = 0;
            if (d <= outer - 1.0) alpha = 255;
            else if (d <= outer) alpha = (int)((outer - d) * 255);

            double t = (double)(x + y) / (2.0 * (size - 1));
            int r = (int)(91 + (160 - 91) * t);
            int g = (int)(140 + (107 - 140) * t);
            int b = 255;
            if (d <= size * 0.18) { r = 255; g = 255; b = 255; }

            p[0] = (unsigned char)b;   /* B */
            p[1] = (unsigned char)g;   /* G */
            p[2] = (unsigned char)r;   /* R */
            p[3] = (unsigned char)alpha;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;

    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static void add_tray(HWND hwnd)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, APP_TITLE, 64);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        logf_("tray: Shell_NotifyIcon failed (%lu)", (unsigned long)GetLastError());
    } else {
        logf_("tray: icon added");
    }
}

static void balloon(HWND hwnd, const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, 64);
    lstrcpynW(nid.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ---------------- menu ---------------- */

static void show_menu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 2, L"重启热点");
    AppendMenuW(menu, MF_STRING, 3, L"停止热点");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 4, L"打开日志目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 5, L"退出");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static void do_command(HWND hwnd, int id)
{
    switch (id) {
    case 1:
        open_ui();
        break;
    case 2:
        balloon(hwnd, APP_TITLE, L"正在重启热点…");
        http_call(L"POST", L"/api/hotspot/restart", "{}", NULL, 0);
        break;
    case 3:
        http_call(L"POST", L"/api/hotspot", "{\"action\":\"stop\"}", NULL, 0);
        balloon(hwnd, APP_TITLE, L"热点已停止");
        break;
    case 4: {
        /* open the folder that actually holds the settings */
        wchar_t dir[MAX_PATH];
        if (g_data[0]) lstrcpynW(dir, g_data, MAX_PATH);
        else _snwprintf(dir, MAX_PATH, L"%s\\data", g_root);
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case 5:
        close_ui();
        http_call(L"POST", L"/api/quit", "{\"stopHotspot\":true}", NULL, 0);
        DestroyWindow(hwnd);
        break;
    }
}

/* ---------------- window procedure ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) show_menu(hwnd);
        else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) open_ui();
        return 0;

    case WM_COMMAND:
        do_command(hwnd, LOWORD(wParam));
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            /* the web UI asked to quit: the server writes a flag file */
            wchar_t quitFile[MAX_PATH * 2];
            if (g_data[0]) _snwprintf(quitFile, MAX_PATH * 2, L"%s\\.quit", g_data);
    else _snwprintf(quitFile, MAX_PATH * 2, L"%s\\data\\.quit", g_root);
            if (GetFileAttributesW(quitFile) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(quitFile);
                close_ui();
                DestroyWindow(hwnd);
                return 0;
            }

            BOOL open = ui_window_exists();
            if (g_uiWasOpen && !open) {
                balloon(hwnd, APP_TITLE,
                        L"界面已关闭，热点仍在右下角运行。右键图标可打开界面或退出。");
            }
            g_uiWasOpen = open;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL already_running(void)
{
    /* Local\ works for a non-elevated launch too; Global\ needs SeCreateGlobalPrivilege */
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\KwifiShell");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"KwifiTrayWnd", NULL);
        if (existing) PostMessageW(existing, WM_COMMAND, 1, 0);
        return TRUE;
    }
    return FALSE;
}

/* ---------------- elevation ---------------- */

static BOOL is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION info;
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    BOOL ok = GetTokenInformation(token, TokenElevation, &info, sizeof(info), &len) && info.TokenIsElevated;
    CloseHandle(token);
    return ok;
}


static void ensure_parent_dirs(wchar_t *path)
{
    for (wchar_t *p = path + 3; *p; p++) {
        if (*p == L'\\') {
            wchar_t saved = *p;
            *p = 0;
            CreateDirectoryW(path, NULL);
            *p = saved;
        }
    }
}

/* the WebView2 loader is embedded; write it somewhere usable and return the path */
static BOOL ensure_wv2_loader(wchar_t *out, size_t cch)
{
    wchar_t candidate[3][MAX_PATH * 2];
    int n = 0;

    /* next to the exe first: a source checkout already has it there */
    _snwprintf(candidate[n++], MAX_PATH * 2, L"%s\\WebView2Loader.dll", g_root);
    if (g_data[0]) _snwprintf(candidate[n++], MAX_PATH * 2, L"%s\\WebView2Loader.dll", g_data);
    {
        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        _snwprintf(candidate[n++], MAX_PATH * 2, L"%sKwifi-webview\\WebView2Loader.dll", tmp);
    }

    for (int i = 0; i < n; i++) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(candidate[i], GetFileExInfoStandard, &info) &&
            info.nFileSizeLow == srv_wv2_loader_size()) {
            lstrcpynW(out, candidate[i], (int)cch);
            return TRUE;
        }

        /* writes into a folder that does not exist yet would fail silently */
        wchar_t dir[MAX_PATH * 2];
        lstrcpynW(dir, candidate[i], MAX_PATH * 2);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) { *slash = 0; mkdirs(dir); }

        if (srv_write_wv2_loader(candidate[i]) &&
            GetFileAttributesExW(candidate[i], GetFileExInfoStandard, &info) &&
            info.nFileSizeLow == srv_wv2_loader_size()) {
            lstrcpynW(out, candidate[i], (int)cch);
            logf_("loader: using %ls", candidate[i]);
            return TRUE;
        }
        logf_("webview loader write failed: %ls (err %lu)", candidate[i],
              (unsigned long)GetLastError());
    }
    return FALSE;
}
/* the icon lives in the executable (written by respatch); fall back to the
   generated one when a build skipped the resource step */
static HICON load_app_icon(int size)
{
    HINSTANCE mod = GetModuleHandleW(NULL);
    HICON h = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                size, size, LR_DEFAULTCOLOR | LR_SHARED);
    if (!h) h = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  0, 0, LR_DEFAULTCOLOR | LR_SHARED | LR_DEFAULTSIZE);
    if (!h) h = make_icon(size);
    return h;
}
static void mkdirs(wchar_t *path)
{
    /* SHCreateDirectoryExW builds every missing level; the hand rolled loop
       silently failed for paths under %LOCALAPPDATA% */
    if (SHCreateDirectoryExW(NULL, path, NULL) != ERROR_SUCCESS) {
        CreateDirectoryW(path, NULL);
    }
}

/* ---------------- windows service ----------------
 *
 * Running the backend as a service is what removes the UAC prompt from normal
 * use: the service is installed once (that single prompt is unavoidable), then
 * it starts at boot as LocalSystem and the interface connects over loopback as a
 * plain user. Nothing in the daily flow needs elevation again.
 */

#define KWIFI_SVC_NAME    L"kwifi"
#define KWIFI_SVC_DISPLAY L"kwifi hotspot service"
#define KWIFI_SVC_DESC    L"kwifi backend. Controls the hotspot, per-device limits and the device list. The interface connects to it on 127.0.0.1."

static SERVICE_STATUS_HANDLE g_svcHandle = NULL;
static SERVICE_STATUS g_svcStatus;
static HANDLE g_svcStop = NULL;
static int g_runningAsService = 0;

static void svc_report(DWORD state, DWORD exitCode, DWORD waitHint)
{
    static DWORD checkpoint = 1;

    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwCurrentState = state;
    g_svcStatus.dwControlsAccepted =
        (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
            ? 0
            : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    g_svcStatus.dwWin32ExitCode = exitCode;
    g_svcStatus.dwServiceSpecificExitCode = 0;
    g_svcStatus.dwCheckPoint =
        (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? checkpoint++ : 0;
    g_svcStatus.dwWaitHint = waitHint;

    if (g_svcHandle) SetServiceStatus(g_svcHandle, &g_svcStatus);
}

static DWORD WINAPI svc_ctrl(DWORD control, DWORD eventType, void *data, void *context)
{
    (void)eventType;
    (void)data;
    (void)context;

    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        svc_report(SERVICE_STOP_PENDING, NO_ERROR, 8000);
        if (g_svcStop) SetEvent(g_svcStop);
    } else if (control == SERVICE_CONTROL_INTERROGATE) {
        svc_report(g_svcStatus.dwCurrentState, NO_ERROR, 0);
    }
    return NO_ERROR;
}

/* service state lives under ProgramData so every user shares it */
static void service_data_dir(wchar_t *out, size_t cch)
{
    wchar_t *env = _wgetenv(L"KWIFI_DATA");
    if (env && *env) {
        lstrcpynW(out, env, (int)cch);
        return;
    }

    wchar_t common[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, common))) {
        if (GetEnvironmentVariableW(L"ProgramData", common, MAX_PATH) == 0) {
            lstrcpynW(common, L"C:\\ProgramData", MAX_PATH);
        }
    }
    _snwprintf(out, cch, L"%s\\Kwifi\\data", common);
}

static void WINAPI svc_main(DWORD argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;

    g_runningAsService = 1;
    g_svcHandle = RegisterServiceCtrlHandlerExW(KWIFI_SVC_NAME, svc_ctrl, NULL);
    if (!g_svcHandle) return;

    svc_report(SERVICE_START_PENDING, NO_ERROR, 8000);

    g_svcStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_svcStop) {
        svc_report(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    wchar_t dataDir[MAX_PATH * 2];
    service_data_dir(dataDir, MAX_PATH * 2);
    mkdirs(dataDir);

    wchar_t exeDir[MAX_PATH];
    lstrcpynW(exeDir, g_exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = 0;

    srv_start(NULL, exeDir, dataDir, 17899);

    svc_report(SERVICE_RUNNING, NO_ERROR, 0);
    WaitForSingleObject(g_svcStop, INFINITE);

    srv_stop();
    CloseHandle(g_svcStop);
    g_svcStop = NULL;
    svc_report(SERVICE_STOPPED, NO_ERROR, 0);
}

int kwifi_is_service(void)
{
    return g_runningAsService;
}

/* ---------------- service install / remove / query ---------------- */

static int service_installed(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    SC_HANDLE svc = OpenServiceW(scm, KWIFI_SVC_NAME, SERVICE_QUERY_STATUS);
    int found = svc ? 1 : 0;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return found;
}

static int service_running_state(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    SC_HANDLE svc = OpenServiceW(scm, KWIFI_SVC_NAME, SERVICE_QUERY_STATUS);
    int running = 0;
    if (svc) {
        SERVICE_STATUS st;
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) running = 1;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

/* copy the settings this user already has into the machine-wide folder */
static void service_migrate_data(void)
{
    wchar_t to[MAX_PATH * 2];
    wchar_t appdata[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    if (!appdata[0]) return;

    service_data_dir(to, MAX_PATH * 2);
    mkdirs(to);

    const wchar_t *files[] = { L"config.json", L"devices.json" };
    for (int i = 0; i < 2; i++) {
        wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2];
        _snwprintf(src, MAX_PATH * 2, L"%s\\Kwifi\\data\\%s", appdata, files[i]);
        if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) continue;

        _snwprintf(dst, MAX_PATH * 2, L"%s\\%s", to, files[i]);
        if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) continue;

        if (CopyFileW(src, dst, TRUE)) logf_("migrated %ls", dst);
    }
}

static int service_install(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        wprintf(L"error: cannot open the service manager (%lu). Run as administrator.\n",
                (unsigned long)GetLastError());
        return 1;
    }

    wchar_t binPath[MAX_PATH * 2];
    _snwprintf(binPath, MAX_PATH * 2, L"\"%s\" --service", g_exe);

    SC_HANDLE svc = CreateServiceW(
        scm, KWIFI_SVC_NAME, KWIFI_SVC_DISPLAY,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, binPath, NULL, NULL, NULL, NULL, NULL);

    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, KWIFI_SVC_NAME, SERVICE_ALL_ACCESS);
        if (svc) {
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                 SERVICE_NO_CHANGE, binPath, NULL, NULL, NULL, NULL, NULL, NULL);
        }
    }

    int rc = 0;
    if (!svc) {
        wprintf(L"error: CreateService failed (%lu)\n", (unsigned long)GetLastError());
        rc = 1;
    } else {
        SERVICE_DESCRIPTIONW desc;
        desc.lpDescription = (LPWSTR)KWIFI_SVC_DESC;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        /* delayed start would leave the hotspot unavailable right after boot */
        SERVICE_DELAYED_AUTO_START_INFO delayed;
        delayed.fDelayedAutostart = FALSE;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);

        service_migrate_data();

        /* the tray should come back at login; it does not need admin */
        kwifi_autostart_set(1, NULL, 0);

        if (!StartServiceW(svc, 0, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING) {
                wprintf(L"warning: StartService failed (%lu); start it from services.msc\n",
                        (unsigned long)err);
            }
        } else {
            wprintf(L"service started\n");
        }
        CloseServiceHandle(svc);
        wprintf(L"service installed: %ls\n", KWIFI_SVC_NAME);
    }

    CloseServiceHandle(scm);
    return rc;
}

static int service_remove(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        wprintf(L"error: run as administrator\n");
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, KWIFI_SVC_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        wprintf(L"service is not installed\n");
        CloseServiceHandle(scm);
        return 0;
    }

    SERVICE_STATUS st;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        wprintf(L"stopping...\n");
        for (int i = 0; i < 40; i++) {
            Sleep(250);
            if (!QueryServiceStatus(svc, &st)) break;
            if (st.dwCurrentState == SERVICE_STOPPED) break;
        }
    }

    int rc = DeleteService(svc) ? 0 : 1;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    kwifi_autostart_set(0, NULL, 0);
    wprintf(L"%s\n", rc == 0 ? L"service removed" : L"service removal failed");
    return rc;
}
/* ---------------- silent elevation via a scheduled task ----------------
 *
 * UAC only ever asks for consent once: when the task is created. Afterwards the
 * task runs the program with highest privileges and Windows does not prompt, so
 * neither the UAC dialog nor its "unknown publisher" line appears again.
 *
 * This is the only free way to get rid of the dialog without a code signing
 * certificate. It is opt-in: the app keeps working exactly as before if the
 * user never installs the task.
 */

#define KWIFI_TASK_NAME L"Kwifi\\KwifiElevated"

static BOOL run_and_wait(wchar_t *cmdline, DWORD timeoutMs)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        return FALSE;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

/* the task always starts with "--tray"; a manual launch drops this marker so the
   elevated instance knows it should show the window */
static void showui_marker_path(wchar_t *out, size_t cch)
{
    _snwprintf(out, cch, L"%s\\.showui", g_data);
}

static void showui_marker_write(void)
{
    wchar_t path[MAX_PATH * 2];
    showui_marker_path(path, MAX_PATH * 2);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

/* TRUE when a fresh marker exists; the marker is consumed */
static BOOL showui_marker_take(void)
{
    wchar_t path[MAX_PATH * 2];
    showui_marker_path(path, MAX_PATH * 2);

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return FALSE;

    ULARGE_INTEGER ft;
    ft.LowPart = info.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = info.ftLastWriteTime.dwHighDateTime;
    ULARGE_INTEGER now;
    GetSystemTimeAsFileTime((FILETIME *)&now);
    /* 60 s in 100 ns units */
    BOOL fresh = (now.QuadPart > ft.QuadPart) && (now.QuadPart - ft.QuadPart < 600000000LL);

    DeleteFileW(path);
    return fresh;
}

static BOOL task_exists(void)
{
    wchar_t cmd[1024];
    _snwprintf(cmd, 1024, L"schtasks.exe /query /tn \"%s\"", KWIFI_TASK_NAME);
    return run_and_wait(cmd, 15000);
}

static BOOL task_install(void)
{
    wchar_t cmd[2048];
    /* /rl highest is what removes the prompt: the elevation happens when the
       task is registered, not when it runs */
    _snwprintf(cmd, 2048,
               L"schtasks.exe /create /tn \"%s\" /tr \"\\\"%s\\\" --elevated --tray\" "
               L"/sc onlogon /rl highest /f",
               KWIFI_TASK_NAME, g_exe);
    if (!run_and_wait(cmd, 30000)) return FALSE;
    return task_exists();
}

static BOOL task_remove(void)
{
    wchar_t cmd[1024];
    _snwprintf(cmd, 1024, L"schtasks.exe /delete /tn \"%s\" /f", KWIFI_TASK_NAME);
    return run_and_wait(cmd, 20000);
}

/* start the elevated instance without any prompt */
static BOOL task_run(void)
{
    wchar_t cmd[1024];
    _snwprintf(cmd, 1024, L"schtasks.exe /run /tn \"%s\"", KWIFI_TASK_NAME);
    return run_and_wait(cmd, 20000);
}
/* ---------------- entry point ---------------- */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    (void)show;
    g_inst = inst;


    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wcscpy(g_exe, exe_path);

    /* --tray: start silently in the notification area (used by autostart) */
    BOOL tray_only = wcsstr(cmdline, L"--tray") != NULL;

    /* the port is shared by the backend and the window; keep them in sync */
    {
        wchar_t *penv = _wgetenv(L"KWIFI_PORT");
        if (penv && *penv) g_port = (unsigned short)_wtoi(penv);
    }

    /* resolve the install data first: the elevation decision and every log line
       below depend on it */
    wcscpy(g_root, exe_path);
    {
        wchar_t *slash = wcsrchr(g_root, L'\\');
        if (slash) *slash = 0;
    }
    {
        wchar_t data_dir[MAX_PATH * 2];
        compute_data_dir(data_dir, MAX_PATH * 2);
        mkdirs(data_dir);

        /* The profile folder may be read-only, controlled by antivirus, or on a
           blocked drive. If it cannot be written the program still has to work,
           so try the folder next to the exe (usually a different drive) before
           falling back to TEMP, which is normally on the same blocked drive. */
        if (!dir_writable(data_dir)) {
            wchar_t beside[MAX_PATH * 2];
            _snwprintf(beside, MAX_PATH * 2, L"%s\\data", g_root);
            mkdirs(beside);
            if (dir_writable(beside)) {
                lstrcpynW(data_dir, beside, MAX_PATH * 2);
                logf_("data dir fallback: %ls", data_dir);
            } else {
                wchar_t tmp[MAX_PATH] = {0};
                GetTempPathW(MAX_PATH, tmp);
                _snwprintf(data_dir, MAX_PATH * 2, L"%sKwifi-data", tmp);
                mkdirs(data_dir);
                logf_("data dir fallback: %ls", data_dir);
            }
        }
        lstrcpynW(g_data, data_dir, MAX_PATH);
    }

    /* ---- service entry points ---------------------------------------- */
    if (wcsstr(cmdline, L"--service") && !wcsstr(cmdline, L"--service-")) {
        SERVICE_TABLE_ENTRYW table[] = {
            { (LPWSTR)KWIFI_SVC_NAME, svc_main },
            { NULL, NULL }
        };
        StartServiceCtrlDispatcherW(table);
        return 0;
    }
    {
        wchar_t *a = wcsstr(cmdline, L"--install-service");
        if (a) return service_install();

        a = wcsstr(cmdline, L"--uninstall-service");
        if (a) return service_remove();

        a = wcsstr(cmdline, L"--service-status");
        if (a) {
            int inst = service_installed();
            int run = service_running_state();
            wprintf(L"%s\n", inst ? (run ? L"installed running" : L"installed stopped")
                                  : L"not installed");
            return inst ? 0 : 1;
        }
    }

    /* ---- self test: written for "double click does nothing" -------------
       Collects everything needed to diagnose a silent start failure into
       data\selftest.txt and shows the same text, so a user can just report it.
    */
    if (wcsstr(cmdline, L"--selftest")) {
        wchar_t report[8192];
        int n = 0;
        wchar_t probe[MAX_PATH * 2];

        n += _snwprintf(report + n, 8192 - n,
                        L"kwifi self test\n"
                        L"================\n"
                        L"exe      : %ls\n"
                        L"elevated : %ls\n"
                        L"data dir : %ls\n",
                        g_exe, is_elevated() ? L"yes" : L"NO (not administrator)",
                        g_data);

        /* data dir writable? */
        _snwprintf(probe, MAX_PATH * 2, L"%s\\.selftest", g_data);
        HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, NULL);
        int writable = (h != INVALID_HANDLE_VALUE);
        if (writable) { CloseHandle(h); DeleteFileW(probe); }
        n += _snwprintf(report + n, 8192 - n, L"writable : %ls\n",
                        writable ? L"yes" : L"NO - settings cannot be saved");

        /* WebView2 loader */
        wchar_t loader[MAX_PATH * 2];
        int haveLoader = ensure_wv2_loader(loader, MAX_PATH * 2);
        n += _snwprintf(report + n, 8192 - n, L"loader   : %ls\n",
                        haveLoader ? loader : L"MISSING");

        /* WebView2 runtime version from the registry */
        {
            wchar_t ver[128] = {0};
            DWORD cb = sizeof(ver);
            const wchar_t *key =
                L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
                L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
            LONG rc = RegGetValueW(HKEY_LOCAL_MACHINE, key, L"pv", RRF_RT_REG_SZ,
                                   NULL, ver, &cb);
            if (rc != ERROR_SUCCESS) {
                cb = sizeof(ver);
                rc = RegGetValueW(HKEY_CURRENT_USER,
                                  L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\"
                                  L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
                                  L"pv", RRF_RT_REG_SZ, NULL, ver, &cb);
            }
            n += _snwprintf(report + n, 8192 - n, L"webview2 : %ls\n",
                            rc == ERROR_SUCCESS ? ver : L"NOT INSTALLED");
        }

        /* is another instance already running? that would make a second start
           exit silently */
        HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\KwifiShell");
        n += _snwprintf(report + n, 8192 - n, L"instance : %ls\n",
                        existing ? L"already running (second start exits silently)"
                                 : L"none");
        if (existing) CloseHandle(existing);

        /* can the port be bound? */
        WSADATA wsa;
        int portOk = 0;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                BOOL yes = TRUE;
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
                struct sockaddr_in a;
                ZeroMemory(&a, sizeof(a));
                a.sin_family = AF_INET;
                a.sin_port = htons(g_port);
                a.sin_addr.s_addr = inet_addr("127.0.0.1");
                portOk = bind(s, (struct sockaddr *)&a, sizeof(a)) == 0;
                closesocket(s);
            }
            WSACleanup();
        }
        n += _snwprintf(report + n, 8192 - n, L"port %u   : %ls\n", (unsigned)g_port,
                        portOk ? L"free" : L"IN USE");

        n += _snwprintf(report + n, 8192 - n,
                        L"\nbackend answers: %ls\n",
                        service_alive() ? L"yes" : L"no");

        _snwprintf(probe, MAX_PATH * 2, L"%s\\selftest.txt", g_data);
        HANDLE out = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (out != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            char utf8[8192];
            int len = WideCharToMultiByte(CP_UTF8, 0, report, -1, utf8, sizeof(utf8), NULL, NULL);
            WriteFile(out, utf8, (DWORD)(len > 0 ? len - 1 : 0), &w, NULL);
            CloseHandle(out);
        }

        MessageBoxW(NULL, report, L"kwifi - self test", MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    /* controlling the hotspot needs admin rights; relaunch once with runas and
       mark the child so it never loops.
       --tray (the autostart entry) only asks for elevation when the hotspot is
       supposed to come up automatically; a plain tray start stays quiet. */
    /* Starting the app must never show UAC: the window, the device list and the
       settings all work unelevated. Elevation is requested only when something
       actually needs it (starting the hotspot, blocking, TTL, task install):
       either silently through a registered task, or through the UAC dialog once
       when the user explicitly clicks "grant administrator rights". */
    BOOL silent = wcsstr(cmdline, L"--probe") || wcsstr(cmdline, L"--autostart");
    BOOL wantsHotspot = tray_only && config_auto_hotspot();
    BOOL explicit_runas = wcsstr(cmdline, L"--runas") != NULL;
    if (!is_elevated() && !silent && !wcsstr(cmdline, L"--elevated") && (explicit_runas || wantsHotspot)) {
        /* a registered task elevates without a prompt; without one we can only
           ask Windows, which shows the UAC dialog once */
        if (task_exists()) {
            showui_marker_write();
            logf_("elevating through the registered task");
            task_run();
            return 0;
        }
        wchar_t args[MAX_PATH * 2];
        _snwprintf(args, MAX_PATH * 2, L"--elevated %s", cmdline ? cmdline : L"");
        HINSTANCE hr = ShellExecuteW(NULL, L"runas", exe_path, args, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32) {
            /* the user declined: keep going so the tray and UI stay reachable */
            logf_("elevation declined (%p)", (void *)(INT_PTR)hr);
        } else {
            logf_("elevation requested for %ls", cmdline);
            return 0;
        }
    }

    logf_("boot: %ls data=%ls elevated=%d", g_root, g_data, is_elevated());

    if (wcsstr(cmdline, L"--probe")) {
        BOOL ok = ensure_service();
        return ok ? 0 : 1;
    }

    {
        wchar_t *as = wcsstr(cmdline, L"--autostart");
        if (as) {
            wchar_t mode[16] = {0};
            const wchar_t *m = as + wcslen(L"--autostart");
            while (*m == L' ' || *m == L'"') m++;
            int i = 0;
            while (*m && *m != L' ' && *m != L'"' && i < 15) mode[i++] = *m++;
            mode[i] = 0;

            wchar_t *res = wcsstr(cmdline, L"--result");
            if (res) {
                res += wcslen(L"--result");
                while (*res == L' ' || *res == L'"') res++;
                int j = 0;
                while (*res && *res != L'"' && j < MAX_PATH * 2 - 1) g_asResult[j++] = *res++;
                g_asResult[j] = 0;
            }
            return do_autostart(mode[0] ? mode : L"status");
        }
    }

    {
        wchar_t *it = wcsstr(cmdline, L"--install-task");
        if (it) {
            BOOL ok = task_install();
            logf_("install task: %d", ok);
            wprintf(L"%s\n", ok ? L"task installed" : L"task install failed");
            return ok ? 0 : 1;
        }
        wchar_t *rt = wcsstr(cmdline, L"--remove-task");
        if (rt) {
            task_remove();
            wprintf(L"task removed\n");
            return 0;
        }
        wchar_t *ts = wcsstr(cmdline, L"--task-status");
        if (ts) {
            wprintf(L"%s\n", task_exists() ? L"installed" : L"missing");
            return task_exists() ? 0 : 1;
        }
    }

    if (already_running()) return 0;

    g_icon = load_app_icon(16);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"KwifiTrayWnd";
    wc.hIcon = load_app_icon(32);
    wc.hIconSm = load_app_icon(16);
    RegisterClassExW(&wc);

    /* a real (but never shown) top-level window: message-only windows cannot
       host a tray icon reliably, and FindWindow cannot locate them */
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"kwifi", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    add_tray(g_hwnd);
    SetTimer(g_hwnd, TIMER_ID, 2000, NULL);

    BOOL wantsUi = !tray_only || showui_marker_take();

    /* when the service is up it owns the backend; this process only needs the
       tray and the window, and it never has to ask for administrator rights */
    BOOL backendReady = service_alive() ? TRUE : ensure_service();
    if (backendReady && service_running_state()) logf_("backend: shared service");

    if (!backendReady) {
        balloon(g_hwnd, APP_TITLE, L"本地服务启动失败，请查看 data\\server.log");
    } else if (!wantsUi) {
        logf_("tray only: interface not opened");
    } else {
        open_ui();
    }
    g_uiWasOpen = ui_window_exists();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon) DestroyIcon(g_icon);
    return 0;
}

/* ---------------- exposed to the backend ---------------- */

int kwifi_task_state(void)
{
    return task_exists() ? 1 : 0;
}

int kwifi_task_install(void)
{
    if (!task_install()) return 0;
    /* the task owns the logon start now; drop the Run key duplicate */
    kwifi_autostart_set(0, NULL, 0);
    return 1;
}

int kwifi_task_remove(void)
{
    return task_remove() ? 1 : 0;
}

/* ---------------- exposed to the backend ---------------- */

int kwifi_is_admin(void)
{
    return is_elevated() ? 1 : 0;
}

/* relaunch ourselves elevated; the caller answers the HTTP request first */
void kwifi_request_elevation(void)
{
    wchar_t args[MAX_PATH * 2];
    _snwprintf(args, MAX_PATH * 2, L"--elevated --tray");

    showui_marker_write();

    HINSTANCE hr = ShellExecuteW(NULL, L"runas", g_exe, args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hr <= 32) {
        logf_("elevation declined (%p)", (void *)(INT_PTR)hr);
        return;
    }
    logf_("elevation granted, handing over");
    /* the elevated copy owns the tray now */
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

/* ---------------- exposed to the backend ---------------- */

int kwifi_service_installed(void) { return service_installed(); }
int kwifi_service_running(void)   { return service_running_state(); }
int kwifi_service_install(void)   { return service_install() == 0 ? 1 : 0; }
int kwifi_service_remove(void)    { return service_remove() == 0 ? 1 : 0; }