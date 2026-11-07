/*
 * vcinpl.dll 动态加载器 (IXXAT VCI 3 兼容 API)
 * 运行时按需加载 IXXAT 驱动 DLL, 避免编译期硬依赖
 */
#include "vci_loader.h"

/* VCI API 函数指针 (加载后指向 DLL 导出符号) */
pfnVciInitialize         Vci_Initialize = NULL;
pfnVciEnumDeviceOpen     Vci_EnumDeviceOpen = NULL;
pfnVciEnumDeviceNext     Vci_EnumDeviceNext = NULL;
pfnVciEnumDeviceClose    Vci_EnumDeviceClose = NULL;
pfnVciDeviceOpen         Vci_DeviceOpen = NULL;
pfnVciDeviceClose        Vci_DeviceClose = NULL;
pfnCanChannelOpen        Vci_ChannelOpen = NULL;
pfnCanChannelInitialize  Vci_ChannelInitialize = NULL;
pfnCanChannelActivate    Vci_ChannelActivate = NULL;
pfnCanChannelClose       Vci_ChannelClose = NULL;
pfnCanChannelPeekMessage Vci_ChannelPeekMessage = NULL;
pfnCanChannelPostMessage Vci_ChannelPostMessage = NULL;
pfnCanControlOpen        Vci_ControlOpen = NULL;
pfnCanControlInitialize  Vci_ControlInitialize = NULL;
pfnCanControlStart       Vci_ControlStart = NULL;
pfnCanControlReset       Vci_ControlReset = NULL;
pfnCanControlClose       Vci_ControlClose = NULL;
pfnCanControlSetAccFilter Vci_ControlSetAccFilter = NULL;
pfnVciFormatError        Vci_FormatError = NULL;

static HMODULE g_hModule = NULL;

/* 符号名 -> 函数指针地址 的映射表, 表驱动加载避免重复代码 */
static struct {
	const char *name;
	void **ptr;
} g_procs[] = {
	{ "vciInitialize",          (void **)&Vci_Initialize },
	{ "vciEnumDeviceOpen",      (void **)&Vci_EnumDeviceOpen },
	{ "vciEnumDeviceNext",      (void **)&Vci_EnumDeviceNext },
	{ "vciEnumDeviceClose",     (void **)&Vci_EnumDeviceClose },
	{ "vciDeviceOpen",          (void **)&Vci_DeviceOpen },
	{ "vciDeviceClose",         (void **)&Vci_DeviceClose },
	{ "canChannelOpen",         (void **)&Vci_ChannelOpen },
	{ "canChannelInitialize",   (void **)&Vci_ChannelInitialize },
	{ "canChannelActivate",     (void **)&Vci_ChannelActivate },
	{ "canChannelClose",        (void **)&Vci_ChannelClose },
	{ "canChannelPeekMessage",  (void **)&Vci_ChannelPeekMessage },
	{ "canChannelPostMessage",  (void **)&Vci_ChannelPostMessage },
	{ "canControlOpen",         (void **)&Vci_ControlOpen },
	{ "canControlInitialize",   (void **)&Vci_ControlInitialize },
	{ "canControlStart",        (void **)&Vci_ControlStart },
	{ "canControlReset",        (void **)&Vci_ControlReset },
	{ "canControlClose",        (void **)&Vci_ControlClose },
	{ "canControlSetAccFilter", (void **)&Vci_ControlSetAccFilter },
	{ "vciFormatError",         (void **)&Vci_FormatError },
	{ NULL, NULL }
};

bool VciLoader_Load(void)
{
	if (g_hModule) {
		return true;
	}

	g_hModule = LoadLibraryW(L"vcinpl.dll");
	if (!g_hModule) {
		return false;
	}

	for (int i = 0; g_procs[i].name; i++) {
		*g_procs[i].ptr = GetProcAddress(g_hModule, g_procs[i].name);
		if (!*g_procs[i].ptr) {
			VciLoader_Unload();
			return false;
		}
	}

	/* vciInitialize 须在任何 VCI 调用前执行一次, 重复调用返回 ALREADY_INIT 视为成功 */
	HRESULT st = Vci_Initialize();
	if (st != VCI_OK && st != VCI_E_ALREADY_INIT) {
		VciLoader_Unload();
		return false;
	}

	return true;
}

void VciLoader_Unload(void)
{
	if (g_hModule) {
		FreeLibrary(g_hModule);
		g_hModule = NULL;
	}
	for (int i = 0; g_procs[i].name; i++) {
		*g_procs[i].ptr = NULL;
	}
}
