#include "can_manager.h"
#include "pcan_loader.h"
#include "fw_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * CAN 管理器
 * ================================================================ */
struct CanManager {
	CRITICAL_SECTION cs;
	TPCANHandle channel;
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

	/* 最近一次 Connect 失败的 PCAN status (0=OK). 供上层友好提示占用/不存在 */
	uint32_t last_error;
};

CanManager *CanManager_Create(void)
{
	CanManager *mgr = (CanManager *)calloc(1, sizeof(CanManager));
	if (mgr) {
		InitializeCriticalSection(&mgr->cs);
		mgr->channel = 0;
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

bool CanManager_Connect(CanManager *mgr, int channel, int baudrate)
{
	if (!mgr || !Pcan_Initialize) return false;

	EnterCriticalSection(&mgr->cs);

	TPCANStatus status = Pcan_Initialize((uint32_t)channel, (uint32_t)baudrate, 0, 0, 0);
	if (status != PCAN_ERROR_OK) {
		mgr->last_error = status;
		LeaveCriticalSection(&mgr->cs);
		if (mgr->msg_cb) mgr->msg_cb("CAN连接失败", mgr->msg_data);
		return false;
	}
	mgr->last_error = PCAN_ERROR_OK;

	mgr->channel = (TPCANHandle)channel;
	mgr->connected = true;

	/* 配置接收过滤器: 固件响应(0x102)、版本字符串(0x105)、RF24 配置(0x111)、手柄状态/心跳.
	 * 0x105 单独加 (不与 0x102 连号范围, 各精确匹配), 否则版本多帧被驱动层丢弃. */
	Pcan_FilterMessages(mgr->channel, CAN_ID_PLATFORM_TX, CAN_ID_PLATFORM_TX, 0);
	Pcan_FilterMessages(mgr->channel, CAN_ID_VERSION_STR, CAN_ID_VERSION_STR, 0);
	Pcan_FilterMessages(mgr->channel, CAN_ID_RF24_CONFIG_RESP, CAN_ID_RF24_CONFIG_RESP, 0);
	Pcan_FilterMessages(mgr->channel, CAN_ID_HANDLER_STATE, CAN_ID_HEARTBEAT, 0);

	LeaveCriticalSection(&mgr->cs);

	if (mgr->msg_cb) mgr->msg_cb("CAN已连接", mgr->msg_data);
	return true;
}

void CanManager_Disconnect(CanManager *mgr)
{
	if (!mgr || !mgr->connected) return;

	CanManager_StopRxThread(mgr);

	EnterCriticalSection(&mgr->cs);
	if (Pcan_Uninitialize) {
		Pcan_Uninitialize(mgr->channel);
	}
	mgr->connected = false;
	LeaveCriticalSection(&mgr->cs);

	if (mgr->msg_cb) mgr->msg_cb("CAN已断开", mgr->msg_data);
}

bool CanManager_IsConnected(CanManager *mgr)
{
	return mgr && mgr->connected;
}

int CanManager_GetChannel(CanManager *mgr)
{
	if (!mgr || !mgr->connected) return -1;
	return (int)mgr->channel;
}

uint32_t CanManager_GetLastError(CanManager *mgr)
{
	return mgr ? mgr->last_error : 0;
}

int CanManager_DetectDevice(CanManager *mgr, char devices[][256], int max_devices)
{
	/* 惰性加载 PCANBasic.dll: 首次检测时按需加载, 驱动未安装则返回 0 */
	if (!Pcan_LookUpChannel) {
		if (!PcanLoader_Load()) return 0;
	}
	if (!Pcan_LookUpChannel) return 0;

	int count = 0;
	for (uint32_t i = 0; i < 16 && count < max_devices; i++) {
		TPCANHandle ch = PCAN_NONEBUS;
		char szLookup[64];
		sprintf(szLookup, "devicetype=pcan_usb,controllernumber=%u", i);
		if (Pcan_LookUpChannel(szLookup, &ch) == PCAN_ERROR_OK && ch != PCAN_NONEBUS) {
			sprintf(devices[count], "PCAN_USB_%u (0x%X)", i, ch);
			count++;
		}
	}
	return count;
}

bool CanManager_Send(CanManager *mgr, uint32_t id, const uint8_t *data, uint8_t len)
{
	if (!mgr || !mgr->connected || !Pcan_Write) return false;

	EnterCriticalSection(&mgr->cs);

	TPCANMsg msg;
	msg.id = id;
	msg.msgtype = 0;
	msg.len = len;
	memcpy(msg.data, data, len);

	TPCANStatus status = Pcan_Write(mgr->channel, &msg);
	LeaveCriticalSection(&mgr->cs);

	return (status == PCAN_ERROR_OK);
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
		if (!mgr->connected || !Pcan_Read) {
			Sleep(10);
			continue;
		}

		TPCANMsg msg;
		TPCANTimestampMsg ts;

		EnterCriticalSection(&mgr->cs);
		TPCANStatus status = Pcan_Read(mgr->channel, &msg, &ts);
		LeaveCriticalSection(&mgr->cs);

		if (status == PCAN_ERROR_OK) {
			/* 固件响应帧 (0x102): 存入 fw_response 并唤醒等待者.
			 * 若是 VERSION 响应 (code=2), offset = 版本字符串总长度,
			 * 据此算预期 0x105 帧数并开启多帧收集模式. */
			if (msg.id == CAN_ID_PLATFORM_TX) {
				EnterCriticalSection(&mgr->cs);
				mgr->fw_response.id = msg.id;
				mgr->fw_response.dlc = msg.len;
				memcpy(mgr->fw_response.data, msg.data, msg.len);
				mgr->fw_got_response = true;
				/* VERSION 响应: offset=字符串总字节数, 据此算 0x105 帧数 (每帧 7B).
				 * 重置 ver_got, 标记查询进行中, 让 GetVersionStr 轮询收集. */
				if (msg.len >= 1 && msg.data[0] == FW_CODE_VERSION) {
					uint32_t total_len = 0;
					if (msg.len >= 8) {
						total_len = (uint32_t)msg.data[4] |
							    ((uint32_t)msg.data[5] << 8) |
							    ((uint32_t)msg.data[6] << 16) |
							    ((uint32_t)msg.data[7] << 24);
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
			if (msg.id == CAN_ID_VERSION_STR && mgr->ver_query_active && msg.len >= 1) {
				uint8_t seq = msg.data[0];
				EnterCriticalSection(&mgr->cs);
				if (seq < 32) {
					uint8_t txt_len = msg.len - 1;
					if (txt_len > 7) txt_len = 7;
					memcpy(mgr->ver_text[seq], msg.data + 1, txt_len);
					/* 不足 7B 的末帧补 '\0' 填充 (协议约定), 便于拼接时统一截断 */
					if (txt_len < 7) {
						memset(mgr->ver_text[seq] + txt_len, 0, 7 - txt_len);
					}
					mgr->ver_got[seq] = true;
				}
				LeaveCriticalSection(&mgr->cs);
			}

		/* 应用帧: 经 frame_cb 上抛 (RF24 配置响应、手柄数据、心跳) */
		if (msg.id == CAN_ID_RF24_CONFIG_RESP ||
			    (msg.id >= CAN_ID_HANDLER_STATE && msg.id <= CAN_ID_HEARTBEAT)) {
				CanFrame frame;
				frame.id = msg.id;
				frame.dlc = msg.len;
				memcpy(frame.data, msg.data, msg.len);

				if (mgr->frame_cb) {
					mgr->frame_cb(&frame, mgr->frame_data);
				}
			}
		} else {
			/* 非成功状态 (含 0x00020 = PCAN_ERROR_QRCVEMPTY 接收队列空):
			 * 均让出 CPU 等待下一轮 */
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
