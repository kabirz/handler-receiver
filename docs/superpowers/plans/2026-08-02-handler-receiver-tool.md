# 手柄-接收器绑定与固件升级工具 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建一个 Win32 纯 C 三 Tab 工具，实现手柄(CAN)/接收器(UDP)设备绑定、手柄 CAN 固件升级、接收器 UDP 固件升级。

**Architecture:** 直接复用 gateway-tool 的 `can_manager`/`udp_manager`/`pcan_loader` 三个模块源码；新写一个 `main.c` 用 `SysTabControl32` 组织三个子对话框页签，运行时 `CreateWindowExW` 动态创建控件；进度弹窗完整复用 gateway-tool 的 `FW_ShowProgress`/`FW_UpdateProgress`/`FW_Done` 机制。

**Tech Stack:** Win32 API + CMake（纯 C11），MinGW 交叉编译 或 MSVC 原生编译，PCANBasic.dll 运行时动态加载。

## Global Constraints

- 项目根：`C:\Users\jxwaz\code\handler-receiver`
- 参考项目：`C:\Users\jxwaz\code\gateway-tool`（源码直接复制，**不得修改**复制的文件内容）
- 编译选项：MinGW 用 `-Os -finput-charset=UTF-8 -fexec-charset=GBK` + `-mwindows -static-libgcc -s`；MSVC 用 `/utf-8` + `/SUBSYSTEM:WINDOWS`
- 链接库：`comctl32 comdlg32 gdi32 ws2_32`
- 定义宏：`UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS`
- 代码规范：注释中文，LOG 字符串英文；中文 UI 用 `SetWindowTextW(L"...")`，ASCII 用 `SetWindowTextA`
- CAN 波特率**固定 250K**（`PCAN_BAUD_250K`），不提供选择
- 固件升级 test_mode **固定 0**（永久升级），不提供测试模式选项
- UDP 配置端口**固定 9200**，本机 bind 9201（与 gateway-tool 镜像约定一致）
- 不做数据通道 UdpManager（无扫描仪数据收发）
- 所有 git 提交前确保编译通过

**测试说明：** 本项目为 Win32 GUI 工具，依赖 PCAN 硬件和 UDP 设备，**无自动化单元测试**。验证方式为：每个任务结束确保 `cmake --build` 编译通过（零警告目标）；最终任务做手动运行验证清单。因此不采用 TDD 红绿循环，改用**编译验证 + 代码自审**作为每步的门控。

---

## File Structure

| 文件 | 来源 | 职责 |
|------|------|------|
| `CMakeLists.txt` | 新写 | 构建配置（仿 gateway-tool） |
| `CMakePresets.json` | 新写（仿 gateway-tool） | MinGW/MSVC preset |
| `include/can_manager.h` | 复制自 gateway-tool | CAN 管理器接口 |
| `include/udp_manager.h` | 复制自 gateway-tool | UDP 管理器接口 |
| `include/pcan_loader.h` | 复制自 gateway-tool | PCANBasic.dll 加载器接口 |
| `include/resource.h` | 新写 | 图标 ID + 子对话框 ID + 控件 ID |
| `src/can_manager.c` | 复制自 gateway-tool | CAN 实现 |
| `src/udp_manager.c` | 复制自 gateway-tool | UDP 实现 |
| `src/pcan_loader.c` | 复制自 gateway-tool | DLL 加载实现 |
| `src/main.c` | 新写 | 主窗口 + Tab + 3 子对话框 + 业务逻辑 |
| `resources/resource.rc` | 新写（仿 gateway-tool，精简） | 图标 + 版本信息 |
| `resources/icon.ico` | 复制自 gateway-tool | 应用图标 |
| `.gitignore` | 已存在 | 忽略 build/out |

---

### Task 1: 复制底层模块与资源，建立可编译骨架

**Files:**
- Create: `include/can_manager.h`, `include/udp_manager.h`, `include/pcan_loader.h`
- Create: `src/can_manager.c`, `src/udp_manager.c`, `src/pcan_loader.c`
- Create: `resources/icon.ico`, `resources/resource.rc`
- Create: `include/resource.h`（仅图标 + 占位对话框 ID）

**Interfaces:**
- Produces: 三个底层模块的完整 API（见 gateway-tool 头文件），供 main.c 调用

- [ ] **Step 1: 复制底层模块源码（不改一字）**

从 gateway-tool 原样复制以下 6 个文件到 handler-receiver 对应路径：
```bash
cp ~/code/gateway-tool/include/can_manager.h  ~/code/handler-receiver/include/
cp ~/code/gateway-tool/include/udp_manager.h  ~/code/handler-receiver/include/
cp ~/code/gateway-tool/include/pcan_loader.h  ~/code/handler-receiver/include/
cp ~/code/gateway-tool/src/can_manager.c      ~/code/handler-receiver/src/
cp ~/code/gateway-tool/src/udp_manager.c      ~/code/handler-receiver/src/
cp ~/code/gateway-tool/src/pcan_loader.c      ~/code/handler-receiver/src/
cp ~/code/gateway-tool/resources/icon.ico     ~/code/handler-receiver/resources/
```

