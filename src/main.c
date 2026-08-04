/*
 * 手柄-接收器工具 - Win32 GUI 应用
 * Tab1: 手柄绑定 (手柄CAN扫描/连接 + 接收器UDP单播连接 + NRF读取比对 + 绑定)
 * Tab2: 手柄升级 (CAN)
 * Tab3: 手柄接收端配置 (UDP, 含固件升级 + 网络参数设置)
 * Tab4: 设备查找 (广播发现接收器真实 IP)
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
#include "fw_image.h"
#include "pcan_loader.h"
#include "udp_manager.h"

#pragma comment(lib, "comctl32.lib")

/* 自定义窗口消息 (工作线程 -> UI 线程) */
#define WM_UPDATE_LOG        (WM_APP + 1)
#define WM_UPDATE_PROGRESS   (WM_APP + 3)
#define WM_FW_COMPLETE       (WM_APP + 6)
#define WM_FW_SHOW_PROGRESS  (WM_APP + 7)
#define WM_FW_REJECTED       (WM_APP + 11) /* keyhash/格式被拒: wParam=1 CAN/0 UDP, 销毁进度窗不显示结果 */
#define WM_DISC_FOUND_IP     (WM_APP + 9)  /* Tab4: 发现新 IP, lParam=_strdup(ip) */
#define WM_DISC_DONE         (WM_APP + 10) /* Tab4: 查找结束, 主线程恢复按钮文字 */

/* CAN 帧 ID 由 can_manager.h 定义 (CAN_ID_RF24_CONFIG_CMD/RESP) */
#define RF24_CMD_GET_CONFIG      0x02

/* 运行时控件 ID (Tab1 手柄绑定) — 仿 gateway-tool 左侧 CAN / 右侧通道配置布局 */
#define IDC_CAN_DEVICE           1001   /* 手柄: 设备下拉 (CBS_DROPDOWNLIST) */
#define IDC_CAN_REFRESH          1002   /* 手柄: 刷新按钮 */
#define IDC_CAN_CONNECT          1003   /* 手柄: 连接/断开 按钮 */
#define IDC_UDP_IP               1004   /* 接收器: 目标 IP (CBS_DROPDOWN) */
#define IDC_UDP_CONNECT          1005   /* 接收器: 连接/断开 按钮 */
#define IDC_UDP_LOCAL_PORT       1010   /* 接收器: 本地端口 (bind, 默认 8602) */
#define IDC_BTN_CHECK_BIND       1006   /* 检测绑定状态 */
#define IDC_BTN_BIND             1007   /* 绑定设备 */
/* Tab2 手柄固件升级 */
#define IDC_HFW_FILE             1101
#define IDC_HFW_BROWSE           1102
#define IDC_HFW_UPGRADE          1103
#define IDC_HFW_VERSION          1104   /* 固件版本静态文本 */
#define IDC_HFW_GETVER           1105   /* 获取版本按钮 */
/* Tab3 接收器固件升级 */
#define IDC_TFW_FILE             1201
#define IDC_TFW_BROWSE           1202
#define IDC_TFW_UPGRADE          1203
#define IDC_TFW_VERSION          1204   /* 固件版本静态文本 */
#define IDC_TFW_GETVER           1205   /* 获取版本按钮 */
/* Tab3 网络参数设置 (SET_NET 0x12 / GET_NET 0x13) */
#define IDC_NET_IP               1210   /* 设置用 IP 输入框 */
#define IDC_NET_PORT             1211   /* 设置用 数据端口 输入框 */
#define IDC_NET_APPLY            1212   /* 设置按钮 */
#define IDC_NET_QUERY            1213   /* 查询按钮 */
/* Tab4 设备查找 */
#define IDC_DISC_START           1301   /* 开始/停止查找 按钮 */
#define IDC_DISC_LIST            1302   /* 发现的 IP 列表 (LISTBOX) */
#define IDC_DISC_COPY            1303   /* 复制选中 IP 到剪贴板 按钮 */

/* 全局状态 */
static HINSTANCE g_hInst;
static HWND g_hMain;
static HWND g_hTab;
static HWND g_hTabDlg[4];

/* CAN 各 tab 独立: g_canTab[0]=Tab1 绑定用 (带 frame_cb 处理 NRF), g_canTab[1]=Tab2 升级用.
 * 每个 tab 持有独立 CanManager 实例 + 独立连接状态, 互不影响 (同一 PCAN 设备被一个 tab
 * Initialize 后, 另一个 tab 再 Initialize 同设备会失败 → 弹窗友好提示占用) */
#define CAN_TAB_BIND    0   /* Tab1: 手柄绑定 (NRF 读取) */
#define CAN_TAB_UPGRADE 1   /* Tab2: 手柄固件升级 */
#define CAN_TAB_COUNT   2
static CanManager *g_canTab[CAN_TAB_COUNT];
static int g_canTabChannel[CAN_TAB_COUNT];   /* 各 tab 已连接的 channel, -1=未连接 */

/* 接收器 UDP 管理器: Tab1(绑定) 和 Tab3(配置) 各用独立实例, 互不耦合.
 * g_udpTabIdx 0=Tab1, 1=Tab3. */
#define UDP_TAB_BIND  0   /* Tab1 手柄绑定页的接收器 */
#define UDP_TAB_CFG   1   /* Tab3 接收端配置页的接收器 */
#define UDP_TAB_COUNT 2
static UdpManager *g_cfgUdp[UDP_TAB_COUNT];
static int g_udpConnected[UDP_TAB_COUNT];

/* Tab4 设备查找: 原生 winsock 广播 GET_NET (0x13), 收集 2s 内响应源 IP.
 * 用独立 socket (本地端口 8602), 不走 UdpManager. g_discRunning=查找中. */
static volatile BOOL g_discRunning;

/* 手柄 NRF (CAN 0x111 响应填入) */
static uint8_t g_handlerCh;
static uint8_t g_handlerAddr[5];
static volatile BOOL g_handlerAddrGot;

/* 接收器 NRF (UDP GET_RF24) */
static uint8_t g_receiverCh;
static uint8_t g_receiverAddr[5];

static char g_handlerFwPath[MAX_PATH];
static char g_receiverFwPath[MAX_PATH];

/* 固件升级进度弹窗 */
static HWND g_progressDlg = NULL;
static HWND g_progressBar = NULL;
static HWND g_progressLabel = NULL;
static HWND g_progressBtn = NULL;
static HWND g_progressReboot = NULL;
static volatile BOOL g_progressDone = FALSE;
static volatile BOOL g_progressIsCan = FALSE;

#define IDC_PROG_BAR    900
#define IDC_PROG_LABEL  901
#define IDC_PROG_BTN    902
#define IDC_PROG_REBOOT 903
#define FW_PROGRESS_CLASS L"ZCodeFwProgress"

/* 子对话框过程: 三个 tab 页共用, 通过 GWL_USERDATA 标记 tab 索引区分.
 * 控件命令统一转发到主窗口的 OnTabCommand 处理 (避免逻辑分散). */
static LRESULT CALLBACK TabChildDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        /* wParam 低字 = 控件 ID, lParam = 控件句柄. 转发时 wParam 透传 (含控件 ID),
         * 子对话框句柄放 lParam, 主窗口 OnTabCommand 据此取 tab index. */
        PostMessageW(g_hMain, WM_APP + 100, wParam, (LPARAM)hDlg);
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

/* 刷新手柄设备下拉框 (扫 PCAN_USB 通道, 仿 gateway-tool RefreshDevices).
 * tabIdx 决定用哪个 CanManager 实例扫描 (各 tab 独立) */
static void RefreshCanDevices(HWND hCombo, int tabIdx)
{
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    char devices[16][256];
    int count = CanManager_DetectDevice(g_canTab[tabIdx], devices, 16);
    for (int i = 0; i < count; i++) {
        /* 设备名是 ASCII, 转 wchar_t 加入下拉 */
        wchar_t wname[256];
        MultiByteToWideChar(CP_ACP, 0, devices[i], -1, wname, 256);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)wname);
    }
    if (count > 0) {
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    }
}

/* 创建手柄 CAN 连接 groupbox (设备下拉 + 刷新 + 连接, 固定 250K).
 * version_id/getver_id < 0 时不显示版本行; fw_*_id > 0 时在版本行下方显示固件区 (内嵌).
 * 返回 groupbox 占用的总高度 (含间距). */
