#include "can_manager.h"
#include "can_backend.h"
#include "fw_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * CAN 管理器 (协议层, 厂商无关; 硬件访问经 can_backend 分发)
 * ================================================================ */
struct CanManager {
	CRITICAL_SECTION cs;
	const CanBackend *be;             /* 当前连接的后端 */
	void *be_ctx;                     /* 后端上下文 */
	CanDeviceInfo dev;                /* 当前连接的设备 (GetDevice 返回) */
	bool connected;
	can_msg_callback msg_cb;
	void *msg_data;
	can_frame_callback frame_cb;
	void *frame_data;
	HANDLE rx_thread;
	volatile bool rx_running;

	/* 固件升级: 事件 + 响应帧 (RX 线程与升级线程同步) */
	HANDLE fw_event;
	CanFrame fw_response;
	volatile bool fw_got_response;

	/* 版本字符串多帧拼接 (RX 线程收集 0x105, GetVersionStr 轮询读取).
	 * ver_query_active=查询进行中; ver_total=预期帧数; ver_got[seq]=该帧已收;
	 * ver_text[seq] 存 7B 文本片段. 最多 32 帧 = 224B (版本串足够). */
	volatile bool ver_query_active;
	uint8_t ver_total;
	volatile bool ver_got[32];
	char ver_text[32][7];

	/* 最近一次 Connect 失败的厂商原始状态码 (0=OK) 及所属后端 (用于取错误文本).
	 * 供上层友好提示占用/不存在 */
	uint32_t last_error;
	const CanBackend *last_error_be;
};

/* 后端注册表: 设备列表顺序 = 枚举顺序 (PCAN 在前, IXXAT 在后) */
const CanBackend *const can_backends[CAN_BACKEND_COUNT] = {
	&can_backend_pcan,
	&can_backend_vci,
};

CanManager *CanManager_Create(void)
{
	CanManager *mgr = (CanManager *)calloc(1, sizeof(CanManager));
	if (mgr) {
		InitializeCriticalSection(&mgr->cs);
		mgr->be = NULL;
		mgr->be_ctx = NULL;
		mgr->connected = false;
		mgr->fw_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	}
	return mgr;
}

void CanManager_Destroy(CanManager *mgr)
{
	if (!mgr) return;
	CanManager_Disconnect(mgr);
	if (mgr->fw_event) CloseHandle(mgr->fw_event);
	DeleteCriticalSection(&mgr->cs);
	free(mgr);
}

bool CanManager_Connect(CanManager *mgr, const CanDeviceInfo *dev, int baud_bps)
{
	if (!mgr || !dev) return false;

	/* 按设备厂商选择后端 */
	const CanBackend *be = NULL;
	for (int i = 0; i < CAN_BACKEND_COUNT; i++) {
		if (can_backends[i]->vendor == dev->vendor) {
			be = can_backends[i];
			break;
		}
	}
	if (!be) return false;

	EnterCriticalSection(&mgr->cs);

	void *ctx = NULL;
	uint32_t err = 0;
	if (!be->connect(dev->handle, baud_bps, &ctx, &err)) {
		mgr->last_error = err;
		mgr->last_error_be = be;
		LeaveCriticalSection(&mgr->cs);
		if (mgr->msg_cb) mgr->msg_cb("CAN连接失败", mgr->msg_data);
		return false;
	}
	mgr->last_error = 0;
	mgr->last_error_be = be;

	mgr->be = be;
	mgr->be_ctx = ctx;
	mgr->dev = *dev;
	mgr->connected = true;

	/* 配置接收过滤: 固件响应(0x102)、版本字符串(0x105)、RF24 配置(0x111)、手柄状态/心跳.
	 * 0x105 单独加 (不与 0x102 连号范围, 各精确匹配), 否则版本多帧被驱动层丢弃.
	 * PCAN 为硬件验收过滤, IXXAT 为软件门控. */
	be->add_rx_range(ctx, CAN_ID_PLATFORM_TX, CAN_ID_PLATFORM_TX);
	be->add_rx_range(ctx, CAN_ID_VERSION_STR, CAN_ID_VERSION_STR);
	be->add_rx_range(ctx, CAN_ID_RF24_CONFIG_RESP, CAN_ID_RF24_CONFIG_RESP);
	be->add_rx_range(ctx, CAN_ID_HANDLER_STATE, CAN_ID_HEARTBEAT);

	LeaveCriticalSection(&mgr->cs);

	if (mgr->msg_cb) mgr->msg_cb("CAN已连接", mgr->msg_data);
	return true;
}