验证复制完整：
```bash
wc -l ~/code/handler-receiver/src/can_manager.c ~/code/handler-receiver/src/udp_manager.c ~/code/handler-receiver/src/pcan_loader.c
```
Expected: `398 .../can_manager.c` / `731 .../udp_manager.c` / `65 .../pcan_loader.c`

- [ ] **Step 2: 写 include/resource.h（最小版，仅图标 + 三个子对话框模板 ID）**

文件 `include/resource.h`：
```c
#ifndef RESOURCE_H
#define RESOURCE_H

/* 图标 */
#define IDI_APP_ICON       101

/* 子对话框模板 (main.c 中 CreateDialog 用) */
#define IDD_TAB_BIND           210
#define IDD_TAB_HANDLER_FW     211
#define IDD_TAB_TRANSMITTER_FW 212

#endif /* RESOURCE_H */
```

注：控件 ID 不在 resource.h 定义，因为控件是运行时动态创建的，控件 ID 直接在 main.c 用 `#define` 局部定义（避免与 resource.rc 模板耦合）。

- [ ] **Step 3: 写 resources/resource.rc（图标 + 版本信息，仿 gateway-tool 精简）**

文件 `resources/resource.rc`：
```
#include <windows.h>
#include "resource.h"

#pragma code_page(65001)

IDI_APP_ICON ICON "icon.ico"

VS_VERSION_INFO VERSIONINFO
FILEVERSION 1,0,0,0
PRODUCTVERSION 1,0,0,0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "Kabirz"
            VALUE "FileDescription", "手柄-接收器工具"
            VALUE "FileVersion", "1.0.0.0"
            VALUE "InternalName", "handler-receiver"
            VALUE "OriginalFilename", "handler-receiver.exe"
            VALUE "ProductName", "手柄-接收器工具"
            VALUE "ProductVersion", "1.0.0.0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0804, 936
    END
END
```

注意：三个 `IDD_TAB_*` 模板此处**不定义 DIALOGEX 块**——子对话框用内存模板 (`DialogBoxIndirectParam` 风格) 或直接 `CreateWindowEx` 自建。为简化，main.c 用 `CreateDialogIndirectParamW` + 最小空模板创建子对话框，故 rc 只需保留 ID 数值不重复。实际上更简单：main.c 不依赖这些模板，直接用 `CreateWindowExW(WC_DIALOG,...)` 创建子对话框窗口。**故 resource.h 中的 IDD_TAB_* 实际不被 rc 引用，仅占数值，可保留也可删——本计划保留以备扩展。**

- [ ] **Step 4: 写最小 CMakeLists.txt（先验证底层模块能编译）**

文件 `CMakeLists.txt`：
```cmake
cmake_minimum_required(VERSION 3.25)
project(HandlerReceiver VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${CMAKE_SOURCE_DIR}/include)

set(SOURCES
    src/main.c
    src/can_manager.c
    src/udp_manager.c
    src/pcan_loader.c
)

if(WIN32)
    list(APPEND SOURCES resources/resource.rc)
endif()

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    comctl32
    comdlg32
    gdi32
    ws2_32
)

if(MINGW)
    target_compile_options(${PROJECT_NAME} PRIVATE -Os -finput-charset=UTF-8 -fexec-charset=GBK)
    target_link_options(${PROJECT_NAME} PRIVATE -mwindows -static-libgcc -s)
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    APP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    APP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
```

- [ ] **Step 5: 写 CMakePresets.json（仿 gateway-tool）**

文件 `CMakePresets.json`：
```json
{
    "version": 7,
    "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
    "configurePresets": [
        {
            "name": "default",
            "displayName": "MinGW (Linux Cross-Compile)",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build",
            "cacheVariables": {
                "CMAKE_SYSTEM_NAME": "Windows",
                "CMAKE_C_COMPILER": "x86_64-w64-mingw32-gcc",
                "CMAKE_RC_COMPILER": "x86_64-w64-mingw32-windres"
            }
        },
        {
            "name": "vs",
            "displayName": "Visual Studio (Windows Native)",
            "binaryDir": "${sourceDir}/out"
        }
    ],
    "buildPresets": [
        { "name": "Release",    "configurePreset": "default", "configuration": "Release" },
        { "name": "vs-release", "configurePreset": "vs",      "configuration": "Release" }
    ]
}
```

- [ ] **Step 6: 写最小 main.c 占位（仅 WinMain + DialogBox 空壳，确保编译通过）**

文件 `src/main.c`：
```c
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine;
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icex);
    MessageBoxW(NULL, L"骨架", L"手柄-接收器工具", MB_OK);
    return 0;
}
```

- [ ] **Step 7: 配置并编译（MSVC，本机 Windows）**

```bash
cd ~/code/handler-receiver
cmake --preset vs
cmake --build out --config Release
```
Expected: 编译成功，生成 `out/bin/HandlerReceiver.exe`（运行会弹"骨架"对话框）。

- [ ] **Step 8: 提交**

