# 手柄-接收器绑定与固件升级工具 设计文档

日期: 2026-08-02
状态: 已批准

## 1. 目标

构建一个 Win32 纯 C 桌面工具，用于：

1. **设备绑定**：扫描并连接手柄 (PCAN/CAN) 与接收器 (UDP)，读取双方 NRF 地址并比对，把手柄 NRF 地址写入接收器完成绑定。
2. **手柄固件升级**：通过 CAN 完成手柄固件更新。
3. **接收器固件升级**：通过 UDP 完成接收器固件更新。

三块功能用 **Tab 页签** 组织，每页组件尽量精简，升级进度走弹窗。

## 2. 技术栈

- **Win32 API + CMake**，纯 C，无 MFC/Qt（与 gateway-tool 完全一致）。
- **Tab 实现**：`SysTabControl32`（CommCtrl）+ 3 个无模式子对话框 (`CreateDialog`)，切换时 `ShowWindow(SW_HIDE/SW_SHOW)`。
- **UI 控件**：运行时 `CreateWindowExW` 动态创建（不写 `.rc` 对话框模板，仅 `.rc` 放版本信息 + 图标）。
- **直接复用** gateway-tool 的三个底层模块源码：
  - `can_manager.c/.h`（CAN 通信、设备扫描、固件升级）
  - `udp_manager.c/.h`（UDP 配置/数据通道、RF24 配置、固件升级、CRC16）
  - `pcan_loader.c/.h`（PCANBasic.dll 动态加载）

## 3. 项目结构

```
handler-receiver/
├── CMakeLists.txt
├── include/
│   ├── can_manager.h      ← 从 gateway-tool 复制
│   ├── udp_manager.h      ← 从 gateway-tool 复制
│   ├── pcan_loader.h      ← 从 gateway-tool 复制
│   └── resource.h         ← 新写（图标 ID + 子对话框模板 ID）
├── src/
│   ├── main.c             ← 新写（主窗口 Tab + 3 子对话框 + 控件动态创建 + 业务逻辑）
│   ├── can_manager.c      ← 从 gateway-tool 复制
│   ├── udp_manager.c      ← 从 gateway-tool 复制
│   └── pcan_loader.c      ← 从 gateway-tool 复制
└── resources/
    ├── resource.rc        ← 新写（仅版本信息 + 图标引用）
    └── icon.ico           ← 从 gateway-tool 复制
```

## 4. 构建配置 (CMakeLists.txt)

与 gateway-tool 对齐：

- `project(HandlerReceiver VERSION 1.0.0 LANGUAGES C)`，`CMAKE_C_STANDARD 11`。
- 源文件：`src/main.c src/can_manager.c src/udp_manager.c src/pcan_loader.c`，WIN32 时追加 `resources/resource.rc`。
- `add_executable(${PROJECT_NAME} WIN32 ${SOURCES})`。
- 定义：`UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS`。
- 链接库：`comctl32 comdlg32 gdi32 ws2_32`。
- 编译选项：
  - MinGW：`-Os -finput-charset=UTF-8 -fexec-charset=GBK`，链接 `-mwindows -static-libgcc -s`。
  - MSVC：`/utf-8`，链接 `/SUBSYSTEM:WINDOWS`。
- 版本宏：`APP_VERSION_MAJOR/MINOR/PATCH`。

## 5. 全局状态

```c
static HINSTANCE g_hInst;
static HWND g_hMain;            /* 主窗口 (含 Tab 控件) */
static HWND g_hTab;             /* SysTabControl32 */
static HWND g_hTabDlg[3];       /* 3 个子对话框句柄 */

static CanManager *g_can;       /* 手柄 CAN */
static UdpManager *g_cfgUdp;    /* 接收器配置通道 (9200) */

static int  g_canConnected;     /* 手柄 CAN 连接状态 */
static int  g_udpConnected;     /* 接收器 UDP 连接状态 */

/* 手柄 NRF (CAN 0x105 响应填入) */
static uint8_t g_handlerCh;
static uint8_t g_handlerAddr[5];
static volatile BOOL g_handlerAddrGot;  /* RF24 响应到达标志 */

/* 接收器 NRF (UDP GET_RF24 同步获取) */
static uint8_t g_receiverCh;
static uint8_t g_receiverAddr[5];

static char g_handlerFwPath[MAX_PATH];
static char g_receiverFwPath[MAX_PATH];
```

