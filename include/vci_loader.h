#ifndef VCI_LOADER_H
#define VCI_LOADER_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

/* IXXAT VCI 3 兼容 API (vcinpl.dll) 本地最小声明, 无需 VCI SDK 头文件.
 * VCI 4 驱动包自带 V3 兼容版 vcinpl.dll, 新老驱动环境均可加载.
 * 结构体布局与常量值参照 python-can can/interfaces/ixxat/{structures,constants}.py
 * (该实现经长期实机验证, 与 vcinpl.dll 实际行为一致). */

/* VCI 设备对象标识 (LUID, 8B) */
typedef union {
	int64_t  AsInt64;
	uint32_t AsLuid[2];
} VCIID;

/* 设备唯一硬件 ID (16B: ASCII 字符串或 GUID) */
typedef union {
	char AsChar[16];
	GUID AsGuid;
} VCIHWID;

/* 设备枚举信息 (vciEnumDeviceNext 输出) */
typedef struct {
	VCIID    VciObjectId;             /* 设备对象 ID (vciDeviceOpen 入参) */
	GUID     DeviceClass;
	uint8_t  DriverMajorVersion;
	uint8_t  DriverMinorVersion;
	uint16_t DriverBuildVersion;
	uint8_t  HardwareBranchVersion;
	uint8_t  HardwareMajorVersion;
	uint8_t  HardwareMinorVersion;
	uint8_t  HardwareBuildVersion;
	VCIHWID  UniqueHardwareId;
	char     Description[128];        /* 设备描述, 如 "USB-to-CAN compact" */
	char     Manufacturer[126];
	uint16_t DriverReleaseVersion;
} VCIDEVICEINFO;

/* CAN 消息信息 (4B). bFlags 位域: [3:0]=dlc, bit4=ovr, bit5=srr, bit6=rtr, bit7=ext
 * (小端位序, 见 python-can CANMSGINFO.Bits; 统一按字节位运算访问, 不用 C 位域) */
typedef struct {
	uint8_t bType;       /* CAN_MSGTYPE_* */
	uint8_t bAddFlags;
	uint8_t bFlags;
	uint8_t bAccept;
} CANMSGINFO_BYTES;

typedef union {
	CANMSGINFO_BYTES Bytes;
	uint32_t         Bits;
} CANMSGINFO;

/* CAN 消息 (canChannelPeekMessage / canChannelPostMessage) */
typedef struct {
	uint32_t   dwTime;
	uint32_t   dwMsgId;
	CANMSGINFO uMsgInfo;
	uint8_t    abData[8];
} CANMSG;

/* 消息类型 (CANMSG.bType) */
#define CAN_MSGTYPE_DATA     0x00
#define CAN_MSGTYPE_INFO     0x01
#define CAN_MSGTYPE_ERROR    0x02
#define CAN_MSGTYPE_STATUS   0x03
#define CAN_MSGTYPE_TIMEOVR  0x05

/* 控制器工作模式 (canControlInitialize bOpMode) */
#define CAN_OPMODE_STANDARD  0x01
#define CAN_OPMODE_EXTENDED  0x02
#define CAN_OPMODE_ERRFRAME  0x04
#define CAN_OPMODE_LISTONLY  0x08

/* 验收过滤: 禁用过滤 = 全部接收 */
#define CAN_ACC_CODE_NONE    0x80000000u
#define CAN_ACC_MASK_NONE    0xFFFFFFFFu

/* VCI 状态码 (SEV_VCI_ERROR = 0xE0001000) */
#define VCI_OK                ((HRESULT)0x00000000L)
#define VCI_E_TIMEOUT         ((HRESULT)0xE000100BL)
#define VCI_E_ACCESSDENIED    ((HRESULT)0xE000100AL)
#define VCI_E_NO_MORE_ITEMS   ((HRESULT)0xE000100FL)
#define VCI_E_NOT_INITIALIZED ((HRESULT)0xE0001010L)
#define VCI_E_ALREADY_INIT    ((HRESULT)0xE0001011L)
#define VCI_E_RXQUEUE_EMPTY   ((HRESULT)0xE0001012L)

