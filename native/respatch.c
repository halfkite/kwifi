/*
 * respatch.c - give kwifi.exe a proper Windows identity.
 *
 * The MinGW-flavoured linker cannot consume .res files, so this tool writes the
 * resources that make the program look like a real application instead of an
 * anonymous blob:
 *
 *   RT_ICON / RT_GROUP_ICON   icon in Explorer, taskbar and the tray
 *   RT_VERSION                product name, version, copyright
 *   RT_MANIFEST               DPI awareness, UTF-8 code page, no shims
 *
 * Usage: respatch <exe> <manifest.xml> <icon.ico> <version> <desc>
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LANG_NEUTRAL_ZH 0x0804

static unsigned char *read_file(const wchar_t *path, DWORD *outSize)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL);
    unsigned char *buf = (unsigned char *)malloc(size ? size : 1);
    DWORD got = 0;
    if (!buf || !ReadFile(h, buf, size, &got, NULL)) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *outSize = got;
    return buf;
}

/* ---------------- version resource ---------------- */

typedef struct {
    unsigned char *p;
    size_t len, cap;
} buf;

static void b_need(buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    while (b->len + extra > b->cap) b->cap = b->cap ? b->cap * 2 : 4096;
    b->p = (unsigned char *)realloc(b->p, b->cap);
}