void CanManager_Disconnect(CanManager *mgr)
{
	if (!mgr || !mgr->connected) return;

	CanManager_StopRxThread(mgr);

	EnterCriticalSection(&mgr->cs);
	if (mgr->be && mgr->be_ctx) {
		mgr->be->disconnect(mgr->be_ctx);
	}
	mgr->be = NULL;
	mgr->be_ctx = NULL;
	mgr->connected = false;
	LeaveCriticalSection(&mgr->cs);

	if (mgr->msg_cb) mgr->msg_cb("CAN已断开", mgr->msg_data);
}

bool CanManager_IsConnected(CanManager *mgr)
{
	return mgr && mgr->connected;
}

const CanDeviceInfo *CanManager_GetDevice(CanManager *mgr)
{
	if (!mgr || !mgr->connected) return NULL;
	return &mgr->dev;
}

uint32_t CanManager_GetLastError(CanManager *mgr)
{
	return mgr ? mgr->last_error : 0;
}

void CanManager_GetLastErrorText(CanManager *mgr, char *buf, size_t buf_len)
{
	if (!buf || buf_len == 0) return;
	buf[0] = '\0';
	if (mgr && mgr->last_error_be && mgr->last_error_be->error_text) {
		mgr->last_error_be->error_text(mgr->last_error, buf, buf_len);
	}
}

int CanManager_DetectDevice(CanManager *mgr, CanDeviceInfo *devices, int max_devices)
{
	(void)mgr;
	/* 依次枚举各后端已安装驱动的设备, 混排进同一列表 */
	int count = 0;
	for (int i = 0; i < CAN_BACKEND_COUNT && count < max_devices; i++) {
		count += can_backends[i]->detect(devices + count, max_devices - count);
	}
	return count;
}

bool CanManager_Send(CanManager *mgr, uint32_t id, const uint8_t *data, uint8_t len)
{
	if (!mgr || !mgr->connected || !mgr->be || !mgr->be_ctx) return false;

	EnterCriticalSection(&mgr->cs);
	bool ok = mgr->be->send(mgr->be_ctx, id, data, len);
	LeaveCriticalSection(&mgr->cs);

	return ok;
}

bool CanManager_SendFrame(CanManager *mgr, const CanFrame *frame)
{
	return CanManager_Send(mgr, frame->id, frame->data, frame->dlc);
}

/* ================================================================
 * RX 线程
 * ================================================================ */