```bash
cd ~/code/handler-receiver
git add -A
git commit -m "feat: 复用 gateway-tool 底层模块, 建立可编译骨架"
```

---

### Task 2: 主窗口 + Tab 控件 + 三个空子对话框

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Produces: `g_hMain`（主窗口）、`g_hTab`（tab 控件）、`g_hTabDlg[3]`（三个子对话框句柄）、`MainDlgProc`、`TabChildDlgProc`

- [ ] **Step 1: 定义全局状态与控件 ID（main.c 顶部）**

在 main.c `#pragma comment` 之后追加：
```c
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
```

- [ ] **Step 2: 写 TabChildDlgProc（通用子对话框过程，处理控件命令转发）**

在 main.c 中追加：
```c
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
```

- [ ] **Step 3: 写注册子对话框窗口类的辅助函数**

```c
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
```

- [ ] **Step 4: 写主窗口过程（含 Tab 控件创建、页签切换、子对话框显示/隐藏）**

```c
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
```

- [ ] **Step 5: 写主窗口过程骨架（WM_CREATE / WM_NOTIFY tab 切换 / WM_CLOSE）**

```c
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
```

- [ ] **Step 6: 改写 WinMain（注册主窗口类 + 消息循环）**

替换 Task 1 的占位 WinMain：
```c
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
```

- [ ] **Step 7: 编译验证**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功。运行 exe 应显示主窗口含三个 tab 标题，切换 tab 显示空白子对话框区。

- [ ] **Step 8: 提交**

```bash
git add -A
git commit -m "feat: 主窗口 + Tab 控件 + 三个空子对话框"
```

---

### Task 3: Tab1 设备绑定页控件创建与命令分发

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Produces: `CreateBindTabControls(HWND)`、`OnTabCommand(HWND hChildDlg, int cmdId)`（命令分发骨架）、`UpdateBindStatus(HWND, const char*)`

- [ ] **Step 1: 写 Tab1 设备绑定页控件创建函数**

在 main.c 追加（在 CreateTabLayout 之前定义，CreateTabLayout 中调用）：
```c
/* 创建各 tab 子对话框控件 (在 CreateTabLayout 后调用) */
static void CreateBindTabControls(HWND hDlg)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 10;
    /* 4 个按钮, 纵向排列, 宽 200, 高 28 */
    struct { const wchar_t *text; int id; } btns[] = {
        { L"手柄设备扫描并连接",  IDC_BTN_SCAN_HANDLER  },
        { L"接收器设备扫描并连接", IDC_BTN_SCAN_RECEIVER },
        { L"检测绑定状态",        IDC_BTN_CHECK_BIND    },
        { L"绑定设备",            IDC_BTN_BIND          },
    };
    for (int i = 0; i < 4; i++) {
        HWND hBtn = CreateWindowExW(0, L"BUTTON", btns[i].text,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, y, 200, 28, hDlg, (HMENU)(INT_PTR)btns[i].id, g_hInst, NULL);
        SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 34;
    }
    /* 状态显示区: 多行只读 EDIT */
    HWND hStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
            WS_VSCROLL | ES_READONLY,
            20, y + 4, 420, 200, hDlg, (HMENU)IDC_STATUS_TEXT, g_hInst, NULL);
    SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
}

static void CreateHandlerFwTabControls(HWND hDlg);
static void CreateTransmitterFwTabControls(HWND hDlg);
```

- [ ] **Step 2: 写 Tab2/Tab3 升级页控件创建（占位，下一任务细化）**

```c
/* 通用: 创建 升级 tab 的 [路径框 + 浏览 + 升级] 三件套 */
static void CreateFwTabControls(HWND hDlg, int file_id, int browse_id, int upgrade_id)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    CreateWindowExW(0, L"STATIC", L"固件文件:",
            WS_CHILD | WS_VISIBLE, 20, 20, 60, 16,
            hDlg, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            80, 18, 260, 22, hDlg, (HMENU)(INT_PTR)file_id, g_hInst, NULL);
    HWND hBrowse = CreateWindowExW(0, L"BUTTON", L"浏览...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            350, 18, 60, 22, hDlg, (HMENU)(INT_PTR)browse_id, g_hInst, NULL);
    HWND hUpg = CreateWindowExW(0, L"BUTTON", L"升级",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            180, 60, 80, 28, hDlg, (HMENU)(INT_PTR)upgrade_id, g_hInst, NULL);
    SendMessageW(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hUpg, WM_SETFONT, (WPARAM)hFont, TRUE);
}

static void CreateHandlerFwTabControls(HWND hDlg)
{
    CreateFwTabControls(hDlg, IDC_HFW_FILE, IDC_HFW_BROWSE, IDC_HFW_UPGRADE);
}

static void CreateTransmitterFwTabControls(HWND hDlg)
{
    CreateFwTabControls(hDlg, IDC_TFW_FILE, IDC_TFW_BROWSE, IDC_TFW_UPGRADE);
}
```

- [ ] **Step 3: 在 CreateTabLayout 中调用三个 Create* 函数**

