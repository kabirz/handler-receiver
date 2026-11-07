#ifndef CAN_BACKEND_H
#define CAN_BACKEND_H

#include "can_manager.h"

/* CAN 硬件后端接口: 每个厂商一个实现 (PCAN / IXXAT VCI).
 * CanManager 经此结构访问硬件, 协议层 (状态机/固件升级) 不感知厂商.
 *
 * connect 成功时返回后端上下文 (*ctx), disconnect/send/read 收到该上下文;
 * 后端自行管理上下文生命周期 (connect 分配, disconnect 释放).
 * connect 失败时若 err 非 NULL, *err = 厂商原始状态码 (PCAN TPCANStatus / VCI HRESULT),
 * 可经 error_text 转为英文说明用于提示.
 * read 返回值: 0=收到帧 (*out 有效), 1=队列空/帧被过滤, 负值=错误.
 * add_rx_range: 追加一段接收 ID 白名单 [from, to] (协议层在 connect 后配置;
 *   PCAN 为硬件验收过滤, VCI 为软件门控; 未配置任何段 = 全部接收).
 * driver_ok: 驱动 DLL 是否可加载 (幂等, 内部触发惰性加载). */
typedef struct CanBackend {
	const char *name;
	CanVendor  vendor;
	int  (*detect)(CanDeviceInfo *out, int max);
	int  (*driver_ok)(void);
	bool (*connect)(uintptr_t handle, int baud_bps, void **ctx, uint32_t *err);
	void (*disconnect)(void *ctx);
	bool (*send)(void *ctx, uint32_t id, const uint8_t *data, uint8_t len);
	int  (*read)(void *ctx, CanFrame *out);
	int  (*add_rx_range)(void *ctx, uint32_t from, uint32_t to);
	void (*error_text)(uint32_t err, char *buf, size_t buf_len);
} CanBackend;

extern const CanBackend can_backend_pcan;
extern const CanBackend can_backend_vci;

/* 后端注册表 (CanManager 枚举/连接时遍历, 顺序即设备列表顺序) */
#define CAN_BACKEND_COUNT 2
extern const CanBackend *const can_backends[CAN_BACKEND_COUNT];

#endif /* CAN_BACKEND_H */
