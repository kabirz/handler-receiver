/*
 * IXXAT VCI 后端: 经 vci_loader 动态调用 vcinpl.dll (VCI 3 兼容 API)
 * 调用序列参照 python-can can/interfaces/ixxat/canlib_vcinpl.py (实机验证实现)
 */
#include "can_backend.h"
#include "vci_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 枚举缓存: CanDeviceInfo.handle 保存枚举索引, connect 时取 VciObjectId */
#define VCI_MAX_DEVICES 16
static VCIDEVICEINFO g_vci_devices[VCI_MAX_DEVICES];
static int g_vci_device_count = 0;

/* 接收 ID 白名单段数上限 (软件门控, 由协议层经 add_rx_range 配置) */
#define VCI_RX_RANGE_MAX 8

/* 后端上下文: 设备/通道/控制器句柄 */
struct vci_ctx {
	HANDLE hDevice;
	HANDLE hChannel;
	HANDLE hControl;
	struct { uint32_t from, to; } rx_ranges[VCI_RX_RANGE_MAX];
	int rx_range_count;
};

/* bps → VCI BT0/BT1 定时寄存器 (值与 python-can constants.py 一致)
 * 未覆盖档位返回 false 拒绝连接, 避免以错误波特率上线 */
static bool vci_bt_from_bps(int bps, uint8_t *bt0, uint8_t *bt1)
{
	switch (bps) {
	case 10000:   *bt0 = 0x31; *bt1 = 0x1C; return true;
	case 20000:   *bt0 = 0x18; *bt1 = 0x1C; return true;
	case 50000:   *bt0 = 0x09; *bt1 = 0x1C; return true;
	case 100000:  *bt0 = 0x04; *bt1 = 0x1C; return true;
	case 125000:  *bt0 = 0x03; *bt1 = 0x1C; return true;
	case 250000:  *bt0 = 0x01; *bt1 = 0x1C; return true;
	case 500000:  *bt0 = 0x00; *bt1 = 0x1C; return true;
	case 667000:  *bt0 = 0x00; *bt1 = 0x18; return true;
	case 800000:  *bt0 = 0x00; *bt1 = 0x16; return true;
	case 1000000: *bt0 = 0x00; *bt1 = 0x14; return true;
	default:      return false;
	}
}

/* 逆序关闭已打开的句柄 (connect 失败回滚与 disconnect 共用),
 * 关闭序列参照 python-can shutdown: Channel → Control(Stop/Reset/Close) → Device */
static void vci_close_handles(struct vci_ctx *c)
{
	if (c->hChannel) Vci_ChannelClose(c->hChannel);
	if (c->hControl) {
		Vci_ControlStart(c->hControl, FALSE);
		Vci_ControlReset(c->hControl);
		Vci_ControlClose(c->hControl);
	}
	if (c->hDevice) Vci_DeviceClose(c->hDevice);
	c->hChannel = NULL;
	c->hControl = NULL;
	c->hDevice = NULL;
}

static int vci_driver_ok(void)
{
	return VciLoader_Load() ? 1 : 0;
}

static int vci_detect(CanDeviceInfo *out, int max)
{
	/* 惰性加载 vcinpl.dll: 驱动未安装则返回 0 */
	if (!Vci_EnumDeviceNext) {
		if (!VciLoader_Load()) return 0;
	}
	if (!Vci_EnumDeviceNext || !Vci_DeviceOpen) return 0;

	HANDLE hEnum = NULL;
	if (Vci_EnumDeviceOpen(&hEnum) != VCI_OK) return 0;

	int count = 0;
	int limit = (max < VCI_MAX_DEVICES) ? max : VCI_MAX_DEVICES;
	while (count < limit) {
		VCIDEVICEINFO info;
		memset(&info, 0, sizeof(info));
		HRESULT st = Vci_EnumDeviceNext(hEnum, &info);
		if (st != VCI_OK) break;  /* NO_MORE_ITEMS 或出错均结束 */

		g_vci_devices[count] = info;
		out[count].vendor = CAN_VENDOR_IXXAT;
		out[count].handle = (uintptr_t)count;
		count++;
	}
	Vci_EnumDeviceClose(hEnum);
	g_vci_device_count = count;

	/* 显示名: 设备自述 (如 "USB-to-CAN compact"), 多台设备加序号区分 */
	for (int i = 0; i < count; i++) {
		char desc[sizeof(g_vci_devices[i].Description) + 1];
		size_t n = sizeof(g_vci_devices[i].Description);
		memcpy(desc, g_vci_devices[i].Description, n);
		desc[n] = '\0';
		while (n > 0 && (desc[n - 1] == ' ' || desc[n - 1] == '\0')) {
			desc[--n] = '\0';  /* 去尾部填充 */
		}
		if (n == 0) {
			strcpy(desc, "VCI device");
		}
		if (count > 1) {
			snprintf(out[i].display, sizeof(out[i].display), "IXXAT %s #%d", desc, i + 1);
		} else {
			snprintf(out[i].display, sizeof(out[i].display), "IXXAT %s", desc);
		}
	}
	return count;
}