修改 `CreateTabLayout`，在创建 `g_hTabDlg[i]` 的 for 循环之后追加：
```c
    /* 创建各 tab 控件 */
    CreateBindTabControls(g_hTabDlg[0]);
    CreateHandlerFwTabControls(g_hTabDlg[1]);
    CreateTransmitterFwTabControls(g_hTabDlg[2]);
```

- [ ] **Step 4: 写状态区更新辅助函数**

```c
/* 追加文本到 Tab1 状态区 */
static void UpdateBindStatus(HWND hChildDlg, const char *msg)
{
    HWND hStatus = GetDlgItem(hChildDlg, IDC_STATUS_TEXT);
    if (!hStatus) return;
    /* UTF-8 → UTF-16 写入 (源码 -fexec-charset=GBK, msg 是 GBK; 直接用 W 转换更稳) */
    int wlen = MultiByteToWideChar(CP_ACP, 0, msg, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *wbuf = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, msg, -1, wbuf, wlen);
    int textLen = GetWindowTextLengthW(hStatus);
    SendMessageW(hStatus, EM_SETSEL, textLen, textLen);
    SendMessageW(hStatus, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
    /* 补换行 */
    SendMessageW(hStatus, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(hStatus, EM_SCROLLCARET, 0, 0);
    free(wbuf);
}
```

- [ ] **Step 5: 写命令分发骨架（OnTabCommand，业务逻辑下个任务填）**

```c
/* 主窗口收到子对话框转发的命令: wParam=子对话框句柄, lParam=原始 WM_COMMAND lParam */
static void OnTabCommand(HWND hChildDlg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    int cmdId = LOWORD(wParam);
    int tabIdx = (int)GetWindowLongPtrW(hChildDlg, GWLP_USERDATA);

    if (tabIdx == 0) {
        /* 设备绑定页 */
        switch (cmdId) {
        case IDC_BTN_SCAN_HANDLER:   /* Task 4 实现 */ break;
        case IDC_BTN_SCAN_RECEIVER:  /* Task 4 实现 */ break;
        case IDC_BTN_CHECK_BIND:     /* Task 4 实现 */ break;
        case IDC_BTN_BIND:           /* Task 4 实现 */ break;
        }
    } else if (tabIdx == 1) {
        /* 手柄固件升级页 */
        switch (cmdId) {
        case IDC_HFW_BROWSE:         /* Task 6 实现 */ break;
        case IDC_HFW_UPGRADE:        /* Task 6 实现 */ break;
        }
    } else if (tabIdx == 2) {
        /* 接收器固件升级页 */
        switch (cmdId) {
        case IDC_TFW_BROWSE:         /* Task 7 实现 */ break;
        case IDC_TFW_UPGRADE:        /* Task 7 实现 */ break;
        }
    }
}
```

- [ ] **Step 6: 在 MainWndProc 的 WM_APP+100 分支调用 OnTabCommand**

修改 MainWndProc：
```c
    case WM_APP + 100: {
        HWND hChildDlg = (HWND)wParam;
        OnTabCommand(hChildDlg, lParam);   /* lParam 是子对话框 WM_COMMAND 的原 lParam */
        return 0;
    }
```

注意：子对话框 `TabChildDlgProc` 里 `PostMessageW(g_hMain, WM_APP+100, (WPARAM)hDlg, lParam)`，其中 lParam 是子对话框收到的 `WM_COMMAND` 的 lParam（含控件 ID 在 LOWORD）。但 PostMessage 后我们丢了 wParam 里的控件 ID。**修正**：应在转发时把控件 ID 编码进去。改为：
```c
/* TabChildDlgProc WM_COMMAND 分支:
 * 用 wParam(子对话框句柄) 的低 16 位放 tab index 不够, 改用:
 *   PostMessageW(g_hMain, WM_APP+100, (WPARAM)hDlg, lParam);
 * 其中 lParam 的 LOWORD 就是控件 ID (原 WM_COMMAND 的 wParam 低字). */
```
实际上 Win32 的 `WM_COMMAND`：控件 ID 在 **wParam 的 LOWORD**，lParam 是控件句柄。所以子对话框 `WM_COMMAND` 进来时 `LOWORD(wParam)` = 控件 ID。我们转发时要保留这个信息：

修正 `TabChildDlgProc`：
```c
    case WM_COMMAND:
        /* wParam 低字 = 控件 ID, lParam = 控件句柄. 把控件 ID 放到转发消息的 wParam 高字,
         * 子对话框句柄放 lParam, 主窗口用 GetWindowLongPtr(GWLP_USERDATA) 取 tab index */
        PostMessageW(g_hMain, WM_APP + 100, wParam, (LPARAM)hDlg);
        return 0;
```
对应修正 `MainWndProc` 和 `OnTabCommand` 签名：
```c
    case WM_APP + 100: {
        OnTabCommand((HWND)lParam, wParam);   /* wParam 含控件 ID */
        return 0;
    }
```
```c
static void OnTabCommand(HWND hChildDlg, WPARAM wParam)
{
    int cmdId = LOWORD(wParam);
    ...
}
```