static int CreateCanGroupBox(HWND hDlg, int yPos, int version_id, int getver_id,
                             int fw_file_id, int fw_browse_id, int fw_upgrade_id)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    BOOL showVer = (version_id > 0 && getver_id > 0);
    BOOL showFw = (fw_file_id > 0 && fw_browse_id > 0 && fw_upgrade_id > 0);
    /* groupbox 高度: 基础(连接行)70 + 版本行26 + 固件区86 */
    int boxH = 70;
    if (showVer) boxH += 26;
    if (showFw) boxH += 86;
    CreateWindowExW(0, L"BUTTON", L"手柄",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, yPos, 436, boxH, hDlg, NULL, g_hInst, NULL);
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"CAN设备:",
            WS_CHILD | WS_VISIBLE, 20, yPos + 24, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hDev = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            76, yPos + 22, 175, 100, hDlg, (HMENU)(INT_PTR)IDC_CAN_DEVICE, g_hInst, NULL);
    SendMessageW(hDev, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            296, yPos + 22, 60, 22, hDlg, (HMENU)(INT_PTR)IDC_CAN_REFRESH, g_hInst, NULL);
    SendMessageW(hRefresh, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCanConn = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            366, yPos + 22, 70, 22, hDlg, (HMENU)(INT_PTR)IDC_CAN_CONNECT, g_hInst, NULL);
    SendMessageW(hCanConn, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 版本行 (可选): "固件版本: xxx" + "获取版本" 按钮 */
    if (showVer) {
        HWND hvLbl = CreateWindowExW(0, L"STATIC", L"固件版本:",
                WS_CHILD | WS_VISIBLE, 20, yPos + 56, 64, 14, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hvLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hVer = CreateWindowExW(0, L"STATIC", L"未知",
                WS_CHILD | WS_VISIBLE, 84, yPos + 56, 200, 14,
                hDlg, (HMENU)(INT_PTR)version_id, g_hInst, NULL);
        SendMessageW(hVer, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hGetVer = CreateWindowExW(0, L"BUTTON", L"获取版本",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                296, yPos + 54, 140, 22, hDlg, (HMENU)(INT_PTR)getver_id, g_hInst, NULL);
        SendMessageW(hGetVer, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* 固件区 (可选, 内嵌 groupbox): 文件路径 + 浏览 + 升级. 紧跟版本行下方 */
    if (showFw) {
        int fy = yPos + (showVer ? 88 : 56);  /* 无版本行时上移 */
        HWND hFLbl = CreateWindowExW(0, L"STATIC", L"固件文件:",
                WS_CHILD | WS_VISIBLE, 20, fy + 4, 60, 16, hDlg, NULL, g_hInst, NULL);
        HWND hFile = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                80, fy + 2, 260, 22, hDlg, (HMENU)(INT_PTR)fw_file_id, g_hInst, NULL);
        HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                350, fy + 2, 60, 22, hDlg, (HMENU)(INT_PTR)fw_browse_id, g_hInst, NULL);
        HWND hUpg = CreateWindowExW(0, L"BUTTON", L"升级",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                180, fy + 40, 80, 28, hDlg, (HMENU)(INT_PTR)fw_upgrade_id, g_hInst, NULL);
        SendMessageW(hFLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hFile,  WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hUpg,   WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* 预填设备列表: tabIdx 来自子对话框 GWLP_USERDATA (Tab1=0, Tab2=1, 对应 CAN_TAB_*) */
    int tabIdx = (int)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
    RefreshCanDevices(hDev, tabIdx);
    return boxH + 8;  /* groupbox 高 + 8 间距 */
}

/* 创建接收器 UDP 连接 groupbox (目标 IP + 连接, 配置端口固定 8601).
 * version_id/getver_id < 0 时不显示版本行; fw_*_id > 0 时在版本行下方显示固件区 (内嵌).
 * 返回 groupbox 占用的总高度. */
static int CreateUdpGroupBox(HWND hDlg, int yPos, int version_id, int getver_id,
                             int fw_file_id, int fw_browse_id, int fw_upgrade_id)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    BOOL showVer = (version_id > 0 && getver_id > 0);
    BOOL showFw = (fw_file_id > 0 && fw_browse_id > 0 && fw_upgrade_id > 0);
    int boxH = 70;
    if (showVer) boxH += 26;
    if (showFw) boxH += 86;
    CreateWindowExW(0, L"BUTTON", L"接收器",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, yPos, 436, boxH, hDlg, NULL, g_hInst, NULL);
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"目标IP:",
            WS_CHILD | WS_VISIBLE, 20, yPos + 24, 44, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            66, yPos + 22, 85, 22, hDlg, (HMENU)(INT_PTR)IDC_UDP_IP, g_hInst, NULL);
    SendMessageW(hIp, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 本地端口 (bind). 固件监听 8601, 上位机本地端口收广播. 两 tab 默认都 8602 */
    HWND hLpLbl = CreateWindowExW(0, L"STATIC", L"本地端口:",
            WS_CHILD | WS_VISIBLE, 156, yPos + 24, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLpLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hLocalPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8602",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            216, yPos + 22, 44, 22, hDlg, (HMENU)(INT_PTR)IDC_UDP_LOCAL_PORT, g_hInst, NULL);
    SendMessageW(hLocalPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUdpConn = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            366, yPos + 22, 70, 22, hDlg, (HMENU)(INT_PTR)IDC_UDP_CONNECT, g_hInst, NULL);
    SendMessageW(hUdpConn, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 版本行 (可选). 标签加宽避免被裁 */
    if (showVer) {
        HWND hvLbl = CreateWindowExW(0, L"STATIC", L"固件版本:",
                WS_CHILD | WS_VISIBLE, 20, yPos + 56, 64, 14, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hvLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hVer = CreateWindowExW(0, L"STATIC", L"未知",
                WS_CHILD | WS_VISIBLE, 84, yPos + 56, 200, 14,
                hDlg, (HMENU)(INT_PTR)version_id, g_hInst, NULL);
        SendMessageW(hVer, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hGetVer = CreateWindowExW(0, L"BUTTON", L"获取版本",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                296, yPos + 54, 140, 22, hDlg, (HMENU)(INT_PTR)getver_id, g_hInst, NULL);
        SendMessageW(hGetVer, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* 固件区 (可选, 内嵌 groupbox) */
    if (showFw) {
        int fy = yPos + (showVer ? 88 : 56);
        HWND hFLbl = CreateWindowExW(0, L"STATIC", L"固件文件:",
                WS_CHILD | WS_VISIBLE, 20, fy + 4, 60, 16, hDlg, NULL, g_hInst, NULL);
        HWND hFile = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                80, fy + 2, 260, 22, hDlg, (HMENU)(INT_PTR)fw_file_id, g_hInst, NULL);
        HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                350, fy + 2, 60, 22, hDlg, (HMENU)(INT_PTR)fw_browse_id, g_hInst, NULL);
        HWND hUpg = CreateWindowExW(0, L"BUTTON", L"升级",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                180, fy + 40, 80, 28, hDlg, (HMENU)(INT_PTR)fw_upgrade_id, g_hInst, NULL);
        SendMessageW(hFLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hFile,  WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hUpg,   WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    return boxH + 8;
}

/* 同步指定 CAN tab 的连接状态到 UI (按钮文字/控件禁用).
 * canTabIdx: CAN_TAB_BIND(Tab1) 或 CAN_TAB_UPGRADE(Tab2), 各 tab 独立. */
static void SyncCanConnState(int canTabIdx)
{
    int connected = (g_canTabChannel[canTabIdx] >= 0);
    /* CAN tab 索引 → 子对话框索引: BIND→Tab1(0), UPGRADE→Tab2(1) */
    HWND h = g_hTabDlg[canTabIdx];
    if (!h) return;
    SetWindowTextW(GetDlgItem(h, IDC_CAN_CONNECT), connected ? L"断开" : L"连接");
    EnableWindow(GetDlgItem(h, IDC_CAN_DEVICE),  connected ? FALSE : TRUE);
    EnableWindow(GetDlgItem(h, IDC_CAN_REFRESH), connected ? FALSE : TRUE);
    /* Tab2 升级按钮 + 获取版本按钮: 仅 UPGRADE tab */
    if (canTabIdx == CAN_TAB_UPGRADE) {
        EnableWindow(GetDlgItem(h, IDC_HFW_UPGRADE),
                     connected && strlen(g_handlerFwPath) > 0 ? TRUE : FALSE);
        EnableWindow(GetDlgItem(h, IDC_HFW_GETVER), connected ? TRUE : FALSE);
    }
}

/* 同步 UDP 连接状态到所有含 UDP groupbox 的 tab (Tab1/Tab3). */
/* tabIdx: 0=Tab1(绑定), 1=Tab3(配置). 只更新对应 tab 的连接 UI. */
static void SyncUdpConnState(int udpTabIdx)
{
    int connected = g_udpConnected[udpTabIdx];
    const wchar_t *text = connected ? L"断开" : L"连接";
    BOOL enable = connected ? FALSE : TRUE;
    /* Tab1(对话框 g_hTabDlg[0]) 对应 udpTabIdx=0; Tab3(g_hTabDlg[2]) 对应 udpTabIdx=1 */
    HWND h = (udpTabIdx == UDP_TAB_BIND) ? g_hTabDlg[0] : g_hTabDlg[2];
    if (h) {
        SetWindowTextW(GetDlgItem(h, IDC_UDP_CONNECT), text);
        EnableWindow(GetDlgItem(h, IDC_UDP_IP), enable);
        EnableWindow(GetDlgItem(h, IDC_UDP_LOCAL_PORT), enable);
    }
    /* Tab3 升级按钮 + 获取版本按钮启用条件 */
    if (udpTabIdx == UDP_TAB_CFG && g_hTabDlg[2]) {
        EnableWindow(GetDlgItem(g_hTabDlg[2], IDC_TFW_UPGRADE),
                     connected && strlen(g_receiverFwPath) > 0 ? TRUE : FALSE);
        EnableWindow(GetDlgItem(g_hTabDlg[2], IDC_TFW_GETVER), connected ? TRUE : FALSE);
    }
}

/* 创建各 tab 子对话框控件 (在 CreateTabLayout 后调用) */
static void CreateBindTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 6;
    y += CreateCanGroupBox(hDlg, y, -1, -1, -1, -1, -1);    /* Tab1 CAN (无版本/无固件区) */
    y += CreateUdpGroupBox(hDlg, y, -1, -1, -1, -1, -1);    /* Tab1 UDP (无版本/无固件区) */

    /* ===== 操作按钮: 检测绑定状态 / 绑定设备 ===== */
    struct { const wchar_t *text; int id; } btns[] = {
        { L"检测绑定状态",  IDC_BTN_CHECK_BIND },
        { L"绑定设备",      IDC_BTN_BIND       },
    };
    for (int i = 0; i < 2; i++) {
        HWND hBtn = CreateWindowExW(0, L"BUTTON", btns[i].text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                30, y, 240, 32, hDlg, (HMENU)(INT_PTR)btns[i].id, g_hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 40;
    }
}

static void CreateHandlerFwTabControls(HWND hDlg)
{
    /* Tab2: 一个 CAN groupbox 包含 连接+版本+固件区 */
    CreateCanGroupBox(hDlg, 6, IDC_HFW_VERSION, IDC_HFW_GETVER,
                      IDC_HFW_FILE, IDC_HFW_BROWSE, IDC_HFW_UPGRADE);
}

static void CreateTransmitterFwTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    /* Tab3: 一个 UDP groupbox 包含 连接+版本+固件区, 下方独立网络参数区 */
    int y = 6;
    y += CreateUdpGroupBox(hDlg, y, IDC_TFW_VERSION, IDC_TFW_GETVER,
                           IDC_TFW_FILE, IDC_TFW_BROWSE, IDC_TFW_UPGRADE);  /* y=6→196 */

    /* 网络参数 groupbox: 设置接收器 IP + 数据端口 (SET_NET 0x12).
     * 掩码固定 255.255.255.0, 网关=IP 末段改 1, 固件自算, 不传. */
    int ny = y;  /* 紧接 UDP groupbox 下方 */
    CreateWindowExW(0, L"BUTTON", L"网络参数",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, ny, 436, 70, hDlg, NULL, g_hInst, NULL);
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"IP:",
            WS_CHILD | WS_VISIBLE, 20, ny + 26, 24, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hNetIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            46, ny + 24, 110, 22, hDlg, (HMENU)(INT_PTR)IDC_NET_IP, g_hInst, NULL);
    SendMessageW(hNetIp, WM_SETFONT, (WPARAM)hFont, TRUE);
    hLbl = CreateWindowExW(0, L"STATIC", L"数据端口:",
            WS_CHILD | WS_VISIBLE, 166, ny + 26, 64, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hNetPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            234, ny + 24, 56, 22, hDlg, (HMENU)(INT_PTR)IDC_NET_PORT, g_hInst, NULL);
    SendMessageW(hNetPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hApply = CreateWindowExW(0, L"BUTTON", L"设置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            300, ny + 24, 60, 22, hDlg, (HMENU)(INT_PTR)IDC_NET_APPLY, g_hInst, NULL);
    SendMessageW(hApply, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hQuery = CreateWindowExW(0, L"BUTTON", L"查询",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            366, ny + 24, 70, 22, hDlg, (HMENU)(INT_PTR)IDC_NET_QUERY, g_hInst, NULL);
    SendMessageW(hQuery, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/* 创建 Tab4 设备查找控件: 开始/停止按钮 + IP 列表 + 复制按钮 */
static void CreateDiscoverTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    /* 开始/停止查找 按钮 */
    HWND hStart = CreateWindowExW(0, L"BUTTON", L"开始查找",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 20, 120, 28, hDlg, (HMENU)(INT_PTR)IDC_DISC_START, g_hInst, NULL);
    SendMessageW(hStart, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 复制选中 IP 按钮 */
    HWND hCopy = CreateWindowExW(0, L"BUTTON", L"复制选中 IP",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            150, 20, 120, 28, hDlg, (HMENU)(INT_PTR)IDC_DISC_COPY, g_hInst, NULL);
    SendMessageW(hCopy, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 发现的 IP 列表 (LISTBOX, 支持单选) */
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
            20, 60, 300, 200, hDlg, (HMENU)(INT_PTR)IDC_DISC_LIST, g_hInst, NULL);
    SendMessageW(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/* CAN 帧回调: 收到 0x111 RF24 配置响应时填 g_handlerAddr 并置标志 */
static void can_frame_cb(const CanFrame *frame, void *user_data)
{
    (void)user_data;
    if (frame->id == CAN_ID_RF24_CONFIG_RESP && frame->dlc >= 7) {
        /* [cmd 1B][channel 1B][addr 5B][reserved 1B] */
        g_handlerCh = frame->data[1];
        memcpy(g_handlerAddr, frame->data + 2, 5);
        g_handlerAddrGot = TRUE;
    }
}

static void can_msg_cb(const char *msg, void *user_data)
{
    /* 底层模块 (CanManager) 的状态消息回调. 无日志区, 静默忽略
     * (关键状态由业务函数各自的 MessageBoxW 提示). */
    (void)msg; (void)user_data;
}

static void udp_msg_cb(const char *msg, void *user_data)
{
    /* 底层模块 (UdpManager) 的状态消息回调. 无日志区, 静默忽略. */
    (void)msg; (void)user_data;
}

/* 发 CAN 0x110 GET_CONFIG 并等待 0x111 响应 (轮询标志, 超时 800ms).
 * 成功返回 true, g_handlerCh/g_handlerAddr 已填. */
static BOOL ReadHandlerNrf(void)
{
    g_handlerAddrGot = FALSE;
    uint8_t data[8] = { 0 };
    data[0] = RF24_CMD_GET_CONFIG;
    if (!CanManager_Send(g_canTab[CAN_TAB_BIND], CAN_ID_RF24_CONFIG_CMD, data, 8)) {
        return FALSE;
    }
    /* 轮询等待 (frame_cb 在 RX 线程置标志) */
    for (int i = 0; i < 80; i++) {
        if (g_handlerAddrGot) return TRUE;
        Sleep(10);
    }
    return FALSE;
}

/* 前向声明: 连接成功后自动读版本, 定义在后面 */
static void OnGetVersionCan(HWND hChildDlg);
static void OnGetVersionUdp(HWND hChildDlg);

/* CAN 连接/断开 (各 tab 独立). hChildDlg 的 GWLP_USERDATA 给出 tab 索引.
 * Tab1(0)→CAN_TAB_BIND, Tab2(1)→CAN_TAB_UPGRADE. 失败友好提示占用原因. */
static void OnCanConnect(HWND hChildDlg)
{
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);
    /* Tab1/Tab2 对应 CAN_TAB_BIND/CAN_TAB_UPGRADE (数值一致) */
    int canTabIdx = (tabIdx == 0) ? CAN_TAB_BIND : CAN_TAB_UPGRADE;

    if (g_canTabChannel[canTabIdx] >= 0) {
        /* 断开 */
        CanManager_Disconnect(g_canTab[canTabIdx]);
        g_canTabChannel[canTabIdx] = -1;
        SyncCanConnState(canTabIdx);
        return;
    }
    /* 连接: 从下拉取选中设备名 */
    int sel = (int)SendMessageW(GetDlgItem(hChildDlg, IDC_CAN_DEVICE), CB_GETCURSEL, 0, 0);
    if (sel < 0) {
        MessageBoxW(g_hMain, L"请先选择设备 (或点刷新扫描)", L"提示",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t wname[256] = { 0 };
    SendMessageW(GetDlgItem(hChildDlg, IDC_CAN_DEVICE), CB_GETLBTEXT, sel, (LPARAM)wname);
    char dev[256] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wname, -1, dev, sizeof(dev), NULL, NULL);

    int channel = 0;
    sscanf(dev, "PCAN_USB_%d (0x%X)", &channel, &channel);

    /* 先检查: 另一个 CAN tab 是否已占同一 channel → 友好提示 */
    for (int other = 0; other < CAN_TAB_COUNT; other++) {
        if (other != canTabIdx && g_canTabChannel[other] == channel) {
            wchar_t wmsg[160];
            const wchar_t *otherName = (other == CAN_TAB_BIND) ? L"手柄绑定页" : L"手柄升级页";
            swprintf(wmsg, 160,
                L"设备 %hs 已被本工具 %s 占用\n请先在该页断开, 或选择其他设备", dev, otherName);
            MessageBoxW(g_hMain, wmsg, L"设备被占用", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (!CanManager_Connect(g_canTab[canTabIdx], channel, PCAN_BAUD_250K)) {
        /* 连接失败: 用 PCAN status 友好提示. status==0 但失败=异常; 否则按常见码归类 */
        uint32_t st = CanManager_GetLastError(g_canTab[canTabIdx]);
        wchar_t wmsg[200];
        if (st == 0) {
            swprintf(wmsg, 200, L"连接失败\n设备 %hs 可能未正确连接或驱动异常", dev);
        } else {
            /* 取 PCAN 错误文本 (英文, Pcan_GetErrorText 在 pcan_loader 已加载) */
            char err[256] = "PCAN error";
            if (Pcan_GetErrorText) {
                Pcan_GetErrorText(st, 0x0409 /* English */, err);  /* 中文 0x0804 可能不支持, 用英文 */
            }
            swprintf(wmsg, 200,
                L"连接失败 (0x%X)\n%hs\n\n设备可能被其他程序占用, 或设备未连接",
                st, err);
        }
        MessageBoxW(g_hMain, wmsg, L"连接失败", MB_OK | MB_ICONERROR);
        return;
    }
    CanManager_StartRxThread(g_canTab[canTabIdx]);
    /* Tab1 (BIND) 先验证设备响应 (0x110 GET_CONFIG), 失败则断开且不标记已连接 */
    if (canTabIdx == CAN_TAB_BIND) {
        if (!ReadHandlerNrf()) {
            MessageBoxW(g_hMain,
                L"连接失败\n"
                L"请确认设备是手柄且处于CAN模式",
                L"连接失败", MB_OK | MB_ICONERROR);
            CanManager_Disconnect(g_canTab[CAN_TAB_BIND]);
            return;
        }
    }
    /* 验证通过 (Tab1) 或非 Tab1 (Tab2): 正式标记已连接 */
    g_canTabChannel[canTabIdx] = channel;
    SyncCanConnState(canTabIdx);
    if (canTabIdx == CAN_TAB_UPGRADE) {
        /* 升级 tab 连接成功后自动读版本 */
        OnGetVersionCan(hChildDlg);
    }
}

/* 接收器连接/断开 (从 IP 框取目标 IP, 配置端口固定 8601). 已连接则断开. */
static void OnUdpConnect(HWND hChildDlg)
{
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);
    int udpTab = (tabIdx == 0) ? UDP_TAB_BIND : UDP_TAB_CFG;
    UdpManager *mgr = g_cfgUdp[udpTab];
    if (g_udpConnected[udpTab]) {
        /* 断开 */
        UdpManager_Unbind(mgr);
        g_udpConnected[udpTab] = 0;
        SyncUdpConnState(udpTab);
        return;
    }
    /* 取 IP 框内容 (纯单播: 必须是具体 IP, 拒绝空/广播地址) */
    wchar_t wip[64] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_UDP_IP), wip, 64);
    char ip[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wip, -1, ip, sizeof(ip), NULL, NULL);
    if (!ip[0]) {
        MessageBoxW(g_hMain, L"请填写接收器具体 IP 地址\n(本页只支持单播, 广播请用「设备查找」页)",
                    L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 校验: 拒绝有限广播 255.255.255.255 和子网定向广播 (x.x.x.255) */
    unsigned long nip = inet_addr(ip);
    if (nip == INADDR_NONE || nip == INADDR_ANY) {
        MessageBoxW(g_hMain, L"IP 地址格式不正确", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    if (strcmp(ip, "255.255.255.255") == 0 || (nip & 0xFF) == 0xFF) {
        MessageBoxW(g_hMain, L"本页只支持单播 IP\n广播发现请用「设备查找」页", L"提示",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    /* 读本地端口 (默认 8602). 远程固定 8601 (配置端口), 单播到指定 IP.
     * Tab1 和 Tab3 独立实例, 默认端口同为 8602 (不会同时用, 用户每次只操作一个 tab). */
    wchar_t wlp[16] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_UDP_LOCAL_PORT), wlp, 16);
    int local_port = _wtoi(wlp);
    if (local_port <= 0 || local_port > 65535) local_port = 8602;
    if (!UdpManager_Bind(mgr, UDP_CHAN_CONFIG, (uint16_t)local_port, ip, 8601)) {
        int err = WSAGetLastError();
        wchar_t wmsg[200];
        swprintf(wmsg, 200,
            L"接收器连接失败\n本地端口 %d 可能被占用 (WSA 错误码: %d)\n请更换本地端口或关闭占用该端口的程序",
            local_port, err);
        MessageBoxW(g_hMain, wmsg, L"连接失败", MB_OK | MB_ICONERROR);
        return;
    }
    UdpManager_StartRxThread(mgr);
    /* Tab1 先验证设备响应 (GET_RF24), 失败则断开且不标记已连接 (不显示"已连接") */
    if (udpTab == UDP_TAB_BIND) {
        uint8_t ch, addr[5];
        if (!UdpManager_GetRF24(mgr, &ch, addr)) {
            MessageBoxW(g_hMain,
                L"连接失败\n"
                L"请确认 IP 正确且接收器已上电",
                L"连接失败", MB_OK | MB_ICONERROR);
            UdpManager_Unbind(mgr);   /* 停 RX 线程 + 关 socket */
            return;
        }
    }
    /* 验证通过 (Tab1) 或非 Tab1 (Tab3): 正式标记已连接 */
    g_udpConnected[udpTab] = 1;
    SyncUdpConnState(udpTab);
    if (udpTab == UDP_TAB_CFG && GetDlgItem(hChildDlg, IDC_TFW_VERSION)) {
        /* Tab3 连接成功后自动读版本 */
        OnGetVersionUdp(hChildDlg);
    }
}

/* ===== Tab4 设备查找: 原生 winsock 广播 GET_NET 收集响应源 IP =====
 * 不走 UdpManager (其 data_cb 不带源 IP); 直接 socket 收发, 拿 recvfrom 的源地址 */

/* 设备查找线程: 广播 GET_NET, 收集 2s 内所有响应的源 IP, 去重后 PostMessage 到主线程 */
static DWORD WINAPI discover_thread(LPVOID param)
{
    (void)param;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;

    BOOL bc = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&bc, sizeof(bc));
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(8602);   /* 本地端口 8602 */
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        closesocket(s);
        return 0;
    }

    /* 用有限广播 255.255.255.255 发 GET_NET (0x13), 确保跨子网到达设备
     * (设备 IP 可能被改到与本机不同子网, 子网定向广播 x.x.x.255 到不了). */
    uint8_t pkt = UDP_CMD_GET_NET;   /* 0x13 */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(8601);
    dst.sin_addr.s_addr = INADDR_BROADCAST;   /* 255.255.255.255 */
    sendto(s, (const char *)&pkt, 1, 0, (struct sockaddr *)&dst, sizeof(dst));

    /* 接收窗口 10s (只收不重发). 取响应源 IP 去重上报.
     * 用户可随时点"停止查找"提前结束 (g_discRunning=FALSE). */
    time_t end = time(NULL) + 10;
    while (g_discRunning && time(NULL) < end) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        struct timeval tv = { 0, 500000 };  /* 500ms 一次, 便于及时响应停止 */
        int r = select(0, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct sockaddr_in src;
        int alen = sizeof(src);
        uint8_t buf[64];
        int n = recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&src, &alen);
        if (n <= 0) continue;
        /* 响应首字节应是 GET_NET (0x13); 宽松校验, 主要取源 IP */
        char *ipstr = inet_ntoa(src.sin_addr);
        if (ipstr) {
            PostMessageA(g_hMain, WM_DISC_FOUND_IP, 0, (LPARAM)_strdup(ipstr));
        }
    }

    closesocket(s);
    /* 查找结束 (10s 到或用户停止), 通知主线程恢复按钮文字 */
    g_discRunning = FALSE;
    PostMessageW(g_hMain, WM_DISC_DONE, 0, 0);
    return 0;
}

/* 开始/停止设备查找 */
static void OnDiscoverStart(HWND hChildDlg)
{
    if (g_discRunning) {
        /* 停止 */
        g_discRunning = FALSE;
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_DISC_START), L"开始查找");
        return;
    }
    /* 开始查找: 不清空列表 (累积去重, 多次扫描同一 IP 只显示一次), 启动发现线程 */
    g_discRunning = TRUE;
    SetWindowTextW(GetDlgItem(hChildDlg, IDC_DISC_START), L"停止查找");
    CreateThread(NULL, 0, discover_thread, NULL, 0, NULL);
}

/* 复制选中 IP 到剪贴板 */
static void OnDiscoverCopy(HWND hChildDlg)
{
    HWND hList = GetDlgItem(hChildDlg, IDC_DISC_LIST);
    int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (sel < 0) {
        MessageBoxW(g_hMain, L"请先在列表中选择一个 IP", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t wip[64] = { 0 };
    SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)wip);
    if (!OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    size_t len = wcslen(wip) + 1;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(wchar_t));
    if (hg) {
        memcpy(GlobalLock(hg), wip, len * sizeof(wchar_t));
        GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
    /* 不弹框, 静默复制 (避免打扰). */
}

/* 设置接收器网络参数 (SET_NET 0x12): IP + 数据端口. 掩码固定, 网关固件自算 */
static void OnNetApply(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t wip[64] = { 0 }, wport[16] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_NET_IP), wip, 64);
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_NET_PORT), wport, 16);
    char ip[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wip, -1, ip, sizeof(ip), NULL, NULL);
    if (!ip[0]) {
        MessageBoxW(g_hMain, L"请填写 IP 地址", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* IP 格式校验 */
    unsigned long nip = inet_addr(ip);
    if (nip == INADDR_NONE) {
        MessageBoxW(g_hMain, L"IP 地址格式不正确", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    int port = _wtoi(wport);
    if (port <= 0 || port > 65535) {
        MessageBoxW(g_hMain, L"数据端口必须在 1-65535 范围内", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* SET_NET: [ip 4B][port 2B BE], 掩码固定 255.255.255.0 网关自动派生 */
    if (UdpManager_SetNet(g_cfgUdp[UDP_TAB_CFG], ip, (uint16_t)port)) {
        MessageBoxW(g_hMain, L"网络参数已发送\n重启接收器后生效", L"成功",
                    MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(g_hMain, L"设置失败 (发送命令失败)", L"错误", MB_OK | MB_ICONERROR);
    }
}

/* 查询接收器网络参数 (GET_NET 0x13): 回填 IP + 数据端口到输入框 */
static void OnNetQuery(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    char ip[16] = { 0 };
    uint16_t port = 0;
    if (UdpManager_GetNet(g_cfgUdp[UDP_TAB_CFG], ip, sizeof(ip), &port)) {
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_NET_IP), ip);
        char port_str[8];
        sprintf(port_str, "%d", port);
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_NET_PORT), port_str);
    } else {
        MessageBoxW(g_hMain, L"查询失败 (接收器未响应)", L"提示", MB_OK | MB_ICONWARNING);
    }
}

/* 获取手柄 CAN 固件版本并显示 (Tab2). CAN 版本是 uint32: 高中低字节=主次补丁 */
static void OnGetVersionCan(HWND hChildDlg)
{
    if (g_canTabChannel[CAN_TAB_UPGRADE] < 0) {
        MessageBoxW(g_hMain, L"请先连接手柄", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    uint32_t ver = 0;
    if (CanManager_GetVersion(g_canTab[CAN_TAB_UPGRADE], &ver)) {
        wchar_t vstr[32];
        swprintf(vstr, 32, L"%d.%d.%d",
                 (ver >> 16) & 0xFF, (ver >> 8) & 0xFF, ver & 0xFF);
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_HFW_VERSION), vstr);
    } else {
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_HFW_VERSION), L"读取失败");
    }
}

/* 获取接收器 UDP 固件版本并显示 (Tab3). UDP 版本是字符串 (如 "0.1.0-dev") */
static void OnGetVersionUdp(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    char ver[64] = { 0 };
    if (UdpManager_GetVersion(g_cfgUdp[UDP_TAB_CFG], ver, sizeof(ver))) {
        wchar_t wver[64];
        MultiByteToWideChar(CP_ACP, 0, ver, -1, wver, 64);
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_TFW_VERSION), wver);
    } else {
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_TFW_VERSION), L"读取失败");
    }
}

/* 检测绑定状态: 读手柄 NRF + 接收器 NRF, 比对 */
static void OnCheckBind(HWND hChildDlg)
{
    if (g_canTabChannel[CAN_TAB_BIND] < 0 || !g_udpConnected[UDP_TAB_BIND]) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 1. 读手柄 NRF */
    if (!ReadHandlerNrf()) {
        MessageBoxW(g_hMain, L"读取手柄地址超时\n请确认手柄已上电", L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }
    /* 2. 读接收器 NRF */
    if (!UdpManager_GetRF24(g_cfgUdp[UDP_TAB_BIND], &g_receiverCh, g_receiverAddr)) {
        MessageBoxW(g_hMain,
            L"读取接收器 NRF 地址超时\n请确认接收器已上电并在同一网络",
            L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    /* 3. 比对 */
    BOOL all_zero = (g_receiverAddr[0]|g_receiverAddr[1]|g_receiverAddr[2]|
                     g_receiverAddr[3]|g_receiverAddr[4]) == 0;
    BOOL same = (memcmp(g_receiverAddr, g_handlerAddr, 5) == 0);

    /* 只显示绑定状态结论, 不显示地址 */
    if (all_zero) {
        MessageBoxW(g_hMain,
            L"当前手柄和接收器未绑定\n点击「绑定设备」可绑定当前手柄和接收器",
            L"绑定状态: 未绑定", MB_OK | MB_ICONINFORMATION);
    } else if (!same) {
        MessageBoxW(g_hMain,
            L"当前手柄和接收器未绑定\n接收器已绑定其他设备\n点击「绑定设备」可重新绑定当前手柄和接收器",
            L"绑定状态: 未绑定", MB_OK | MB_ICONWARNING);
    } else {
        MessageBoxW(g_hMain, L"当前手柄和接收器已绑定", L"绑定状态: 已绑定", MB_OK | MB_ICONINFORMATION);
    }
}

/* 绑定设备: 把手柄 NRF 地址写入接收器 */
static void OnBind(HWND hChildDlg)
{
    (void)hChildDlg;
    if (g_canTabChannel[CAN_TAB_BIND] < 0 || !g_udpConnected[UDP_TAB_BIND]) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 确保已有手柄地址 (没读过则先读) */
    if (!g_handlerAddrGot) {
        if (!ReadHandlerNrf()) {
            MessageBoxW(g_hMain, L"读取手柄地址超时", L"错误",
                        MB_OK | MB_ICONERROR);
            return;
        }
    }
    /* 先读接收器地址, 检查是否已绑定当前手柄 */
    uint8_t recvAddr[5];
    uint8_t recvCh;
    if (!UdpManager_GetRF24(g_cfgUdp[UDP_TAB_BIND], &recvCh, recvAddr)) {
        MessageBoxW(g_hMain, L"读取接收器地址超时", L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }
    BOOL recvBound = (recvAddr[0]|recvAddr[1]|recvAddr[2]|recvAddr[3]|recvAddr[4]) != 0;
    if (recvBound && memcmp(recvAddr, g_handlerAddr, 5) == 0) {
        MessageBoxW(g_hMain, L"当前手柄和接收器已绑定", L"已绑定",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g_hMain, L"确认绑定当前手柄和接收器吗?",
                    L"确认绑定", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (UdpManager_SetRF24(g_cfgUdp[UDP_TAB_BIND], g_handlerCh, g_handlerAddr)) {
        MessageBoxW(g_hMain, L"绑定成功", L"绑定成功", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(g_hMain, L"绑定失败", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}

/* 固件升级线程参数: CAN 与 UDP 共用 (UDP 分支在 Task 7 填) */
typedef struct {
    HWND hMain;
    char path[MAX_PATH];
    int isCan;          /* 1=CAN, 0=UDP */
} FwUpgradeParam;

/* CAN 升级 msg_cb 上下文: 携带主窗口 + keyhash 拒绝标志.
 * msg_cb 在工作线程被 can_manager 调用, 检测到 keyhash 拒绝时置标志,
 * 线程主循环据此发 WM_FW_REJECTED (而非 WM_FW_COMPLETE), 避免再弹进度窗结果. */
typedef struct {
    HWND hMain;
    BOOL keyhashRejected;
} CanFwMsgCtx;

/* CAN 升级 msg_cb: can_manager 内部状态消息 (UTF-8 char*).
 * 只对 keyhash 校验失败这种需用户知晓的拒绝弹框; 其余进度/完成/失败消息
 * 进度窗 (FW_Done) 已展示, 这里静默, 避免弹框过多打扰用户. */
static void can_fw_msg_cb(const char *msg, void *user_data)
{
    CanFwMsgCtx *ctx = (CanFwMsgCtx *)user_data;
    if (!msg) return;
    /* 仅 keyhash 拒绝需要弹框提示并标记 (后续发 WM_FW_REJECTED 销毁进度窗). */
    if (strstr(msg, "keyhash")) {
        if (ctx) ctx->keyhashRejected = TRUE;
        wchar_t wmsg[256];
        MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg, sizeof(wmsg) / sizeof(wmsg[0]));
        MessageBoxW(g_hMain, wmsg, L"固件升级", MB_OK | MB_ICONWARNING);
    }
}

static void can_fw_progress_cb(const char *pct_str, void *user_data)
{
    HWND hMain = (HWND)user_data;
    if (!hMain || !pct_str) return;
    /* 第一次进度回调: keyhash 已通过, 进入数据传输 → 此时才弹进度窗 */
    if (!g_progressDlg) {
        PostMessageA(hMain, WM_FW_SHOW_PROGRESS, 0, 0);
    }
    int pct = atoi(pct_str);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    wchar_t buf[32];
    swprintf(buf, 32, L"升级中 %d%%", pct);
    PostMessageA(hMain, WM_UPDATE_PROGRESS, (WPARAM)pct, (LPARAM)_wcsdup(buf));
}

/* 固件升级工作线程: CAN/UDP 共用. 升级完成后 PostMessage WM_FW_COMPLETE */
static DWORD WINAPI fw_upgrade_thread(LPVOID param)
{
    FwUpgradeParam *p = (FwUpgradeParam *)param;
    HWND hMain = p->hMain;
    g_progressIsCan = p->isCan ? TRUE : FALSE;

    /* 升级前: 校验 MCUboot 镜像头 + 提取 KEYHASH (32B, 供 FW 端校验).
     * 非 MCUboot 文件 (任意二进制/文本) 直接拒绝, 不进入升级流程.
     * CAN/UDP 共用, 弹框提示 (与其它错误处理一致的 MessageBoxW). */
    uint8_t fw_kh[IMG_KEYHASH_LEN];
    bool has_kh = false;
    {
        HANDLE hChk = CreateFileA(p->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hChk == INVALID_HANDLE_VALUE) {
            MessageBoxW(g_hMain, L"打开固件文件失败", L"错误", MB_OK | MB_ICONERROR);
            PostMessageA(hMain, WM_FW_COMPLETE, p->isCan ? 1 : 0, 0);
            free(p);
            return 0;
        }
        DWORD chkSize = GetFileSize(hChk, NULL);
        uint8_t *chkData = (uint8_t *)malloc(chkSize);
        DWORD nb;
        BOOL rdOk = ReadFile(hChk, chkData, chkSize, &nb, NULL) && nb == chkSize;
        CloseHandle(hChk);
        BOOL headerOk = FALSE;
        if (rdOk) {
            headerOk = fw_image_validate_header(chkData, chkSize);
            if (headerOk) {
                has_kh = fw_image_extract_keyhash(chkData, chkSize, fw_kh);
            }
        }
        free(chkData);
        if (!headerOk) {
            MessageBoxW(g_hMain, L"固件文件格式非法 (非 MCUboot 镜像), 已拒绝",
                        L"固件升级", MB_OK | MB_ICONWARNING);
            PostMessageA(hMain, WM_FW_COMPLETE, p->isCan ? 1 : 0, 0);
            free(p);
            return 0;
        }
    }

    /* 进度窗延迟到第一次进度回调时才弹 (keyhash 校验通过、进入数据传输后),
     * 避免 keyhash 校验失败时进度窗先弹出造成顺序错乱. */

    bool result = false;
    bool rejected = false;  /* keyhash 校验被 FW 拒绝: 只弹警告框, 不再显示进度窗结果 */
    if (p->isCan) {
        /* CAN 升级: test_mode 固定 0 (永久). keyhash 随 0x104 帧前置发送供 FW 校验.
         * msg_cb 经 CanFwMsgCtx 捕获 keyhash 拒绝并弹框; 标记后线程尾发 WM_FW_REJECTED. */
        CanFwMsgCtx ctx = { hMain, FALSE };
        result = CanManager_FirmwareUpgrade(g_canTab[CAN_TAB_UPGRADE], p->path, 0,
                                            has_kh ? fw_kh : NULL,
                                            can_fw_msg_cb, (void *)&ctx,
                                            can_fw_progress_cb, (void *)hMain);
        if (ctx.keyhashRejected) rejected = true;
    } else {
        /* UDP 升级: START(size+keyhash) → DATA(256B/包, offset 校验) → END(crc+testmode) */
        HANDLE hFile = CreateFileA(p->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            MessageBoxW(g_hMain, L"打开固件文件失败", L"错误", MB_OK | MB_ICONERROR);
        } else {
            DWORD fileSize = GetFileSize(hFile, NULL);
            uint8_t *fileData = (uint8_t *)malloc(fileSize);
            DWORD bytesRead;
            ReadFile(hFile, fileData, fileSize, &bytesRead, NULL);
            CloseHandle(hFile);

            uint16_t crc = UdpManager_CRC16_CCITT(fileData, fileSize);

            result = false;
            uint8_t fw_resp = 0;
            if (UdpManager_FirmwareStart(g_cfgUdp[UDP_TAB_CFG], fileSize, has_kh ? fw_kh : NULL, &fw_resp)) {
                /* START 成功 = keyhash 已通过, 此时才弹进度窗 */
                PostMessageA(hMain, WM_FW_SHOW_PROGRESS, 0, 0);
                int offset = 0, chunk = 256;
                int last_pct = -1;
                result = true;
                while (offset < (int)fileSize) {
                    int send_len = ((int)fileSize - offset > chunk) ? chunk : ((int)fileSize - offset);
                    uint32_t got = 0;
                    if (!UdpManager_FirmwareData(g_cfgUdp[UDP_TAB_CFG], fileData + offset,
                                                 send_len, offset + send_len, &got)) {
                        wchar_t wmsg[128];
                        swprintf(wmsg, 128,
                            L"数据发送失败 offset=%d\n固件 offset=%lu",
                            offset, got);
                        MessageBoxW(g_hMain, wmsg, L"升级失败", MB_OK | MB_ICONERROR);
                        result = false;
                        break;
                    }
                    offset += send_len;
                    /* 仅当整数百分比变化时才发进度, 否则每个 256B 包都重绘 label -> 闪烁. */
                    int pct = (int)((long long)offset * 100 / fileSize);
                    if (pct != last_pct) {
                        wchar_t buf[32];
                        swprintf(buf, 32, L"升级中 %d%%", pct);
                        PostMessageA(hMain, WM_UPDATE_PROGRESS, (WPARAM)pct, (LPARAM)_wcsdup(buf));
                        last_pct = pct;
                    }
                }
                if (result) {
                    if (UdpManager_FirmwareEnd(g_cfgUdp[UDP_TAB_CFG], 0, crc)) {
                        /* 成功, 不弹窗 (FW_Done 会显示) */
                    } else {
                        MessageBoxW(g_hMain, L"烧写失败 (CRC 不匹配或错误)", L"升级失败",
                                    MB_OK | MB_ICONERROR);
                        result = false;
                    }
                }
            } else {
                /* START 失败: resp=2 为 keyhash 校验被拒, 单独给出明确警告并标记拒绝. */
                if (fw_resp == 2) {
                    MessageBoxW(g_hMain,
                        L"FW: 固件 keyhash 校验失败, 已拒绝升级",
                        L"升级被拒绝", MB_OK | MB_ICONWARNING);
                    rejected = true;
                } else {
                    MessageBoxW(g_hMain, L"开始烧写失败 (固件未响应 START)", L"升级失败",
                                MB_OK | MB_ICONERROR);
                }
            }
            free(fileData);
        }
    }

    /* keyhash 被拒: 只弹过警告框, 销毁进度窗不显示结果; 否则正常 WM_FW_COMPLETE. */
    if (rejected) {
        PostMessageA(hMain, WM_FW_REJECTED, p->isCan ? 1 : 0, 0);
    } else {
        PostMessageA(hMain, WM_FW_COMPLETE, p->isCan ? 1 : 0, result ? 1 : 0);
    }
    free(p);
    return 0;
}

/* 主窗口收到子对话框转发的命令: hChildDlg=子对话框句柄, wParam 含控件 ID (LOWORD) */
static void OnTabCommand(HWND hChildDlg, WPARAM wParam)
{
    int cmdId = LOWORD(wParam);
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);

    /* CAN/UDP 连接命令三 tab 共享 (Tab1/Tab2 有 CAN, Tab1/Tab3 有 UDP) */
    switch (cmdId) {
    case IDC_CAN_REFRESH:       RefreshCanDevices(GetDlgItem(hChildDlg, IDC_CAN_DEVICE), tabIdx); return;
    case IDC_CAN_CONNECT:       OnCanConnect(hChildDlg);   return;
    case IDC_UDP_CONNECT:       OnUdpConnect(hChildDlg);   return;
    }

    if (tabIdx == 0) {
        /* 手柄绑定页: 剩余专属按钮 */
        switch (cmdId) {
        case IDC_BTN_CHECK_BIND:    OnCheckBind(hChildDlg);    break;
        case IDC_BTN_BIND:          OnBind(hChildDlg);         break;
        }
    } else if (tabIdx == 1) {
        /* 手柄固件升级页 (CAN) */
        switch (cmdId) {
        case IDC_HFW_BROWSE: {
            OPENFILENAMEA ofn;
            char file[MAX_PATH] = { 0 };
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_hMain;
            ofn.lpstrFilter = "Firmware (*.bin)\0*.bin\0All\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                strcpy(g_handlerFwPath, file);
                SetWindowTextA(GetDlgItem(hChildDlg, IDC_HFW_FILE), file);
                EnableWindow(GetDlgItem(hChildDlg, IDC_HFW_UPGRADE),
                             g_canTabChannel[CAN_TAB_UPGRADE] >= 0 ? TRUE : FALSE);
            }
            break;
        }
        case IDC_HFW_UPGRADE:
            if (g_canTabChannel[CAN_TAB_UPGRADE] >= 0 && strlen(g_handlerFwPath) > 0) {
                EnableWindow(GetDlgItem(hChildDlg, IDC_HFW_UPGRADE), FALSE);
                EnableWindow(GetDlgItem(hChildDlg, IDC_HFW_BROWSE), FALSE);
                FwUpgradeParam *param = (FwUpgradeParam *)malloc(sizeof(FwUpgradeParam));
                param->hMain = g_hMain;
                strcpy(param->path, g_handlerFwPath);
                param->isCan = 1;
                CreateThread(NULL, 0, fw_upgrade_thread, param, 0, NULL);
            } else if (g_canTabChannel[CAN_TAB_UPGRADE] < 0) {
                MessageBoxW(g_hMain, L"请先在本页连接手柄设备", L"提示",
                            MB_OK | MB_ICONWARNING);
            }
            break;
        case IDC_HFW_GETVER:          OnGetVersionCan(hChildDlg); break;
        }
    } else if (tabIdx == 2) {
        /* 接收器固件升级页 (UDP) */
        switch (cmdId) {
        case IDC_TFW_BROWSE: {
            OPENFILENAMEA ofn;
            char file[MAX_PATH] = { 0 };
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_hMain;
            ofn.lpstrFilter = "Firmware (*.bin)\0*.bin\0All\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                strcpy(g_receiverFwPath, file);
                SetWindowTextA(GetDlgItem(hChildDlg, IDC_TFW_FILE), file);
                EnableWindow(GetDlgItem(hChildDlg, IDC_TFW_UPGRADE), g_udpConnected[UDP_TAB_CFG] ? TRUE : FALSE);
            }
            break;
        }
        case IDC_TFW_UPGRADE:
            if (g_udpConnected[UDP_TAB_CFG] && strlen(g_receiverFwPath) > 0) {
                EnableWindow(GetDlgItem(hChildDlg, IDC_TFW_UPGRADE), FALSE);
                EnableWindow(GetDlgItem(hChildDlg, IDC_TFW_BROWSE), FALSE);
                FwUpgradeParam *param = (FwUpgradeParam *)malloc(sizeof(FwUpgradeParam));
                param->hMain = g_hMain;
                strcpy(param->path, g_receiverFwPath);
                param->isCan = 0;
                CreateThread(NULL, 0, fw_upgrade_thread, param, 0, NULL);
            } else if (!g_udpConnected[UDP_TAB_CFG]) {
                MessageBoxW(g_hMain, L"请先连接接收器", L"提示",
                            MB_OK | MB_ICONWARNING);
            }
            break;
        case IDC_NET_APPLY:          OnNetApply(hChildDlg);    break;
        case IDC_NET_QUERY:          OnNetQuery(hChildDlg);    break;
        case IDC_TFW_GETVER:         OnGetVersionUdp(hChildDlg); break;
        }
    } else if (tabIdx == 3) {
        /* 设备查找页 (Tab4) */
        switch (cmdId) {
        case IDC_DISC_START:         OnDiscoverStart(hChildDlg); break;
        case IDC_DISC_COPY:          OnDiscoverCopy(hChildDlg);  break;
        }
    }
}

/* 创建 Tab 控件 + 3 个子对话框, 在 WM_CREATE 中调用 */
static void CreateTabLayout(HWND hWnd)
{
    /* Tab 控件位于顶部 */
    g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
            0, 0, 480, 360, hWnd, (HMENU)1, g_hInst, NULL);

    /* 三个页签标题 */
    TCITEMW ti; ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"手柄绑定";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"手柄升级";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"手柄接收端配置";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 2, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"设备查找";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 3, (LPARAM)&ti);

    /* Tab 标题字体: 系统默认是粗体 (菜单字体), 改为 Segoe UI 9pt 常规, 更清爽 */
    HFONT hTabFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(g_hTab, WM_SETFONT, (WPARAM)hTabFont, TRUE);

    /* 子对话框显示区: tab 下方 */
    RECT rcTab;
    GetClientRect(g_hTab, &rcTab);
    SendMessageW(g_hTab, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcTab);

    RegisterTabChildClass();
    DWORD childStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    for (int i = 0; i < 4; i++) {
        g_hTabDlg[i] = CreateWindowExW(0, TABCHILD_CLASS, L"",
                childStyle,
                rcTab.left, rcTab.top,
                rcTab.right - rcTab.left, rcTab.bottom - rcTab.top,
                g_hTab, NULL, g_hInst, NULL);
        SetWindowLongPtrW(g_hTabDlg[i], GWLP_USERDATA, (LONG_PTR)i);
        ShowWindow(g_hTabDlg[i], i == 0 ? SW_SHOW : SW_HIDE);
    }

    /* 创建各 tab 控件 */
    CreateBindTabControls(g_hTabDlg[0]);
    CreateHandlerFwTabControls(g_hTabDlg[1]);
    CreateTransmitterFwTabControls(g_hTabDlg[2]);
    CreateDiscoverTabControls(g_hTabDlg[3]);
}

/* ===== 固件升级进度弹窗 (移植自 gateway-tool) ===== */

static LRESULT CALLBACK ProgressWndProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROG_BTN) {
            if (g_progressDone) DestroyWindow(hDlg);
        } else if (LOWORD(wParam) == IDC_PROG_REBOOT) {
            /* 发重启命令 (CAN 升级走 UPGRADE tab 的 CanManager, UDP 走 g_cfgUdp).
             * 重启后设备会断开重连, 旧连接失效 → 提示用户约 30s 后手动重连+读版本确认 */
            if (g_progressIsCan) {
                if (g_canTab[CAN_TAB_UPGRADE]) CanManager_Reboot(g_canTab[CAN_TAB_UPGRADE]);
            } else {
                if (g_cfgUdp[UDP_TAB_CFG]) UdpManager_Reboot(g_cfgUdp[UDP_TAB_CFG]);
            }
            DestroyWindow(hDlg);
            MessageBoxW(g_hMain,
                L"重启中, 约 30 秒完成固件升级\n完成后可直接查看升级后的版本信息",
                L"重启中", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    case WM_CLOSE:
        if (g_progressDone) DestroyWindow(hDlg);
        return 0;
    case WM_DESTROY:
        g_progressDlg = NULL;
        g_progressBar = NULL;
        g_progressLabel = NULL;
        g_progressBtn = NULL;
        g_progressReboot = NULL;
        return 0;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wParam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

static void FW_ShowProgress(HWND hParent)
{
    if (g_progressDlg) return;
    static BOOL s_registered = FALSE;
    if (!s_registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = ProgressWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = FW_PROGRESS_CLASS;
        RegisterClassW(&wc);
        s_registered = TRUE;
    }
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = { 0, 0, 300, 150 };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    g_progressDone = FALSE;
    g_progressDlg = CreateWindowExW(exStyle, FW_PROGRESS_CLASS, L"固件升级",
            style, 0, 0, winW, winH, hParent, NULL, g_hInst, NULL);
    RECT rcParent, rcDlg;
    GetWindowRect(hParent, &rcParent);
    GetWindowRect(g_progressDlg, &rcDlg);
    int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
    SetWindowPos(g_progressDlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);

    g_progressLabel = CreateWindowExW(0, L"STATIC", L"准备升级...",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 18, 260, 24, g_progressDlg, (HMENU)IDC_PROG_LABEL, g_hInst, NULL);
    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(g_progressLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_progressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            20, 52, 260, 16, g_progressDlg, (HMENU)IDC_PROG_BAR, g_hInst, NULL);
    SendMessageW(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
    g_progressBtn = CreateWindowExW(0, L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            110, 92, 80, 28, g_progressDlg, (HMENU)IDC_PROG_BTN, g_hInst, NULL);
    SendMessageW(g_progressBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_progressReboot = CreateWindowExW(0, L"BUTTON", L"重启设备",
            WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
            0, 0, 0, 0, g_progressDlg, (HMENU)IDC_PROG_REBOOT, g_hInst, NULL);
    SendMessageW(g_progressReboot, WM_SETFONT, (WPARAM)hFont, TRUE);
    ShowWindow(g_progressDlg, SW_SHOWNORMAL);
    UpdateWindow(g_progressDlg);
    EnableWindow(hParent, FALSE);
}

static void FW_UpdateProgress(int pct, const wchar_t *text)
{
    if (!g_progressDlg) return;
    SendMessageW(g_progressBar, PBM_SETPOS, pct, 0);
    if (text) SetWindowTextW(g_progressLabel, text);
}

static void FW_Done(HWND hParent, BOOL success)
{
    if (!g_progressDlg) return;
    g_progressDone = TRUE;
    ShowWindow(g_progressBar, SW_HIDE);
    SetWindowLongPtrW(g_progressLabel, GWL_STYLE,
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE);
    SetWindowPos(g_progressLabel, HWND_TOP, 20, 25, 260, 48, SWP_NOZORDER);
    if (success) {
        SetWindowTextW(g_progressLabel, L"升级完成！\n点击重启设备生效");
        SetWindowTextW(g_progressBtn, L"确定");
        EnableWindow(g_progressBtn, TRUE);
        SetWindowPos(g_progressBtn, HWND_TOP, 165, 92, 80, 28, SWP_SHOWWINDOW);
        SetWindowTextW(g_progressReboot, L"重启设备");
        EnableWindow(g_progressReboot, TRUE);
        SetWindowPos(g_progressReboot, HWND_TOP, 55, 92, 95, 28, SWP_SHOWWINDOW);
    } else {
        SetWindowTextW(g_progressLabel, L"升级失败\n请重试或检查设备连接");
        ShowWindow(g_progressReboot, SW_HIDE);
        EnableWindow(g_progressReboot, FALSE);
        SetWindowTextW(g_progressBtn, L"确定");
        EnableWindow(g_progressBtn, TRUE);
        SetWindowPos(g_progressBtn, HWND_TOP, 110, 92, 80, 28, SWP_SHOWWINDOW);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(g_progressDlg);
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
            for (int i = 0; i < 4; i++) {
                ShowWindow(g_hTabDlg[i], i == sel ? SW_SHOW : SW_HIDE);
            }
        }
        return 0;
    }

    case WM_APP + 100: {
        OnTabCommand((HWND)lParam, wParam);   /* wParam 含控件 ID (LOWORD) */
        return 0;
    }


    case WM_FW_SHOW_PROGRESS:
        FW_ShowProgress(hWnd);
        return 0;

    case WM_UPDATE_PROGRESS: {
        int pct = (int)wParam;
        wchar_t *text = (wchar_t *)lParam;
        FW_UpdateProgress(pct, text);
        if (text) free(text);
        return 0;
    }

    case WM_DISC_FOUND_IP: {
        /* 发现新 IP, 去重后加入 Tab4 列表. lParam=_strdup(ip ASCII) */
        char *ip = (char *)lParam;
        if (ip) {
            HWND hList = GetDlgItem(g_hTabDlg[3], IDC_DISC_LIST);
            if (hList) {
                /* 去重: 遍历已有项比对 */
                int cnt = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
                BOOL dup = FALSE;
                wchar_t wip[64];
                MultiByteToWideChar(CP_ACP, 0, ip, -1, wip, 64);
                for (int i = 0; i < cnt; i++) {
                    wchar_t tmp[64];
                    SendMessageW(hList, LB_GETTEXT, i, (LPARAM)tmp);
                    if (wcscmp(tmp, wip) == 0) { dup = TRUE; break; }
                }
                if (!dup) {
                    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)wip);
                }
            }
            free(ip);
        }
        return 0;
    }

    case WM_DISC_DONE:
        /* 查找线程结束 (10s 到或用户点停止), 恢复 Tab4 按钮文字 */
        if (g_hTabDlg[3]) {
            SetWindowTextW(GetDlgItem(g_hTabDlg[3], IDC_DISC_START), L"开始查找");
        }
        return 0;

    case WM_FW_COMPLETE: {
        BOOL isCan = (wParam == 1);
        BOOL success = (lParam == 1);
        /* 恢复对应 tab 的升级/浏览按钮 */
        int upgradeId = isCan ? IDC_HFW_UPGRADE : IDC_TFW_UPGRADE;
        int browseId  = isCan ? IDC_HFW_BROWSE  : IDC_TFW_BROWSE;
        HWND hTabChild = isCan ? g_hTabDlg[1] : g_hTabDlg[2];
        EnableWindow(GetDlgItem(hTabChild, upgradeId), TRUE);
        EnableWindow(GetDlgItem(hTabChild, browseId), TRUE);
        FW_Done(hWnd, success);
        return 0;
    }

    case WM_FW_REJECTED: {
        /* keyhash/格式被拒: 警告框已弹过, 这里只销毁进度窗 + 恢复主窗口和按钮,
         * 不调用 FW_Done (避免进度窗显示 "升级失败" 造成双框). */
        BOOL isCan = (wParam == 1);
        int upgradeId = isCan ? IDC_HFW_UPGRADE : IDC_TFW_UPGRADE;
        int browseId  = isCan ? IDC_HFW_BROWSE  : IDC_TFW_BROWSE;
        HWND hTabChild = isCan ? g_hTabDlg[1] : g_hTabDlg[2];
        if (g_progressDlg) {
            g_progressDone = TRUE;
            DestroyWindow(g_progressDlg);  /* WM_DESTROY 会清空 g_progressDlg 等全局 */
        }
        EnableWindow(hWnd, TRUE);
        EnableWindow(GetDlgItem(hTabChild, upgradeId), TRUE);
        EnableWindow(GetDlgItem(hTabChild, browseId), TRUE);
        SetForegroundWindow(hWnd);
        return 0;
    }

    case WM_CLOSE:
        /* 确保升级进行中时阻止关闭 */
        if (g_progressDlg && !g_progressDone) {
            MessageBoxW(hWnd, L"固件升级进行中, 请等待完成", L"提示", MB_OK | MB_ICONWARNING);
            return 0;
        }
        DestroyWindow(hWnd);
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

    /* 创建 CAN/UDP 管理器: 各 CAN tab 独立 CanManager (Tab1 绑定 / Tab2 升级).
     * 只有 BIND tab 挂 frame_cb (处理 0x111 NRF 响应); UPGRADE tab 只用于固件升级 */
    for (int i = 0; i < CAN_TAB_COUNT; i++) {
        g_canTab[i] = CanManager_Create();
        g_canTabChannel[i] = -1;
        CanManager_SetMsgCallback(g_canTab[i], can_msg_cb, NULL);
        if (i == CAN_TAB_BIND) {
            CanManager_SetFrameCallback(g_canTab[i], can_frame_cb, NULL);
        }
    }
    g_cfgUdp[UDP_TAB_BIND] = UdpManager_Create();
    g_cfgUdp[UDP_TAB_CFG] = UdpManager_Create();
    UdpManager_SetMsgCallback(g_cfgUdp[UDP_TAB_BIND], udp_msg_cb, NULL);
    UdpManager_SetMsgCallback(g_cfgUdp[UDP_TAB_CFG], udp_msg_cb, NULL);

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

    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
    /* 主窗口居中屏幕 (CW_USEDEFAULT 对 overlapped 窗口默认靠左上角) */
    int sx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    HWND hWnd = CreateWindowExW(0, L"ZCodeHandlerReceiver", L"手柄-接收器工具",
            WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
            sx, sy, winW, winH,
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
    /* 消息循环退出后销毁 */
    for (int i = 0; i < CAN_TAB_COUNT; i++) {
        if (g_canTab[i]) CanManager_Destroy(g_canTab[i]);
    }
    for (int i = 0; i < UDP_TAB_COUNT; i++) {
        if (g_cfgUdp[i]) UdpManager_Destroy(g_cfgUdp[i]);
    }
    return (int)m.wParam;
}