/* VCI API 函数指针类型 (__stdcall 调用约定) */
typedef HRESULT (__stdcall *pfnVciInitialize)(void);
typedef HRESULT (__stdcall *pfnVciEnumDeviceOpen)(HANDLE *phEnum);
typedef HRESULT (__stdcall *pfnVciEnumDeviceNext)(HANDLE hEnum, VCIDEVICEINFO *pInfo);
typedef HRESULT (__stdcall *pfnVciEnumDeviceClose)(HANDLE hEnum);
typedef HRESULT (__stdcall *pfnVciDeviceOpen)(VCIID *pVciId, HANDLE *phDevice);
typedef HRESULT (__stdcall *pfnVciDeviceClose)(HANDLE hDevice);
typedef HRESULT (__stdcall *pfnCanChannelOpen)(HANDLE hDevice, uint32_t dwCanNo,
					       int fExclusive, HANDLE *phChannel);
typedef HRESULT (__stdcall *pfnCanChannelInitialize)(HANDLE hChannel, uint16_t wRxFifoSize,
						     uint16_t wRxThreshold, uint16_t wTxFifoSize,
						     uint16_t wTxThreshold);
typedef HRESULT (__stdcall *pfnCanChannelActivate)(HANDLE hChannel, int fEnable);
typedef HRESULT (__stdcall *pfnCanChannelClose)(HANDLE hChannel);
typedef HRESULT (__stdcall *pfnCanChannelPeekMessage)(HANDLE hChannel, CANMSG *pMsg);
typedef HRESULT (__stdcall *pfnCanChannelPostMessage)(HANDLE hChannel, CANMSG *pMsg);
typedef HRESULT (__stdcall *pfnCanControlOpen)(HANDLE hDevice, uint32_t dwCanNo,
					       HANDLE *phControl);
typedef HRESULT (__stdcall *pfnCanControlInitialize)(HANDLE hControl, uint8_t bOpMode,
						     uint8_t bBt0, uint8_t bBt1);
typedef HRESULT (__stdcall *pfnCanControlStart)(HANDLE hControl, int fStart);
typedef HRESULT (__stdcall *pfnCanControlReset)(HANDLE hControl);
typedef HRESULT (__stdcall *pfnCanControlClose)(HANDLE hControl);
typedef HRESULT (__stdcall *pfnCanControlSetAccFilter)(HANDLE hControl, int bExtended,
						       uint32_t dwCode, uint32_t dwMask);
typedef void (__stdcall *pfnVciFormatError)(HRESULT hrError, char *pszText, uint32_t dwSize);

/* 动态加载/卸载 vcinpl.dll (加载成功后自动调用一次 vciInitialize) */
bool VciLoader_Load(void);
void VciLoader_Unload(void);

/* VCI API 函数指针 (加载后非 NULL 即可用) */
extern pfnVciInitialize         Vci_Initialize;
extern pfnVciEnumDeviceOpen     Vci_EnumDeviceOpen;
extern pfnVciEnumDeviceNext     Vci_EnumDeviceNext;
extern pfnVciEnumDeviceClose    Vci_EnumDeviceClose;
extern pfnVciDeviceOpen         Vci_DeviceOpen;
extern pfnVciDeviceClose        Vci_DeviceClose;
extern pfnCanChannelOpen        Vci_ChannelOpen;
extern pfnCanChannelInitialize  Vci_ChannelInitialize;
extern pfnCanChannelActivate    Vci_ChannelActivate;
extern pfnCanChannelClose       Vci_ChannelClose;
extern pfnCanChannelPeekMessage Vci_ChannelPeekMessage;
extern pfnCanChannelPostMessage Vci_ChannelPostMessage;
extern pfnCanControlOpen        Vci_ControlOpen;
extern pfnCanControlInitialize  Vci_ControlInitialize;
extern pfnCanControlStart       Vci_ControlStart;
extern pfnCanControlReset       Vci_ControlReset;
extern pfnCanControlClose       Vci_ControlClose;
extern pfnCanControlSetAccFilter Vci_ControlSetAccFilter;
extern pfnVciFormatError        Vci_FormatError;

#endif /* VCI_LOADER_H */