- [ ] **Step 7: 编译验证**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功。运行 exe，Tab1 显示 4 个按钮 + 状态区，Tab2/Tab3 显示路径+浏览+升级。点击按钮无反应（业务未实现）。

- [ ] **Step 8: 提交**

```bash
git add -A
git commit -m "feat: Tab1/2/3 控件创建 + 命令分发骨架"
```

---

### Task 4: Tab1 设备绑定业务逻辑（扫描/连接/NRF读取/绑定）

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Consumes: `CanManager_DetectDevice`/`Connect`/`StartRxThread`/`Send`/`Create`/`SetFrameCallback`/`SetMsgCallback`；`UdpManager_Create`/`Bind`/`StartRxThread`/`GetRF24`/`SetRF24`/`SetMsgCallback`
- Produces: `OnScanHandler`/`OnScanReceiver`/`OnCheckBind`/`OnBind`，CAN frame_cb 填 `g_handlerAddr`

- [ ] **Step 1: 写 CAN frame 回调（处理 0x105 响应填手柄 NRF）**

```c
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
    (void)user_data;
    /* CAN 状态消息转发到 Tab1 状态区 */
    HWND hChild = g_hTabDlg[0];
    if (hChild) {
        char buf[256];
        sprintf(buf, "[CAN] %s", msg ? msg : "");
        PostMessageA(g_hMain, WM_UPDATE_STATUS, 0, (LPARAM)_strdup(buf));
    }
}
```

- [ ] **Step 2: 写 UDP msg 回调**

```c
static void udp_msg_cb(const char *msg, void *user_data)
{
    (void)user_data;
    HWND hChild = g_hTabDlg[0];
    if (hChild) {
        char buf[256];
        sprintf(buf, "[UDP] %s", msg ? msg : "");
        PostMessageA(g_hMain, WM_UPDATE_STATUS, 0, (LPARAM)_strdup(buf));
    }
}
```

- [ ] **Step 3: 在 WinMain 创建 CAN/UDP 管理器并注册回调**

在 WinMain 的 `g_hInst = hInstance;` 之后追加：
```c
    /* 创建 CAN/UDP 管理器 */
    g_can = CanManager_Create();
    g_cfgUdp = UdpManager_Create();
    CanManager_SetMsgCallback(g_can, can_msg_cb, NULL);
    CanManager_SetFrameCallback(g_can, can_frame_cb, NULL);
    UdpManager_SetMsgCallback(g_cfgUdp, udp_msg_cb, NULL);
```

并在 WinMain 消息循环结束（return 之前）销毁：
```c
    /* 消息循环退出后销毁 */
    CanManager_Destroy(g_can);
    UdpManager_Destroy(g_cfgUdp);
```

（注意把 `return (int)m.wParam;` 改为先销毁再 return）

- [ ] **Step 4: 写 OnScanHandler（手柄设备扫描并连接）**

```c
/* 手柄设备扫描并连接: 扫描 PCAN 设备, 0 个弹窗提示, 占用弹窗提示, 成功则 250K 连接 */
static void OnScanHandler(HWND hChildDlg)
{
    if (g_canConnected) {
        UpdateBindStatus(hChildDlg, "手柄已连接, 如需重连请先断开");
        return;
    }
    char devices[16][256];
    int count = CanManager_DetectDevice(g_can, devices, 16);
    if (count == 0) {
        MessageBoxW(g_hMain, L"未扫描到设备，请连接设备", L"提示", MB_OK | MB_ICONWARNING);
        UpdateBindStatus(hChildDlg, "未扫描到 PCAN 设备");
        return;
    }
    /* 取第一个设备 */
    int channel = 0;
    sscanf(devices[0], "PCAN_USB_%d (0x%X)", &channel, &channel);

    if (!CanManager_Connect(g_can, channel, PCAN_BAUD_250K)) {
        MessageBoxW(g_hMain, L"设备被占用，请查看并释放", L"连接失败",
                    MB_OK | MB_ICONERROR);
        UpdateBindStatus(hChildDlg, "CAN 连接失败 (设备可能被占用)");
        return;
    }
    CanManager_StartRxThread(g_can);
    g_canConnected = 1;
    char buf[128];
    sprintf(buf, "手柄已连接: %s (250Kbps)", devices[0]);
    UpdateBindStatus(hChildDlg, buf);
}
```

- [ ] **Step 5: 写 OnScanReceiver（接收器设备扫描并连接）**

```c
/* 接收器设备扫描并连接: 通过 255.255.255.255 连 UDP 配置端口 9200 */
static void OnScanReceiver(HWND hChildDlg)
{
    if (g_udpConnected) {
        UpdateBindStatus(hChildDlg, "接收器已连接, 如需重连请先断开");
        return;
    }
    /* 本地 9201 (固件监听 9200, 上位机本地 9201 收广播), 远程 9200, 显式有限广播 */
    if (!UdpManager_Bind(g_cfgUdp, UDP_CHAN_CONFIG, 9201, "255.255.255.255", 9200)) {
        int err = WSAGetLastError();
        wchar_t wmsg[160];
        swprintf(wmsg, 160,
            L"接收器连接失败\n本地端口 9201 可能被占用 (WSA 错误码: %d)\n请关闭占用该端口的程序后重试",
            err);
        MessageBoxW(g_hMain, wmsg, L"连接失败", MB_OK | MB_ICONERROR);
        UpdateBindStatus(hChildDlg, "UDP 配置通道 bind 失败 (端口 9201 可能被占用)");
        return;
    }
    UdpManager_StartRxThread(g_cfgUdp);
    g_udpConnected = 1;
    UpdateBindStatus(hChildDlg, "接收器 UDP 已连接 (广播 255.255.255.255:9200)");
}
```

