/*
 * 手柄-接收器工具 - Win32 GUI 应用
 * Tab1: 设备绑定 (手柄CAN扫描/连接 + 接收器UDP连接 + NRF读取比对 + 绑定)
 * Tab2: 手柄固件升级 (CAN)
 * Tab3: 接收器固件升级 (UDP)
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "resource.h"
#include "can_manager.h"
#include "pcan_loader.h"
#include "udp_manager.h"

#pragma comment(lib, "comctl32.lib")

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine;
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icex);
    MessageBoxW(NULL, L"骨架", L"手柄-接收器工具", MB_OK);
    return 0;
}
