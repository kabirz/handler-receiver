/*
 * 手柄-接收机工具 - Win32 GUI 应用
 * Tab0: 接收机配置 (UDP: 接收机IP/配置端口 + 上行IP/上行端口/数据端口)
 * Tab1: 手柄绑定 (手柄CAN扫描/连接 + 接收机UDP单播连接 + NRF读取比对 + 绑定)
 * Tab2: 固件升级 (CAN手柄升级 + UDP接收机升级)
 * Tab3: 设备查找 (DISCOVER 广播发现接收机真实 IP)
 * 隐藏页: 调试 (经 gateway UDP 读取手柄数据 + 扫描仪数据模拟发送, Ctrl+Shift+B 切换)
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <time.h>
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

/* 应用版本号 (由 CMakeLists.txt 的 APP_VERSION_MAJOR/MINOR/PATCH 宏传入).
 * 两层宏 + # 字符串化为窄串, 再用 L"" 前缀拼接成宽字符串 "主.次.修订"
 * (MSVC 不支持 L#x 写法, 故用 L"" "..." 相邻字面量拼接). */
#define ZC_STR2(x) #x
#define ZC_STR(x)  ZC_STR2(x)
#define APP_VERSION_W L"" ZC_STR(APP_VERSION_MAJOR) L"." \
                      L"" ZC_STR(APP_VERSION_MINOR) L"." \
                      L"" ZC_STR(APP_VERSION_PATCH)

/* 自定义窗口消息 (工作线程 -> UI 线程) */
#define WM_UPDATE_LOG        (WM_APP + 1)
#define WM_UPDATE_PROGRESS   (WM_APP + 3)
#define WM_FW_COMPLETE       (WM_APP + 6)
#define WM_FW_SHOW_PROGRESS  (WM_APP + 7)
#define WM_FW_REJECTED       (WM_APP + 11) /* keyhash/格式被拒: wParam=1 CAN/0 UDP, 销毁进度窗不显示结果 */
#define WM_DISC_FOUND_IP     (WM_APP + 9)  /* Tab4: 发现新 IP, lParam=_strdup(ip) */
#define WM_DISC_DONE         (WM_APP + 10) /* Tab4: 查找结束, 主线程恢复按钮文字 */
#define WM_DBG_HANDLER       (WM_APP + 12) /* Tab5: 收到手柄状态帧, 刷新显示 */

/* CAN 帧 ID 由 can_manager.h 定义 (CAN_ID_RF24_CONFIG_CMD/RESP) */
#define RF24_CMD_GET_CONFIG      0x02

/* 运行时控件 ID (Tab1 手柄绑定) — 仿 gateway-tool 左侧 CAN / 右侧通道配置布局 */
#define IDC_CAN_DEVICE           1001   /* 手柄: 设备下拉 (CBS_DROPDOWNLIST) */
#define IDC_CAN_REFRESH          1002   /* 手柄: 刷新按钮 */
#define IDC_CAN_CONNECT          1003   /* 手柄: 连接/断开 按钮 */
#define IDC_UDP_IP               1004   /* 接收机: 接收机 IP 输入框 */
#define IDC_UDP_CONNECT          1005   /* 接收机: 连接/断开 按钮 */
#define IDC_BTN_CHECK_BIND       1006   /* 检测绑定状态 */
#define IDC_BTN_BIND             1007   /* 绑定设备 */
/* Tab2 手柄固件升级 */
#define IDC_HFW_FILE             1101
#define IDC_HFW_BROWSE           1102
#define IDC_HFW_UPGRADE          1103
#define IDC_HFW_VERSION          1104   /* 固件版本静态文本 */
#define IDC_HFW_GETVER           1105   /* 获取版本按钮 */
/* Tab3 接收机固件升级 */
#define IDC_TFW_FILE             1201
#define IDC_TFW_BROWSE           1202
#define IDC_TFW_UPGRADE          1203
#define IDC_TFW_VERSION          1204   /* 固件版本静态文本 */
#define IDC_TFW_GETVER           1205   /* 获取版本按钮 */
/* Tab0 接收机配置. 行1: 接收机IP(可配)+配置端口(只读); 行2: 上行IP+上行端口+数据端口(只读). */
#define IDC_CFG_IP               1500   /* 行1 接收机IP 输入框 (SET_IP / DISCOVER) */
#define IDC_CFG_CFGPORT          1501   /* 行1 配置端口 (只读回填, DISCOVER 返回) */
#define IDC_CFG_APPLY            1502   /* 行1 配置按钮 (SET_IP) */
#define IDC_CFG_QUERY            1503   /* 行1 查询按钮 (DISCOVER) */
#define IDC_CFG_UPIP             1504   /* 行2 上行IP 输入框 (host_ip, SET_HOST) */
#define IDC_CFG_UPPORT           1505   /* 行2 上行数据监听端口 输入框 (host_port) */
#define IDC_CFG_DATAPORT         1506   /* 行2 接收机数据监听端口 (只读, data_port) */
#define IDC_CFG_UPAPPLY          1507   /* 行2 配置按钮 (SET_HOST) */
#define IDC_CFG_UPQUERY          1508   /* 行2 查询按钮 (GET_NET) */
#define IDC_CFG_FACTORY_RESET    1509   /* 恢复出厂设置 (FACTORY_RESET 0x16) */
#define IDC_CFG_REBOOT           1510   /* 重启接收机 (REBOOT 0x05) */
/* Tab4 设备查找 */
#define IDC_DISC_START           1301   /* 开始/停止查找 按钮 */
#define IDC_DISC_LIST            1302   /* 发现的 IP 列表 (LISTBOX) */
#define IDC_DISC_COPY            1303   /* 复制选中 IP 到剪贴板 按钮 */
/* Tab5 调试 */
#define IDC_DBG_HX_VAL           1401   /* 手柄 X 角度 显示 */
#define IDC_DBG_HY_VAL           1402   /* 手柄 Y 角度 显示 */
#define IDC_DBG_HBTN_VAL         1403   /* 手柄 按键 显示 */
#define IDC_DBG_HBTIME_VAL       1405   /* 手柄 最近心跳时间 显示 */
#define IDC_DBG_HCNT_VAL         1404   /* 手柄 收到帧数 显示 */
#define IDC_DBG_OVB              1410   /* 扫描仪: 超挖值 输入 */
#define IDC_DBG_LASER            1411   /* 扫描仪: 激光距离 输入 */
#define IDC_DBG_CX               1412   /* 扫描仪: 坐标 X 输入 */
#define IDC_DBG_CY               1413   /* 扫描仪: 坐标 Y 输入 */
#define IDC_DBG_CZ               1414   /* 扫描仪: 坐标 Z 输入 */
#define IDC_DBG_SEND_ODO         1420   /* 发送 0x263 超挖+激光 单帧 */
#define IDC_DBG_SEND_XY          1421   /* 发送 0x363 坐标X/Y 单帧 */
#define IDC_DBG_SEND_Z           1422   /* 发送 0x463 坐标Z 单帧 */
#define IDC_DBG_AUTO             1423   /* 自动周期发送 开关按钮 */
#define IDC_DBG_PERIOD           1424   /* 自动周期 (ms) 输入 */
#define IDC_DBG_GW_IP            1430   /* 网关 IP 输入 */
#define IDC_DBG_LOCAL_PORT       1431   /* 本地数据端口 输入 (bind) */
#define IDC_DBG_CONNECT          1433   /* 网关 UDP 连接/断开 按钮 */

/* Tab5 调试: 网关数据端口 (收发数据帧, 与 gateway 固件默认值一致, 固定不可配) */
#define DBG_GW_DATA_PORT_DEFAULT 9600

/* 全局状态 */
static HINSTANCE g_hInst;
static HWND g_hMain;
static HWND g_hTab;
static HWND g_hTabDlg[5];

/* CAN 各 tab 独立: g_canTab[0]=Tab1 绑定用 (带 frame_cb 处理 NRF), g_canTab[1]=Tab2 升级用.
 * 每个 tab 持有独立 CanManager 实例 + 独立连接状态, 互不影响 (同一 PCAN 设备被一个 tab
 * Initialize 后, 另一个 tab 再 Initialize 同设备会失败 → 弹窗友好提示占用) */
#define CAN_TAB_BIND    0   /* Tab1: 手柄绑定 (NRF 读取) */
#define CAN_TAB_UPGRADE 1   /* Tab2: 手柄固件升级 */
#define CAN_TAB_COUNT   2
static CanManager *g_canTab[CAN_TAB_COUNT];
static int g_canTabChannel[CAN_TAB_COUNT];   /* 各 tab 已连接的 channel, -1=未连接 */

/* 接收机 UDP 管理器: Tab1(绑定) 和 Tab3(配置) 各用独立实例, 互不耦合.
 * g_udpTabIdx 0=Tab1, 1=Tab3. */
#define UDP_TAB_BIND  0   /* Tab1 手柄绑定页的接收机 */
#define UDP_TAB_CFG   1   /* Tab3 接收端配置页的接收机 */
#define UDP_TAB_COUNT 2
static UdpManager *g_cfgUdp[UDP_TAB_COUNT];
static int g_udpConnected[UDP_TAB_COUNT];
static BOOL g_udpBroadcast[UDP_TAB_COUNT];  /* 目标 IP = 255.255.255.255 有限广播 */

/* Tab4 设备查找: 原生 winsock 广播 DISCOVER (0x15), 解析回复 [ip][config_port].
 * 用独立 socket (绑 8601 = CONFIG_PORT+1, 固件跨子网回复端口), 不走 UdpManager. g_discRunning=查找中. */
static volatile BOOL g_discRunning;

/* Tab5 调试: 手柄状态帧 (0x1E3) 解析值 (RX 线程写, UI 线程读).
 * gateway UDP 转发帧格式: [frame_id 2B BE][payload], payload 内 x/y/button 大端. */
static volatile int g_dbgX;
static volatile int g_dbgY;
static volatile int g_dbgBtn;
static volatile int g_dbgFrameCnt;
/* 最近一次心跳 (0x763) 的本地时刻, 编码为 HHMMSS 十进制整数 */
static volatile DWORD g_dbgHeartHMS;
/* 自动周期发送开关 (工作线程 + UI 共用) */
static volatile BOOL g_dbgAutoSend;
static HANDLE g_dbgSendThread = NULL;
/* Tab5 调试: UDP 数据通道 (收发经 gateway 的数据帧).
 * 网关转发目标已在 Tab1 配置为指向本机, 这里只需绑定数据端口收/发. */
