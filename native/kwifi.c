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

#define APP_TITLE   L"kwifi"

/* native/kgwebview.cpp - the interface window (WebView2, no browser) */
extern HWND kg_webview_open(HINSTANCE inst, const wchar_t *url,
                            const wchar_t *userDataDir, const wchar_t *dllPath);
extern BOOL kg_webview_alive(void);
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

    _snprintf(path, sizeof(path), "%ls\\data\\kwifi.log", g_root);
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
    wchar_t *env = _wgetenv(L"KWIFI_DATA");
    if (env && *env) lstrcpynW(dataDir, env, MAX_PATH * 2);
    else _snwprintf(dataDir, MAX_PATH * 2, L"%s\\data", g_root);

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

/* where the backend keeps its state (must match server.c) */
static void compute_data_dir(wchar_t *out, size_t cch)
{
    wchar_t *env = _wgetenv(L"KWIFI_DATA");
    if (env && *env) {
        lstrcpynW(out, env, (int)cch);
        return;
    }
    if (GetFileAttributesW(L"..\\package.json") != INVALID_FILE_ATTRIBUTES && g_root[0]) {
        /* developer checkout: keep data next to the sources */
        _snwprintf(out, cch, L"%s\\data", g_root);
        return;
    }
    wchar_t appdata[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
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
        _snwprintf(dir, MAX_PATH * 2, L"%s\\data", g_root);
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

    /* keep the exe folder clean: cache and loader live in the data directory */
    wchar_t profile[MAX_PATH * 2];
    _snwprintf(profile, MAX_PATH * 2, L"%s\\webview", g_data);
    mkdirs(profile);

    /* the loader ships inside this exe; unpack it next to the data */
    wchar_t loader[MAX_PATH * 2];
    _snwprintf(loader, MAX_PATH * 2, L"%s\\WebView2Loader.dll", g_data);
    if (GetFileAttributesW(loader) == INVALID_FILE_ATTRIBUTES) {
        srv_write_wv2_loader(loader);
    }

    logf_("open_ui: creating webview window");
    kg_webview_open(g_inst, url, profile, loader);
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
        wchar_t dir[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%s\\data", g_root);
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
            _snwprintf(quitFile, MAX_PATH * 2, L"%s\\data\\.quit", g_root);
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

static void mkdirs(wchar_t *path)
{
    /* SHCreateDirectoryExW builds every missing level; the hand rolled loop
       silently failed for paths under %LOCALAPPDATA% */
    if (SHCreateDirectoryExW(NULL, path, NULL) != ERROR_SUCCESS) {
        CreateDirectoryW(path, NULL);
    }
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
        lstrcpynW(g_data, data_dir, MAX_PATH);
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

    g_icon = make_icon(16);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"KwifiTrayWnd";
    RegisterClassExW(&wc);

    /* a real (but never shown) top-level window: message-only windows cannot
       host a tray icon reliably, and FindWindow cannot locate them */
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"kwifi", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    add_tray(g_hwnd);
    SetTimer(g_hwnd, TIMER_ID, 2000, NULL);

    BOOL wantsUi = !tray_only || showui_marker_take();

    if (!ensure_service()) {
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