注意：本工具**不使用数据通道 UdpManager**（不做扫描仪数据收发），只用一个配置通道实例。

## 6. 协议细节（复用 gateway-tool）

### 6.1 CAN（手柄）

- 帧定义（`can_manager.h`）：`CAN_ID_PLATFORM_RX=0x101 / TX=0x102 / FW_DATA_RX=0x103 / RF24_CONFIG_RESP=0x105`。
- RF24 命令帧（main.c 局部常量）：`CAN_ID_RF24_CONFIG_CMD=0x104`，`RF24_CMD_GET_CONFIG=0x02`。
- **设备扫描**：`CanManager_DetectDevice` 轮询 0~15 号 PCAN_USB 通道。
- **连接**：`CanManager_Connect(mgr, channel, PCAN_BAUD_250K)` —— **波特率固定 250K**，不提供选择。
- **NRF 读取**：发 `0x104 [0x02,0,0,0,0,0,0,0]`，RX 线程收到 `0x105 [0x02][ch][addr 5B][...]` 经 frame_cb 上抛，置 `g_handlerAddrGot=TRUE`。主线程等待该标志（超时 500ms）。
- **固件升级**：`CanManager_FirmwareUpgrade(mgr, path, 0, msg_cb, ..., progress_cb, ...)`。test_mode 固定 0（永久升级），不提供测试模式选项。

### 6.2 UDP（接收器）

- 配置端口固定 `GATEWAY_CONFIG_PORT=9200`，本机 bind 端口 9201（与 gateway-tool 镜像约定一致：固件监听 9200，上位机本地 9201）。
- **连接**：`UdpManager_Bind(g_cfgUdp, UDP_CHAN_CONFIG, 9201, "255.255.255.255", 9200)` —— 显式有限广播 `255.255.255.255`。
- **NRF 读取**：`UdpManager_GetRF24(g_cfgUdp, &ch, addr)`（内部发 0x15，同步等响应）。
- **NRF 设置（绑定）**：`UdpManager_SetRF24(g_cfgUdp, g_handlerCh, g_handlerAddr)`（发 0x14）。
- **固件升级**：`UdpManager_FirmwareStart(size)` → 循环 `UdpManager_FirmwareData(buf, 256, off, &got)` → `UdpManager_FirmwareEnd(0, crc)`，CRC 用 `UdpManager_CRC16_CCITT`。test_mode 固定 0。

## 7. UI 设计

### 7.1 主窗口

- `CreateWindowExW(WC_DIALOG ...)` 或注册一个简单窗口类，标题"手柄-接收器工具"。
- 内含一个 `SysTabControl32`（顶部），3 个 tab 项："设备绑定"、"手柄固件升级"、"接收器固件升级"。
- Tab 切换 (`TCN_SELCHANGE`)：隐藏当前子对话框，显示新选中的。
- 主窗口图标用 `resources/icon.ico`。

### 7.2 Tab 1：设备绑定

子对话框 `IDD_TAB_BIND`，纵向排列 4 个按钮 + 一个只读状态显示区 (多行 EDIT/STATIC)：

| 控件 | 类型 | 行为 |
|------|------|------|
| 手柄设备扫描并连接 | BUTTON | 见 §8.1 |
| 接收器设备扫描并连接 | BUTTON | 见 §8.2 |
| 检测绑定状态 | BUTTON | 见 §8.3 |
| 绑定设备 | BUTTON | 见 §8.4 |
| 状态显示 | EDIT (只读多行) | 显示连接状态、NRF 地址、绑定结果 |

按钮上方/下方各显示一行连接状态文字（手柄：已连接/未连接；接收器：已连接/未连接）。