static DWORD WINAPI rx_thread_proc(LPVOID param)
{
	CanManager *mgr = (CanManager *)param;

	while (mgr->rx_running) {
		if (!mgr->connected || !mgr->be || !mgr->be_ctx) {
			Sleep(10);
			continue;
		}

		CanFrame frame;

		EnterCriticalSection(&mgr->cs);
		int rd = mgr->be->read(mgr->be_ctx, &frame);
		LeaveCriticalSection(&mgr->cs);

		if (rd == 0) {
			/* 固件响应帧 (0x102): 存入 fw_response 并唤醒等待者.
			 * 若是 VERSION 响应 (code=2), offset = 版本字符串总长度,
			 * 据此算预期 0x105 帧数并开启多帧收集模式. */
			if (frame.id == CAN_ID_PLATFORM_TX) {
				EnterCriticalSection(&mgr->cs);
				mgr->fw_response.id = frame.id;
				mgr->fw_response.dlc = frame.dlc;
				memcpy(mgr->fw_response.data, frame.data, frame.dlc);
				mgr->fw_got_response = true;
				/* VERSION 响应: offset=字符串总字节数, 据此算 0x105 帧数 (每帧 7B).
				 * 重置 ver_got, 标记查询进行中, 让 GetVersionStr 轮询收集. */
				if (frame.dlc >= 1 && frame.data[0] == FW_CODE_VERSION) {
					uint32_t total_len = 0;
					if (frame.dlc >= 8) {
						total_len = (uint32_t)frame.data[4] |
							    ((uint32_t)frame.data[5] << 8) |
							    ((uint32_t)frame.data[6] << 16) |
							    ((uint32_t)frame.data[7] << 24);
					}
					/* 帧数 = ceil(total_len / 7), 上限 32 */
					uint8_t frames = (uint8_t)((total_len + 6) / 7);
					if (frames > 32) frames = 32;
					mgr->ver_total = frames;
					for (int i = 0; i < 32; i++) {
						mgr->ver_got[i] = false;
					}
					mgr->ver_query_active = true;
				}
				SetEvent(mgr->fw_event);
				LeaveCriticalSection(&mgr->cs);
			}

			/* 版本字符串分帧 (0x105): [seq 1B][text 7B]. 查询进行中时按 seq 存入缓冲. */
			if (frame.id == CAN_ID_VERSION_STR && mgr->ver_query_active && frame.dlc >= 1) {
				uint8_t seq = frame.data[0];
				EnterCriticalSection(&mgr->cs);
				if (seq < 32) {
					uint8_t txt_len = frame.dlc - 1;
					if (txt_len > 7) txt_len = 7;
					memcpy(mgr->ver_text[seq], frame.data + 1, txt_len);
					/* 不足 7B 的末帧补 '\0' 填充 (协议约定), 便于拼接时统一截断 */
					if (txt_len < 7) {
						memset(mgr->ver_text[seq] + txt_len, 0, 7 - txt_len);
					}
					mgr->ver_got[seq] = true;
				}
				LeaveCriticalSection(&mgr->cs);
			}

		/* 应用帧: 经 frame_cb 上抛 (RF24 配置响应、手柄数据、心跳) */
		if (frame.id == CAN_ID_RF24_CONFIG_RESP ||
			    (frame.id >= CAN_ID_HANDLER_STATE && frame.id <= CAN_ID_HEARTBEAT)) {
				if (mgr->frame_cb) {
					mgr->frame_cb(&frame, mgr->frame_data);
				}
			}
		} else {
			/* 队列空或状态异常: 均让出 CPU 等待下一轮 */
			Sleep(1);
		}
	}

	return 0;
}

void CanManager_StartRxThread(CanManager *mgr)
{
	if (!mgr || mgr->rx_running) return;
	mgr->rx_running = true;
	mgr->rx_thread = CreateThread(NULL, 0, rx_thread_proc, mgr, 0, NULL);
}

void CanManager_StopRxThread(CanManager *mgr)
{
	if (!mgr || !mgr->rx_running) return;
	mgr->rx_running = false;
	if (mgr->rx_thread) {
		WaitForSingleObject(mgr->rx_thread, 2000);
		CloseHandle(mgr->rx_thread);
		mgr->rx_thread = NULL;
	}
}

/* ================================================================
 * 固件升级响应同步
 * ================================================================ */
static bool wait_fw_response(CanManager *mgr, uint32_t timeout_ms, uint32_t *code, uint32_t *val)
{
	ResetEvent(mgr->fw_event);
	mgr->fw_got_response = false;

	DWORD result = WaitForSingleObject(mgr->fw_event, timeout_ms);

	if (result == WAIT_OBJECT_0 && mgr->fw_got_response) {
		/* 解析响应帧: code = data[0], val = data[4..7] (小端) */
		if (code) *code = mgr->fw_response.data[0];
		if (val) {
			*val = (uint32_t)mgr->fw_response.data[4] |
			       ((uint32_t)mgr->fw_response.data[5] << 8) |
			       ((uint32_t)mgr->fw_response.data[6] << 16) |
			       ((uint32_t)mgr->fw_response.data[7] << 24);
		}
		return true;
	}
	return false;
}

