/*
 * PCAN 后端: 经 pcan_loader 动态调用 PCANBasic.dll
 * 原实现自 can_manager.c 迁入, 行为保持一致
 */
#include "can_backend.h"
#include "pcan_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 后端上下文: PCAN 通道句柄 */
struct pcan_ctx {
	uint32_t channel;
};

/* bps → PCAN BTR0BTR1 波特率常量 (未覆盖档位回退 250K) */
static uint32_t pcan_baud_from_bps(int bps)
{
	switch (bps) {
	case 1000000: return PCAN_BAUD_1M;
	case 800000:  return PCAN_BAUD_800K;
	case 500000:  return PCAN_BAUD_500K;
	case 250000:  return PCAN_BAUD_250K;
	case 125000:  return PCAN_BAUD_125K;
	case 100000:  return PCAN_BAUD_100K;
	case 50000:   return PCAN_BAUD_50K;
	case 20000:   return PCAN_BAUD_20K;
	case 10000:   return PCAN_BAUD_10K;
	default:      return PCAN_BAUD_250K;
	}
}

static int pcan_driver_ok(void)
{
	return PcanLoader_Load() ? 1 : 0;
}

static int pcan_detect(CanDeviceInfo *out, int max)
{
	/* 惰性加载 PCANBasic.dll: 首次检测时按需加载, 驱动未安装则返回 0 */
	if (!Pcan_LookUpChannel) {
		if (!PcanLoader_Load()) return 0;
	}
	if (!Pcan_LookUpChannel) return 0;

	int count = 0;
	for (uint32_t i = 0; i < 16 && count < max; i++) {
		TPCANHandle ch = PCAN_NONEBUS;
		char szLookup[64];
		sprintf(szLookup, "devicetype=pcan_usb,controllernumber=%u", i);
		if (Pcan_LookUpChannel(szLookup, &ch) == PCAN_ERROR_OK && ch != PCAN_NONEBUS) {
			out[count].vendor = CAN_VENDOR_PCAN;
			out[count].handle = ch;
			sprintf(out[count].display, "PCAN USB %u (0x%X)", i + 1, ch);
			count++;
		}
	}
	return count;
}

static bool pcan_connect(uintptr_t handle, int baud_bps, void **ctx, uint32_t *err)
{
	struct pcan_ctx *c = (struct pcan_ctx *)malloc(sizeof(*c));
	if (!c) return false;
	c->channel = (uint32_t)handle;

	TPCANStatus status = Pcan_Initialize(c->channel, pcan_baud_from_bps(baud_bps), 0, 0, 0);
	if (status != PCAN_ERROR_OK) {
		if (err) *err = status;
		free(c);
		return false;
	}
	if (err) *err = PCAN_ERROR_OK;

	/* 接收过滤 (硬件验收过滤) 由协议层在 connect 后经 add_rx_range 配置 */

	*ctx = c;
	return true;
}

static void pcan_disconnect(void *ctx)
{
	struct pcan_ctx *c = (struct pcan_ctx *)ctx;
	if (!c) return;
	if (Pcan_Uninitialize) {
		Pcan_Uninitialize(c->channel);
	}
	free(c);
}

static bool pcan_send(void *ctx, uint32_t id, const uint8_t *data, uint8_t len)
{
	struct pcan_ctx *c = (struct pcan_ctx *)ctx;

	TPCANMsg msg;
	msg.id = id;
	msg.msgtype = 0;
	msg.len = len;
	memcpy(msg.data, data, len);

	return Pcan_Write(c->channel, &msg) == PCAN_ERROR_OK;
}

static int pcan_read(void *ctx, CanFrame *out)
{
	struct pcan_ctx *c = (struct pcan_ctx *)ctx;

	TPCANMsg msg;
	TPCANTimestampMsg ts;
	TPCANStatus status = Pcan_Read(c->channel, &msg, &ts);

	if (status == PCAN_ERROR_OK) {
		out->id = msg.id;
		out->dlc = msg.len;
		memcpy(out->data, msg.data, msg.len);
		return 0;
	}
	/* 非成功状态 (含接收队列空/总线状态) 均按"无帧"让出, 与原 RX 轮询行为一致 */
	return 1;
}

static int pcan_add_rx_range(void *ctx, uint32_t from, uint32_t to)
{
	struct pcan_ctx *c = (struct pcan_ctx *)ctx;
	/* mode 0 = PCAN_MODE_STANDARD (标准帧) */
	return Pcan_FilterMessages(c->channel, from, to, 0) == PCAN_ERROR_OK ? 1 : 0;
}

static void pcan_error_text(uint32_t err, char *buf, size_t buf_len)
{
	if (!buf || buf_len == 0) return;
	/* CAN_GetErrorText 要求缓冲 >= 256B; 0x09 = 英文 */
	char text[256] = "PCAN error";
	if (Pcan_GetErrorText) {
		Pcan_GetErrorText(err, 0x09, text);
	}
	strncpy(buf, text, buf_len - 1);
	buf[buf_len - 1] = '\0';
}

const CanBackend can_backend_pcan = {
	.name = "PCAN",
	.vendor = CAN_VENDOR_PCAN,
	.detect = pcan_detect,
	.driver_ok = pcan_driver_ok,
	.connect = pcan_connect,
	.disconnect = pcan_disconnect,
	.send = pcan_send,
	.read = pcan_read,
	.add_rx_range = pcan_add_rx_range,
	.error_text = pcan_error_text,
};