static bool vci_connect(uintptr_t handle, int baud_bps, void **ctx, uint32_t *err)
{
	if (err) *err = 0;
	if (handle >= (uintptr_t)g_vci_device_count) return false;

	uint8_t bt0, bt1;
	if (!vci_bt_from_bps(baud_bps, &bt0, &bt1)) return false;

	struct vci_ctx *c = (struct vci_ctx *)calloc(1, sizeof(*c));
	if (!c) return false;

	/* 设备 → CAN 通道 (控制器 0, 共享模式) + FIFO → 控制器初始化 + 启动 */
	HRESULT st;
	st = Vci_DeviceOpen(&g_vci_devices[handle].VciObjectId, &c->hDevice);
	if (st != VCI_OK) goto fail;
	st = Vci_ChannelOpen(c->hDevice, 0, FALSE, &c->hChannel);
	if (st != VCI_OK) goto fail;
	st = Vci_ChannelInitialize(c->hChannel, 512, 1, 128, 1);
	if (st != VCI_OK) goto fail;
	st = Vci_ChannelActivate(c->hChannel, TRUE);
	if (st != VCI_OK) goto fail;
	st = Vci_ControlOpen(c->hDevice, 0, &c->hControl);
	if (st != VCI_OK) goto fail;

	/* 标准帧模式 + 波特率. canControlInitialize 后控制器默认接收所有帧, 不要再调
	 * canControlSetAccFilter —— 实测 CODE_NONE/MASK_NONE 组合反而挡掉全部接收
	 * (USB-to-CAN V2 compact, VCI 4 兼容层), 接收 ID 白名单由 read 软件门控完成 */
	st = Vci_ControlInitialize(c->hControl, CAN_OPMODE_STANDARD, bt0, bt1);
	if (st != VCI_OK) goto fail;

	st = Vci_ControlStart(c->hControl, TRUE);
	if (st != VCI_OK) goto fail;

	/* 清空 FIFO 中的历史帧, 避免 MOT 显示残留旧数据 */
	{
		CANMSG msg;
		while (Vci_ChannelPeekMessage(c->hChannel, &msg) == VCI_OK) {
		}
	}

	*ctx = c;
	return true;

fail:
	if (err) *err = (uint32_t)st;
	vci_close_handles(c);
	free(c);
	return false;
}

static void vci_disconnect(void *ctx)
{
	struct vci_ctx *c = (struct vci_ctx *)ctx;
	if (!c) return;
	vci_close_handles(c);
	free(c);
}

static bool vci_send(void *ctx, uint32_t id, const uint8_t *data, uint8_t len)
{
	struct vci_ctx *c = (struct vci_ctx *)ctx;

	if (len > 8) len = 8;
	CANMSG msg;
	memset(&msg, 0, sizeof(msg));
	msg.dwMsgId = id;
	msg.uMsgInfo.Bytes.bType = CAN_MSGTYPE_DATA;
	msg.uMsgInfo.Bytes.bFlags = len & 0x0F;  /* 低 4 位 DLC, ext=0 标准帧 */
	memcpy(msg.abData, data, len);

	/* PostMessage: 非阻塞入 TX FIFO (固件升级不可阻塞发送, 见 can_manager 注释) */
	return Vci_ChannelPostMessage(c->hChannel, &msg) == VCI_OK;
}

static int vci_read(void *ctx, CanFrame *out)
{
	struct vci_ctx *c = (struct vci_ctx *)ctx;

	CANMSG msg;
	HRESULT st = Vci_ChannelPeekMessage(c->hChannel, &msg);
	if (st != VCI_OK) {
		return (st == VCI_E_RXQUEUE_EMPTY || st == VCI_E_TIMEOUT) ? 1 : -1;
	}

	/* 仅标准数据帧: 丢弃 TIMEOVR/INFO/ERROR 等周期性非数据消息 */
	if (msg.uMsgInfo.Bytes.bType != CAN_MSGTYPE_DATA) return 1;
	if (msg.uMsgInfo.Bytes.bFlags & 0x80) return 1;  /* ext 位: 扩展帧 */

	/* 软件 ID 门控: 未配置任何段 = 全部接收; 配置后仅放行白名单内的帧
	 * (与 PCAN 硬件验收过滤语义对应, 由协议层经 add_rx_range 配置) */
	uint32_t id = msg.dwMsgId;
	if (c->rx_range_count > 0) {
		bool pass = false;
		for (int i = 0; i < c->rx_range_count; i++) {
			if (id >= c->rx_ranges[i].from && id <= c->rx_ranges[i].to) {
				pass = true;
				break;
			}
		}
		if (!pass) return 1;
	}

	uint8_t dlc = msg.uMsgInfo.Bytes.bFlags & 0x0F;
	if (dlc > 8) dlc = 8;
	out->id = id;
	out->dlc = dlc;
	memcpy(out->data, msg.abData, dlc);
	return 0;
}

static int vci_add_rx_range(void *ctx, uint32_t from, uint32_t to)
{
	struct vci_ctx *c = (struct vci_ctx *)ctx;
	if (!c || c->rx_range_count >= VCI_RX_RANGE_MAX) return 0;
	c->rx_ranges[c->rx_range_count].from = from;
	c->rx_ranges[c->rx_range_count].to = to;
	c->rx_range_count++;
	return 1;
}

static void vci_error_text(uint32_t err, char *buf, size_t buf_len)
{
	if (!buf || buf_len == 0) return;
	buf[0] = '\0';
	if (Vci_FormatError) {
		Vci_FormatError((HRESULT)err, buf, (uint32_t)buf_len);
	}
	if (buf[0] == '\0') {
		snprintf(buf, buf_len, "VCI error 0x%08X", err);
	}
	buf[buf_len - 1] = '\0';
}

const CanBackend can_backend_vci = {
	.name = "IXXAT VCI",
	.vendor = CAN_VENDOR_IXXAT,
	.detect = vci_detect,
	.driver_ok = vci_driver_ok,
	.connect = vci_connect,
	.disconnect = vci_disconnect,
	.send = vci_send,
	.read = vci_read,
	.add_rx_range = vci_add_rx_range,
	.error_text = vci_error_text,
};