### 7.3 Tab 2：手柄固件升级

子对话框 `IDD_TAB_HANDLER_FW`，组件精简：

| 控件 | 类型 |
|------|------|
| 固件路径 | EDIT (只读) |
| 浏览 | BUTTON |
| 升级 | BUTTON（仅 CAN 已连接且路径非空时启用）|

无日志区（日志走 MessageBox / 升级弹窗）。

### 7.4 Tab 3：接收器固件升级

子对话框 `IDD_TAB_TRANSMITTER_FW`，与 Tab 2 同构：

| 控件 | 类型 |
|------|------|
| 固件路径 | EDIT (只读) |
| 浏览 | BUTTON |
| 升级 | BUTTON（仅 UDP 已连接且路径非空时启用）|

## 8. 业务逻辑

### 8.1 手柄设备扫描并连接

```
count = CanManager_DetectDevice(g_can)
if count == 0:
    MessageBoxW("未扫描到设备，请连接设备", 警告)
    return
// 取第一个设备
解析 channel
if !CanManager_Connect(g_can, channel, PCAN_BAUD_250K):
    MessageBoxW("设备被占用，请查看并释放", 错误)   // Initialize 失败多为占用/驱动
    return
CanManager_StartRxThread(g_can)
g_canConnected = 1
状态显示: "手柄已连接"
```

`CanManager_Connect` 内部已设 RX 过滤器（0x102/0x105/0x1E3~0x763）。frame_cb 处理 0x105 响应填 `g_handlerAddr/g_handlerCh/g_handlerAddrGot`。

### 8.2 接收器设备扫描并连接

```
if !UdpManager_Bind(g_cfgUdp, UDP_CHAN_CONFIG, 9201, "255.255.255.255", 9200):
    // Bind 失败 = socket 创建失败 或 本地端口 9201 被占用
    sprintf 原因 (WSAGetLastError / "本地端口 9201 可能被占用")
    MessageBoxW(原因, 错误)
    return
UdpManager_StartRxThread(g_cfgUdp)
g_udpConnected = 1
状态显示: "接收器已连接 (广播 255.255.255.255:9200)"
```

> 注意：UDP 是无连接的，"连接成功"只代表 socket bind 成功。真正的设备可达性在"检测绑定状态"时通过 GET_RF24 是否有响应来验证。

### 8.3 检测绑定状态

要求：手柄已连接 + 接收器已连接。否则提示"请先连接手柄和接收器"。

```
// 1. 读手柄 NRF
g_handlerAddrGot = FALSE
CanManager_Send(g_can, 0x104, [0x02,0,0,0,0,0,0,0], 8)
等待 g_handlerAddrGot (500ms 超时, 轮询 Sleep(10))
if !got:
    MessageBoxW("读取手柄 NRF 地址超时", 错误)
    return

// 2. 读接收器 NRF
if !UdpManager_GetRF24(g_cfgUdp, &g_receiverCh, g_receiverAddr):
    MessageBoxW("读取接收器 NRF 地址超时（请确认接收器已上电并在同一网络）", 错误)
    return

// 3. 比对
全零 = (g_receiverAddr 5 字节全为 0)
相同 = memcmp(g_receiverAddr, g_handlerAddr, 5) == 0
if 全零:
    MessageBoxW("设备未绑定（接收器 NRF 地址为空）", 提示)
else if !相同:
    MessageBoxW("接收器已绑定其他设备", 警告)
else:
    MessageBoxW("已绑定本设备", 成功)
状态区显示两地址对比
```

### 8.4 绑定设备

要求：手柄已连接（有 `g_handlerAddr`）+ 接收器已连接。否则提示。

```
if g_handlerAddrGot == FALSE:   // 还没读过手柄地址
    先执行 8.3 的手柄 NRF 读取
    if 失败: return

if UdpManager_SetRF24(g_cfgUdp, g_handlerCh, g_handlerAddr):
    MessageBoxW("绑定成功", 成功)
else:
    MessageBoxW("绑定失败（发送命令失败）", 错误)
```