static UdpManager *g_dbgUdp;
static BOOL g_dbgUdpConnected;

/* 调试 tab 默认隐藏, 用 Ctrl+Shift+B 切换显示 */
#define IDH_TOGGLE_DEBUG  1001
static BOOL g_dbgTabShown;

/* 手柄 NRF 地址 (CAN 0x111 响应填入; 信道固定 1, 不再读取) */
static uint8_t g_handlerAddr[5];
static volatile BOOL g_handlerAddrGot;

/* 接收机 NRF 地址 (UDP GET_RF24; 信道固定 1, 不再读取) */
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

/* ===== 全局 UI 缩放 (1.5x) =====
 * 所有控件按原坐标创建后, 由 ScaleChildWindows 后处理缩放, 无需改动创建代码.
 * S(n)=n*3/2 整数运算; g_hUiFont 是放大后的统一字体 (替代 DEFAULT_GUI_FONT). */
#define UI_SCALE_NUM  3
#define UI_SCALE_DEN  2
static int S(int v) { return v * UI_SCALE_NUM / UI_SCALE_DEN; }
static HFONT g_hUiFont = NULL;

/* 缩放 parent 的所有直接子窗口: 坐标/尺寸×SCALE + 换 g_hUiFont.
 * groupbox 是空容器 (控件是其 sibling 而非 child), 遍历直接子窗口即覆盖全部控件.
 * 非 groupbox 控件 (标签/按钮/编辑框/下拉框) 高度额外 +UI_HPAD 补偿:
 * 原始 h=22 是为 9pt 字体设计, ×1.5=33 给 13.5pt 偏紧, 文字会被上下裁, 加 6px 留白. */
#define UI_HPAD  6
static void ScaleChildWindows(HWND parent)
{
    HWND h = GetWindow(parent, GW_CHILD);
    while (h) {
        RECT rc;
        GetWindowRect(h, &rc);
        MapWindowPoints(NULL, parent, (LPPOINT)&rc, 2);
        int w = S(rc.right - rc.left);
        int hh = S(rc.bottom - rc.top);
        /* groupbox (BS_GROUPBOX) 高度不动 (改了会破坏间距); 其余控件高度 +UI_HPAD */
        wchar_t cls[16] = { 0 };
        GetClassNameW(h, cls, sizeof(cls) / sizeof(cls[0]));
        LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
        bool is_groupbox = (wcscmp(cls, L"Button") == 0 &&
                            (style & BS_GROUPBOX) == BS_GROUPBOX);
        if (!is_groupbox) hh += UI_HPAD;
        SetWindowPos(h, NULL, S(rc.left), S(rc.top), w, hh, SWP_NOZORDER);
        if (g_hUiFont) {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }
}

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
    /* groupbox 高度: 基础(连接行)70 + 版本行26 + 固件区34(单行) */
    int boxH = 70;
    if (showVer) boxH += 26;
    if (showFw) boxH += 34;
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
                366, yPos + 54, 70, 22, hDlg, (HMENU)(INT_PTR)getver_id, g_hInst, NULL);
        SendMessageW(hGetVer, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* 固件区 (可选, 内嵌 groupbox): 文件路径 + 浏览 + 升级, 单行布局.
     * 浏览和升级并排在右侧并右对齐 (升级右边贴 groupbox 右内边距, 与连接按钮同右边界 436). */
    if (showFw) {
        int fy = yPos + (showVer ? 88 : 56);  /* 无版本行时上移 */
        HWND hFLbl = CreateWindowExW(0, L"STATIC", L"固件文件:",
                WS_CHILD | WS_VISIBLE, 20, fy + 4, 60, 16, hDlg, NULL, g_hInst, NULL);
        HWND hFile = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                80, fy + 2, 236, 22, hDlg, (HMENU)(INT_PTR)fw_file_id, g_hInst, NULL);
        HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                320, fy + 2, 56, 22, hDlg, (HMENU)(INT_PTR)fw_browse_id, g_hInst, NULL);
        HWND hUpg = CreateWindowExW(0, L"BUTTON", L"升级",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                380, fy + 2, 56, 22, hDlg, (HMENU)(INT_PTR)fw_upgrade_id, g_hInst, NULL);
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

/* 创建接收机 UDP 连接 groupbox (接收机 IP + 连接, 配置端口固定 8600).
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
    if (showFw) boxH += 34;
    CreateWindowExW(0, L"BUTTON", L"接收机",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, yPos, 436, boxH, hDlg, NULL, g_hInst, NULL);
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"接收机IP:",
            WS_CHILD | WS_VISIBLE, 20, yPos + 24, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            78, yPos + 22, 110, 22, hDlg, (HMENU)(INT_PTR)IDC_UDP_IP, g_hInst, NULL);
    SendMessageW(hIp, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 本地端口由 OS 自动分配 (配置通道 bind 临时端口; 固件回复到发送方源端口, 无需固定).
     * 连接按钮 x=366 与 CAN 连接按钮 (及 Tab3 下方查询按钮) 垂直对齐. */
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
                366, yPos + 54, 70, 22, hDlg, (HMENU)(INT_PTR)getver_id, g_hInst, NULL);
        SendMessageW(hGetVer, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* 固件区 (可选, 内嵌 groupbox): 文件路径 + 浏览 + 升级, 单行布局.
     * 浏览和升级并排在右侧并右对齐 (升级右边贴 groupbox 右内边距, 与连接按钮同右边界 436). */
    if (showFw) {
        int fy = yPos + (showVer ? 88 : 56);
        HWND hFLbl = CreateWindowExW(0, L"STATIC", L"固件文件:",
                WS_CHILD | WS_VISIBLE, 20, fy + 4, 60, 16, hDlg, NULL, g_hInst, NULL);
        HWND hFile = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                80, fy + 2, 236, 22, hDlg, (HMENU)(INT_PTR)fw_file_id, g_hInst, NULL);
        HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                320, fy + 2, 56, 22, hDlg, (HMENU)(INT_PTR)fw_browse_id, g_hInst, NULL);
        HWND hUpg = CreateWindowExW(0, L"BUTTON", L"升级",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                380, fy + 2, 56, 22, hDlg, (HMENU)(INT_PTR)fw_upgrade_id, g_hInst, NULL);
        SendMessageW(hFLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hFile,  WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hUpg,   WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    return boxH + 8;
}

/* 同步指定 CAN tab 的连接状态到 UI (按钮文字/控件禁用).
 * canTabIdx: CAN_TAB_BIND(Tab1 手柄绑定) 或 CAN_TAB_UPGRADE(Tab2 固件升级), 各 tab 独立. */
