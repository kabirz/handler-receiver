/*
 * 手柄-接收器工具 - Win32 GUI 应用
 * Tab1: 手柄绑定 (手柄CAN扫描/连接 + 接收器UDP连接 + NRF读取比对 + 绑定)
 * Tab2: 手柄升级 (CAN)
 * Tab3: 手柄接收端升级 (UDP)
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

/* CAN 帧 ID 与 RF24 命令 (与 can_manager.h 互补, main.c 局部用) */
#define CAN_ID_RF24_CONFIG_CMD   0x104
#define RF24_CMD_GET_CONFIG      0x02

/* 运行时控件 ID (Tab1 手柄绑定) — 仿 gateway-tool 左侧 CAN / 右侧通道配置布局 */
#define IDC_CAN_DEVICE           1001   /* 手柄: 设备下拉 (CBS_DROPDOWNLIST) */
#define IDC_CAN_REFRESH          1002   /* 手柄: 刷新按钮 */
#define IDC_CAN_CONNECT          1003   /* 手柄: 连接/断开 按钮 */
#define IDC_UDP_IP               1004   /* 接收器: 目标 IP (CBS_DROPDOWN) */
#define IDC_UDP_CONNECT          1005   /* 接收器: 连接/断开 按钮 */
#define IDC_BTN_CHECK_BIND       1006   /* 检测绑定状态 */
#define IDC_BTN_BIND             1007   /* 绑定设备 */
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

/* 刷新手柄设备下拉框 (扫 PCAN_USB 通道, 仿 gateway-tool RefreshDevices) */
static void RefreshCanDevices(HWND hCombo)
{
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    char devices[16][256];
    int count = CanManager_DetectDevice(g_can, devices, 16);
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

/* 创建各 tab 子对话框控件 (在 CreateTabLayout 后调用) */
static void CreateBindTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    /* ===== 手柄区 groupbox (仿 gateway-tool 左侧 CAN): 设备下拉 + 刷新 + 连接, 无波特率 (固定 250K) ===== */
    CreateWindowExW(0, L"BUTTON", L"手柄 (CAN, 250K)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 6, 436, 70, hDlg, NULL, g_hInst, NULL);
    /* "设备:" 标签 */
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"设备:",
            WS_CHILD | WS_VISIBLE, 20, 30, 36, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 设备下拉 (CBS_DROPDOWNLIST, 不可编辑). 高度=下拉展开后列表总高, 按实际设备数给小一些 */
    HWND hDev = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            56, 28, 115, 100, hDlg, (HMENU)(INT_PTR)IDC_CAN_DEVICE, g_hInst, NULL);
    SendMessageW(hDev, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 刷新 / 连接 按钮 */
    HWND hRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            296, 28, 60, 22, hDlg, (HMENU)(INT_PTR)IDC_CAN_REFRESH, g_hInst, NULL);
    SendMessageW(hRefresh, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCanConn = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            366, 28, 70, 22, hDlg, (HMENU)(INT_PTR)IDC_CAN_CONNECT, g_hInst, NULL);
    SendMessageW(hCanConn, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 预填设备列表 */
    RefreshCanDevices(hDev);

    /* ===== 接收器区 groupbox (仿 gateway-tool 右侧通道配置, 只保留 IP): 目标IP + 连接 ===== */
    CreateWindowExW(0, L"BUTTON", L"接收器 (UDP)",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, 84, 436, 70, hDlg, NULL, g_hInst, NULL);
    hLbl = CreateWindowExW(0, L"STATIC", L"目标IP:",
            WS_CHILD | WS_VISIBLE, 20, 108, 44, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* IP 输入框 (纯 EDIT, 用户手输). 留空 (无默认值) */
    HWND hIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            66, 106, 110, 22, hDlg, (HMENU)(INT_PTR)IDC_UDP_IP, g_hInst, NULL);
    SendMessageW(hIp, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUdpConn = CreateWindowExW(0, L"BUTTON", L"连接",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            366, 106, 70, 22, hDlg, (HMENU)(INT_PTR)IDC_UDP_CONNECT, g_hInst, NULL);
    SendMessageW(hUdpConn, WM_SETFONT, (WPARAM)hFont, TRUE);

    /* ===== 操作按钮: 检测绑定状态 / 绑定设备 ===== */
    int y = 168;
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

/* 通用: 创建 升级 tab 的 [路径框 + 浏览 + 升级] 三件套 */
static void CreateFwTabControls(HWND hDlg, int file_id, int browse_id, int upgrade_id)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND hLabel = CreateWindowExW(0, L"STATIC", L"固件文件:",
            WS_CHILD | WS_VISIBLE, 20, 20, 60, 16,
            hDlg, NULL, g_hInst, NULL);
    HWND hFile = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            80, 18, 260, 22, hDlg, (HMENU)(INT_PTR)file_id, g_hInst, NULL);
    HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            350, 18, 60, 22, hDlg, (HMENU)(INT_PTR)browse_id, g_hInst, NULL);
    HWND hUpg = CreateWindowExW(0, L"BUTTON", L"升级",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            180, 60, 80, 28, hDlg, (HMENU)(INT_PTR)upgrade_id, g_hInst, NULL);
    SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hFile,  WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hUpg,   WM_SETFONT, (WPARAM)hFont, TRUE);
}

