/*
 * kgwebview.cpp - the interface window of kwifi.
 *
 * The UI is a web page, but it must not be shown in a browser: this host owns a
 * normal Win32 window and renders the page with the WebView2 control that ships
 * with Edge. Closing the window hides it (the tray keeps running).
 *
 * WebView2Loader.dll is loaded dynamically, so the build needs no MSVC import
 * library and the program can report a clear error when the runtime is absent.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <objbase.h>
#include <string>
#include "WebView2.h"

/* MinGW has no MSVC-style __uuidof for MIDL interfaces, so declare them here */
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                0x6c4819f3, 0xc9b7, 0x4260, 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c)

typedef HRESULT(STDAPICALLTYPE *CreateEnvFn)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

static HWND g_hwnd = NULL;
static ICoreWebView2Controller *g_controller = NULL;
static ICoreWebView2 *g_webview = NULL;
static std::wstring g_url;
static std::wstring g_userData;
static bool g_ready = false;
static bool g_failed = false;
static HRESULT g_lastError = S_OK;

static void resize_webview(void)
{
    if (!g_controller || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

/* ---------------- COM helpers (no WRL under MinGW) ---------------- */

template <class I>
class Handler : public I
{
public:
    Handler() : refs_(1) {}
    virtual ~Handler() {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(I)) {
            *ppv = static_cast<I *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

private:
    LONG refs_;
};

class ControllerHandler : public Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
{
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override
    {
        if (FAILED(result) || !controller) {
            /* remember it: the caller retries with a fresh profile before it
               bothers the user with a dialog */
            g_failed = true;
            g_lastError = result;
            return S_OK;
        }

        g_controller = controller;
        g_controller->AddRef();
        controller->get_CoreWebView2(&g_webview);

        resize_webview();
        g_controller->put_IsVisible(TRUE);

        if (g_webview) {
            ICoreWebView2Settings *settings = nullptr;
            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);
                settings->Release();
            }
            /* show something while the backend connects: if the navigation never
           completes the user sees the address instead of a blank frame */
        {
            std::wstring placeholder =
                L"<html><head><meta charset=\"utf-8\"><style>"
                L"body{font-family:system-ui,Segoe UI,sans-serif;background:#f6f8fc;color:#334155;"
                L"display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
                L"div{text-align:center;line-height:1.9}h3{font-weight:600;color:#1e293b}"
                L"code{background:#e2e8f0;padding:2px 8px;border-radius:6px;font-size:13px}"
                L"</style></head><body><div><h3>正在连接 kwifi 服务…</h3>"
                L"<p>如果一直停在这里，说明本地服务没有启动。</p>"
                L"<p><code>" + g_url + L"</code></p></div></body></html>";
            g_webview->NavigateToString(placeholder.c_str());
        }
        g_webview->Navigate(g_url.c_str());
        }

        g_ready = true;
        return S_OK;
    }
};

class EnvironmentHandler : public Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
{
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override
    {
        if (FAILED(result) || !env) {
            g_failed = true;
            g_lastError = result;
            return S_OK;
        }
        env->CreateCoreWebView2Controller(g_hwnd, new ControllerHandler());
        return S_OK;
    }
};

/* ---------------- window ---------------- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        resize_webview();
        return 0;
    case WM_SETFOCUS:
        if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_CLOSE:
        /* closing the window keeps kwifi alive in the tray */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool init_webview(const wchar_t *dllPath, const wchar_t *userDataDir)
{
    HMODULE loader = LoadLibraryW(dllPath);
    if (!loader) return false;

    auto create = (CreateEnvFn)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!create) return false;

    std::wstring data = userDataDir ? userDataDir : L"";
    g_userData = data;
    return SUCCEEDED(create(nullptr, data.empty() ? nullptr : data.c_str(),
                            nullptr, new EnvironmentHandler()));
}

/* ---------------- exported API ---------------- */

extern "C" {

__declspec(dllexport) HWND kg_webview_open(HINSTANCE inst, const wchar_t *url,
                                           const wchar_t *userDataDir,
                                           const wchar_t *dllPath)
{
    if (g_hwnd && IsWindow(g_hwnd)) {
        ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return g_hwnd;
    }

    g_url = url ? url : L"";
    g_failed = false;
    g_lastError = S_OK;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"KwifiUiWindow";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    /* the icon lives in the executable (RT_GROUP_ICON 1); registering it on the
       class makes the title bar and the taskbar pick it up */
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32,
                                 LR_DEFAULTCOLOR | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16,
                                   LR_DEFAULTCOLOR | LR_SHARED);
    RegisterClassExW(&wc);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 1200, h = 860;
    if (work.right - work.left < w) w = work.right - work.left;
    if (work.bottom - work.top < h) h = work.bottom - work.top;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"kwifi",
                             WS_OVERLAPPEDWINDOW,
                             x, y, w, h, NULL, NULL, inst, NULL);
    if (!g_hwnd) return NULL;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (!init_webview(dllPath, userDataDir)) {
        MessageBoxW(g_hwnd, L"找不到 WebView2Loader.dll，无法显示界面。", L"kwifi",
                    MB_ICONERROR | MB_OK);
    }
    return g_hwnd;
}

__declspec(dllexport) BOOL kg_webview_alive(void)
{
    return g_hwnd && IsWindow(g_hwnd) ? TRUE : FALSE;
}

/* why the controller never appeared; the caller retries before reporting it */
__declspec(dllexport) BOOL kg_webview_failed(void)
{
    return g_failed ? TRUE : FALSE;
}

__declspec(dllexport) long kg_webview_last_error(void)
{
    return (long)g_lastError;
}

__declspec(dllexport) void kg_webview_show(void)
{
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

__declspec(dllexport) void kg_webview_close(void)
{
    if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
    if (g_webview) {
        g_webview->Release();
        g_webview = nullptr;
    }
    if (g_controller) {
        g_controller->Close();
        g_controller->Release();
        g_controller = nullptr;
    }
    g_ready = false;
}

} /* extern "C" */