- [ ] **Step 6: 写读取手柄 NRF 的辅助函数（发 0x104 GET_CONFIG 并等响应）**

```c
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
```

- [ ] **Step 7: 写 OnCheckBind（检测绑定状态）**

```c
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
        UpdateBindStatus(hChildDlg, "读取手柄 NRF 超时");
        return;
    }
    /* 2. 读接收器 NRF */
    if (!UdpManager_GetRF24(g_cfgUdp, &g_receiverCh, g_receiverAddr)) {
        MessageBoxW(g_hMain,
            L"读取接收器 NRF 地址超时\n请确认接收器已上电并在同一网络",
            L"错误", MB_OK | MB_ICONERROR);
        UpdateBindStatus(hChildDlg, "读取接收器 NRF 超时");
        return;
    }
    /* 3. 比对 */
    BOOL all_zero = (g_receiverAddr[0]|g_receiverAddr[1]|g_receiverAddr[2]|
                     g_receiverAddr[3]|g_receiverAddr[4]) == 0;
    BOOL same = (memcmp(g_receiverAddr, g_handlerAddr, 5) == 0);

    char h_str[16], r_str[16];
    sprintf(h_str, "%02x%02x%02x%02x%02x", g_handlerAddr[0], g_handlerAddr[1],
            g_handlerAddr[2], g_handlerAddr[3], g_handlerAddr[4]);
    sprintf(r_str, "%02x%02x%02x%02x%02x", g_receiverAddr[0], g_receiverAddr[1],
            g_receiverAddr[2], g_receiverAddr[3], g_receiverAddr[4]);
    char buf[160];
    sprintf(buf, "手柄NRF=%s(ch%d) 接收器NRF=%s(ch%d)", h_str, g_handlerCh, r_str, g_receiverCh);
    UpdateBindStatus(hChildDlg, buf);

    if (all_zero) {
        MessageBoxW(g_hMain, L"设备未绑定\n接收器 NRF 地址为空", L"绑定状态",
                    MB_OK | MB_ICONINFORMATION);
    } else if (!same) {
        MessageBoxW(g_hMain, L"接收器已绑定其他设备", L"绑定状态",
                    MB_OK | MB_ICONWARNING);
    } else {
        MessageBoxW(g_hMain, L"已绑定本设备", L"绑定状态",
                    MB_OK | MB_ICONINFORMATION);
    }
}
```

- [ ] **Step 8: 写 OnBind（绑定设备）**

```c
/* 绑定设备: 把手柄 NRF 地址写入接收器 */
static void OnBind(HWND hChildDlg)
{
    if (!g_canConnected || !g_udpConnected) {
        MessageBoxW(g_hMain, L"请先连接手柄和接收器", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    /* 确保已有手柄 NRF (没读过则先读) */
    if (!g_handlerAddrGot) {
        if (!ReadHandlerNrf()) {
            MessageBoxW(g_hMain, L"读取手柄 NRF 地址超时", L"错误",
                        MB_OK | MB_ICONERROR);
            UpdateBindStatus(hChildDlg, "绑定失败: 读取手柄 NRF 超时");
            return;
        }
    }
    if (MessageBoxW(g_hMain, L"确认把手柄 NRF 地址写入接收器?",
                    L"确认绑定", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (UdpManager_SetRF24(g_cfgUdp, g_handlerCh, g_handlerAddr)) {
        char buf[128];
        char h_str[16];
        sprintf(h_str, "%02x%02x%02x%02x%02x", g_handlerAddr[0], g_handlerAddr[1],
                g_handlerAddr[2], g_handlerAddr[3], g_handlerAddr[4]);
        sprintf(buf, "绑定成功, 已写入地址 %s (ch%d)", h_str, g_handlerCh);
        UpdateBindStatus(hChildDlg, buf);
        MessageBoxW(g_hMain, L"绑定成功", L"成功", MB_OK | MB_ICONINFORMATION);
    } else {
        UpdateBindStatus(hChildDlg, "绑定失败: 发送 SetRF24 命令失败");
        MessageBoxW(g_hMain, L"绑定失败\n发送 SetRF24 命令失败", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}
```

- [ ] **Step 9: 在 OnTabCommand 中接入 4 个业务函数**

