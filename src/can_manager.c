#include "can_manager.h"
#include "pcan_loader.h"
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
		LeaveCriticalSection(&mgr->cs);
		if (mgr->msg_cb) mgr->msg_cb("CAN连接失败", mgr->msg_data);
		return false;
	}

	mgr->channel = (TPCANHandle)channel;
	mgr->connected = true;

	/* 配置接收过滤器: 固件响应、RF24 配置、手柄状态/心跳 */
	Pcan_FilterMessages(mgr->channel, CAN_ID_PLATFORM_TX, CAN_ID_PLATFORM_TX, 0);
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
			/* 固件响应帧: 存入 fw_response 并唤醒升级线程 */
			if (msg.id == CAN_ID_PLATFORM_TX) {
				EnterCriticalSection(&mgr->cs);
				mgr->fw_response.id = msg.id;
				mgr->fw_response.dlc = msg.len;
				memcpy(mgr->fw_response.data, msg.data, msg.len);
				mgr->fw_got_response = true;
				SetEvent(mgr->fw_event);
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
bool CanManager_GetVersion(CanManager *mgr, uint32_t *version)
{
	if (!version) return false;

	uint8_t data[8] = {0};
	data[0] = FW_CMD_VERSION;

	if (!CanManager_Send(mgr, CAN_ID_PLATFORM_RX, data, 8)) {
		return false;
	}

	/* 等待版本响应, 超时 500ms */
	uint32_t code = 0, val = 0;
	if (wait_fw_response(mgr, 500, &code, &val)) {
		if (code == FW_CODE_VERSION) {
			*version = val;
			return true;
		}
	}
	return false;
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

	/* 等待 Flash 擦除完成 (返回 FW_CODE_OFFSET) */
	uint32_t code = 0, val = 0;
	if (!wait_fw_response(mgr, 15000, &code, &val) || code != FW_CODE_OFFSET) {
		free(fileData);
		if (msg_cb) msg_cb("等待Flash擦除超时", msg_data);
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

		/* 进度回调 */
		if (progress_cb) {
			char buf[32];
			int pct = (int)((long long)offset * 100 / total);
			sprintf(buf, "%d%%", pct);
			progress_cb(buf, progress_data);
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
				can_msg_callback msg_cb, void *msg_data,
				can_msg_callback progress_cb, void *progress_data)
{
	if (!mgr || !mgr->connected) return false;
	EnterCriticalSection(&mgr->cs);
	bool result = doFirmwareUpgrade(mgr, firmware_path, test_mode, msg_cb, msg_data,
					progress_cb, progress_data);
	LeaveCriticalSection(&mgr->cs);
	return result;
}

void CanManager_SetMsgCallback(CanManager *mgr, can_msg_callback cb, void *data)
{
	if (mgr) { mgr->msg_cb = cb; mgr->msg_data = data; }
}

void CanManager_SetFrameCallback(CanManager *mgr, can_frame_callback cb, void *data)
{
	if (mgr) { mgr->frame_cb = cb; mgr->frame_data = data; }
}