/* ================================================================
 * 控制命令
 * ================================================================ */
/* 查询版本字符串 (新版固件: 多帧拼接).
 * 流程: 发 0x101 cmd=VERSION(2) → 收 0x102 code=VERSION (offset=字符串总长,
 * RX 线程据此开启 ver_query_active 多帧收集) → 收集 N 帧 0x105 → 拼接写入 buf.
 * 超时 500ms 等 0x102, 之后轮询 2s 收齐 0x105 帧. */
bool CanManager_GetVersionStr(CanManager *mgr, char *buf, size_t buf_len)
{
	if (!mgr || !buf || buf_len == 0) return false;
	buf[0] = '\0';

	uint8_t data[8] = {0};
	data[0] = FW_CMD_VERSION;

	mgr->ver_query_active = false;
	mgr->ver_total = 0;

	/* 先清等待状态再发命令, 避免 0x102 在 Send 返回与 wait_fw_response 重置之间到达
	 * 被吞掉 (CAN 250K 两帧往返 <1ms, 极易触发). 重置后由 RX 线程置 fw_got_response. */
	ResetEvent(mgr->fw_event);
	mgr->fw_got_response = false;

	if (!CanManager_Send(mgr, CAN_ID_PLATFORM_RX, data, 8)) {
		return false;
	}

	/* 等 0x102 VERSION 响应 (offset=字符串总长). RX 线程收到后会置 ver_query_active. */
	DWORD result = WaitForSingleObject(mgr->fw_event, 500);
	if (result != WAIT_OBJECT_0 || !mgr->fw_got_response) {
		return false;
	}
	uint32_t code = mgr->fw_response.data[0];
	if (code != FW_CODE_VERSION) {
		return false;
	}

	/* 轮询等待所有 ver_total 帧到齐 (每 10ms 检查一次, 总超时 2s) */
	uint8_t total = mgr->ver_total;
	if (total == 0) {
		/* 字符串长度为 0: 无 0x105 帧, 直接返回空串 */
		return true;
	}
	for (int waited = 0; waited < 2000; waited += 10) {
		bool all = true;
		EnterCriticalSection(&mgr->cs);
		for (uint8_t i = 0; i < total; i++) {
			if (!mgr->ver_got[i]) { all = false; break; }
		}
		LeaveCriticalSection(&mgr->cs);
		if (all) break;
		Sleep(10);
	}

	/* 按 seq 拼接 text, 遇 '\0' 截断. NUL 终止写入 buf.
	 * 末帧不足 7B 已由 RX 线程 '\0' 填充, 故遇 '\0' 即终止拼接. */
	size_t out = 0;
	bool truncated = false;
	for (uint8_t i = 0; i < total && out + 1 < buf_len && !truncated; i++) {
		bool got;
		char chunk[7];
		EnterCriticalSection(&mgr->cs);
		got = mgr->ver_got[i];
		memcpy(chunk, mgr->ver_text[i], 7);
		LeaveCriticalSection(&mgr->cs);
		if (!got) break;  /* 帧缺失, 截断 */

		for (int j = 0; j < 7; j++) {
			if (chunk[j] == '\0') { truncated = true; break; }
			buf[out++] = chunk[j];
			if (out + 1 >= buf_len) break;
		}
	}
	mgr->ver_query_active = false;
	buf[out] = '\0';
	return out > 0;
}

bool CanManager_Reboot(CanManager *mgr)
{
	uint8_t data[8] = {0};
	data[0] = FW_CMD_REBOOT;
	return CanManager_Send(mgr, CAN_ID_PLATFORM_RX, data, 8);
}

