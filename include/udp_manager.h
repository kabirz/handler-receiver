#ifndef UDP_MANAGER_H
#define UDP_MANAGER_H

/* winsock2 必须在 windows.h 之前 include, 否则 windows.h 会拉入 winsock1
 * 与 iphlpapi 的现代类型 (IP_ADAPTER_ADDRESSES 等) 冲突 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

/* UDP 配置命令格式: [cmd 1B][data...] (无魔数头, 走配置端口)
 * 与旧版不同: 不再有 0xAA 0x55 魔数头, 通道已分离 */
#define GATEWAY_DATA_PORT_DEFAULT   9090  /* 数据端口默认 (可配, 固件持久化) */
#define GATEWAY_CONFIG_PORT         8601  /* 配置端口 (固件固定, 不可改) */

/* UDP 命令码 (命令帧首字节, 走配置端口 8601).
 * 0x01-0x05: udp_fw_upgrade 库内命令 (FW_START/DATA/END/GET_VERSION/REBOOT)
 * 0x12+:     应用业务命令 (分网络/RF24/HOST 三组, 各含 set/get).
 *            掩码固定 255.255.255.0, 网关 = IP 末段改 1, 均不在帧中传输. */
enum udp_cmd {
	UDP_CMD_FW_START    = 0x01,
	UDP_CMD_FW_DATA     = 0x02,
	UDP_CMD_FW_END      = 0x03,
	UDP_CMD_GET_VERSION = 0x04,
	UDP_CMD_REBOOT      = 0x05,
	UDP_CMD_SET_NET     = 0x12,  /* [mac 6B][ip 4B][port 2B BE] = 12B → 回显同序 12B (MAC 守卫) */
	UDP_CMD_GET_NET     = 0x13,  /* (空) → [mac 6B][ip 4B][port 2B BE] = 12B (含本机 MAC) */
	UDP_CMD_SET_RF24    = 0x14,  /* [ch 1B][addr 5B] = 6B → 回显同序 6B */
	UDP_CMD_GET_RF24    = 0x15,  /* (空) → [ch 1B][addr 5B] = 6B */
	UDP_CMD_SET_NET_MODE = 0x16, /* [mode 1B] (0=静态,1=DHCP) → 回显 1B (持久化, 重启生效) */
	UDP_CMD_GET_NET_MODE = 0x17, /* (空) → [mode 1B] */
	UDP_CMD_SET_HOST    = 0x18,  /* [host ip 4B][port 2B BE] = 6B → 回显同序 6B (持久化) */
	UDP_CMD_GET_HOST    = 0x19,  /* (空) → [host ip 4B][port 2B BE] = 6B */
};

/* 通道类型: 一个 UdpManager 实例对应一个通道 (单 socket).
 * 配置通道收发命令 [cmd][data...], 数据通道收发数据帧 [frame_id 2B BE][payload].
 * 由调用方在 Bind 时指定, RX 线程据此决定走命令响应回调还是数据回调 */
typedef enum {
	UDP_CHAN_CONFIG,   /* 配置通道: 命令收发, msg_cb 上报响应, data_cb 不用 */
	UDP_CHAN_DATA,     /* 数据通道: 数据帧收发, data_cb 上报, msg_cb 不用 */
} UdpChannel;

/* 无线接收器配置 (GET_NET + GET_RF24 + GET_HOST 三组响应合并后的结果).
 *   GET_NET  (0x13): [mac 6B][ip 4B][port 2B BE] = 12B → mac/ip/data_port
 *   GET_RF24 (0x15): [ch 1B][addr 5B] = 6B → rf24_channel/rf24_addr
 *   GET_HOST (0x19): [host ip 4B][port 2B BE] = 6B → host_ip/host_port
 * 掩码固定 255.255.255.0; 网关 = IP 末段改 1 (上位机不传, 固件自算).
 * config_port 恒为 8601 (硬编码). */
typedef struct {
	uint8_t mac[6];             /* 设备 MAC (GET_NET 回复带, 供 SET_NET 广播守卫) */
	uint8_t rf24_channel;
	uint8_t rf24_addr[5];
	uint16_t data_port;         /* 固件数据端口 (固件 bind) */
	char ip[16];                /* 固件 IP (点分十进制) */
	char host_ip[16];           /* 上位机目标 IP (固件 nRF24 数据转发目标) */
	uint16_t host_port;         /* 上位机目标端口 (固件 nRF24 数据转发目标) */
} GatewayConfig;

/* 回调类型: 状态消息 / 透传数据 */
typedef void (*udp_msg_callback)(const char *msg, void *user_data);
typedef void (*udp_data_callback)(const uint8_t *data, size_t len, void *user_data);

/* UDP 管理器 (不透明指针, 单 socket, 单通道) */
typedef struct UdpManager UdpManager;

/* 生命周期 */
UdpManager *UdpManager_Create(void);
void UdpManager_Destroy(UdpManager *mgr);

/* 连接/断开: 创建 socket 绑定本机 0.0.0.0:local_port, 设广播 + REUSEADDR.
 * chan 决定本实例是配置还是数据通道 (仅影响 RX 分发逻辑).
 * 远程目标 = remote_ip:remote_port: remote_ip 为 NULL/空/0.0.0.0/非法 → 广播自动发现,
 * 否则单播到该 IP. (收到对端包后 RX 线程会自动学习并更新 remote_addr) */