static void b_bytes(buf *b, const void *data, size_t n)
{
    b_need(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void b_word(buf *b, WORD v) { b_bytes(b, &v, 2); }
static void b_dword(buf *b, DWORD v) { b_bytes(b, &v, 4); }

/* UTF-16 string, NUL terminated, padded to DWORD */
static void b_wstr(buf *b, const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    b_bytes(b, s, n);
    while (b->len % 4) { unsigned char z = 0; b_bytes(b, &z, 1); }
}

/* write a node header whose wValueLength covers the following bytes */
static void b_node_header(buf *b, WORD valueLength, WORD type, const wchar_t *key)
{
    size_t start = b->len;
    b_word(b, 0);                      /* wLength, patched later */
    b_word(b, valueLength);
    b_word(b, type);
    b_wstr(b, key);
    /* key must start at a DWORD boundary already; length patched by caller */
    (void)start;
}

static void patch_len(buf *b, size_t start)
{
    WORD len = (WORD)(b->len - start);
    memcpy(b->p + start, &len, 2);
}

static void b_block(buf *b, const wchar_t *key, const buf *children)
{
    size_t start = b->len;
    b_word(b, 0);
    b_word(b, 0);                      /* wValueLength: children only */
    b_word(b, 1);                      /* wType: text */
    b_wstr(b, key);
    b_bytes(b, children->p, children->len);
    patch_len(b, start);
}

static void b_string(buf *b, const wchar_t *key, const wchar_t *value)
{
    size_t start = b->len;
    b_word(b, 0);
    b_word(b, (WORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    b_word(b, 1);
    b_wstr(b, key);
    b_wstr(b, value);
    patch_len(b, start);
}

static DWORD parse_version(const char *text)
{
    int a = 0, b = 0, c = 0, d = 0;
    sscanf(text, "%d.%d.%d.%d", &a, &b, &c, &d);
    return ((DWORD)(a & 0xff) << 24) | ((DWORD)(b & 0xff) << 16) |
           ((DWORD)(c & 0xff) << 8) | (DWORD)(d & 0xff);
}

static buf build_version(const char *version, const wchar_t *desc)
{
    wchar_t wver[64] = {0};
    MultiByteToWideChar(CP_UTF8, 0, version, -1, wver, 64);

    buf stringsZh, stringsEn, fileInfo, fixed, root;
    ZeroMemory(&stringsZh, sizeof(stringsZh));
    ZeroMemory(&stringsEn, sizeof(stringsEn));
    ZeroMemory(&fileInfo, sizeof(fileInfo));
    ZeroMemory(&fixed, sizeof(fixed));
    ZeroMemory(&root, sizeof(root));

    b_string(&stringsZh, L"CompanyName", L"kwifi");
    b_string(&stringsZh, L"FileDescription", desc);
    b_string(&stringsZh, L"FileVersion", wver);
    b_string(&stringsZh, L"InternalName", L"kwifi");
    b_string(&stringsZh, L"LegalCopyright", L"MIT License");
    b_string(&stringsZh, L"OriginalFilename", L"kwifi.exe");
    b_string(&stringsZh, L"ProductName", L"kwifi");
    b_string(&stringsZh, L"ProductVersion", wver);

    b_string(&stringsEn, L"CompanyName", L"kwifi");
    b_string(&stringsEn, L"FileDescription", L"kwifi hotspot console");
    b_string(&stringsEn, L"FileVersion", wver);
    b_string(&stringsEn, L"InternalName", L"kwifi");
    b_string(&stringsEn, L"LegalCopyright", L"MIT License");
    b_string(&stringsEn, L"OriginalFilename", L"kwifi.exe");
    b_string(&stringsEn, L"ProductName", L"kwifi");
    b_string(&stringsEn, L"ProductVersion", wver);

    b_block(&fileInfo, L"080404b0", &stringsZh);
    b_block(&fileInfo, L"040904b0", &stringsEn);

    /* VS_FIXEDFILEINFO */
    DWORD v = parse_version(version);
    b_dword(&fixed, 0xFEEF04BD);       /* dwSignature */
    b_dword(&fixed, 0x00010000);       /* dwStrucVersion */
    b_dword(&fixed, v);                /* dwFileVersionMS */
    b_dword(&fixed, 0);                /* dwFileVersionLS */
    b_dword(&fixed, v);                /* dwProductVersionMS */
    b_dword(&fixed, 0);                /* dwProductVersionLS */
    b_dword(&fixed, 0x3f);             /* dwFileFlagsMask */
    b_dword(&fixed, 0);                /* dwFileFlags */
    b_dword(&fixed, 0x00040004);       /* dwFileOS: VOS_NT_WINDOWS32 */
    b_dword(&fixed, 0x00000001);       /* dwFileType: VFT_APP */
    b_dword(&fixed, 0);                /* dwFileSubtype */
    b_dword(&fixed, 0);                /* dwFileDateMS */
    b_dword(&fixed, 0);                /* dwFileDateLS */

    /* root VS_VERSIONINFO: wValueLength = 52 bytes of VS_FIXEDFILEINFO */
    size_t start = root.len;
    b_word(&root, 0);
    b_word(&root, 52);
    b_word(&root, 0);
    b_wstr(&root, L"VS_VERSION_INFO");
    b_bytes(&root, fixed.p, fixed.len);
    b_block(&root, L"StringFileInfo", &fileInfo);
    patch_len(&root, start);

    free(stringsZh.p);
    free(stringsEn.p);
    free(fileInfo.p);
    free(fixed.p);
    return root;
}

/* ---------------- icon resources ---------------- */

static int add_icon(HANDLE upd, const unsigned char *ico, DWORD iconSize)
{
    if (iconSize < 6) return 0;
    WORD reserved = *(const WORD *)(ico + 0);
    WORD type = *(const WORD *)(ico + 2);
    WORD count = *(const WORD *)(ico + 4);
    if (reserved != 0 || type != 1 || count == 0) return 0;
    if (6 + count * 16 > iconSize) return 0;

    /* RT_GROUP_ICON */
    buf group;
    ZeroMemory(&group, sizeof(group));
    b_word(&group, 0);
    b_word(&group, 1);
    b_word(&group, count);

    int added = 0;
    for (int i = 0; i < count; i++) {
        const unsigned char *entry = ico + 6 + i * 16;
        unsigned char w = entry[0];
        unsigned char h = entry[1];
        unsigned char colors = entry[2];
        unsigned char reservedByte = entry[3];
        WORD planes = *(const WORD *)(entry + 4);
        WORD bpp = *(const WORD *)(entry + 6);
        DWORD bytes = *(const DWORD *)(entry + 8);
        DWORD offset = *(const DWORD *)(entry + 12);

        DWORD skip = 0;
        memcpy(&skip, entry + 8, 4);
        if (offset + bytes > iconSize) continue;

        WORD id = (WORD)(i + 1);
        if (!UpdateResourceW(upd, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(id),
                             LANG_NEUTRAL_ZH, (LPVOID)(ico + offset), bytes)) {
            continue;
        }

        unsigned char ge[14];
        ge[0] = w;
        ge[1] = h;
        ge[2] = colors;
        ge[3] = reservedByte;
        memcpy(ge + 4, &planes, 2);
        memcpy(ge + 6, &bpp, 2);
        memcpy(ge + 8, &bytes, 4);
        memcpy(ge + 12, &id, 2);
        b_bytes(&group, ge, 14);
        added++;
    }

    if (added) {
        UpdateResourceW(upd, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1),
                        LANG_NEUTRAL_ZH, group.p, (DWORD)group.len);
    }
    free(group.p);
    return added;
}

/* ---------------- entry point ---------------- */

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: respatch <exe> <manifest.xml> [icon.ico] [version] [description]\n");
        return 2;
    }

    const wchar_t *exePath = argv[1];
    const wchar_t *manifestPath = argv[2];
    const wchar_t *iconPath = argc > 3 ? argv[3] : NULL;
    char version[32] = "0.6.0.0";
    if (argc > 4) WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, version, sizeof(version), NULL, NULL);

    wchar_t desc[128] = L"kwifi \x70ed\x70b9\x63a7\x5236\x53f0";
    if (argc > 5) lstrcpynW(desc, argv[5], 128);

    DWORD manifestSize = 0;
    unsigned char *manifest = read_file(manifestPath, &manifestSize);
    if (!manifest) {
        fwprintf(stderr, L"cannot read %ls\n", manifestPath);
        return 2;
    }

    HANDLE upd = BeginUpdateResourceW(exePath, FALSE);
    if (!upd) {
        fwprintf(stderr, L"BeginUpdateResource failed (%lu) - close the exe first\n",
                 (unsigned long)GetLastError());
        free(manifest);
        return 1;
    }

    int ok = 0;
    /* UTF-8 manifest bytes, resource id 1 */
    if (UpdateResourceW(upd, MAKEINTRESOURCEW(24), MAKEINTRESOURCEW(1),
                        LANG_NEUTRAL_ZH, manifest, manifestSize)) {
        ok++;
    } else {
        fwprintf(stderr, L"manifest update failed (%lu)\n", (unsigned long)GetLastError());
    }

    buf ver = build_version(version, desc);
    if (UpdateResourceW(upd, MAKEINTRESOURCEW(16), MAKEINTRESOURCEW(1),
                        LANG_NEUTRAL_ZH, ver.p, (DWORD)ver.len)) {
        ok++;
    } else {
        fwprintf(stderr, L"version update failed (%lu)\n", (unsigned long)GetLastError());
    }
    free(ver.p);

    if (iconPath) {
        DWORD iconSize = 0;
        unsigned char *icon = read_file(iconPath, &iconSize);
        if (icon) {
            int n = add_icon(upd, icon, iconSize);
            if (n > 0) ok++;
            free(icon);
        }
    }

    if (!EndUpdateResourceW(upd, FALSE)) {
        fwprintf(stderr, L"EndUpdateResource failed (%lu)\n", (unsigned long)GetLastError());
        free(manifest);
        return 1;
    }

    free(manifest);
    wprintf(L"resources attached: %d\n", ok);
    return ok >= 2 ? 0 : 1;
}
