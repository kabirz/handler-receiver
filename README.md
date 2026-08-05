# Handler Receiver

手柄-接收器配置工具,基于 Win32 原生 GUI(C 语言)。提供手柄绑定、固件升级、接收器网络/RF24 参数配置等功能,通过 PCAN-USB(CAN)与 UDP 与设备通信。

## 功能

应用包含四个标签页:

| Tab | 功能 | 说明 |
|-----|------|------|
| **Tab1** 手柄绑定 | CAN + UDP | 手柄 CAN 扫描/连接,接收器 UDP 单播连接,NRF 读取比对并完成绑定 |
| **Tab2** 手柄升级 | CAN | 通过 CAN 对手柄进行固件升级(支持 keyhash 校验、临时/永久模式) |
| **Tab3** 接收端配置 | UDP | 接收器固件升级 + 网络参数设置/查询(`SET_NET`/`GET_NET`,含 MAC 守卫) + 目标主机配置(`SET_HOST`/`GET_HOST`) |
| **Tab4** 设备查找 | UDP 广播 | 广播发现接收器真实 IP,可复制到剪贴板 |

## 通信协议要点

- **CAN 帧端口**(详见 `include/can_manager.h`):平台收发 `0x101/0x102`,固件数据 `0x103`,keyhash `0x104`,RF24 配置 `0x110/0x111` 等。
- **UDP 端口**:配置端口固定 **8601**,数据端口默认 9090(可配,固件持久化)。
- **UDP 命令帧**:`[cmd 1B][data...]`,无魔数头。命令码 `0x01–0x05` 为固件升级,`0x12+` 为业务命令(网络 / RF24 / 网络模式 / 目标主机)。
- **SET_NET MAC 守卫**:`SET_NET(0x12)` 帧首 6B 为目标设备 MAC,固件单播/广播均校验匹配才执行。须先 `GET_NET(0x13)` 学习 MAC(响应含 MAC),设置时自动带上。
- **目标主机**:`SET_HOST(0x18)/GET_HOST(0x19)` 配置固件 nRF24 数据转发目标 `host_ip:host_port`(默认 `192.168.11.100:8602`,持久化)。
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