修改 OnTabCommand 的 tabIdx==0 分支：
```c
    if (tabIdx == 0) {
        switch (cmdId) {
        case IDC_BTN_SCAN_HANDLER:  OnScanHandler(hChildDlg);  break;
        case IDC_BTN_SCAN_RECEIVER: OnScanReceiver(hChildDlg); break;
        case IDC_BTN_CHECK_BIND:    OnCheckBind(hChildDlg);    break;
        case IDC_BTN_BIND:          OnBind(hChildDlg);         break;
        }
    }
```

- [ ] **Step 10: 在 MainWndProc 处理 WM_UPDATE_STATUS**

```c
    case WM_UPDATE_STATUS: {
        char *text = (char *)lParam;
        if (text) {
            UpdateBindStatus(g_hTabDlg[0], text);
            free(text);
        }
        return 0;
    }
```

- [ ] **Step 11: 编译验证**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功（无设备时可运行，点按钮会弹相应提示框）。

- [ ] **Step 12: 提交**

```bash
git add -A
git commit -m "feat: Tab1 设备绑定业务 (扫描/连接/NRF读取/绑定)"
```

---

### Task 5: 进度弹窗机制（复用 gateway-tool）

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Produces: `FW_ShowProgress(HWND)`、`FW_UpdateProgress(int, const wchar_t*)`、`FW_Done(HWND, BOOL)`、全局 `g_progressIsCan`

- [ ] **Step 1: 定义进度弹窗全局变量与控件 ID**

在 main.c 全局状态区追加：
```c
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
```

- [ ] **Step 2: 写 ProgressWndProc（直接搬自 gateway-tool，逻辑一致）**

```c
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
```

- [ ] **Step 3: 写 FW_ShowProgress（直接搬自 gateway-tool）**

```c
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
```

- [ ] **Step 4: 在 MainWndProc 处理 WM_FW_SHOW_PROGRESS / WM_UPDATE_PROGRESS**

```c
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
```

- [ ] **Step 5: 编译验证**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功（进度弹窗函数已就绪，但暂无调用入口，下个任务接入）。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: 复用 gateway-tool 固件升级进度弹窗机制"
```

---

### Task 6: Tab2 手柄 CAN 固件升级

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Consumes: `CanManager_FirmwareUpgrade`、`FW_ShowProgress`/`FW_Done`
- Produces: `fw_upgrade_thread`（CAN 分支）、浏览/升级命令处理

- [ ] **Step 1: 写固件升级线程参数结构与 CAN/UDP 共用线程**

```c
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
        /* UDP 升级分支在 Task 7 填 */
        result = false;
    }

    PostMessageA(hMain, WM_FW_COMPLETE, p->isCan ? 1 : 0, result ? 1 : 0);
    free(p);
    return 0;
}
```

- [ ] **Step 2: 写 WM_FW_COMPLETE 处理（恢复按钮 + FW_Done）**

在 MainWndProc 追加：
```c
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
```

- [ ] **Step 3: 写浏览按钮处理（手柄固件）**

在 OnTabCommand 的 tabIdx==1 分支填：
```c
    } else if (tabIdx == 1) {
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
            /* Step 4 实现 */
            break;
        }
    }
```

- [ ] **Step 4: 写升级按钮处理（手柄固件）**

在 OnTabCommand 的 `case IDC_HFW_UPGRADE:` 填：
```c
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
```

- [ ] **Step 5: 编译验证**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功。运行 → Tab2 选文件后升级按钮启用（需 CAN 已连接）。

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "feat: Tab2 手柄 CAN 固件升级"
```

---

### Task 7: Tab3 接收器 UDP 固件升级

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Consumes: `UdpManager_FirmwareStart/Data/End`、`UdpManager_CRC16_CCITT`

- [ ] **Step 1: 在 fw_upgrade_thread 填 UDP 分支**

修改 `fw_upgrade_thread` 的 `else` 分支（替换 Task 6 的占位 `result = false;`）：
```c
    } else {
        /* UDP 升级: START(size) → DATA(256B/包, offset 校验) → END(crc+testmode) */
        HANDLE hFile = CreateFileA(p->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            MessageBoxW(NULL, L"打开固件文件失败", L"错误", MB_OK | MB_ICONERROR);
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
                        MessageBoxW(NULL, wmsg, L"升级失败", MB_OK | MB_ICONERROR);
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
                        MessageBoxW(NULL, L"烧写失败 (CRC 不匹配或错误)", L"升级失败",
                                    MB_OK | MB_ICONERROR);
                        result = false;
                    }
                }
            } else {
                MessageBoxW(NULL, L"开始烧写失败 (固件未响应 START)", L"升级失败",
                            MB_OK | MB_ICONERROR);
            }
            free(fileData);
        }
    }
```

- [ ] **Step 2: 在 OnTabCommand 填 tabIdx==2 分支（浏览 + 升级）**

```c
    } else if (tabIdx == 2) {
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
```

- [ ] **Step 3: 编译验证**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功。

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "feat: Tab3 接收器 UDP 固件升级"
```

---

### Task 8: 连接状态联动与最终打磨

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: 升级 tab 按钮启用状态联动**

在 OnScanHandler 成功连接后，启用 Tab2 升级按钮（若已选文件）：
```c
    /* OnScanHandler 末尾追加: 若手柄固件路径已选, 启用 Tab2 升级按钮 */
    if (strlen(g_handlerFwPath) > 0) {
        EnableWindow(GetDlgItem(g_hTabDlg[1], IDC_HFW_UPGRADE), TRUE);
    }