### 8.5 手柄固件升级 (Tab 2)

```
if !g_canConnected: MessageBoxW("请先连接手柄", 警告); return
if path 为空: return
禁用升级/浏览按钮
CreateThread(fw_thread, isCan=1, path)
```

`fw_thread`（与 gateway-tool 一致）：
- `PostMessage(WM_FW_SHOW_PROGRESS)` 弹进度窗
- `CanManager_FirmwareUpgrade(g_can, path, 0, msg_cb, progress_cb)` —— test_mode 固定 0
- 完成后 `PostMessage(WM_FW_COMPLETE, isCan=1, success)`
- 进度窗 `FW_Done` 显示结果 + 重启按钮（CAN 走 `CanManager_Reboot`）

### 8.6 接收器固件升级 (Tab 3)

```
if !g_udpConnected: MessageBoxW("请先连接接收器", 警告); return
if path 为空: return
禁用升级/浏览按钮
CreateThread(fw_thread, isCan=0, path)
```

`fw_thread` (UDP 分支)：
- 读文件 → 算 `UdpManager_CRC16_CCITT`
- `UdpManager_FirmwareStart(size)` → 循环 `UdpManager_FirmwareData(buf+off, 256, off+256, &got)` (进度 PostMessage) → `UdpManager_FirmwareEnd(0, crc)`
- `PostMessage(WM_FW_COMPLETE, isCan=0, success)`
- 进度窗 `FW_Done`，重启按钮走 `UdpManager_Reboot`

## 9. 进度弹窗（复用 gateway-tool）

完整复用 `FW_ShowProgress / FW_UpdateProgress / FW_Done` 三函数 + `ProgressWndProc`：
- 自定义窗口类 `ZCodeFwProgress`，300×150 客户区。
- 进度条 + 状态 label + 关闭/确定按钮 + 重启按钮。
- 升级中禁用关闭；完成后成功显示"重启设备"+"确定"，失败仅"确定"。
- `g_progressIsCan` 标志决定重启走 CAN 还是 UDP。
- 模态效果：`EnableWindow(hMain, FALSE/TRUE)`。

## 10. 线程模型

- **主线程**：UI 消息循环。
- **CAN RX 线程**：`CanManager_StartRxThread`，回调 `frame_cb` 处理 0x105 → 置标志。
- **UDP RX 线程**：`UdpManager_StartRxThread`，配置通道收响应唤醒同步等待 (`send_and_wait`)。
- **固件升级线程**：每次升级 `CreateThread`，完成后 PostMessage 回主线程。

跨线程通信全部经 `PostMessage`（与 gateway-tool 一致），回调里 strdup 字符串，主线程 free。

## 11. 编码规范（与 gateway-tool 一致）

- 代码注释中文，LOG 字符串英文。
- C 代码中文用 `SetWindowTextW(L"...")`，数字/ASCII 用 `SetWindowTextA`。
- MinGW 编译 `-fexec-charset=GBK`，源码 UTF-8 保存。
- `resource.rc` 用 `#pragma code_page(65001)`。

## 12. 错误处理

- 所有设备操作失败均 `MessageBoxW` 弹窗提示具体原因。
- 超时（读 NRF、固件 ACK）明确提示"超时"。
- 升级失败时进度弹窗 label 显示"升级失败"，并在主窗口弹一个 MessageBox 汇总关键诊断信息（失败阶段、offset、CRC 等，来自升级线程的 PostMessage 文本）。本工具无日志区，所有诊断走弹窗。

## 13. 不在范围内（YAGNI）

- 不做扫描仪数据收发（移除数据通道 UdpManager）。
- 不提供 CAN 波特率选择（固定 250K）。
- 不提供 test_mode（临时升级）选项，固定永久升级。
- 不做网络参数 (IP/端口/DHCP) 设置 UI（绑定工具不需要）。
- 不做版本查询/重启的独立按钮（重启仅在升级成功弹窗里）。
- 不做日志区（状态用 Tab1 的只读状态显示区 + 弹窗）。