static void SyncCanConnState(int canTabIdx)
{
    int connected = (g_canTabChannel[canTabIdx] >= 0);
    /* CAN tab 索引 → 子对话框索引: BIND→Tab1(g_hTabDlg[1]), UPGRADE→Tab2(g_hTabDlg[2]) */
    HWND h = (canTabIdx == CAN_TAB_BIND) ? g_hTabDlg[1] : g_hTabDlg[2];
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

/* 同步 UDP 连接状态到含 UDP groupbox 的 tab.
 * udpTabIdx: UDP_TAB_BIND(Tab1 手柄绑定) 或 UDP_TAB_CFG(Tab0 接收机配置 + Tab2 固件升级).
 * UDP_TAB_CFG 跨 Tab0/Tab2 两个对话框 (同一接收机实例), 需都更新. */
static void SyncUdpConnState(int udpTabIdx)
{
    int connected = g_udpConnected[udpTabIdx];
    const wchar_t *text = connected ? L"断开" : L"连接";
    BOOL enable = connected ? FALSE : TRUE;
    BOOL bcast = g_udpBroadcast[udpTabIdx];

    /* 更新连接按钮 + IP 框. BIND→Tab1; CFG→Tab0 和 Tab2. */
    HWND tabs[2];
    int ntab = 0;
    if (udpTabIdx == UDP_TAB_BIND) {
        tabs[ntab++] = g_hTabDlg[1];
    } else {
        tabs[ntab++] = g_hTabDlg[0];  /* Tab0 接收机配置 */
        tabs[ntab++] = g_hTabDlg[2];  /* Tab2 固件升级 */
    }
    for (int i = 0; i < ntab; i++) {
        HWND h = tabs[i];
        if (!h) continue;
        SetWindowTextW(GetDlgItem(h, IDC_UDP_CONNECT), text);
        EnableWindow(GetDlgItem(h, IDC_UDP_IP), enable);
    }

    /* Tab2 (固件升级) 接收机升级/获取版本按钮: 广播模式下禁用 (单台才有意义). */
    if (udpTabIdx == UDP_TAB_CFG && g_hTabDlg[2]) {
        EnableWindow(GetDlgItem(g_hTabDlg[2], IDC_TFW_UPGRADE),
                     connected && !bcast && strlen(g_receiverFwPath) > 0 ? TRUE : FALSE);
        EnableWindow(GetDlgItem(g_hTabDlg[2], IDC_TFW_GETVER),
                     connected && !bcast ? TRUE : FALSE);
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

/* 创建 Tab0 接收机配置控件: 连接 groupbox + 接收机参数 groupbox(包接收机配置/上行参数).
 * 接收机配置 (接收机IP/配置端口) 和 上行参数 (上行IP/上行端口/数据端口) 各为子 groupbox,
 * 共同被外层 "接收机参数" groupbox 包住 (连接 groupbox 标题已是"接收机", 外层改名避免重名). */
static void CreateReceiverConfigTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 6;
    /* 连接 groupbox (接收机IP + 连接按钮). 版本/固件区不显示 (本 tab 只做配置). */
    y += CreateUdpGroupBox(hDlg, y, -1, -1, -1, -1, -1);

    /* 外层 "接收机参数" groupbox: 包住下方 "接收机配置" + "上行参数" 两个子 groupbox.
     * (连接 groupbox 标题已是"接收机", 外层改名避免重名; 只影响 Tab0)
     * 高度 = 标题区 18 + 子groupbox1(60) + 间距 4 + 子groupbox2(90) + 底边 10 = 182 */
    int oy = y;
    int outerH = 18 + 60 + 4 + 90 + 10;
    CreateWindowExW(0, L"BUTTON", L"接收机参数",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, oy, 436, outerH, hDlg, NULL, g_hInst, NULL);

    /* 子 groupbox 1 "接收机配置": 接收机IP(可配) + 配置端口(只读) + 配置/查询.
     * 嵌入外层 groupbox 内, 左右各留 10 内边距. */
    int g1y = oy + 18;
    CreateWindowExW(0, L"BUTTON", L"接收机配置",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            20, g1y, 416, 60, hDlg, NULL, g_hInst, NULL);
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"接收机IP:",
            WS_CHILD | WS_VISIBLE, 30, g1y + 26, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCfgIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            88, g1y + 24, 110, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_IP, g_hInst, NULL);
    SendMessageW(hCfgIp, WM_SETFONT, (WPARAM)hFont, TRUE);
    hLbl = CreateWindowExW(0, L"STATIC", L"配置端口:",
            WS_CHILD | WS_VISIBLE, 206, g1y + 26, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCfgPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            264, g1y + 24, 56, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_CFGPORT, g_hInst, NULL);
    SendMessageW(hCfgPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCfgApply = CreateWindowExW(0, L"BUTTON", L"配置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            330, g1y + 24, 50, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_APPLY, g_hInst, NULL);
    SendMessageW(hCfgApply, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hCfgQuery = CreateWindowExW(0, L"BUTTON", L"查询",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            386, g1y + 24, 44, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_QUERY, g_hInst, NULL);
    SendMessageW(hCfgQuery, WM_SETFONT, (WPARAM)hFont, TRUE);

    /* 子 groupbox 2 "上行参数": 两行布局.
     * 行1: 上行IP (host_ip) + 上行端口 (host_port)
     * 行2: 数据端口 (data_port, 只读) + 配置/查询按钮 */
    int g2y = g1y + 60 + 4;
    CreateWindowExW(0, L"BUTTON", L"上行参数",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            20, g2y, 416, 90, hDlg, NULL, g_hInst, NULL);
    /* 行1: 上行IP + 上行端口 */
    hLbl = CreateWindowExW(0, L"STATIC", L"上行IP:",
            WS_CHILD | WS_VISIBLE, 30, g2y + 26, 44, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUpIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            74, g2y + 24, 120, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_UPIP, g_hInst, NULL);
    SendMessageW(hUpIp, WM_SETFONT, (WPARAM)hFont, TRUE);
    hLbl = CreateWindowExW(0, L"STATIC", L"上行端口:",
            WS_CHILD | WS_VISIBLE, 206, g2y + 26, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUpPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            264, g2y + 24, 56, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_UPPORT, g_hInst, NULL);
    SendMessageW(hUpPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 行2: 数据端口 (只读) + 配置/查询按钮 (与 groupbox1 按钮同列) */
    hLbl = CreateWindowExW(0, L"STATIC", L"数据端口:",
            WS_CHILD | WS_VISIBLE, 30, g2y + 58, 56, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hDataPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            88, g2y + 56, 56, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_DATAPORT, g_hInst, NULL);
    SendMessageW(hDataPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUpApply = CreateWindowExW(0, L"BUTTON", L"配置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            330, g2y + 56, 50, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_UPAPPLY, g_hInst, NULL);
    SendMessageW(hUpApply, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hUpQuery = CreateWindowExW(0, L"BUTTON", L"查询",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            386, g2y + 56, 44, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_UPQUERY, g_hInst, NULL);
    SendMessageW(hUpQuery, WM_SETFONT, (WPARAM)hFont, TRUE);
    (void)hCfgApply; (void)hCfgQuery; (void)hUpApply; (void)hUpQuery;

    /* ===== 操作按钮: 连接 / 恢复出厂设置 / 重启 (与连接按钮同排, 整体右对齐到 436).
     * 连接按钮由 CreateUdpGroupBox 共享创建 (原 x=366,w=70, 右边贴 436), 在本 tab 需左移,
     * 让出空间给两个新按钮, 三者以 groupbox 右边 436 为基线右对齐. 按钮行 y = 6+22 = 28. */
    int by = 6 + 22;
    HWND hConn = GetDlgItem(hDlg, IDC_UDP_CONNECT);  /* 连接 (最左) */
    if (hConn) SetWindowPos(hConn, NULL, 210, by, 66, 22, SWP_NOZORDER);
    HWND hReset = CreateWindowExW(0, L"BUTTON", L"恢复出厂设置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            282, by, 88, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_FACTORY_RESET, g_hInst, NULL);
    SendMessageW(hReset, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hReboot = CreateWindowExW(0, L"BUTTON", L"重启",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            376, by, 60, 22, hDlg, (HMENU)(INT_PTR)IDC_CFG_REBOOT, g_hInst, NULL);
    SendMessageW(hReboot, WM_SETFONT, (WPARAM)hFont, TRUE);
    (void)hConn;
}

/* 创建 Tab2 固件升级控件: 上 CAN 手柄升级 + 下 UDP 接收机升级 */
static void CreateFwUpgradeTabControls(HWND hDlg)
{
    int y = 6;
    /* 上半: CAN groupbox (连接+版本+固件区), 手柄升级 */
    y += CreateCanGroupBox(hDlg, y, IDC_HFW_VERSION, IDC_HFW_GETVER,
                           IDC_HFW_FILE, IDC_HFW_BROWSE, IDC_HFW_UPGRADE);
    /* 下半: UDP groupbox (连接+版本+固件区), 接收机升级 */
    y += CreateUdpGroupBox(hDlg, y, IDC_TFW_VERSION, IDC_TFW_GETVER,
                           IDC_TFW_FILE, IDC_TFW_BROWSE, IDC_TFW_UPGRADE);
}

/* 创建 Tab4 设备查找控件: 开始/停止按钮 + 设备列表 + 复制按钮 */
static void CreateDiscoverTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    /* 开始/停止查找 按钮 */
    HWND hStart = CreateWindowExW(0, L"BUTTON", L"开始查找",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 20, 120, 28, hDlg, (HMENU)(INT_PTR)IDC_DISC_START, g_hInst, NULL);
    SendMessageW(hStart, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 复制选中 IP 按钮 (从 "ip:port" 条目中取 IP) */
    HWND hCopy = CreateWindowExW(0, L"BUTTON", L"复制选中 IP",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            150, 20, 120, 28, hDlg, (HMENU)(INT_PTR)IDC_DISC_COPY, g_hInst, NULL);
    SendMessageW(hCopy, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 列表标题提示 (y 留足间距: 按钮缩放后 +UI_HPAD 会增高, 标题下移避开) */
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"发现的设备 (IP:配置端口)",
            WS_CHILD | WS_VISIBLE, 20, 56, 200, 14, hDlg, NULL, g_hInst, NULL);
    SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
    /* 发现的设备列表 (LISTBOX, 支持单选), 条目格式 "ip:config_port" */
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
            20, 80, 300, 172, hDlg, (HMENU)(INT_PTR)IDC_DISC_LIST, g_hInst, NULL);
    SendMessageW(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/* 创建 Tab5 调试页控件: gateway UDP 连接 + 手柄数据显示 + 扫描仪模拟发送 */
static void CreateDebugTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 6;

    /* gateway UDP 连接: 网关IP + 本地端口 + 连接按钮, 同一行 */
    CreateWindowExW(0, L"BUTTON", L"网关",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, y, 436, 52, hDlg, NULL, g_hInst, NULL);
    {
        int rowy = y + 20;
        HWND hLbl = CreateWindowExW(0, L"STATIC", L"网关IP:",
                WS_CHILD | WS_VISIBLE, 20, rowy, 52, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hIp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                72, rowy - 2, 90, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_GW_IP, g_hInst, NULL);
        SendMessageW(hIp, WM_SETFONT, (WPARAM)hFont, TRUE);
        hLbl = CreateWindowExW(0, L"STATIC", L"本地端口:",
                WS_CHILD | WS_VISIBLE, 200, rowy, 52, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hLp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"9602",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                254, rowy - 2, 44, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_LOCAL_PORT, g_hInst, NULL);
        SendMessageW(hLp, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hConn = CreateWindowExW(0, L"BUTTON", L"连接",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                326, rowy - 4, 110, 23, hDlg, (HMENU)(INT_PTR)IDC_DBG_CONNECT, g_hInst, NULL);
        SendMessageW(hConn, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    y += 60;

    /* 手柄数据显示 groupbox (只读标签, 无边框, 由 WM_DBG_HANDLER 刷新):
     * 行1: X / Y / 按键; 行2: 心跳时间 / 帧数 */
    CreateWindowExW(0, L"BUTTON", L"手柄数据",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, y, 436, 64, hDlg, NULL, g_hInst, NULL);
    struct { const wchar_t *label; int id; int lx; int vx; } rowsA[] = {
        { L"X角度:", IDC_DBG_HX_VAL, 18, 58 },
        { L"Y角度:", IDC_DBG_HY_VAL, 164, 204 },
        { L"按键:",  IDC_DBG_HBTN_VAL, 306, 344 },
    };
    for (int i = 0; i < 3; i++) {
        int rowy = y + 20;
        HWND hLbl = CreateWindowExW(0, L"STATIC", rowsA[i].label,
                WS_CHILD | WS_VISIBLE, rowsA[i].lx, rowy, 40, 14, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hVal = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                rowsA[i].vx, rowy, 80, 14, hDlg, (HMENU)(INT_PTR)rowsA[i].id, g_hInst, NULL);
        SendMessageW(hVal, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    {
        int rowy = y + 38;
        struct { const wchar_t *label; int id; int lx; int vx; int vw; } rowB[] = {
            { L"心跳时间:", IDC_DBG_HBTIME_VAL, 18, 76, 140 },
            { L"帧数:",    IDC_DBG_HCNT_VAL, 236, 272, 60 },
        };
        for (int i = 0; i < 2; i++) {
            HWND hLbl = CreateWindowExW(0, L"STATIC", rowB[i].label,
                    WS_CHILD | WS_VISIBLE, rowB[i].lx, rowy, 56, 14, hDlg, NULL, g_hInst, NULL);
            SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
            HWND hVal = CreateWindowExW(0, L"STATIC", L"",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    rowB[i].vx, rowy, rowB[i].vw, 14, hDlg,
                    (HMENU)(INT_PTR)rowB[i].id, g_hInst, NULL);
            SendMessageW(hVal, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
    }
    y += 72;
    CreateWindowExW(0, L"BUTTON", L"扫描仪数据模拟发送",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, y, 436, 140, hDlg, NULL, g_hInst, NULL);

    /* row1: 超挖值 + 激光距离 + 发送按钮 */
    {
        int rowy = y + 24;
        HWND hLbl = CreateWindowExW(0, L"STATIC", L"超挖值:",
                WS_CHILD | WS_VISIBLE, 20, rowy, 56, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hOv = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                76, rowy - 2, 66, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_OVB, g_hInst, NULL);
        SendMessageW(hOv, WM_SETFONT, (WPARAM)hFont, TRUE);
        hLbl = CreateWindowExW(0, L"STATIC", L"激光距离:",
                WS_CHILD | WS_VISIBLE, 156, rowy, 64, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hLs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                224, rowy - 2, 66, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_LASER, g_hInst, NULL);
        SendMessageW(hLs, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hBtn = CreateWindowExW(0, L"BUTTON", L"发送超挖/激光",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                326, rowy - 3, 110, 23, hDlg, (HMENU)(INT_PTR)IDC_DBG_SEND_ODO, g_hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* row2: X坐标 + Y坐标 + 发送按钮 */
    {
        int rowy = y + 54;
        HWND hLbl = CreateWindowExW(0, L"STATIC", L"X坐标:",
                WS_CHILD | WS_VISIBLE, 20, rowy, 56, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCx = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                76, rowy - 2, 66, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_CX, g_hInst, NULL);
        SendMessageW(hCx, WM_SETFONT, (WPARAM)hFont, TRUE);
        hLbl = CreateWindowExW(0, L"STATIC", L"Y坐标:",
                WS_CHILD | WS_VISIBLE, 156, rowy, 56, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCy = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2000",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                224, rowy - 2, 66, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_CY, g_hInst, NULL);
        SendMessageW(hCy, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hBtn = CreateWindowExW(0, L"BUTTON", L"发送X/Y",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                326, rowy - 3, 110, 23, hDlg, (HMENU)(INT_PTR)IDC_DBG_SEND_XY, g_hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* row3: Z坐标 + 发送按钮 */
    {
        int rowy = y + 84;
        HWND hLbl = CreateWindowExW(0, L"STATIC", L"Z坐标:",
                WS_CHILD | WS_VISIBLE, 20, rowy, 56, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCz = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"3000",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                76, rowy - 2, 66, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_CZ, g_hInst, NULL);
        SendMessageW(hCz, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hBtn = CreateWindowExW(0, L"BUTTON", L"发送Z",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                326, rowy - 3, 110, 23, hDlg, (HMENU)(INT_PTR)IDC_DBG_SEND_Z, g_hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    /* row4: 自动发送 + 周期 */
    {
        int rowy = y + 114;
        HWND hAuto = CreateWindowExW(0, L"BUTTON", L"开始自动发送",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                326, rowy - 3, 110, 23, hDlg, (HMENU)(INT_PTR)IDC_DBG_AUTO, g_hInst, NULL);
        SendMessageW(hAuto, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hLbl = CreateWindowExW(0, L"STATIC", L"周期(ms):",
                WS_CHILD | WS_VISIBLE, 20, rowy, 56, 16, hDlg, NULL, g_hInst, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hPer = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"500",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                80, rowy - 2, 40, 21, hDlg, (HMENU)(INT_PTR)IDC_DBG_PERIOD, g_hInst, NULL);
        SendMessageW(hPer, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
}

/* 从输入框读取数值 (UI 线程调用). 空/非法返回 fallback */
static int DbgGetEditInt(HWND hDlg, int id, int fallback)
{
    wchar_t wbuf[32] = { 0 };
    GetWindowTextW(GetDlgItem(hDlg, id), wbuf, 32);
    int v = _wtoi(wbuf);
    return v ? v : fallback;
}

/* 从输入框读取字符串 (ASCII) */
static void DbgGetEditText(HWND hDlg, int id, char *out, size_t cap)
{
    wchar_t wbuf[64] = { 0 };
    GetWindowTextW(GetDlgItem(hDlg, id), wbuf, 64);
    WideCharToMultiByte(CP_ACP, 0, wbuf, -1, out, (int)cap, NULL, NULL);
}

/* 经 gateway UDP 发送数据帧: [frame_id 2B BE][payload] */
static void DbgSendFrame(uint16_t id, const uint8_t *payload, size_t plen)
{
    if (!g_dbgUdpConnected) return;
    uint8_t buf[10];
    buf[0] = (uint8_t)((id >> 8) & 0xFF);
    buf[1] = (uint8_t)(id & 0xFF);
    memcpy(buf + 2, payload, plen);
    UdpManager_SendData(g_dbgUdp, buf, plen + 2);
}

/* 发送 0x263 超挖+激光 帧 (payload 大端, 与手柄/网关协议一致) */
static void DbgSendOdo(HWND hDlg)
{
    int ovb = DbgGetEditInt(hDlg, IDC_DBG_OVB, 0);
    int laser = DbgGetEditInt(hDlg, IDC_DBG_LASER, 1000);
    uint8_t data[8] = { 0 };
    data[0] = 0x03;                 /* bit0=overbreak_valid, bit1=laser_valid */
    data[2] = (uint8_t)((ovb >> 8) & 0xFF);   /* overbreak int16 BE */
    data[3] = (uint8_t)(ovb & 0xFF);
    data[4] = (uint8_t)((laser >> 24) & 0xFF); /* laser uint32 BE */
    data[5] = (uint8_t)((laser >> 16) & 0xFF);
    data[6] = (uint8_t)((laser >> 8) & 0xFF);
    data[7] = (uint8_t)(laser & 0xFF);
    DbgSendFrame(CAN_ID_OVERBREAK_LASER, data, 8);
}

/* 发送 0x363 坐标 X/Y 帧 */
static void DbgSendXY(HWND hDlg)
{
    int cx = DbgGetEditInt(hDlg, IDC_DBG_CX, 0);
    int cy = DbgGetEditInt(hDlg, IDC_DBG_CY, 0);
    uint8_t data[8] = { 0 };
    data[0] = (uint8_t)((cx >> 24) & 0xFF);   /* coordX int32 BE */
    data[1] = (uint8_t)((cx >> 16) & 0xFF);
    data[2] = (uint8_t)((cx >> 8) & 0xFF);
    data[3] = (uint8_t)(cx & 0xFF);
    data[4] = (uint8_t)((cy >> 24) & 0xFF);   /* coordY int32 BE */
    data[5] = (uint8_t)((cy >> 16) & 0xFF);
    data[6] = (uint8_t)((cy >> 8) & 0xFF);
    data[7] = (uint8_t)(cy & 0xFF);
    DbgSendFrame(CAN_ID_COORD_XY, data, 8);
}

/* 发送 0x463 坐标 Z 帧 (data[4] bit0 = coordz_valid) */
static void DbgSendZ(HWND hDlg)
{
    int cz = DbgGetEditInt(hDlg, IDC_DBG_CZ, 0);
    uint8_t data[8] = { 0 };
    data[0] = (uint8_t)((cz >> 24) & 0xFF);   /* coordZ int32 BE */
    data[1] = (uint8_t)((cz >> 16) & 0xFF);
    data[2] = (uint8_t)((cz >> 8) & 0xFF);
    data[3] = (uint8_t)(cz & 0xFF);
    data[4] = 0x01;                 /* coordz_valid */
    DbgSendFrame(CAN_ID_COORD_Z, data, 8);
}

/* 自动发送工作线程: 周期发送全部三帧模拟数据, 每次数值取随机区间. */
static DWORD WINAPI DbgAutoSendThread(LPVOID param)
{
    HWND hDlg = (HWND)param;
    srand((unsigned)GetTickCount());
    while (g_dbgAutoSend) {
        if (!g_dbgUdpConnected) break;
        uint8_t data[8] = { 0 };

        /* 0x263 超挖+激光: bit0=overbreak_valid, bit1=laser_valid */
        int ovb = -2000 + rand() % 4001;
        int laser = rand() % 10001;
        data[0] = 0x03;
        data[2] = (uint8_t)((ovb >> 8) & 0xFF);
        data[3] = (uint8_t)(ovb & 0xFF);
        data[4] = (uint8_t)((laser >> 24) & 0xFF);
        data[5] = (uint8_t)((laser >> 16) & 0xFF);
        data[6] = (uint8_t)((laser >> 8) & 0xFF);
        data[7] = (uint8_t)(laser & 0xFF);
        DbgSendFrame(CAN_ID_OVERBREAK_LASER, data, 8);

        /* 0x363 坐标 X/Y */
        memset(data, 0, sizeof(data));
        int cx = -5000 + rand() % 10001;
        int cy = -5000 + rand() % 10001;
        data[0] = (uint8_t)((cx >> 24) & 0xFF);
        data[1] = (uint8_t)((cx >> 16) & 0xFF);
        data[2] = (uint8_t)((cx >> 8) & 0xFF);
        data[3] = (uint8_t)(cx & 0xFF);
        data[4] = (uint8_t)((cy >> 24) & 0xFF);
        data[5] = (uint8_t)((cy >> 16) & 0xFF);
        data[6] = (uint8_t)((cy >> 8) & 0xFF);
        data[7] = (uint8_t)(cy & 0xFF);
        DbgSendFrame(CAN_ID_COORD_XY, data, 8);

        /* 0x463 坐标 Z: data[4] bit0 = coordz_valid */
        memset(data, 0, sizeof(data));
        int cz = -5000 + rand() % 10001;
        data[0] = (uint8_t)((cz >> 24) & 0xFF);
        data[1] = (uint8_t)((cz >> 16) & 0xFF);
        data[2] = (uint8_t)((cz >> 8) & 0xFF);
        data[3] = (uint8_t)(cz & 0xFF);
        data[4] = 0x01;
        DbgSendFrame(CAN_ID_COORD_Z, data, 8);

        int period = DbgGetEditInt(hDlg, IDC_DBG_PERIOD, 500);
        if (period < 10) period = 10;
        if (period > 60000) period = 60000;
        Sleep(period);
    }
    g_dbgAutoSend = FALSE;
    return 0;
}

/* 同步调试页 UDP 连接状态到 UI */
static void SyncDebugUdp(HWND hDlg)
{
    if (!hDlg) hDlg = g_hTabDlg[4];
    if (!hDlg) return;
    SetWindowTextW(GetDlgItem(hDlg, IDC_DBG_CONNECT),
                   g_dbgUdpConnected ? L"断开" : L"连接");
    EnableWindow(GetDlgItem(hDlg, IDC_DBG_GW_IP), g_dbgUdpConnected ? FALSE : TRUE);
    EnableWindow(GetDlgItem(hDlg, IDC_DBG_LOCAL_PORT), g_dbgUdpConnected ? FALSE : TRUE);
}

/* 调试页 UDP 连接/断开: 绑数据通道, 收发手柄/扫描帧.
 * 网关 nRF24 数据转发目标已在 Tab1 配置为指向本机 (SET_HOST), 无需重复设置. */
static void OnDebugUdpConnect(HWND hDlg)
{
    if (g_dbgUdpConnected) {
        g_dbgAutoSend = FALSE;
        UdpManager_Unbind(g_dbgUdp);
        g_dbgUdpConnected = FALSE;
        SyncDebugUdp(hDlg);
        return;
    }

    char ip[64] = { 0 };
    DbgGetEditText(hDlg, IDC_DBG_GW_IP, ip, sizeof(ip));
    if (!ip[0] || inet_addr(ip) == INADDR_NONE) {
        MessageBoxW(g_hMain, L"请填写网关 IP 地址", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    int local_port = DbgGetEditInt(hDlg, IDC_DBG_LOCAL_PORT, 9602);
    if (local_port <= 0 || local_port > 65535) local_port = 9602;
    int gw_data_port = DBG_GW_DATA_PORT_DEFAULT;

    /* 数据通道: 绑本地端口, 向网关数据端口发/收数据帧 */
    if (!UdpManager_Bind(g_dbgUdp, UDP_CHAN_DATA, (uint16_t)local_port, ip, (uint16_t)gw_data_port)) {
        int err = WSAGetLastError();
        wchar_t wmsg[200];
        swprintf(wmsg, 200,
            L"网关连接失败\n本地端口 %d 可能被占用 (WSA 错误码: %d)\n请更换本地端口",
            local_port, err);
        MessageBoxW(g_hMain, wmsg, L"连接失败", MB_OK | MB_ICONERROR);
        return;
    }
    UdpManager_StartRxThread(g_dbgUdp);
    g_dbgUdpConnected = TRUE;
    SyncDebugUdp(hDlg);
}

/* CAN 帧回调: 收到 0x111 RF24 配置响应时填 g_handlerAddr 并置标志.
 * 新格式 [cmd 1B][addr 5B][reserved 2B] (信道固定 1, 不返回). */
static void can_frame_cb(const CanFrame *frame, void *user_data)
{
    (void)user_data;
    if (frame->id == CAN_ID_RF24_CONFIG_RESP && frame->dlc >= 6) {
        memcpy(g_handlerAddr, frame->data + 1, 5);
        g_handlerAddrGot = TRUE;
    }
}

/* 调试页 UDP 数据回调: 解析经 gateway 转发的手柄数据帧.
 * 帧格式: [frame_id 2B BE][payload]:
 *   0x1E3 手柄状态: [x 2B BE][y 2B BE][btn];  0x763 心跳: [1B]. */
static void dbg_udp_data_cb(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    if (len < 2) return;
    uint16_t id = (uint16_t)((data[0] << 8) | data[1]);
    const uint8_t *p = data + 2;
    size_t plen = len - 2;
    if (id == CAN_ID_HANDLER_STATE && plen >= 5) {
        g_dbgX = (int)((int16_t)((p[0] << 8) | p[1]));
        g_dbgY = (int)((int16_t)((p[2] << 8) | p[3]));
        g_dbgBtn = p[4];
        g_dbgFrameCnt++;
        PostMessageW(g_hMain, WM_DBG_HANDLER, 0, 0);
    } else if (id == CAN_ID_HEARTBEAT && plen >= 1) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        g_dbgHeartHMS = (DWORD)(st.wHour * 10000 + st.wMinute * 100 + st.wSecond);
        g_dbgFrameCnt++;
        PostMessageW(g_hMain, WM_DBG_HANDLER, 0, 0);
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
 * 成功返回 true, g_handlerAddr 已填 (信道固定 1, 不再读取). */
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
static void OnGetVersionCan(HWND hChildDlg, BOOL alertOnFail);
static void OnGetVersionUdp(HWND hChildDlg, BOOL alertOnFail);

/* CAN 连接/断开 (各 tab 独立). hChildDlg 的 GWLP_USERDATA 给出 tab 索引.
 * 新布局: Tab1(手柄绑定)→CAN_TAB_BIND, Tab2(固件升级)→CAN_TAB_UPGRADE. 失败友好提示占用原因. */
static void OnCanConnect(HWND hChildDlg)
{
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);
    /* Tab1→CAN_TAB_BIND, Tab2→CAN_TAB_UPGRADE */
    int canTabIdx = (tabIdx == 1) ? CAN_TAB_BIND : CAN_TAB_UPGRADE;

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
    sscanf(dev, "PCAN-USB: %Xh", &channel);

    /* 先检查: 另一个 CAN tab 是否已占同一 channel → 友好提示 */
    for (int other = 0; other < CAN_TAB_COUNT; other++) {
        if (other != canTabIdx && g_canTabChannel[other] == channel) {
            wchar_t wmsg[160];
            const wchar_t *otherName = (other == CAN_TAB_BIND) ? L"手柄绑定页" : L"固件升级页";
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
                L"未识别到手柄设备\n"
                L"请确认手柄设备未休眠且处于CAN模式",
                L"连接失败", MB_OK | MB_ICONERROR);
            CanManager_Disconnect(g_canTab[CAN_TAB_BIND]);
            return;
        }
    }
    /* 验证通过 (Tab1) 或非 Tab1 (Tab2): 正式标记已连接 */
    g_canTabChannel[canTabIdx] = channel;
    SyncCanConnState(canTabIdx);
    if (canTabIdx == CAN_TAB_UPGRADE) {
        /* 升级 tab 连接成功后自动读版本; 失败弹框提示用户检查连接 */
        OnGetVersionCan(hChildDlg, TRUE);
    }
}

/* 接收机连接/断开 (从 IP 框取目标 IP, 配置端口固定 8600). 已连接则断开.
 * Tab1(手柄绑定)→UDP_TAB_BIND; Tab0(配置)/Tab2(固件升级)→UDP_TAB_CFG (同一接收机实例). */
static void OnUdpConnect(HWND hChildDlg)
{
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);
    int udpTab = (tabIdx == 1) ? UDP_TAB_BIND : UDP_TAB_CFG;
    UdpManager *mgr = g_cfgUdp[udpTab];
    if (g_udpConnected[udpTab]) {
        /* 断开 */
        UdpManager_Unbind(mgr);
        g_udpConnected[udpTab] = 0;
        g_udpBroadcast[udpTab] = FALSE;
        SyncUdpConnState(udpTab);
        return;
    }
    /* 取 IP 框内容. Tab1(绑定) 要求单播; Tab0(配置)/Tab2(固件升级) 允许有限广播 255.255.255.255
     * (同子网广播下 SET_IP/GET_NET/SET_HOST 仍可用; 跨子网仅 DISCOVER 有效).
     * 子网定向广播 (x.x.x.255) 固件跨子网不可靠, 各 tab 均拒绝. */
    wchar_t wip[64] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_UDP_IP), wip, 64);
    char ip[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wip, -1, ip, sizeof(ip), NULL, NULL);
    if (!ip[0]) {
        MessageBoxW(g_hMain, L"请填写接收机 IP 地址", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 有限广播 255.255.255.255: inet_addr 返回 INADDR_NONE (与错误同值), 用字符串识别 */
    BOOL is_limited_bcast = (strcmp(ip, "255.255.255.255") == 0);
    if (is_limited_bcast) {
        if (udpTab == UDP_TAB_BIND) {
            MessageBoxW(g_hMain, L"绑定页只支持单播 IP\n广播请用「接收机配置」或「设备查找」页",
                        L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        /* Tab0(配置)/Tab2(固件升级) 允许: 同子网广播下可配置单台 */
    } else {
        unsigned long nip = inet_addr(ip);
        if (nip == INADDR_NONE || nip == INADDR_ANY) {
            MessageBoxW(g_hMain, L"IP 地址格式不正确", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        if ((nip & 0xFF) == 0xFF) {
            MessageBoxW(g_hMain, L"不支持子网定向广播 (x.x.x.255)\n请用 255.255.255.255 有限广播",
                        L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    /* 本地端口由 OS 自动分配 (传 0 给 Bind). 远程固定 8600 (配置端口), 单播到指定 IP.
     * 固件回复到发送方源端口 (OS 临时端口), 配置通道无需固定本地端口. */
    if (!UdpManager_Bind(mgr, UDP_CHAN_CONFIG, 0, ip, 8600)) {
        int err = WSAGetLastError();
        wchar_t wmsg[200];
        swprintf(wmsg, 200,
            L"接收机连接失败 (WSA 错误码: %d)\n请检查网络或设备 IP",
            err);
        MessageBoxW(g_hMain, wmsg, L"连接失败", MB_OK | MB_ICONERROR);
        return;
    }
    UdpManager_StartRxThread(mgr);
    /* Tab1 先验证设备响应 (GET_RF24), 失败则断开且不标记已连接 (不显示"已连接") */
    if (udpTab == UDP_TAB_BIND) {
        uint8_t addr[5];
        if (!UdpManager_GetRF24(mgr, addr)) {
            MessageBoxW(g_hMain,
                L"连接失败\n"
                L"请确认 IP 正确且接收机已上电",
                L"连接失败", MB_OK | MB_ICONERROR);
            UdpManager_Unbind(mgr);   /* 停 RX 线程 + 关 socket */
            return;
        }
    }
    /* 验证通过 (Tab1 绑定) 或非绑定 tab: 正式标记已连接 */
    g_udpConnected[udpTab] = 1;
    g_udpBroadcast[udpTab] = is_limited_bcast;
    SyncUdpConnState(udpTab);
    if (udpTab == UDP_TAB_CFG && !is_limited_bcast && GetDlgItem(hChildDlg, IDC_TFW_VERSION)) {
        /* Tab2 (固件升级) 单播连接成功后自动读版本 (Tab0 无版本框会跳过; 广播模式跳过: 多设备歧义);
         * 失败弹框提示用户检查连接 */
        OnGetVersionUdp(hChildDlg, TRUE);
    }
}

/* ===== Tab4 设备查找: 原生 winsock 广播 DISCOVER 收集响应 =====
 * 不走 UdpManager (其回调不带源 IP); 直接 socket 收发.
 * 用 DISCOVER (0x15): 设备回复 [0x15][ip 4B][config_port 2B], 显示 ip:config_port.
 *
 * 收包端口必须绑 8601 (CONFIG_PORT+1): 固件跨子网时把 DISCOVER 回复定向广播到
 * CONFIG_PORT+1=8601; 同子网时回复到发送方源端口 (我们即 8601). 故绑 8601 两种情况都能收到. */

/* 设备查找线程: 广播 DISCOVER, 收集 10s 内所有响应, 解析回复 ip+port 上报去重 */
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
    local.sin_port = htons(8601);   /* 本地端口 8601 (固件跨子网回复端口 CONFIG_PORT+1) */
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        closesocket(s);
        return 0;
    }

    /* 用有限广播 255.255.255.255 发 DISCOVER (0x15), 确保跨子网到达设备
     * (设备 IP 可能被改到与本机不同子网, 子网定向广播 x.x.x.255 到不了). */
    uint8_t pkt = UDP_CMD_DISCOVER;   /* 0x15 */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(8600);       /* 配置端口 8600 */
    dst.sin_addr.s_addr = INADDR_BROADCAST;   /* 255.255.255.255 */
    sendto(s, (const char *)&pkt, 1, 0, (struct sockaddr *)&dst, sizeof(dst));

    /* 接收窗口 10s (只收不重发). 解析回复 [0x15][ip 4B BE][config_port 2B BE].
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
        /* 响应首字节应为 DISCOVER (0x15); 负载 [ip 4B BE][config_port 2B BE].
         * 优先用负载里的 ip (设备本机 IP, 经路由回复时源 IP 不准).
         * 显示格式 "ip:config_port". */
        if (n >= 7 && buf[0] == UDP_CMD_DISCOVER) {
            uint16_t cport = ((uint16_t)buf[5] << 8) | buf[6];
            char entry[32];
            sprintf(entry, "%u.%u.%u.%u:%u", buf[1], buf[2], buf[3], buf[4], cport);
            PostMessageA(g_hMain, WM_DISC_FOUND_IP, 0, (LPARAM)_strdup(entry));
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
    /* 开始查找: 先清空历史记录, 再启动发现线程 */
    HWND hList = GetDlgItem(hChildDlg, IDC_DISC_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_discRunning = TRUE;
    SetWindowTextW(GetDlgItem(hChildDlg, IDC_DISC_START), L"停止查找");
    CreateThread(NULL, 0, discover_thread, NULL, 0, NULL);
}

/* 复制选中条目的 IP (去掉 :config_port) 到剪贴板, 便于粘贴到 Tab3 接收机 IP 框 */
static void OnDiscoverCopy(HWND hChildDlg)
{
    HWND hList = GetDlgItem(hChildDlg, IDC_DISC_LIST);
    int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (sel < 0) {
        MessageBoxW(g_hMain, L"请先在列表中选择一个设备", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t wentry[64] = { 0 };
    SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)wentry);
    /* 列表项格式 "ip:port", 截掉 ':' 及之后内容, 只留 IP */
    wchar_t *colon = wcschr(wentry, L':');
    if (colon) *colon = L'\0';
    if (!OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    size_t len = wcslen(wentry) + 1;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(wchar_t));
    if (hg) {
        memcpy(GlobalLock(hg), wentry, len * sizeof(wchar_t));
        GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
    /* 不弹框, 静默复制 (避免打扰). */
}

/* ===== Tab0 接收机配置 业务函数 (走 UDP_TAB_CFG 实例) ===== */

/* 行1 配置 (SET_IP 0x10): 设置接收机静态 IP. 掩码固定, 网关固件自算.
 * SET_IP 仅发 4B IP, 解析 1B 成功/失败回复. */
static void OnCfgApply(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t wip[64] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_CFG_IP), wip, 64);
    char ip[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wip, -1, ip, sizeof(ip), NULL, NULL);
    if (!ip[0]) {
        MessageBoxW(g_hMain, L"请填写接收机 IP 地址", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    if (inet_addr(ip) == INADDR_NONE) {
        MessageBoxW(g_hMain, L"IP 地址格式不正确", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* SET_IP: [ip 4B BE] → [1B: 1=成功/0=失败]. 失败 = IP 非法 或 DHCP 模式 */
    bool ok = false;
    if (UdpManager_SetIp(g_cfgUdp[UDP_TAB_CFG], ip, &ok)) {
        if (ok) {
            MessageBoxW(g_hMain, L"接收机 IP 已设置, 重启后生效", L"成功",
                        MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(g_hMain, L"设备拒绝设置\n(IP 非法或处于 DHCP 模式)", L"设置失败",
                        MB_OK | MB_ICONWARNING);
        }
    } else {
        MessageBoxW(g_hMain, L"设置失败 (接收机未响应)", L"错误", MB_OK | MB_ICONERROR);
    }
}

/* 行1 查询 (DISCOVER 0x15): 单播发现, 回填接收机 IP + 配置端口.
 * DISCOVER 响应 [ip 4B][config_port 2B] = 6B. */
static void OnCfgQuery(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    char ip[16] = { 0 };
    uint16_t config_port = 0;
    if (UdpManager_Discover(g_cfgUdp[UDP_TAB_CFG], ip, sizeof(ip), &config_port)) {
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_CFG_IP), ip);
        char port_str[8];
        sprintf(port_str, "%d", config_port);
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_CFG_CFGPORT), port_str);
    } else {
        MessageBoxW(g_hMain, L"查询失败 (接收机未响应 DISCOVER)", L"提示",
                    MB_OK | MB_ICONWARNING);
    }
}

/* 行2 配置 (SET_HOST 0x14): 设置上行 IP (host_ip) + 上行数据监听端口 (host_port).
 * 固件把 nRF24 数据固定单播到此 IP:端口; 上位机本地数据端口应与之相同才能收到转发数据. */
static void OnCfgUpApply(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    wchar_t wip[64] = { 0 }, wport[16] = { 0 };
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_CFG_UPIP), wip, 64);
    GetWindowTextW(GetDlgItem(hChildDlg, IDC_CFG_UPPORT), wport, 16);
    char ip[64] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, wip, -1, ip, sizeof(ip), NULL, NULL);
    if (!ip[0]) {
        MessageBoxW(g_hMain, L"请填写上行 IP 地址", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    if (inet_addr(ip) == INADDR_NONE) {
        MessageBoxW(g_hMain, L"IP 地址格式不正确", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    int port = _wtoi(wport);
    if (port <= 0 || port > 65535) port = 9602;   /* 默认 host_port 9602 */
    if (UdpManager_SetHost(g_cfgUdp[UDP_TAB_CFG], ip, (uint16_t)port)) {
        MessageBoxW(g_hMain, L"上行参数已设置\n接收机 nRF24 数据将转发到此地址", L"成功",
                    MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(g_hMain, L"设置失败 (发送命令失败)", L"错误", MB_OK | MB_ICONERROR);
    }
}

/* 行2 查询 (GET_NET 0x11): 回填上行 IP + 上行端口 (host_port) + 数据端口 (data_port, 只读).
 * GET_NET 响应 [data_port 2B][host_ip 4B][host_port 2B] = 8B. */
static void OnCfgUpQuery(HWND hChildDlg)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    char host_ip[16] = { 0 };
    uint16_t data_port = 0, host_port = 0;
    if (UdpManager_GetNet(g_cfgUdp[UDP_TAB_CFG], &data_port, host_ip, sizeof(host_ip), &host_port)) {
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_CFG_UPIP), host_ip);
        char port_str[8];
        sprintf(port_str, "%d", host_port);
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_CFG_UPPORT), port_str);
        sprintf(port_str, "%d", data_port);
        SetWindowTextA(GetDlgItem(hChildDlg, IDC_CFG_DATAPORT), port_str);
    } else {
        MessageBoxW(g_hMain, L"查询失败 (接收机未响应)", L"提示", MB_OK | MB_ICONWARNING);
    }
}

/* 恢复出厂设置: 由固件 (FACTORY_RESET 0x16) 内部统一恢复全部出厂参数并自行重启.
 * 上位机只发一条命令, 不再分别设置 RF24/HOST/IP, 也无需再发 Reboot. */
static void OnCfgFactoryReset(HWND hChildDlg)
{
    (void)hChildDlg;
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    if (MessageBoxW(g_hMain, L"确认恢复所有默认参数吗?",
                    L"恢复出厂设置", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    /* FACTORY_RESET (0x16): 固件内部恢复全部出厂参数并自行重启 */
    bool ok = false;
    if (UdpManager_FactoryReset(g_cfgUdp[UDP_TAB_CFG], &ok)) {
        if (ok) {
            MessageBoxW(g_hMain, L"已恢复所有默认参数, 接收机正在重启",
                        L"恢复出厂设置", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(g_hMain, L"接收机拒绝恢复出厂设置", L"设置失败",
                        MB_OK | MB_ICONWARNING);
        }
    } else {
        MessageBoxW(g_hMain, L"恢复失败 (接收机未响应)", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}

/* 重启接收机: REBOOT (0x05). 重启后当前连接失效, 需重新连接. */
static void OnCfgReboot(HWND hChildDlg)
{
    (void)hChildDlg;
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    if (MessageBoxW(g_hMain, L"确认重启接收机吗?",
                    L"重启接收机", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (UdpManager_Reboot(g_cfgUdp[UDP_TAB_CFG])) {
        MessageBoxW(g_hMain, L"接收机正在重启", L"重启接收机",
                    MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(g_hMain, L"重启失败 (接收机未响应)", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}

/* 获取手柄 CAN 固件版本并显示 (Tab2). 新版固件回多帧拼接的版本字符串 (如 v0.1.4_0b4ee3).
 * alertOnFail=TRUE 时读取失败会弹框提示, 用于连接后自动读取场景. */
static void OnGetVersionCan(HWND hChildDlg, BOOL alertOnFail)
{
    if (g_canTabChannel[CAN_TAB_UPGRADE] < 0) {
        MessageBoxW(g_hMain, L"请先连接手柄", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    char ver[64] = { 0 };
    if (CanManager_GetVersionStr(g_canTab[CAN_TAB_UPGRADE], ver, sizeof(ver))) {
        wchar_t wver[64];
        MultiByteToWideChar(CP_ACP, 0, ver, -1, wver, 64);
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_HFW_VERSION), wver);
    } else {
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_HFW_VERSION), L"读取失败");
        if (alertOnFail) {
            MessageBoxW(g_hMain,
                L"读取手柄固件版本失败，请确定设备是否正确连接。",
                L"固件升级", MB_OK | MB_ICONWARNING);
        }
    }
}

/* 获取接收机 UDP 固件版本并显示 (Tab3). 新版固件回版本字符串 (如 v0.1.0_0b4ee3).
 * alertOnFail=TRUE 时读取失败会弹框提示, 用于连接后自动读取场景. */
static void OnGetVersionUdp(HWND hChildDlg, BOOL alertOnFail)
{
    if (!g_udpConnected[UDP_TAB_CFG]) {
        MessageBoxW(g_hMain, L"请先连接接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    char ver[64] = { 0 };
    if (UdpManager_GetVersion(g_cfgUdp[UDP_TAB_CFG], ver, sizeof(ver))) {
        wchar_t wver[64];
        MultiByteToWideChar(CP_ACP, 0, ver, -1, wver, 64);
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_TFW_VERSION), wver);
    } else {
        SetWindowTextW(GetDlgItem(hChildDlg, IDC_TFW_VERSION), L"读取失败");
        if (alertOnFail) {
            MessageBoxW(g_hMain,
                L"读取接收机固件版本失败，请确定设备是否正确连接。",
                L"固件升级", MB_OK | MB_ICONWARNING);
        }
    }
}

/* 检测绑定状态: 读手柄 NRF + 接收机 NRF, 比对 */
static void OnCheckBind(HWND hChildDlg)
{
    if (g_canTabChannel[CAN_TAB_BIND] < 0 || !g_udpConnected[UDP_TAB_BIND]) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收机", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 1. 读手柄 NRF */
    if (!ReadHandlerNrf()) {
        MessageBoxW(g_hMain, L"读取手柄地址超时\n请确认手柄已上电", L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }
    /* 2. 读接收机 NRF */
    if (!UdpManager_GetRF24(g_cfgUdp[UDP_TAB_BIND], g_receiverAddr)) {
        MessageBoxW(g_hMain,
            L"读取接收机 NRF 地址超时\n请确认接收机已上电并在同一网络",
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
            L"当前手柄和接收机未绑定\n点击「绑定设备」可绑定当前手柄和接收机",
            L"绑定状态: 未绑定", MB_OK | MB_ICONINFORMATION);
    } else if (!same) {
        MessageBoxW(g_hMain,
            L"当前手柄和接收机未绑定\n接收机已绑定其他设备\n点击「绑定设备」可重新绑定当前手柄和接收机",
            L"绑定状态: 未绑定", MB_OK | MB_ICONWARNING);
    } else {
        MessageBoxW(g_hMain, L"当前手柄和接收机已绑定", L"绑定状态: 已绑定", MB_OK | MB_ICONINFORMATION);
    }
}

/* 绑定设备: 把手柄 NRF 地址写入接收机 */
static void OnBind(HWND hChildDlg)
{
    (void)hChildDlg;
    if (g_canTabChannel[CAN_TAB_BIND] < 0 || !g_udpConnected[UDP_TAB_BIND]) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收机", L"提示", MB_OK | MB_ICONWARNING);
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
    /* 先读接收机地址, 检查是否已绑定当前手柄 */
    uint8_t recvAddr[5];
    if (!UdpManager_GetRF24(g_cfgUdp[UDP_TAB_BIND], recvAddr)) {
        MessageBoxW(g_hMain, L"读取接收机地址超时", L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }
    BOOL recvBound = (recvAddr[0]|recvAddr[1]|recvAddr[2]|recvAddr[3]|recvAddr[4]) != 0;
    if (recvBound && memcmp(recvAddr, g_handlerAddr, 5) == 0) {
        MessageBoxW(g_hMain, L"当前手柄和接收机已绑定", L"已绑定",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(g_hMain, L"确认绑定当前手柄和接收机吗?",
                    L"确认绑定", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (UdpManager_SetRF24(g_cfgUdp[UDP_TAB_BIND], g_handlerAddr)) {
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

/* 主窗口收到子对话框转发的命令: hChildDlg=子对话框句柄, wParam 含控件 ID (LOWORD).
 * tabIdx (GWLP_USERDATA): 0=接收机配置 1=手柄绑定 2=固件升级 3=设备查找 */
static void OnTabCommand(HWND hChildDlg, WPARAM wParam)
{
    int cmdId = LOWORD(wParam);
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);

    /* CAN/UDP 连接命令多 tab 共享 (Tab1/Tab2 有 CAN; Tab0/Tab1/Tab2 有 UDP) */
    switch (cmdId) {
    case IDC_CAN_REFRESH:       RefreshCanDevices(GetDlgItem(hChildDlg, IDC_CAN_DEVICE), tabIdx); return;
    case IDC_CAN_CONNECT:       OnCanConnect(hChildDlg);   return;
    case IDC_UDP_CONNECT:       OnUdpConnect(hChildDlg);   return;
    }

    if (tabIdx == 0) {
        /* 接收机配置页: 行1 (SET_IP/DISCOVER) + 行2 (SET_HOST/GET_NET) */
        switch (cmdId) {
        case IDC_CFG_APPLY:         OnCfgApply(hChildDlg);         break;
        case IDC_CFG_QUERY:         OnCfgQuery(hChildDlg);         break;
        case IDC_CFG_UPAPPLY:       OnCfgUpApply(hChildDlg);       break;
        case IDC_CFG_UPQUERY:       OnCfgUpQuery(hChildDlg);       break;
        case IDC_CFG_FACTORY_RESET: OnCfgFactoryReset(hChildDlg);  break;
        case IDC_CFG_REBOOT:        OnCfgReboot(hChildDlg);        break;
        }
    } else if (tabIdx == 1) {
        /* 手柄绑定页: 剩余专属按钮 */
        switch (cmdId) {
        case IDC_BTN_CHECK_BIND:    OnCheckBind(hChildDlg);    break;
        case IDC_BTN_BIND:          OnBind(hChildDlg);         break;
        }
    } else if (tabIdx == 2) {
        /* 固件升级页: 上半 CAN 手柄升级 (HFW) + 下半 UDP 接收机升级 (TFW) */
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
        case IDC_HFW_GETVER:          OnGetVersionCan(hChildDlg, FALSE); break;
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
                MessageBoxW(g_hMain, L"请先连接接收机", L"提示",
                            MB_OK | MB_ICONWARNING);
            }
            break;
        case IDC_TFW_GETVER:         OnGetVersionUdp(hChildDlg, FALSE); break;
        }
    } else if (tabIdx == 3) {
        /* 设备查找页 (Tab4) */
        switch (cmdId) {
        case IDC_DISC_START:         OnDiscoverStart(hChildDlg); break;
        case IDC_DISC_COPY:          OnDiscoverCopy(hChildDlg);  break;
        }
    } else if (tabIdx == 4) {
        /* 调试页 (Tab5): 网关 UDP 连接 + 扫描仪模拟发送 + 自动发送开关 */
        switch (cmdId) {
        case IDC_DBG_CONNECT:        OnDebugUdpConnect(hChildDlg);    break;
        case IDC_DBG_SEND_ODO:       DbgSendOdo(hChildDlg);           break;
        case IDC_DBG_SEND_XY:        DbgSendXY(hChildDlg);            break;
        case IDC_DBG_SEND_Z:         DbgSendZ(hChildDlg);             break;
        case IDC_DBG_AUTO:
            if (g_dbgAutoSend) {
                /* 停止: 置标志, 工作线程下一轮自我退出 */
                g_dbgAutoSend = FALSE;
                SetWindowTextW(GetDlgItem(hChildDlg, IDC_DBG_AUTO), L"开始自动发送");
            } else {
                if (!g_dbgUdpConnected) {
                    MessageBoxW(g_hMain, L"请先在本页连接网关", L"提示",
                                MB_OK | MB_ICONWARNING);
                    break;
                }
                g_dbgAutoSend = TRUE;
                SetWindowTextW(GetDlgItem(hChildDlg, IDC_DBG_AUTO), L"停止自动发送");
                if (g_dbgSendThread) CloseHandle(g_dbgSendThread);
                g_dbgSendThread = CreateThread(NULL, 0, DbgAutoSendThread, hChildDlg, 0, NULL);
            }
            break;
        }
    }
}

/* 创建 Tab 控件 + 3 个子对话框, 在 WM_CREATE 中调用 */
static void CreateTabLayout(HWND hWnd)
{
    /* Tab 控件位于顶部 (尺寸×SCALE; 实际放大量由 InitInstance 的主窗口容纳) */
    g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
            0, 0, S(480), S(324), hWnd, (HMENU)1, g_hInst, NULL);

    /* 四个页签标题 (从左到右: 接收机配置 / 手柄绑定 / 固件升级 / 设备查找) */
    TCITEMW ti; ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"接收机配置";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"手柄绑定";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"固件升级";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 2, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"设备查找";
    SendMessageW(g_hTab, TCM_INSERTITEMW, 3, (LPARAM)&ti);
    /* 调试页默认隐藏, 由 Ctrl+Shift+B 切换 (见 ToggleDebugTab) */

    /* Tab 标题字体: Segoe UI 常规, 字号×SCALE (1.5x → -18) */
    HFONT hTabFont = CreateFontW(S(-12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(g_hTab, WM_SETFONT, (WPARAM)hTabFont, TRUE);

    /* 子对话框显示区: tab 下方 */
    RECT rcTab;
    GetClientRect(g_hTab, &rcTab);
    SendMessageW(g_hTab, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcTab);

    RegisterTabChildClass();
    DWORD childStyle = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    for (int i = 0; i < 5; i++) {
        g_hTabDlg[i] = CreateWindowExW(0, TABCHILD_CLASS, L"",
                childStyle,
                rcTab.left, rcTab.top,
                rcTab.right - rcTab.left, rcTab.bottom - rcTab.top,
                g_hTab, NULL, g_hInst, NULL);
        SetWindowLongPtrW(g_hTabDlg[i], GWLP_USERDATA, (LONG_PTR)i);
        ShowWindow(g_hTabDlg[i], i == 0 ? SW_SHOW : SW_HIDE);
    }

    /* 创建各 tab 控件 (按新索引: 0=接收机配置 1=手柄绑定 2=固件升级 3=设备查找) */
    CreateReceiverConfigTabControls(g_hTabDlg[0]);
    CreateBindTabControls(g_hTabDlg[1]);
    CreateFwUpgradeTabControls(g_hTabDlg[2]);
    CreateDiscoverTabControls(g_hTabDlg[3]);
    CreateDebugTabControls(g_hTabDlg[4]);

    /* 全局 1.5x 缩放: 控件已按原坐标建完, 这里后处理缩放每个 tab 子对话框的子控件 + 换字体.
     * tab 子对话框自身尺寸已由 rcTab (来自缩放后的 g_hTab) 确定, 无需再缩. */
    for (int i = 0; i < 5; i++) {
        ScaleChildWindows(g_hTabDlg[i]);
    }

    /* 初始按钮状态: 未连接 → 禁用依赖连接的操作 (升级/版本/目标主机) */
    SyncUdpConnState(UDP_TAB_BIND);
    SyncUdpConnState(UDP_TAB_CFG);
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
    /* 进度窗尺寸×SCALE (300x150 → 450x225) */
    RECT rc = { 0, 0, S(300), S(150) };
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

    /* 控件按原坐标创建, 下方统一 ScaleChildWindows 缩放.
     * 进度窗字体用更大一号 (原 -14 ×SCALE = -21). */
    HFONT hFont = CreateFontW(S(-14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_progressLabel = CreateWindowExW(0, L"STATIC", L"准备升级...",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 18, 260, 24, g_progressDlg, (HMENU)IDC_PROG_LABEL, g_hInst, NULL);
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
    /* 缩放进度窗内所有子控件 (坐标/尺寸×SCALE; ScaleChildWindows 会把字体设成 g_hUiFont,
     * 进度窗需更大字体, 故缩放后再恢复 hFont). */
    ScaleChildWindows(g_progressDlg);
    SendMessageW(g_progressLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_progressBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
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
    /* 运行时动态定位: 进度窗已缩放, 子控件坐标须×SCALE 保持对齐 */
    SetWindowPos(g_progressLabel, HWND_TOP, S(20), S(25), S(260), S(48), SWP_NOZORDER);
    if (success) {
        SetWindowTextW(g_progressLabel, L"升级完成！\n点击重启设备生效");
        SetWindowTextW(g_progressBtn, L"确定");
        EnableWindow(g_progressBtn, TRUE);
        SetWindowPos(g_progressBtn, HWND_TOP, S(165), S(92), S(80), S(28), SWP_SHOWWINDOW);
        SetWindowTextW(g_progressReboot, L"重启设备");
        EnableWindow(g_progressReboot, TRUE);
        SetWindowPos(g_progressReboot, HWND_TOP, S(55), S(92), S(95), S(28), SWP_SHOWWINDOW);
    } else {
        SetWindowTextW(g_progressLabel, L"升级失败\n请重试或检查设备连接");
        ShowWindow(g_progressReboot, SW_HIDE);
        EnableWindow(g_progressReboot, FALSE);
        SetWindowTextW(g_progressBtn, L"确定");
        EnableWindow(g_progressBtn, TRUE);
        SetWindowPos(g_progressBtn, HWND_TOP, S(110), S(92), S(80), S(28), SWP_SHOWWINDOW);
    }
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(g_progressDlg);
}

/* 切换调试 tab 显示/隐藏 (Ctrl+Shift+B 触发).
 * 调试页始终为最后一个 tab item, 其 g_hTabDlg 索引固定为 4. */
static void ToggleDebugTab(void)
{
    g_dbgTabShown = !g_dbgTabShown;
    if (g_dbgTabShown) {
        TCITEMW ti;
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"调试";
        SendMessageW(g_hTab, TCM_INSERTITEMW, 4, (LPARAM)&ti);
        /* 自动选中调试页并显示其组件, 免去手动点击 */
        SendMessageW(g_hTab, TCM_SETCURSEL, 4, 0);
        for (int i = 0; i < 5; i++)
            ShowWindow(g_hTabDlg[i], i == 4 ? SW_SHOW : SW_HIDE);
    } else {
        int sel = (int)SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0);
        if (sel == 4) {
            SendMessageW(g_hTab, TCM_SETCURSEL, 0, 0);
            for (int i = 0; i < 5; i++)
                ShowWindow(g_hTabDlg[i], i == 0 ? SW_SHOW : SW_HIDE);
        }
        SendMessageW(g_hTab, TCM_DELETEITEM, 4, 0);
        ShowWindow(g_hTabDlg[4], SW_HIDE);
    }
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hWnd;
        CreateTabLayout(hWnd);
        RegisterHotKey(hWnd, IDH_TOGGLE_DEBUG, MOD_CONTROL | MOD_SHIFT, 'B');
        return 0;

    case WM_HOTKEY:
        if (wParam == IDH_TOGGLE_DEBUG) ToggleDebugTab();
        return 0;

    case WM_NOTIFY: {
        LPNMHDR nmh = (LPNMHDR)lParam;
        if (nmh->hwndFrom == g_hTab && nmh->code == TCN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0);
            for (int i = 0; i < 5; i++) {
                ShowWindow(g_hTabDlg[i], i == sel ? SW_SHOW : SW_HIDE);
            }
        }
        return 0;
    }

    case WM_APP + 100: {
        OnTabCommand((HWND)lParam, wParam);   /* wParam 含控件 ID (LOWORD) */
        return 0;
    }

    case WM_DBG_HANDLER: {
        /* 调试页收到手柄数据/心跳, 刷新只读标签 */
        HWND hDbg = g_hTabDlg[4];
        if (hDbg) {
            wchar_t tmp[32];
            swprintf(tmp, 32, L"%d", g_dbgX);
            SetWindowTextW(GetDlgItem(hDbg, IDC_DBG_HX_VAL), tmp);
            swprintf(tmp, 32, L"%d", g_dbgY);
            SetWindowTextW(GetDlgItem(hDbg, IDC_DBG_HY_VAL), tmp);
            swprintf(tmp, 32, L"%d", g_dbgBtn);
            SetWindowTextW(GetDlgItem(hDbg, IDC_DBG_HBTN_VAL), tmp);
            DWORD hms = g_dbgHeartHMS;
            swprintf(tmp, 32, L"%02d:%02d:%02d", hms / 10000, (hms / 100) % 100, hms % 100);
            SetWindowTextW(GetDlgItem(hDbg, IDC_DBG_HBTIME_VAL), tmp);
            swprintf(tmp, 32, L"%d", g_dbgFrameCnt);
            SetWindowTextW(GetDlgItem(hDbg, IDC_DBG_HCNT_VAL), tmp);
        }
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
        /* CAN 手柄升级 (HFW) 与 UDP 接收机升级 (TFW) 都在 Tab2 (g_hTabDlg[2]) */
        int upgradeId = isCan ? IDC_HFW_UPGRADE : IDC_TFW_UPGRADE;
        int browseId  = isCan ? IDC_HFW_BROWSE  : IDC_TFW_BROWSE;
        HWND hTabChild = g_hTabDlg[2];
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
        HWND hTabChild = g_hTabDlg[2];
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
        UnregisterHotKey(hWnd, IDH_TOGGLE_DEBUG);
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
     * BIND tab 挂 frame_cb (处理 0x111 NRF 响应); UPGRADE tab 只用于固件升级.
     * Tab5 调试页用独立 UDP 数据通道 (见下方 g_dbgUdp). */
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

    /* Tab5 调试: UDP 数据通道 (收发经 gateway 的数据帧);
     * 网关 nRF24 转发目标已在 Tab1 配置为指向本机, 无需 SET_HOST. */
    g_dbgUdp = UdpManager_Create();
    UdpManager_SetDataCallback(g_dbgUdp, dbg_udp_data_cb, NULL);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ZCodeHandlerReceiver";
    RegisterClassW(&wc);

    /* 统一放大字体 (DEFAULT_GUI_FONT 9pt → 1.5x ≈ 13.5pt = -18), 供所有控件缩放后使用 */
    g_hUiFont = CreateFontW(S(-12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    /* 主窗口尺寸 480x344 ×SCALE (1.5x → 720x516 客户区, 含 tab 显示区) */
    RECT rc = { 0, 0, S(480), S(344) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE, 0);

    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
    /* 主窗口居中屏幕 (CW_USEDEFAULT 对 overlapped 窗口默认靠左上角) */
    int sx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    HWND hWnd = CreateWindowExW(0, L"ZCodeHandlerReceiver",
            L"手柄-接收机工具 v" APP_VERSION_W,
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
    g_dbgAutoSend = FALSE;
    if (g_dbgSendThread) {
        WaitForSingleObject(g_dbgSendThread, 2000);
        CloseHandle(g_dbgSendThread);
        g_dbgSendThread = NULL;
    }
    for (int i = 0; i < CAN_TAB_COUNT; i++) {
        if (g_canTab[i]) CanManager_Destroy(g_canTab[i]);
    }
    for (int i = 0; i < UDP_TAB_COUNT; i++) {
        if (g_cfgUdp[i]) UdpManager_Destroy(g_cfgUdp[i]);
    }
    if (g_dbgUdp) UdpManager_Destroy(g_dbgUdp);
    return (int)m.wParam;
}
