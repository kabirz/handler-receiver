# Handler Receiver

手柄-接收机配置工具,基于 Win32 原生 GUI(C 语言)。提供接收机网络配置、手柄绑定、固件升级、设备发现等功能,通过 PCAN-USB(CAN)与 UDP 与设备通信。

## 功能

应用包含四个标签页:

| Tab | 功能 | 说明 |
|-----|------|------|
| **Tab1** 接收机配置 | UDP | 接收机 UDP 连接 + 行1 接收机IP(`SET_IP`)/配置端口(`DISCOVER` 查询) + 行2 上行IP+上行端口(`SET_HOST`)/数据端口(`GET_NET` 查询) |
| **Tab2** 手柄绑定 | CAN + UDP | 手柄 CAN 扫描/连接,接收机 UDP 单播连接,NRF 读取比对并完成绑定 |
| **Tab3** 固件升级 | CAN + UDP | 上半 CAN 手柄固件升级 + 下半 UDP 接收机固件升级(支持 keyhash 校验) |
| **Tab4** 设备查找 | UDP 广播 | 广播 `DISCOVER(0x15)` 发现接收机 IP(解析回复 `[ip][config_port]`),可复制到剪贴板 |

## 通信协议要点

- **CAN 帧 ID**(详见 `include/can_manager.h`):平台收发 `0x101/0x102`,固件数据 `0x103`,keyhash `0x104`,版本字符串分帧 `0x105`,RF24 配置 `0x110/0x111` 等。
- **UDP 端口**:配置端口固定 **8600**,数据端口默认 **9600**(nRF24↔上位机透传,固件固定)。
- **UDP 命令帧**:`[cmd 1B][data...]`,无魔数头。命令码 `0x01–0x05` 为固件升级,`0x10+` 为业务命令(网络 / RF24 / 目标主机 / 发现)。多字节整数均为大端序(BE)。
- **设备发现**:`DISCOVER(0x15)` 是唯一允许跨子网广播的命令。先广播 `[0x15]`,设备回复 `[ip 4B][config_port 2B]`,据此单播后续命令。跨子网广播其他命令会被设备静默丢弃。
- **网络参数**:`SET_IP(0x10)` 仅发 4B IP(回复 1B 成功/失败),持久化重启生效。`GET_NET(0x11)` 返回 `[data_port 2B][host_ip 4B][host_port 2B]`(不含设备本机 IP,需用 `DISCOVER`)。
- **RF24**:`SET_RF24(0x12)/GET_RF24(0x13)` 仅 5B 地址,**信道固定 1**,不可配。
- **目标主机**:`SET_HOST(0x14)` 配置固件 nRF24 数据转发目标 `host_ip:host_port`(默认 `192.168.11.150:9602`,持久化)。查询复用 `GET_NET`(`GET_HOST` 已移除)。
- **版本字符串**:格式 `v<M>.<m>.<p>_<6hex>`(如 `v0.1.0_0b4ee3`)。UDP `GET_VERSION(0x04)` 单帧返回;CAN 经 `0x105` 多帧拼接(`0x102` offset=总长,后续 `[seq 1B][text 7B]` × N 帧)。
- **网络约定**:子网掩码固定 `255.255.255.0`,网关 = IP 末段改 1,由固件自算,上位机不传。
- **CRC16**:采用 CRC16-CCITT(poly `0x1021`,init `0x0000`),与固件对齐。

## 目录结构

```
.
├── CMakeLists.txt          # 构建脚本 (C11, Win32 GUI 子系统)
├── CMakePresets.json        # MinGW 交叉编译 / Visual Studio 预设
├── include/                 # 模块接口 (can/udp/fw/pcan)
├── src/                     # 模块实现 + Win32 UI (main.c)
├── resources/               # 图标 + Windows 资源文件
└── tools/build.bat          # MSVC 一键编译脚本
```

## 构建

### 方式一:Visual Studio(Windows 原生)

前置:已安装 Visual Studio(含 C++ 工具集)与 CMake ≥ 3.25。

```bat
tools\build.bat
:: 产物: out\bin\Release\HandlerReceiver.exe
```

或手动:

```bat
cmake --preset vs
cmake --build out --config Release
```

### 方式二:MinGW(Linux 交叉编译)

```bash
cmake --preset default
cmake --build build --config Release
```

> 编译时通过 `APP_VERSION_MAJOR/MINOR/PATCH` 宏注入版本号(来自 `CMakeLists.txt` 的 `project(... VERSION)`)。

## 依赖

- **PCAN-USB API**:CAN 通信依赖 PEAK PCAN-USB 驱动与 `PCANBasic.h`(由 `src/can_manager.c` 调用)。
- **Winsock2 / iphlpapi**:UDP 通信与本机网卡枚举,系统自带。

## 许可

内部工具,未指定开源许可。