static void CreateHandlerFwTabControls(HWND hDlg)
{
    CreateFwTabControls(hDlg, IDC_HFW_FILE, IDC_HFW_BROWSE, IDC_HFW_UPGRADE);
}

static void CreateTransmitterFwTabControls(HWND hDlg)
{
    CreateFwTabControls(hDlg, IDC_TFW_FILE, IDC_TFW_BROWSE, IDC_TFW_UPGRADE);
}

/* CAN 帧回调: 收到 0x105 RF24 配置响应时填 g_handlerAddr 并置标志 */
static void can_frame_cb(const CanFrame *frame, void *user_data)
{
    (void)user_data;
    if (frame->id == 0x105 /* CAN_ID_RF24_CONFIG_RESP */ && frame->dlc >= 7) {
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

/* 发 CAN 0x104 GET_CONFIG 并等待 0x105 响应 (轮询标志, 超时 800ms).
 * 成功返回 true, g_handlerCh/g_handlerAddr 已填. */
static BOOL ReadHandlerNrf(void)
{
    g_handlerAddrGot = FALSE;
    uint8_t data[8] = { 0 };
    data[0] = RF24_CMD_GET_CONFIG;
    if (!CanManager_Send(g_can, CAN_ID_RF24_CONFIG_CMD, data, 8)) {
        return FALSE;
    }
    /* 轮询等待 (frame_cb 在 RX 线程置标志) */
    for (int i = 0; i < 80; i++) {
        if (g_handlerAddrGot) return TRUE;
        Sleep(10);
    }
    return FALSE;
}

/* 手柄连接/断开 (从下拉取选中设备, 固定 250K). 已连接则断开. */
static void OnCanConnect(HWND hChildDlg)
{
    if (g_canConnected) {
        /* 断开 */
        CanManager_Disconnect(g_can);
        g_canConnected = 0;
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_CAN_CONNECT), L"连接");
        EnableWindow(GetDlgItem(hChildDlg, IDC_CAN_DEVICE), TRUE);
        EnableWindow(GetDlgItem(hChildDlg, IDC_CAN_REFRESH), TRUE);
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

    if (!CanManager_Connect(g_can, channel, PCAN_BAUD_250K)) {
        MessageBoxW(g_hMain, L"设备被占用，请查看并释放", L"连接失败",
                    MB_OK | MB_ICONERROR);
        return;
    }
    CanManager_StartRxThread(g_can);
    g_canConnected = 1;
    SetWindowTextW(GetDlgItem(hChildDlg, IDC_CAN_CONNECT), L"断开");
    EnableWindow(GetDlgItem(hChildDlg, IDC_CAN_DEVICE), FALSE);
    EnableWindow(GetDlgItem(hChildDlg, IDC_CAN_REFRESH), FALSE);
    /* 若手柄固件路径已选, 启用 Tab2 升级按钮 */
    if (strlen(g_handlerFwPath) > 0) {
        EnableWindow(GetDlgItem(g_hTabDlg[1], IDC_HFW_UPGRADE), TRUE);
    }
}

/* 接收器连接/断开 (从 IP 框取目标 IP, 配置端口固定 9200). 已连接则断开. */
static void OnUdpConnect(HWND hChildDlg)
{
    if (g_udpConnected) {
        /* 断开 */
        UdpManager_Unbind(g_cfgUdp);
        g_udpConnected = 0;
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_UDP_CONNECT), L"连接");
        EnableWindow(GetDlgItem(hChildDlg, IDC_UDP_IP), TRUE);
        return;
    }
    /* 取 IP 框内容 (CBS_DROPDOWN 可编辑, 读编辑文本) */
    wchar_t wip[64] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_UDP_IP), wip, 64);
    char ip[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wip, -1, ip, sizeof(ip), NULL, NULL);
    if (!ip[0]) {
        MessageBoxW(g_hMain, L"请填写目标 IP\n(如 255.255.255.255 表示广播)", L"提示",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    /* 本地 9201, 远程 9200. IP 空/0 → 广播自动发现; 255.255.255.255 → 有限广播 */
    if (!UdpManager_Bind(g_cfgUdp, UDP_CHAN_CONFIG, 9201, ip, 9200)) {
        int err = WSAGetLastError();
        wchar_t wmsg[160];
        swprintf(wmsg, 160,
            L"接收器连接失败\n本地端口 9201 可能被占用 (WSA 错误码: %d)\n请关闭占用该端口的程序后重试",
            err);
        MessageBoxW(g_hMain, wmsg, L"连接失败", MB_OK | MB_ICONERROR);
        return;
    }
    UdpManager_StartRxThread(g_cfgUdp);
    g_udpConnected = 1;
    SetWindowTextW(GetDlgItem(hChildDlg, IDC_UDP_CONNECT), L"断开");
    EnableWindow(GetDlgItem(hChildDlg, IDC_UDP_IP), FALSE);
    /* 若接收器固件路径已选, 启用 Tab3 升级按钮 */
    if (strlen(g_receiverFwPath) > 0) {
        EnableWindow(GetDlgItem(g_hTabDlg[2], IDC_TFW_UPGRADE), TRUE);
    }
}

/* 检测绑定状态: 读手柄 NRF + 接收器 NRF, 比对 */
static void OnCheckBind(HWND hChildDlg)
{
    if (!g_canConnected || !g_udpConnected) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 1. 读手柄 NRF */
    if (!ReadHandlerNrf()) {
        MessageBoxW(g_hMain, L"读取手柄 NRF 地址超时\n请确认手柄已上电", L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }
    /* 2. 读接收器 NRF */
    if (!UdpManager_GetRF24(g_cfgUdp, &g_receiverCh, g_receiverAddr)) {
        MessageBoxW(g_hMain,
            L"读取接收器 NRF 地址超时\n请确认接收器已上电并在同一网络",
            L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    /* 3. 比对 */
    BOOL all_zero = (g_receiverAddr[0]|g_receiverAddr[1]|g_receiverAddr[2]|
                     g_receiverAddr[3]|g_receiverAddr[4]) == 0;
    BOOL same = (memcmp(g_receiverAddr, g_handlerAddr, 5) == 0);

    /* 地址信息 (hex ASCII 安全) 拼入弹窗. 一个弹窗同时给出地址对比 + 绑定状态结论 */
    wchar_t wmsg[256];
    swprintf(wmsg, 256,
        L"手柄 NRF: %02x%02x%02x%02x%02x (ch%d)\n接收器 NRF: %02x%02x%02x%02x%02x (ch%d)\n\n",
        g_handlerAddr[0], g_handlerAddr[1], g_handlerAddr[2],
        g_handlerAddr[3], g_handlerAddr[4], g_handlerCh,
        g_receiverAddr[0], g_receiverAddr[1], g_receiverAddr[2],
        g_receiverAddr[3], g_receiverAddr[4], g_receiverCh);

    if (all_zero) {
        wcscat(wmsg, L"设备未绑定 (接收器 NRF 地址为空)");
        MessageBoxW(g_hMain, wmsg, L"绑定状态: 未绑定", MB_OK | MB_ICONINFORMATION);
    } else if (!same) {
        wcscat(wmsg, L"接收器已绑定其他设备");
        MessageBoxW(g_hMain, wmsg, L"绑定状态: 其他设备", MB_OK | MB_ICONWARNING);
    } else {
        wcscat(wmsg, L"已绑定本设备");
        MessageBoxW(g_hMain, wmsg, L"绑定状态: 已绑定", MB_OK | MB_ICONINFORMATION);
    }
}

/* 绑定设备: 把手柄 NRF 地址写入接收器 */
static void OnBind(HWND hChildDlg)
{
    (void)hChildDlg;
    if (!g_canConnected || !g_udpConnected) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 确保已有手柄 NRF (没读过则先读) */
    if (!g_handlerAddrGot) {
        if (!ReadHandlerNrf()) {
            MessageBoxW(g_hMain, L"读取手柄 NRF 地址超时", L"错误",
                        MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (MessageBoxW(g_hMain, L"确认把手柄 NRF 地址写入接收器?",
                    L"确认绑定", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (UdpManager_SetRF24(g_cfgUdp, g_handlerCh, g_handlerAddr)) {
        wchar_t wmsg[128];
        swprintf(wmsg, 128, L"绑定成功\n已写入地址 %02x%02x%02x%02x%02x (ch%d)",
                 g_handlerAddr[0], g_handlerAddr[1], g_handlerAddr[2],
                 g_handlerAddr[3], g_handlerAddr[4], g_handlerCh);
        MessageBoxW(g_hMain, wmsg, L"绑定成功", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(g_hMain, L"绑定失败\n发送 SetRF24 命令失败", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}

/* 固件升级线程参数: CAN 与 UDP 共用 (UDP 分支在 Task 7 填) */
typedef struct {
    HWND hMain;
    char path[MAX_PATH];
    int isCan;          /* 1=CAN, 0=UDP */
} FwUpgradeParam;

/* CAN 升级进度回调: 转 PostMessage 到主线程 */
static void can_fw_progress_cb(const char *pct_str, void *user_data)
{
    HWND hMain = (HWND)user_data;
    if (!hMain || !pct_str) return;
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

    PostMessageA(hMain, WM_FW_SHOW_PROGRESS, 0, 0);

    bool result = false;
    if (p->isCan) {
        /* CAN 升级: test_mode 固定 0 (永久). 传 NULL msg_cb 避免 gateway-tool 内部
         * 日志走我们的回调链 (进度走 progress_cb 即可) */
        result = CanManager_FirmwareUpgrade(g_can, p->path, 0,
                                            NULL, NULL,
                                            can_fw_progress_cb, (void *)hMain);
    } else {
        /* UDP 升级: START(size) → DATA(256B/包, offset 校验) → END(crc+testmode) */
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
            if (UdpManager_FirmwareStart(g_cfgUdp, fileSize)) {
                int offset = 0, chunk = 256;
                result = true;
                while (offset < (int)fileSize) {
                    int send_len = ((int)fileSize - offset > chunk) ? chunk : ((int)fileSize - offset);
                    uint32_t got = 0;
                    if (!UdpManager_FirmwareData(g_cfgUdp, fileData + offset,
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
                    int pct = (int)((long long)offset * 100 / fileSize);
                    wchar_t buf[32];
                    swprintf(buf, 32, L"升级中 %d%%", pct);
                    PostMessageA(hMain, WM_UPDATE_PROGRESS, (WPARAM)pct, (LPARAM)_wcsdup(buf));
                }
                if (result) {
                    if (UdpManager_FirmwareEnd(g_cfgUdp, 0, crc)) {
                        /* 成功, 不弹窗 (FW_Done 会显示) */
                    } else {
                        MessageBoxW(g_hMain, L"烧写失败 (CRC 不匹配或错误)", L"升级失败",
                                    MB_OK | MB_ICONERROR);
                        result = false;
                    }
                }
            } else {
                MessageBoxW(g_hMain, L"开始烧写失败 (固件未响应 START)", L"升级失败",
                            MB_OK | MB_ICONERROR);
            }
            free(fileData);
        }
    }

    PostMessageA(hMain, WM_FW_COMPLETE, p->isCan ? 1 : 0, result ? 1 : 0);
    free(p);
    return 0;
}

/* 主窗口收到子对话框转发的命令: hChildDlg=子对话框句柄, wParam 含控件 ID (LOWORD) */
static void OnTabCommand(HWND hChildDlg, WPARAM wParam)
{
    int cmdId = LOWORD(wParam);
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);

    if (tabIdx == 0) {
        /* 手柄绑定页 */
        switch (cmdId) {
        case IDC_CAN_REFRESH:       RefreshCanDevices(GetDlgItem(hChildDlg, IDC_CAN_DEVICE)); break;
        case IDC_CAN_CONNECT:       OnCanConnect(hChildDlg);   break;
        case IDC_UDP_CONNECT:       OnUdpConnect(hChildDlg);   break;
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
                EnableWindow(GetDlgItem(hChildDlg, IDC_HFW_UPGRADE), g_canConnected ? TRUE : FALSE);
            }
            break;
        }
        case IDC_HFW_UPGRADE:
            if (g_canConnected && strlen(g_handlerFwPath) > 0) {
                EnableWindow(GetDlgItem(hChildDlg, IDC_HFW_UPGRADE), FALSE);
                EnableWindow(GetDlgItem(hChildDlg, IDC_HFW_BROWSE), FALSE);
                FwUpgradeParam *param = (FwUpgradeParam *)malloc(sizeof(FwUpgradeParam));
                param->hMain = g_hMain;
                strcpy(param->path, g_handlerFwPath);
                param->isCan = 1;
                CreateThread(NULL, 0, fw_upgrade_thread, param, 0, NULL);
            } else if (!g_canConnected) {
                MessageBoxW(g_hMain, L"请先连接手柄 (Tab1)", L"提示",
                            MB_OK | MB_ICONWARNING);
            }
            break;
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
                EnableWindow(GetDlgItem(hChildDlg, IDC_TFW_UPGRADE), g_udpConnected ? TRUE : FALSE);
            }
            break;
        }
        case IDC_TFW_UPGRADE:
            if (g_udpConnected && strlen(g_receiverFwPath) > 0) {
                EnableWindow(GetDlgItem(hChildDlg, IDC_TFW_UPGRADE), FALSE);
                EnableWindow(GetDlgItem(hChildDlg, IDC_TFW_BROWSE), FALSE);
                FwUpgradeParam *param = (FwUpgradeParam *)malloc(sizeof(FwUpgradeParam));
                param->hMain = g_hMain;
                strcpy(param->path, g_receiverFwPath);
                param->isCan = 0;
                CreateThread(NULL, 0, fw_upgrade_thread, param, 0, NULL);
            } else if (!g_udpConnected) {
                MessageBoxW(g_hMain, L"请先连接接收器 (Tab1)", L"提示",
                            MB_OK | MB_ICONWARNING);
            }
            break;
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
    ti.pszText = (LPWSTR)L"手柄接收端升级";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 2, (LPARAM)&ti);

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
    for (int i = 0; i < 3; i++) {
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
}

/* ===== 固件升级进度弹窗 (移植自 gateway-tool) ===== */

static LRESULT CALLBACK ProgressWndProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROG_BTN) {
            if (g_progressDone) DestroyWindow(hDlg);
        } else if (LOWORD(wParam) == IDC_PROG_REBOOT) {
            if (g_progressIsCan) {
                if (g_can) CanManager_Reboot(g_can);
            } else {
                if (g_cfgUdp) UdpManager_Reboot(g_cfgUdp);
            }
            DestroyWindow(hDlg);
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
            for (int i = 0; i < 3; i++) {
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

    /* 创建 CAN/UDP 管理器 */
    g_can = CanManager_Create();
    g_cfgUdp = UdpManager_Create();
    CanManager_SetMsgCallback(g_can, can_msg_cb, NULL);
    CanManager_SetFrameCallback(g_can, can_frame_cb, NULL);
    UdpManager_SetMsgCallback(g_cfgUdp, udp_msg_cb, NULL);

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
    CanManager_Destroy(g_can);
    UdpManager_Destroy(g_cfgUdp);
    return (int)m.wParam;
}