bool UdpManager_Bind(UdpManager *mgr, UdpChannel chan,
                     uint16_t local_port, const char *remote_ip, uint16_t remote_port);
void UdpManager_Unbind(UdpManager *mgr);
bool UdpManager_IsBound(UdpManager *mgr);

/* 数据发送 (原始字节 / 协议命令帧).
 * SendData 发原始字节到 remote_addr (数据帧 [frame_id 2B BE][payload]).
 * SendCommand 发 [cmd][data...] (无魔数头). 两者走同一个 socket, 由调用方
 * 保证配置实例只发命令、数据实例只发数据. */
bool UdpManager_SendData(UdpManager *mgr, const uint8_t *data, size_t len);
bool UdpManager_SendCommand(UdpManager *mgr, uint8_t cmd, const uint8_t *data, uint8_t len);

/* 无线接收器配置 — 分网络/RF24/HOST 三组, 各含 set/get (走配置实例).
 * 掩码固定 255.255.255.0, 网关 = IP 末段改 1, 固件自算, 上位机不传.
 * SET_NET 帧首 6B 为目标设备 MAC (固件校验: 单播/广播均须匹配才执行),
 * 故调用前应先 GetNet 拿到设备 MAC. */
bool UdpManager_SetNet(UdpManager *mgr, const uint8_t *mac, const char *ip, uint16_t port);
bool UdpManager_GetNet(UdpManager *mgr, uint8_t *mac, char *ip, size_t ip_len, uint16_t *port);
bool UdpManager_SetRF24(UdpManager *mgr, uint8_t ch, const uint8_t *addr);
bool UdpManager_GetRF24(UdpManager *mgr, uint8_t *ch, uint8_t *addr);

/* 网络模式 (静态/DHCP) 切换 (走配置实例). mode: 0=静态, 1=DHCP.
 * 持久化, 重启生效. SetNetMode 发后建议配合 Reboot. */
bool UdpManager_SetNetMode(UdpManager *mgr, uint8_t mode);
bool UdpManager_GetNetMode(UdpManager *mgr, uint8_t *mode);

/* 上位机目标 (HOST) 配置 (走配置实例): 固件把 nRF24 数据固定单播到
 * host_ip:host_port (默认 192.168.11.100:8602, 持久化). */
bool UdpManager_SetHost(UdpManager *mgr, const char *ip, uint16_t port);
bool UdpManager_GetHost(UdpManager *mgr, char *ip, size_t ip_len, uint16_t *port);

/* 查询版本 (GET_VERSION): 同步等待, 填入 version 字符串 (NUL 终止).
 * buf_len 为 buf 容量. 返回 true 表示成功. */
bool UdpManager_GetVersion(UdpManager *mgr, char *buf, size_t buf_len);

bool UdpManager_Reboot(UdpManager *mgr);

/* CRC16-CCITT (poly 0x1021, init 0x0000), 与固件 crc16_ccitt 对齐 */
uint16_t UdpManager_CRC16_CCITT(const uint8_t *data, size_t len);

/* 固件升级 (配置端口 8601, 0x01-0x03 由 udp_fw_upgrade 库处理):
 *   Start: 发 [0x01][size 4B LE][可选 32B keyhash], 固件回 [0x01][resp], 成功返回 true
 *     resp: 0=失败, 1=成功, 2=keyhash 不一致被拒绝
 *   Data:  发 [0x02][data ≤511B], 固件回 [0x02][offset 4B LE], 校验 offset==*got_offset
 *   End:   发 [0x03][test_mode 1B][crc16 2B LE], 固件回 [0x03][1/0]
 * test_mode: 0=永久, 1=临时. 失败返回 false.
 * Start 的 out_resp (可为 NULL) 输出原始 resp, 供调用方区分 keyhash 拒绝 (2). */
bool UdpManager_FirmwareStart(UdpManager *mgr, uint32_t size, const uint8_t *keyhash,
			      uint8_t *out_resp);
bool UdpManager_FirmwareData(UdpManager *mgr, const uint8_t *data, size_t len,
			     uint32_t expected_offset, uint32_t *got_offset);
bool UdpManager_FirmwareEnd(UdpManager *mgr, uint8_t test_mode, uint16_t crc16);

/* 回调设置 */
void UdpManager_SetMsgCallback(UdpManager *mgr, udp_msg_callback cb, void *data);
void UdpManager_SetDataCallback(UdpManager *mgr, udp_data_callback cb, void *data);

/* 收集本机各网卡的子网定向广播地址 (点分十进制字符串, 如 "192.168.1.255").
 * addrs/cap 为输出缓冲 (每项 16 字节). 返回填充数量.
 * 用于填充 UI 下拉框: 255.255.255.255 + 各网卡广播. */
int UdpManager_GetBroadcastAddrs(char addrs[][16], int cap);

/* 接收线程 (后台 recvfrom, 按 chan 分发命令响应/数据帧) */
void UdpManager_StartRxThread(UdpManager *mgr);
void UdpManager_StopRxThread(UdpManager *mgr);

#endif /* UDP_MANAGER_H */