/* ================================================================
 * 固件升级
 * ================================================================ */
static bool doFirmwareUpgrade(CanManager *mgr, const char *firmware_path, int test_mode,
			      const uint8_t *keyhash,
			      can_msg_callback msg_cb, void *msg_data,
			      can_msg_callback progress_cb, void *progress_data)
{
	HANDLE hFile = CreateFileA(firmware_path, GENERIC_READ, 0, NULL,
				   OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		if (msg_cb) msg_cb("无法打开固件文件", msg_data);
		return false;
	}

	DWORD fileSize = GetFileSize(hFile, NULL);
	uint8_t *fileData = (uint8_t *)malloc(fileSize);
	DWORD bytesRead;

	if (!ReadFile(hFile, fileData, fileSize, &bytesRead, NULL) || bytesRead != fileSize) {
		free(fileData);
		CloseHandle(hFile);
		if (msg_cb) msg_cb("无法读取固件文件", msg_data);
		return false;
	}
	CloseHandle(hFile);

	/* 校验 MCUboot 镜像头: 非 MCUboot 镜像 (任意文件) 直接拒绝, 不进入升级流程. */
	if (!fw_image_validate_header(fileData, fileSize)) {
		free(fileData);
		if (msg_cb) msg_cb("固件文件格式非法 (非 MCUboot 镜像), 已拒绝", msg_data);
		return false;
	}

	/* 发送升级 keyhash (0x104): 从签名镜像提取 32B, 分 5 帧 (1B seq + 7B chunk).
	 * FW 端在 START 前据此校验, 不一致返回 KEYHASH_ERROR 拒绝.
	 * keyhash 参数为 NULL 时回退到内部从镜像提取; 提取失败则跳过 (兼容旧 FW). */
	uint8_t kh_buf[IMG_KEYHASH_LEN];

	if (!keyhash) {
		if (fw_image_extract_keyhash(fileData, fileSize, kh_buf)) {
			keyhash = kh_buf;
		}
	}
	if (keyhash) {
		for (int i = 0; i < 5; i++) {
			uint8_t kh[8] = {0};
			kh[0] = (uint8_t)i;
			int rem = (int)IMG_KEYHASH_LEN - i * 7;
			int chunk = (rem > 7) ? 7 : rem;
			memcpy(kh + 1, keyhash + i * 7, chunk);
			if (!CanManager_Send(mgr, CAN_ID_KEYHASH_RX, kh, 8)) {
				free(fileData);
				if (msg_cb) msg_cb("发送 keyhash 帧失败", msg_data);
				return false;
			}
		}
	}

	/* 发送升级开始命令: cmd=data[0], size=data[4..7] (小端) */
	uint8_t cmd[8] = {0};
	uint32_t size_le = fileSize;
	cmd[0] = FW_CMD_START_UPDATE;
	memcpy(cmd + 4, &size_le, 4);

	if (!CanManager_Send(mgr, CAN_ID_PLATFORM_RX, cmd, 8)) {
		free(fileData);
		if (msg_cb) msg_cb("发送开始命令失败", msg_data);
		return false;
	}

	/* 等待 Flash 擦除完成 (返回 FW_CODE_OFFSET);
	 * keyhash 不一致时 FW 回 FW_CODE_KEYHASH_ERROR 拒绝. */
	uint32_t code = 0, val = 0;
	if (!wait_fw_response(mgr, 15000, &code, &val)) {
		free(fileData);
		if (msg_cb) msg_cb("等待Flash擦除超时", msg_data);
		return false;
	}
	if (code == FW_CODE_KEYHASH_ERROR) {
		free(fileData);
		if (msg_cb) msg_cb("FW: 固件 keyhash 校验失败, 已拒绝升级", msg_data);
		return false;
	}
	if (code != FW_CODE_OFFSET) {
		free(fileData);
		if (msg_cb) msg_cb("启动固件升级被拒绝", msg_data);
		return false;
	}

	if (msg_cb) {
		char buf[128];
		sprintf(buf, "固件升级已启动, 大小: %u 字节", fileSize);
		msg_cb(buf, msg_data);
	}

	/* 分帧发送固件数据, 每 8 字节一帧 */
	int offset = 0;
	int total = fileSize;
	int ack_count = 0;
	int last_pct = -1;

	while (offset < total) {
		int chunk = (total - offset > 8) ? 8 : (total - offset);

		if (!CanManager_Send(mgr, CAN_ID_FW_DATA_RX, fileData + offset, chunk)) {
			free(fileData);
			if (msg_cb) msg_cb("发送固件数据失败", msg_data);
			return false;
		}

		offset += chunk;
		ack_count++;

		/* 每 64 字节 (8 帧) 等待一次 ACK, 确认已写入 */
		if (ack_count % 8 == 0 || offset >= total) {
			if (!wait_fw_response(mgr, 1000, &code, &val)) {
				/* ACK 超时: 容错继续发送, 后续失败由最终确认捕获 */
			}
		}

		/* 进度回调: 仅当整数百分比变化时才回调, 否则每 8 字节一帧都会触发
		 * UI 重绘 (SetWindowTextW) -> "升级中 NN%" 文字持续闪烁. */
		if (progress_cb) {
			int pct = (int)((long long)offset * 100 / total);
			if (pct != last_pct) {
				char buf[32];
				sprintf(buf, "%d%%", pct);
				progress_cb(buf, progress_data);
				last_pct = pct;
			}
		}
	}

	/* 发送升级确认命令: val=test_mode?0:1 (0=临时升级重启后回滚, 1=永久升级) */
	cmd[0] = FW_CMD_CONFIRM;
	memset(cmd + 1, 0, 7);
	uint32_t confirm_val = test_mode ? 0 : 1;
	memcpy(cmd + 4, &confirm_val, 4);  /* val 放 data[4..7] 小端 */
	CanManager_Send(mgr, CAN_ID_PLATFORM_RX, cmd, 8);

	/* 等待最终确认: 成功返回 val=0x55AA55AA */
	if (!wait_fw_response(mgr, 30000, &code, &val)) {
		free(fileData);
		if (msg_cb) msg_cb("等待固件确认超时", msg_data);
		return false;
	}

	free(fileData);

	if (code == FW_CODE_CONFIRM && val == 0x55AA55AA) {
		if (msg_cb) msg_cb("固件升级完成", msg_data);
		return true;
	} else {
		if (msg_cb) msg_cb("固件升级失败", msg_data);
		return false;
	}
}

bool CanManager_FirmwareUpgrade(CanManager *mgr, const char *firmware_path, int test_mode,
				const uint8_t *keyhash,
				can_msg_callback msg_cb, void *msg_data,
				can_msg_callback progress_cb, void *progress_data)
{
	if (!mgr || !mgr->connected) return false;
	/* 注意: 不可在此持锁包裹 doFirmwareUpgrade. wait_fw_response 会阻塞在 fw_event 上,
	 * 而唤醒该事件的 RX 线程需获取同一 cs 才能存响应帧 -> 会死锁导致 "等待Flash擦除超时".
	 * 发送路径由 CanManager_Send 内部的 cs 自保护, 这里无需再加锁. */
	return doFirmwareUpgrade(mgr, firmware_path, test_mode, keyhash, msg_cb, msg_data,
				 progress_cb, progress_data);
}

void CanManager_SetMsgCallback(CanManager *mgr, can_msg_callback cb, void *data)
{
	if (mgr) { mgr->msg_cb = cb; mgr->msg_data = data; }
}

void CanManager_SetFrameCallback(CanManager *mgr, can_frame_callback cb, void *data)
{
	if (mgr) { mgr->frame_cb = cb; mgr->frame_data = data; }
}
