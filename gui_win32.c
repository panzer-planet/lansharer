/* Win32 front end: pick an adapter, watch peers appear. Socket reads arrive as window messages, so no threads. */
#define UNICODE
#define _UNICODE
#include "net.h"
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
/* Modern-looking controls instead of the Windows 95 style. */
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_SOCKET (WM_APP + 1)
enum { ID_TIMER = 1, ID_ADAPTER = 100 };

static HWND label_adapter, combo, label_peers, list, status;
static HFONT font;
static UINT dpi = 96;
static adapter ads[MAX_ADAPTERS];
static int nads;
static adapter *me;
static peer peers[MAX_PEERS];
static char beacon[sizeof MAGIC + NAME_LEN];
static SOCKET sock = INVALID_SOCKET;

static int S(int v) { return MulDiv(v, dpi, 96); } /* 96-dpi pixels -> real pixels */

static void send_utf8(HWND h, UINT msg, WPARAM wp, const char *s) {
    wchar_t w[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 256)) w[0] = 0;
    SendMessageW(h, msg, wp, (LPARAM)w);
}

static void refresh(void) {
    char line[160];
    int n = 0;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].ip.s_addr) {
            snprintf(line, sizeof line, "%s\t%s", peers[i].name, inet_ntoa(peers[i].ip));
            send_utf8(list, LB_ADDSTRING, 0, line);
            n++;
        }
    snprintf(line, sizeof line, "Announcing as \"%s\" to %s  -  %d peer%s", beacon + strlen(MAGIC),
             inet_ntoa(broadcast_addr(me)), n, n == 1 ? "" : "s");
    send_utf8(status, WM_SETTEXT, 0, line);
}

static void select_adapter(int i) {
    if (i < 0 || i >= nads) return;
    me = &ads[i];
    memset(peers, 0, sizeof peers); /* old peers belong to the previous network */
    send_beacon(sock, me, beacon);
    refresh();
}

static int same_adapters(const adapter *a, const adapter *b, int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(a[i].name, b[i].name) || a[i].ip.s_addr != b[i].ip.s_addr || a[i].mask.s_addr != b[i].mask.s_addr)
            return 0;
    return 1;
}

/* Re-read adapters so sleep, Wi-Fi switches and new DHCP leases don't leave us announcing on a dead network. */
static void reload_adapters(int force) {
    adapter fresh[MAX_ADAPTERS], old = {0};
    char line[160];
    int n = list_adapters(fresh, MAX_ADAPTERS), pick = -1;
    if (!force && n == nads && same_adapters(fresh, ads, n)) return;
    if (me) old = *me;
    memcpy(ads, fresh, sizeof fresh);
    nads = n;
    me = NULL;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof line, "%s  (%s)", ads[i].name, inet_ntoa(ads[i].ip));
        send_utf8(combo, CB_ADDSTRING, 0, line);
        if (pick < 0 && !strcmp(ads[i].name, old.name)) pick = i; /* keep the user's choice if it still exists */
    }
    if (pick < 0) pick = 0;
    EnableWindow(combo, n > 0);
    if (!n) {
        memset(peers, 0, sizeof peers);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        SetWindowTextW(status, L"No active network adapters found.");
        return;
    }
    SendMessageW(combo, CB_SETCURSEL, pick, 0);
    if (ads[pick].ip.s_addr == old.ip.s_addr && ads[pick].mask.s_addr == old.mask.s_addr) {
        me = &ads[pick]; /* same network as before: keep the peers we already know */
        refresh();
    } else {
        select_adapter(pick);
    }
}

static void apply_font(HWND hwnd) {
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    HWND all[] = {label_adapter, combo, label_peers, list, status};
    int tab = 150; /* dialog units: room for a 63-char name before the IP column */
    dpi = GetDpiForWindow(hwnd);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    if (font) DeleteObject(font);
    font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (int i = 0; i < 5; i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(list, LB_SETTABSTOPS, 1, (LPARAM)&tab);
}

static void layout(HWND hwnd) {
    RECT r;
    int m = S(12), lw = S(64), y = m;
    GetClientRect(hwnd, &r);
    int w = r.right - 2 * m;
    MoveWindow(label_adapter, m, y + S(4), lw, S(20), TRUE);
    MoveWindow(combo, m + lw, y, w - lw, S(300), TRUE); /* combobox height includes its dropdown */
    y += S(40);
    MoveWindow(label_peers, m, y, w, S(20), TRUE);
    y += S(22);
    MoveWindow(list, m, y, w, r.bottom - y - m - S(28), TRUE);
    MoveWindow(status, m, r.bottom - m - S(20), w, S(20), TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW *)lp)->hInstance;
        /* Each label is created right before its control so screen readers pair them. */
        label_adapter = CreateWindowW(L"STATIC", L"Adapter:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL, hi, NULL);
        combo = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                              0, 0, 0, 0, hwnd, (HMENU)ID_ADAPTER, hi, NULL);
        label_peers = CreateWindowW(L"STATIC", L"Peers on this network:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL,
                                    hi, NULL);
        list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_SORT | LBS_USETABSTOPS |
                                   LBS_NOINTEGRALHEIGHT,
                               0, 0, 0, 0, hwnd, NULL, hi, NULL);
        status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 0, 0, 0, 0, hwnd, NULL, hi, NULL);
        apply_font(hwnd);
        return 0;
    }
    case WM_SIZE:
        layout(hwnd);
        return 0;
    case WM_DPICHANGED: { /* window dragged to a monitor with different scaling */
        RECT *r = (RECT *)lp;
        apply_font(hwnd);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ADAPTER && HIWORD(wp) == CBN_SELCHANGE)
            select_adapter((int)SendMessageW(combo, CB_GETCURSEL, 0, 0));
        return 0;
    case WM_TIMER:
        reload_adapters(0);
        if (me) {
            send_beacon(sock, me, beacon);
            if (prune_peers(peers)) refresh();
        }
        return 0;
    case WM_SOCKET:
        if (me && handle_packet(sock, me, beacon, peers)) refresh();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show) {
    WNDCLASSW wc = {0};
    MSG m;
    (void)prev, (void)cmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); /* sharp text on scaled displays */
    if (net_init() != 0 || (sock = open_socket()) == INVALID_SOCKET) {
        MessageBoxW(NULL, L"Could not open the lansharer network port.\nIs lansharer already running?", L"lansharer",
                    MB_ICONERROR);
        return 1;
    }
    make_beacon(beacon, sizeof beacon);

    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"lansharer";
    RegisterClassW(&wc);
    dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowW(L"lansharer", L"lansharer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(460),
                              S(380), NULL, NULL, hi, NULL);
    if (!hwnd) return 1;

    WSAAsyncSelect(sock, hwnd, WM_SOCKET, FD_READ);
    SetTimer(hwnd, ID_TIMER, INTERVAL_MS, NULL);
    reload_adapters(1);
    ShowWindow(hwnd, show);
    while (GetMessageW(&m, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(hwnd, &m)) { /* Tab / Shift+Tab between controls */
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    return 0;
}
