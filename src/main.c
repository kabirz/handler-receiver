/*
 * 手柄-接收器工具 - Win32 GUI 应用
 * Tab1: 设备绑定 (手柄CAN扫描/连接 + 接收器UDP连接 + NRF读取比对 + 绑定)
 * Tab2: 手柄固件升级 (CAN)
 * Tab3: 接收器固件升级 (UDP)
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "resource.h"
#include "can_manager.h"
#include "pcan_loader.h"
#include "udp_manager.h"

#pragma comment(lib, "comctl32.lib")

/* 自定义窗口消息 (工作线程 -> UI 线程) */
#define WM_UPDATE_LOG        (WM_APP + 1)
#define WM_UPDATE_PROGRESS   (WM_APP + 3)
#define WM_FW_COMPLETE       (WM_APP + 6)
#define WM_FW_SHOW_PROGRESS  (WM_APP + 7)
#define WM_UPDATE_STATUS     (WM_APP + 8)  /* 更新 Tab1 状态区 */

/* CAN 帧 ID 与 RF24 命令 (与 can_manager.h 互补, main.c 局部用) */
#define CAN_ID_RF24_CONFIG_CMD   0x104
#define RF24_CMD_GET_CONFIG      0x02

/* 运行时控件 ID (Tab1 设备绑定) */
#define IDC_BTN_SCAN_HANDLER     1001
#define IDC_BTN_SCAN_RECEIVER    1002
#define IDC_BTN_CHECK_BIND       1003
#define IDC_BTN_BIND             1004
#define IDC_STATUS_TEXT          1005
/* Tab2 手柄固件升级 */
#define IDC_HFW_FILE             1101
#define IDC_HFW_BROWSE           1102
#define IDC_HFW_UPGRADE          1103
/* Tab3 接收器固件升级 */
#define IDC_TFW_FILE             1201
#define IDC_TFW_BROWSE           1202
#define IDC_TFW_UPGRADE          1203

/* 全局状态 */
static HINSTANCE g_hInst;
static HWND g_hMain;
static HWND g_hTab;
static HWND g_hTabDlg[3];

static CanManager *g_can;
static UdpManager *g_cfgUdp;

static int g_canConnected;
static int g_udpConnected;

/* 手柄 NRF (CAN 0x105 响应填入) */
static uint8_t g_handlerCh;
static uint8_t g_handlerAddr[5];
static volatile BOOL g_handlerAddrGot;

/* 接收器 NRF (UDP GET_RF24) */
static uint8_t g_receiverCh;
static uint8_t g_receiverAddr[5];

static char g_handlerFwPath[MAX_PATH];
static char g_receiverFwPath[MAX_PATH];

/* 子对话框过程: 三个 tab 页共用, 通过 GWL_USERDATA 标记 tab 索引区分.
 * 控件命令统一转发到主窗口的 OnTabCommand 处理 (避免逻辑分散). */
static LRESULT CALLBACK TabChildDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        /* 把子对话框的命令转发给主窗口处理 */
        PostMessageW(g_hMain, WM_APP + 100, (WPARAM)hDlg, lParam);
        return 0;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wParam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

#define TABCHILD_CLASS L"ZCodeTabChild"
static BOOL g_tabChildRegistered = FALSE;

static void RegisterTabChildClass(void)
{
    if (g_tabChildRegistered) return;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = TabChildDlgProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = TABCHILD_CLASS;
    RegisterClassW(&wc);
    g_tabChildRegistered = TRUE;
}

/* 主窗口过程 */
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* 创建 Tab 控件 + 3 个子对话框, 在 WM_CREATE 中调用 */
static void CreateTabLayout(HWND hWnd)
{
    /* Tab 控件位于顶部 */
    g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
            0, 0, 480, 360, hWnd, (HMENU)1, g_hInst, NULL);

    /* 三个页签标题 */
    TCITEMW ti; ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"设备绑定";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"手柄固件升级";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"接收器固件升级";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 2, (LPARAM)&ti);

    /* 子对话框显示区: tab 下方 */
    RECT rcTab;
    GetClientRect(g_hTab, &rcTab);
    SendMessageW(g_hTab, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcTab);

    RegisterTabChildClass();
    DWORD childStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    for (int i = 0; i < 3; i++) {
        g_hTabDlg[i] = CreateWindowExW(0, TABCHILD_CLASS, L"",
                childStyle,
                rcTab.left, rcTab.top,
                rcTab.right - rcTab.left, rcTab.bottom - rcTab.top,
                g_hTab, NULL, g_hInst, NULL);
        SetWindowLongPtrW(g_hTabDlg[i], GWLP_USERDATA, (LONG_PTR)i);
        ShowWindow(g_hTabDlg[i], i == 0 ? SW_SHOW : SW_HIDE);
    }
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hWnd;
        CreateTabLayout(hWnd);
        return 0;

    case WM_NOTIFY: {
        LPNMHDR nmh = (LPNMHDR)lParam;
        if (nmh->hwndFrom == g_hTab && nmh->code == TCN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0);
            for (int i = 0; i < 3; i++) {
                ShowWindow(g_hTabDlg[i], i == sel ? SW_SHOW : SW_HIDE);
            }
        }
        return 0;
    }

    case WM_APP + 100:
        /* 子对话框命令转发, lParam 是子对话框的 WM_COMMAND lParam */
        /* OnTabCommand 将在 Task 3 实现, 此处暂忽略 */
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine;
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icex);

    g_hInst = hInstance;

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ZCodeHandlerReceiver";
    RegisterClassW(&wc);

    /* 主窗口尺寸 480x380 (含 tab 显示区) */
    RECT rc = { 0, 0, 480, 380 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE, 0);

    HWND hWnd = CreateWindowExW(0, L"ZCodeHandlerReceiver", L"手柄-接收器工具",
            WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            NULL, NULL, hInstance, NULL);
    if (!hWnd) return 1;

    /* 应用图标 */
    HICON hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (hIcon) {
        SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        /* Tab 键导航: IsDialogMessage 处理子对话框控件焦点 */
        if (!IsDialogMessageW(g_hMain, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
