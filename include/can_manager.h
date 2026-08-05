#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

/* CAN 帧 ID (设备协议约定) */
#define CAN_ID_PLATFORM_RX  0x101
#define CAN_ID_PLATFORM_TX  0x102
#define CAN_ID_FW_DATA_RX   0x103
#define CAN_ID_KEYHASH_RX   0x104
#define CAN_ID_VERSION_STR  0x105  /* 手柄→平台: 版本字符串分帧 [seq 1B][text 7B] */
#define CAN_ID_RF24_CONFIG_CMD  0x110
#define CAN_ID_RF24_CONFIG_RESP 0x111
#define CAN_ID_HANDLER_STATE 0x1E3
#define CAN_ID_HEARTBEAT    0x763
#define CAN_ID_OVERBREAK_LASER 0x263
#define CAN_ID_COORD_XY     0x363
#define CAN_ID_COORD_Z      0x463

/* 固件升级命令码 (data[0]) */
enum fw_cmd {
	FW_CMD_START_UPDATE = 0,
	FW_CMD_CONFIRM,
	FW_CMD_VERSION,
	FW_CMD_REBOOT,
};

/* 固件升级响应码 (响应帧 data[0]) */
enum fw_code {
	FW_CODE_OFFSET = 0,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIRM,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
	FW_CODE_KEYHASH_ERROR,
};

/* CAN 帧结构 (应用层) */
typedef struct {
	uint32_t id;
	uint8_t dlc;
	uint8_t data[8];
} CanFrame;

/* 回调类型: 状态消息 / 数据帧 */
typedef void (*can_msg_callback)(const char *msg, void *user_data);
typedef void (*can_frame_callback)(const CanFrame *frame, void *user_data);

/* CAN 管理器 (不透明指针) */
typedef struct CanManager CanManager;

/* 生命周期 */
CanManager *CanManager_Create(void);
void CanManager_Destroy(CanManager *mgr);

/* 连接管理 */
bool CanManager_Connect(CanManager *mgr, int channel, int baudrate);
void CanManager_Disconnect(CanManager *mgr);
bool CanManager_IsConnected(CanManager *mgr);

/* 当前已连接的 PCAN channel (未连接返回 -1). 用于跨实例占用判断 */
int CanManager_GetChannel(CanManager *mgr);

/* 最近一次 Connect 失败时的 PCAN status (成功返回 PCAN_ERROR_OK=0).
 * 用于友好提示失败原因 (设备不存在 / 被占用等) */
uint32_t CanManager_GetLastError(CanManager *mgr);

/* 设备枚举 (扫描 PCAN-USB 通道) */
int CanManager_DetectDevice(CanManager *mgr, char devices[][256], int max_devices);

/* 数据发送 */
bool CanManager_Send(CanManager *mgr, uint32_t id, const uint8_t *data, uint8_t len);
bool CanManager_SendFrame(CanManager *mgr, const CanFrame *frame);

/* 设备控制 (版本查询 / 重启) */
/* 查询固件版本字符串 (多帧 0x105 拼接). 发 0x101 cmd=VERSION(2), 收 0x102 code=VERSION
 * (offset=字符串总长度) 后, 收集 N 帧 0x105 [seq 1B][text 7B] 按 seq 拼接, 遇 '\0' 截断.
 * buf 为输出缓冲 (NUL 终止), buf_len 为容量. 返回 true = 成功收到完整字符串. */
bool CanManager_GetVersionStr(CanManager *mgr, char *buf, size_t buf_len);
bool CanManager_Reboot(CanManager *mgr);

/* 固件升级 (阻塞, 内部按 8 字节分帧并同步等待 ACK).
 * test_mode: 0=永久升级, 1=临时升级 (重启后恢复原固件).
 * keyhash: 从签名镜像提取的 32B keyhash (0x104 前置发送, 供 FW 端校验);
 *          可为 NULL (跳过, 兼容旧 FW). */
bool CanManager_FirmwareUpgrade(CanManager *mgr, const char *firmware_path, int test_mode,
				const uint8_t *keyhash,
				can_msg_callback msg_cb, void *msg_data,
				can_msg_callback progress_cb, void *progress_data);

/* 回调设置 */
void CanManager_SetMsgCallback(CanManager *mgr, can_msg_callback cb, void *data);
void CanManager_SetFrameCallback(CanManager *mgr, can_frame_callback cb, void *data);

/* 接收线程 (后台轮询 CAN_Read, 分发到回调) */
void CanManager_StartRxThread(CanManager *mgr);
void CanManager_StopRxThread(CanManager *mgr);

#endif /* CAN_MANAGER_H */