```
OnScanReceiver 成功后同理：
```c
    if (strlen(g_receiverFwPath) > 0) {
        EnableWindow(GetDlgItem(g_hTabDlg[2], IDC_TFW_UPGRADE), TRUE);
    }
```

- [ ] **Step 2: 主窗口 WM_CLOSE 清理**

在 MainWndProc 追加（在 WM_DESTROY 之前）：
```c
    case WM_CLOSE:
        /* 确保升级进行中时阻止关闭 */
        if (g_progressDlg && !g_progressDone) {
            MessageBoxW(hWnd, L"固件升级进行中, 请等待完成", L"提示", MB_OK | MB_ICONWARNING);
            return 0;
        }
        DestroyWindow(hWnd);
        return 0;
```

- [ ] **Step 3: 编译验证（最终）**

```bash
cd ~/code/handler-receiver
cmake --build out --config Release
```
Expected: 编译成功，零错误。

- [ ] **Step 4: 静态自审检查清单**

逐项核对：
- [ ] `src/can_manager.c` / `udp_manager.c` / `pcan_loader.c` 与 gateway-tool **逐字节相同**（`diff` 验证）
- [ ] main.c 无未使用变量警告
- [ ] 所有 MessageBoxW 中文文案正确
- [ ] Tab1 四个按钮逻辑覆盖需求
- [ ] Tab2/Tab3 升级按钮在未连接时禁用
- [ ] 进度弹窗升级中不可关闭

diff 验证命令：
```bash
diff ~/code/gateway-tool/src/can_manager.c ~/code/handler-receiver/src/can_manager.c
diff ~/code/gateway-tool/src/udp_manager.c ~/code/handler-receiver/src/udp_manager.c
diff ~/code/gateway-tool/src/pcan_loader.c ~/code/handler-receiver/src/pcan_loader.c
```
Expected: 无输出（完全相同）。

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "feat: 连接状态联动 + 关闭保护 + 最终打磨"
```

---

## 手动运行验证清单（需硬件，由用户在真机执行）

完成所有任务后，用户在装有 PCAN 驱动 + 接收器设备的 Windows 机器上验证：

1. **启动**：双击 `out/bin/HandlerReceiver.exe`，窗口显示三个 Tab。
2. **Tab1 - 手柄扫描**：点"手柄设备扫描并连接" → 连接成功状态区显示；未插 PCAN → 弹"未扫描到设备"；PCAN 被其他程序占用 → 弹"设备被占用"。
3. **Tab1 - 接收器扫描**：点"接收器设备扫描并连接" → 状态区显示已连接；端口被占用 → 弹具体错误码。
4. **Tab1 - 检测绑定**：点"检测绑定状态" → 接收器地址全 0 弹"未绑定"；与手柄不同弹"已绑定其他设备"；相同弹"已绑定本设备"。
5. **Tab1 - 绑定**：点"绑定设备" → 确认后接收器 NRF 写入手柄地址，弹"绑定成功"。
6. **Tab2 - 手柄升级**：选 `.bin` → 升级 → 进度弹窗滚动 → 完成弹"重启设备" → 点重启生效。
7. **Tab3 - 接收器升级**：选 `.bin` → 升级 → 进度弹窗滚动 → 完成弹"重启设备" → 点重启生效。

---

## Self-Review

**1. Spec 覆盖：**
- §6.1 CAN 协议 → Task 1（复制 can_manager）+ Task 4（OnScanHandler 用 250K + ReadHandlerNrf 用 0x104）✓
- §6.2 UDP 协议 → Task 1（复制 udp_manager）+ Task 4（OnScanReceiver 用 255.255.255.255:9200/9201）✓
- §7.1 主窗口 Tab → Task 2 ✓
- §7.2 Tab1 四按钮 → Task 3（控件）+ Task 4（业务）✓
- §7.3 Tab2 三控件 → Task 3（控件）+ Task 6（业务）✓
- §7.4 Tab3 三控件 → Task 3（控件）+ Task 7（业务）✓
- §8.1-8.4 绑定业务 → Task 4 ✓
- §8.5-8.6 升级业务 → Task 6/7 ✓
- §9 进度弹窗 → Task 5 ✓
- §12 错误处理 → 各 Task 内 MessageBoxW ✓

**2. Placeholder 扫描：** 无 TBD/TODO，所有代码块完整。✓

**3. Type 一致性：**
- `g_progressIsCan` 在 Task 5 定义、Task 6 使用 ✓
- `FwUpgradeParam` 在 Task 6 定义、Task 7 复用 ✓
- `OnTabCommand(HWND, WPARAM)` 签名 Task 3 定义、Task 4/6/7 调用一致 ✓
- 控件 ID（IDC_BTN_* / IDC_HFW_* / IDC_TFW_*）跨 Task 一致 ✓

无问题，计划完整。
