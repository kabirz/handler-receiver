#include "udp_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iphlpapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

/* 同步等待响应的超时 (GET_CONFIG / GET_VERSION) */
#define UDP_RESP_TIMEOUT_MS 500

/* ================================================================
 * 子网定向广播: Windows 发 255.255.255.255 (有限广播) 时, 多网卡主机路由表
 * 无法决定从哪个接口发出 → 包被丢弃. 改用各网卡的子网定向广播 (如
 * 192.168.1.255), 遍历所有非回环网卡逐个发出, 确保板子无论连哪个网卡都能收到.
 * ================================================================ */

/* 收集本机所有非回环网卡的子网定向广播地址. 返回填充数量. */
static int collect_broadcast_addrs(unsigned long *addrs, int max_cnt)
{
	/* 先用 GetIpAddrTable 取 IP; 掩码用 IP_ADAPTER_ADDRESSES 取.
	 * 这里用一次 GetAdaptersAddresses 同时拿 IP 和 IPv4Mask */
	ULONG bufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
	int cnt = 0;

	if (pAddrs == NULL) {
		return 0;
	}

	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
		      GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;

	if (GetAdaptersAddresses(AF_INET, flags, NULL, pAddrs, &bufLen) != NO_ERROR) {
		free(pAddrs);
		return 0;
	}

	PIP_ADAPTER_ADDRESSES p = pAddrs;
	while (p && cnt < max_cnt) {
		/* 跳过未启用的适配器 */
		if (p->OperStatus != IfOperStatusUp) {
			p = p->Next;
			continue;
		}
		/* 跳过回环和虚拟隧道 (常见 VPN/虚拟网卡) */
		if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    p->IfType == IF_TYPE_TUNNEL) {
			p = p->Next;
			continue;
		}

		PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress;
		while (ua && cnt < max_cnt) {
			struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
			unsigned long ip = sa->sin_addr.s_addr;
			ULONG plen;
			unsigned long mask, bcast;

			/* 跳过回环 (127.x), 未配置 (0.x), link-local (169.254.x) */
			if ((ip & htonl(0xFF000000)) == htonl(0x7F000000) ||
			    (ip & htonl(0xFFFF0000)) == htonl(0xA9FE0000) ||
			    ip == 0) {
				ua = ua->Next;
				continue;
			}

			/* OnLinkPrefixLength = IPv4 前缀长度 (Win Vista+).
			 * 定向广播 = (ip & mask) | ~mask */
			plen = ua->OnLinkPrefixLength;
			mask = (plen == 0) ? 0 : htonl(0xFFFFFFFF << (32 - plen));
			bcast = (ip & mask) | ~mask;

			addrs[cnt++] = bcast;
			ua = ua->Next;
		}
		p = p->Next;
	}

	free(pAddrs);
	return cnt;
}

/* 收集本机各网卡的子网定向广播地址 (点分十进制), 用于 UI 下拉框填充. */
int UdpManager_GetBroadcastAddrs(char addrs[][16], int cap)
{
	ULONG bufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
	int cnt = 0;

	if (pAddrs == NULL || cap <= 0) {
		return 0;
	}

	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
		      GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;

	if (GetAdaptersAddresses(AF_INET, flags, NULL, pAddrs, &bufLen) != NO_ERROR) {
		free(pAddrs);
		return 0;
	}

	for (PIP_ADAPTER_ADDRESSES p = pAddrs; p && cnt < cap; p = p->Next) {
		if (p->OperStatus != IfOperStatusUp ||
		    p->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    p->IfType == IF_TYPE_TUNNEL) {
			continue;
		}
		for (PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress; ua && cnt < cap; ua = ua->Next) {
			struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
			unsigned long ip = sa->sin_addr.s_addr;

			if ((ip & htonl(0xFF000000)) == htonl(0x7F000000) ||
			    (ip & htonl(0xFFFF0000)) == htonl(0xA9FE0000) ||
			    ip == 0) {
				continue;
			}
			ULONG plen = ua->OnLinkPrefixLength;
			if (plen == 0 || plen >= 32) {
				continue;
			}
			unsigned long mask = htonl(0xFFFFFFFF << (32 - plen));
			unsigned long bcast = (ip & mask) | ~mask;
			struct in_addr ba;

			ba.s_addr = bcast;
			strncpy(addrs[cnt], inet_ntoa(ba), 15);
			addrs[cnt][15] = '\0';
			cnt++;
		}
	}

	free(pAddrs);
	return cnt;
}

/* 判断给定 IP (网络序 s_addr) 是否与本机某个已启用网卡在同一子网.
 * 用于 RX 学习对端地址前验证: 跨子网时不切单播 (单播发不过去, 保持广播). */
static bool is_local_subnet(unsigned long ip)
{
	ULONG bufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
	bool same = false;

	if (pAddrs == NULL) {
		return false;
	}

	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
		      GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;

	if (GetAdaptersAddresses(AF_INET, flags, NULL, pAddrs, &bufLen) != NO_ERROR) {
		free(pAddrs);
		return false;
	}

	for (PIP_ADAPTER_ADDRESSES p = pAddrs; p && !same; p = p->Next) {
		if (p->OperStatus != IfOperStatusUp ||
		    p->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    p->IfType == IF_TYPE_TUNNEL) {
			continue;
		}
		for (PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
			struct sockaddr_in *sa = (struct sockaddr_in *)ua->Address.lpSockaddr;
			unsigned long localip = sa->sin_addr.s_addr;

			if ((localip & htonl(0xFF000000)) == htonl(0x7F000000) ||
			    (localip & htonl(0xFFFF0000)) == htonl(0xA9FE0000) ||
			    localip == 0) {
				continue;
			}
			ULONG plen = ua->OnLinkPrefixLength;
			if (plen == 0 || plen >= 32) {
				continue;
			}
			unsigned long mask = htonl(0xFFFFFFFF << (32 - plen));
			if ((ip & mask) == (localip & mask)) {
				same = true;
				break;
			}
		}
	}

	free(pAddrs);
	return same;
}


struct UdpManager {
	SOCKET sock;
	UdpChannel chan;                 /* 本实例通道类型 (RX 分发用) */
	struct sockaddr_in remote_addr;
	bool bound;
	bool broadcast_mode;             /* 目标 IP 空 = 广播: 发送时遍历所有网卡广播地址 */

	/* 广播地址列表 (broadcast_mode 下发送时遍历). 单播模式下为空, 用 remote_addr */
	unsigned long bcast_addrs[8];
	int bcast_cnt;

	/* 同步等待响应的状态 (仅配置通道使用) */
	volatile uint8_t pending_cmd;    /* 正在等待的命令码, 0 = 无 */
	uint8_t resp_buf[300];
	size_t resp_len;
	HANDLE resp_event;

	udp_msg_callback msg_cb;
	void *msg_data;
	udp_data_callback data_cb;
	void *data_data;

	HANDLE rx_thread;
	volatile bool rx_running;
};

UdpManager *UdpManager_Create(void)
{
	UdpManager *mgr = (UdpManager *)calloc(1, sizeof(UdpManager));
	if (mgr) {
		mgr->sock = INVALID_SOCKET;
		mgr->resp_event = CreateEvent(NULL, TRUE, FALSE, NULL);
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
	return mgr;
}

void UdpManager_Destroy(UdpManager *mgr)
{
	if (!mgr) return;
	UdpManager_Unbind(mgr);
	if (mgr->resp_event) CloseHandle(mgr->resp_event);
	WSACleanup();
	free(mgr);
}

bool UdpManager_Bind(UdpManager *mgr, UdpChannel chan,
                     uint16_t local_port, const char *remote_ip, uint16_t remote_port)
{
	if (!mgr) return false;

	if (mgr->sock != INVALID_SOCKET) {
		closesocket(mgr->sock);
		mgr->sock = INVALID_SOCKET;
	}

	mgr->chan = chan;
	mgr->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (mgr->sock == INVALID_SOCKET) return false;

	/* 强制端口独占 (SO_EXCLUSIVEADDRUSE 必须在 bind 前设). 端口被其他程序占用时
	 * bind 失败 → Bind 返回 false → 调用方弹窗报错, 避免静默"连接成功但收不到包".
	 * (Windows UDP 默认不强制独占; SO_REUSEADDR 更是允许抢占, 这里明确禁用) */
	BOOL exclusive = TRUE;
	setsockopt(mgr->sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		   (const char *)&exclusive, sizeof(exclusive));

	/* 允许广播收发 (Windows 发广播必须设 SO_BROADCAST, 须在 bind 前设) */
	BOOL broadcast = TRUE;
	setsockopt(mgr->sock, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast));

	/* 本地: 绑 0.0.0.0:local_port (可收广播, 多网卡由路由表自动选路) */
	struct sockaddr_in local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin_family = AF_INET;
	local_addr.sin_port = htons(local_port);
	local_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(mgr->sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		closesocket(mgr->sock);
		mgr->sock = INVALID_SOCKET;
		return false;
	}

	/* 远程目标 = remote_ip:remote_port.
	 *   remote_ip = "255.255.255.255" → 有限广播 (255.255.255.255, 唯一能跨网段到达的方式)
	 *   remote_ip 留空/0.0.0.0/非法   → 子网定向广播自动发现 (收集各网卡广播地址)
	 *   其它                          → 单播到该 IP
	 * 收到对端包后 RX 线程会学习并覆盖 remote_addr (单播时与指定一致).
	 * 显式重置 broadcast_mode, 避免上次会话残留 (IP 学习会把广播切为单播). */
	bool unicast = false;
	bool limited_bcast = false;     /* 显式 255.255.255.255: 有限广播 (跨网段可达) */
	memset(&mgr->remote_addr, 0, sizeof(mgr->remote_addr));
	mgr->remote_addr.sin_family = AF_INET;
	mgr->remote_addr.sin_port = htons(remote_port);
	if (remote_ip && *remote_ip) {
		/* inet_addr("255.255.255.255") 在 Winsock 返回 INADDR_NONE (与 0xFFFFFFFF 同值),
		 * 故用字符串比较显式识别有限广播, 避免歧义. */
		if (strcmp(remote_ip, "255.255.255.255") == 0) {
			limited_bcast = true;
		} else {
			unsigned long a = inet_addr(remote_ip);
			if (a != INADDR_NONE && a != 0) {
				mgr->remote_addr.sin_addr.s_addr = a;
				unicast = true;
			}
		}
	}
	mgr->broadcast_mode = false;   /* 默认单播; 下方广播分支会覆盖 */
	if (!unicast) {
		mgr->broadcast_mode = true;
		if (limited_bcast) {
			/* 显式有限广播 255.255.255.255: 唯一能跨网段到达设备的方式 (路由器/驱动层转发).
			 * 子网定向广播 (x.x.x.255) 只在本地子网有效, 跨网段到不了设备. */
			mgr->bcast_addrs[0] = INADDR_BROADCAST;
			mgr->bcast_cnt = 1;
		} else {
			/* 自动发现: 收集所有非回环网卡的子网定向广播地址, 发送时逐个发出,
			 * 确保板子无论连哪个网卡都能收到 */
			mgr->bcast_cnt = collect_broadcast_addrs(mgr->bcast_addrs,
								 (int)(sizeof(mgr->bcast_addrs) / sizeof(mgr->bcast_addrs[0])));
			if (mgr->bcast_cnt == 0) {
				/* 兜底: 取不到网卡信息时退回有限广播 */
				mgr->bcast_addrs[0] = INADDR_BROADCAST;
				mgr->bcast_cnt = 1;
			}
		}
		mgr->remote_addr.sin_addr.s_addr = mgr->bcast_addrs[0];  /* 供提示消息显示 */
	}

	mgr->bound = true;

	if (mgr->msg_cb) {
		/* local_port=0 时 OS 分配临时端口, getsockname 取实际端口显示.
		 * (提示消息是唯一需要真实端口的地方; 业务收发不依赖此值) */
		uint16_t actual_port = local_port;
		struct sockaddr_in bound_addr;
		int balen = sizeof(bound_addr);
		if (getsockname(mgr->sock, (struct sockaddr *)&bound_addr, &balen) == 0) {
			actual_port = ntohs(bound_addr.sin_port);
		}
		const char *chan_name = (chan == UDP_CHAN_CONFIG) ? "配置" : "数据";
		char buf[128];
		if (unicast) {
			sprintf(buf, "UDP %s通道已连接: 本地 %d → %s:%d", chan_name,
				actual_port, remote_ip, remote_port);
		} else {
			struct in_addr b;
			b.s_addr = mgr->remote_addr.sin_addr.s_addr;
			sprintf(buf, "UDP %s通道已连接: 本地 %d → 广播 %s:%d", chan_name,
				actual_port, inet_ntoa(b), remote_port);
		}
		mgr->msg_cb(buf, mgr->msg_data);
	}

	return true;
}

void UdpManager_Unbind(UdpManager *mgr)
{
	if (!mgr) return;
	UdpManager_StopRxThread(mgr);
	if (mgr->sock != INVALID_SOCKET) {
		closesocket(mgr->sock);
		mgr->sock = INVALID_SOCKET;
	}
	mgr->bound = false;
}

bool UdpManager_IsBound(UdpManager *mgr)
{
	return mgr && mgr->bound;
}

/* 内部发送: 广播模式遍历所有网卡广播地址逐个发, 单播发 remote_addr.
 * 只要至少一个发送成功即返回 true */
static bool udp_send_raw(UdpManager *mgr, const uint8_t *buf, int len)
{
	if (mgr->broadcast_mode) {
		bool any_ok = false;
		for (int i = 0; i < mgr->bcast_cnt; i++) {
			struct sockaddr_in dst;
			memset(&dst, 0, sizeof(dst));
			dst.sin_family = AF_INET;
			dst.sin_port = mgr->remote_addr.sin_port;
			dst.sin_addr.s_addr = mgr->bcast_addrs[i];
			if (sendto(mgr->sock, (const char *)buf, len, 0,
				   (struct sockaddr *)&dst, sizeof(dst)) == len) {
				any_ok = true;
			}
		}
		return any_ok;
	}

	int sent = sendto(mgr->sock, (const char *)buf, len, 0,
			  (struct sockaddr *)&mgr->remote_addr,
			  sizeof(mgr->remote_addr));
	return (sent == len);
}

bool UdpManager_SendData(UdpManager *mgr, const uint8_t *data, size_t len)
{
	if (!mgr || !mgr->bound || mgr->sock == INVALID_SOCKET) return false;

	return udp_send_raw(mgr, data, (int)len);
}

bool UdpManager_SendCommand(UdpManager *mgr, uint8_t cmd, const uint8_t *data, uint8_t len)
{
	if (!mgr || !mgr->bound || mgr->sock == INVALID_SOCKET) return false;

	/* [cmd 1B][data...] 无魔数头 */
	uint8_t buf[260] = {0};
	buf[0] = cmd;
	if (data && len > 0 && len < sizeof(buf) - 1) {
		memcpy(buf + 1, data, len);
	}

	return udp_send_raw(mgr, buf, (int)(len + 1));
}

/* fw_exchange 定义在下方固件升级段, 此处前置声明供 UdpManager_SetIp 使用 */
static int fw_exchange(UdpManager *mgr, uint8_t cmd,
		       const uint8_t *data, uint16_t data_len,
		       uint8_t *out_buf, uint8_t out_cap, DWORD timeout_ms);

/* 设置设备静态 IP: [ip 4B BE] = 4B → [1B: 1=成功/0=失败].
 * 持久化, 重启生效. 失败: IP 非法 (0.0.0.0/环回/组播/广播/保留段) 或 DHCP 模式.
 * 掩码固定 255.255.255.0, 网关 = IP 末段改 1, 固件自算, 上位机不传. */
bool UdpManager_SetIp(UdpManager *mgr, const char *ip, bool *out_ok)
{
	uint8_t data[4] = {0};

	if (ip) {
		struct in_addr a;

		a.s_addr = inet_addr(ip);
		if (a.s_addr == INADDR_NONE) return false;
		memcpy(data, &a.s_addr, 4);
	}

	uint8_t resp = 0;
	int n = fw_exchange(mgr, UDP_CMD_SET_IP, data, 4, &resp, 1, UDP_RESP_TIMEOUT_MS);

	bool ok = (n == 1 && resp == 1);
	if (out_ok) *out_ok = ok;
	/* 收到回复 (无论成功/失败) 即返回 true; out_ok 区分结果 */
	return (n == 1);
}

/* 设置 RF24 地址: [addr 5B] = 5B → 回显 5B. 信道固定 1, 不在帧中. */
bool UdpManager_SetRF24(UdpManager *mgr, const uint8_t *addr)
{
	uint8_t data[5] = {0};

	if (addr) memcpy(data, addr, 5);

	return UdpManager_SendCommand(mgr, UDP_CMD_SET_RF24, data, sizeof(data));
}

/* 通用: 发送命令并同步等待同命令码的响应.
 * 成功时 resp_buf/resp_len 填入响应 [cmd][data...] 中 data 部分 (去掉首字节 cmd).
 * 返回 true = 收到响应. */
static bool send_and_wait(UdpManager *mgr, uint8_t cmd,
			  const uint8_t *data, uint8_t len)
{
	if (!mgr || !mgr->bound) return false;

	ResetEvent(mgr->resp_event);
	mgr->resp_len = 0;
	mgr->pending_cmd = cmd;

	bool ok = UdpManager_SendCommand(mgr, cmd, data, len);
	if (!ok) {
		mgr->pending_cmd = 0;
		return false;
	}

	DWORD wr = WaitForSingleObject(mgr->resp_event, UDP_RESP_TIMEOUT_MS);
	mgr->pending_cmd = 0;

	return (wr == WAIT_OBJECT_0) && (mgr->resp_len > 0);
}

/* 查询网络参数: (空) → [data_port 2B][host_ip 4B][host_port 2B] = 8B.
 * host_ip 为点分十进制输出缓冲 (容量 host_ip_len, 至少 16); 三组出参均可空.
 * 注: 配置端口不在此响应中, 需用 Discover 获取. */
bool UdpManager_GetNet(UdpManager *mgr, uint16_t *data_port,
		       char *host_ip, size_t host_ip_len, uint16_t *host_port)
{
	if (host_ip && host_ip_len < 16) return false;

	if (!send_and_wait(mgr, UDP_CMD_GET_NET, NULL, 0)) {
		return false;
	}
	if (mgr->resp_len < 8) {
		return false;
	}

	const uint8_t *p = mgr->resp_buf;

	if (data_port) {
		*data_port = ((uint16_t)p[0] << 8) | p[1];
	}
	if (host_ip) {
		sprintf(host_ip, "%u.%u.%u.%u", p[2], p[3], p[4], p[5]);
	}
	if (host_port) {
		*host_port = ((uint16_t)p[6] << 8) | p[7];
	}
	return true;
}

/* 查询 RF24 地址: (空) → [addr 5B] = 5B. 信道固定 1, 不返回. addr 为 5B 出参缓冲 (可空). */
bool UdpManager_GetRF24(UdpManager *mgr, uint8_t *addr)
{
	if (!send_and_wait(mgr, UDP_CMD_GET_RF24, NULL, 0)) {
		return false;
	}
	if (mgr->resp_len < 5) {
		return false;
	}

	if (addr) memcpy(addr, mgr->resp_buf, 5);
	return true;
}

/* 设置上位机目标: [host_ip 4B BE][port 2B BE] = 6B → 同序回显 6B. 持久化, 即时生效.
 * 固件把 nRF24 数据固定单播到此 ip:port (不再广播/学习发送方). 仅发命令, 不等回复. */
bool UdpManager_SetHost(UdpManager *mgr, const char *ip, uint16_t port)
{
	uint8_t data[6] = {0};

	if (ip) {
		struct in_addr a;

		a.s_addr = inet_addr(ip);
		memcpy(data + 0, &a.s_addr, 4);
	}
	data[4] = (port >> 8) & 0xFF;
	data[5] = port & 0xFF;

	return UdpManager_SendCommand(mgr, UDP_CMD_SET_HOST, data, sizeof(data));
}

/* 恢复出厂设置: (空) → [1B: 1=成功/0=失败].
 * 固件内部恢复全部出厂参数 (RF24/IP/HOST 等) 并自行重启, 上位机无需再发 Reboot. */
bool UdpManager_FactoryReset(UdpManager *mgr, bool *out_ok)
{
	uint8_t resp = 0;
	int n = fw_exchange(mgr, UDP_CMD_FACTORY_RESET, NULL, 0, &resp, 1, UDP_RESP_TIMEOUT_MS);

	bool ok = (n == 1 && resp == 1);
	if (out_ok) *out_ok = ok;
	/* 收到回复 (无论成功/失败) 即返回 true; out_ok 区分结果 */
	return (n == 1);
}

/* DISCOVER (0x15): 广播发现设备. (空) → [ip 4B BE][config_port 2B BE] = 6B.
 * ip 为设备本机 IP (点分十进制, 容量 ip_len ≥ 16); config_port 出参可空.
 * 同步等待回复 (500ms 超时). */
bool UdpManager_Discover(UdpManager *mgr, char *ip, size_t ip_len, uint16_t *config_port)
{
	if (ip && ip_len < 16) return false;

	if (!send_and_wait(mgr, UDP_CMD_DISCOVER, NULL, 0)) {
		return false;
	}
	if (mgr->resp_len < 6) {
		return false;
	}

	const uint8_t *p = mgr->resp_buf;

	if (ip) {
		sprintf(ip, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
	}
	if (config_port) {
		*config_port = ((uint16_t)p[4] << 8) | p[5];
	}
	return true;
}

bool UdpManager_GetVersion(UdpManager *mgr, char *buf, size_t buf_len)
{
	if (!buf || buf_len == 0) return false;

	if (!send_and_wait(mgr, UDP_CMD_GET_VERSION, NULL, 0)) {
		return false;
	}

	size_t n = mgr->resp_len;
	if (n >= buf_len) n = buf_len - 1;
	memcpy(buf, mgr->resp_buf, n);
	buf[n] = '\0';
	return true;
}

bool UdpManager_Reboot(UdpManager *mgr)
{
	return UdpManager_SendCommand(mgr, UDP_CMD_REBOOT, NULL, 0);
}

/* 通用: 发 [cmd][data...] 并等回复. 回复的 data 部分填入 out_buf (去掉首字节 cmd).
 * 返回回复 data 长度; -1 = 失败/超时. data_len 用 uint16_t 避免 256B 溢出 */
static int fw_exchange(UdpManager *mgr, uint8_t cmd,
		       const uint8_t *data, uint16_t data_len,
		       uint8_t *out_buf, uint8_t out_cap, DWORD timeout_ms)
{
	if (!mgr || !mgr->bound) return -1;
	if (data_len > 511) return -1;

	uint8_t buf[512];

	buf[0] = cmd;
	if (data && data_len > 0) {
		memcpy(buf + 1, data, data_len);
	}

	ResetEvent(mgr->resp_event);
	mgr->resp_len = 0;
	mgr->pending_cmd = cmd;

	bool ok = udp_send_raw(mgr, buf, (int)(data_len + 1));
	if (!ok) {
		mgr->pending_cmd = 0;
		return -1;
	}

	DWORD wr = WaitForSingleObject(mgr->resp_event, timeout_ms);
	mgr->pending_cmd = 0;

	if (wr != WAIT_OBJECT_0 || mgr->resp_len < 1) {
		return -1;
	}
	int n = (int)mgr->resp_len;

	if (n > out_cap) n = out_cap;
	if (out_buf && n > 0) {
		memcpy(out_buf, mgr->resp_buf, n);
	}
	return n;
}

uint16_t UdpManager_CRC16_CCITT(const uint8_t *data, size_t len)
{
	/* 与 Zephyr crc16_ccitt (subsys/crc/crc16_sw.c) 完全一致的实现.
	 * 注意: 这是 Zephyr 特化的 bit-reflected 变体, 非标准 MSB-first CCITT. */
	uint16_t seed = 0x0000;

	for (; len > 0; len--) {
		uint8_t e, f;

		e = (uint8_t)seed ^ *data;
		++data;
		f = (uint8_t)(e ^ (e << 4));
		seed = (uint16_t)((seed >> 8) ^ ((uint16_t)f << 8) ^ ((uint16_t)f << 3) ^
				  ((uint16_t)f >> 4));
	}
	return seed;
}

bool UdpManager_FirmwareStart(UdpManager *mgr, uint32_t size, const uint8_t *keyhash,
			      uint8_t *out_resp)
{
	uint8_t data[36];
	unsigned dlen = 4;

	data[0] = size & 0xFF;
	data[1] = (size >> 8) & 0xFF;
	data[2] = (size >> 16) & 0xFF;
	data[3] = (size >> 24) & 0xFF;

	/* 携带 32B keyhash (从签名镜像提取) 供 FW 端校验, 不一致回状态 2 拒绝.
	 * keyhash 为 NULL 则退回旧 4B 帧 (无校验, 兼容). */
	if (keyhash) {
		memcpy(data + 4, keyhash, 32);
		dlen = 36;
	}

	uint8_t resp = 0;
	/* FW_START 擦除整个 slot1 分区, 耗时较长, 给 5s 超时 */
	int n = fw_exchange(mgr, UDP_CMD_FW_START, data, dlen, &resp, 1, 5000);

	if (out_resp) *out_resp = resp;
	/* resp: 0=失败, 1=成功, 2=keyhash 不一致被拒绝 */
	return (n == 1 && resp == 1);
}

bool UdpManager_FirmwareData(UdpManager *mgr, const uint8_t *data, size_t len,
			     uint32_t expected_offset, uint32_t *got_offset)
{
	uint8_t resp[4];
	int n = fw_exchange(mgr, UDP_CMD_FW_DATA, data, (uint16_t)len, resp, 4, 1000);

	if (n != 4) return false;
	uint32_t off = resp[0] | (resp[1] << 8) | (resp[2] << 16) | ((uint32_t)resp[3] << 24);

	if (got_offset) *got_offset = off;
	return (off == expected_offset);
}

bool UdpManager_FirmwareEnd(UdpManager *mgr, uint8_t test_mode, uint16_t crc16)
{
	uint8_t data[3];

	data[0] = test_mode;
	data[1] = crc16 & 0xFF;
	data[2] = (crc16 >> 8) & 0xFF;

	uint8_t resp;
	/* FW_END: flush + 读回 slot1 重算 CRC, 耗时较长, 给 10s 超时 */
	int n = fw_exchange(mgr, UDP_CMD_FW_END, data, 3, &resp, 1, 10000);

	return (n == 1 && resp == 1);
}

/* 处理配置通道收到的响应包 [cmd][data...].
 * 若是当前正在等待的命令, 唤醒同步调用; 否则上报 msg_cb */
static void handle_config_response(UdpManager *mgr, const uint8_t *buf, size_t len)
{
	if (len < 1) return;
	uint8_t cmd = buf[0];

	if (mgr->pending_cmd != 0 && cmd == mgr->pending_cmd) {
		size_t n = len - 1;
		if (n > sizeof(mgr->resp_buf)) n = sizeof(mgr->resp_buf);
		memcpy(mgr->resp_buf, buf + 1, n);
		mgr->resp_len = n;
		SetEvent(mgr->resp_event);
		return;
	}

	if (mgr->msg_cb) {
		char msg[128];
		sprintf(msg, "UDP 响应: cmd=0x%02x 长度=%d", cmd, (int)(len - 1));
		mgr->msg_cb(msg, mgr->msg_data);
	}
}

static DWORD WINAPI udp_rx_thread_proc(LPVOID param)
{
	UdpManager *mgr = (UdpManager *)param;
	uint8_t buf[600];

	while (mgr->rx_running) {
		if (!mgr->bound) {
			Sleep(10);
			continue;
		}

		struct sockaddr_in src;
		int alen = sizeof(src);
		int received = recvfrom(mgr->sock, (char *)buf, sizeof(buf), 0,
					(struct sockaddr *)&src, &alen);
		if (received <= 0) {
			continue;
		}

		/* 学习发送方地址 (后续发包到此).
		 * 仅当对端与本机同子网时才切单播; 跨子网 (如设备经路由回复) 单播发不过去,
		 * 此时保持广播模式, 确保后续通信可达. */
		bool same = is_local_subnet(src.sin_addr.s_addr);
		if (same) {
			mgr->remote_addr = src;
			mgr->broadcast_mode = false;
		}

		/* 按 chan 分发 */
		if (mgr->chan == UDP_CHAN_CONFIG) {
			handle_config_response(mgr, buf, (size_t)received);
		} else if (mgr->data_cb) {
			mgr->data_cb(buf, (size_t)received, mgr->data_data);
		}
	}

	return 0;
}

void UdpManager_StartRxThread(UdpManager *mgr)
{
	if (!mgr || mgr->rx_running) return;
	mgr->rx_running = true;
	mgr->rx_thread = CreateThread(NULL, 0, udp_rx_thread_proc, mgr, 0, NULL);
}

void UdpManager_StopRxThread(UdpManager *mgr)
{
	if (!mgr || !mgr->rx_running) return;
	mgr->rx_running = false;

	/* 先关闭 socket 强制让阻塞在 recvfrom 的 RX 线程立即返回 (收到错误),
	 * 否则要等 WaitForSingleObject 超时 (2s) 才退出 → 断开按钮卡顿 */
	if (mgr->sock != INVALID_SOCKET) {
		closesocket(mgr->sock);
		mgr->sock = INVALID_SOCKET;
	}

	if (mgr->rx_thread) {
		WaitForSingleObject(mgr->rx_thread, 1000);
		CloseHandle(mgr->rx_thread);
		mgr->rx_thread = NULL;
	}
}

void UdpManager_SetMsgCallback(UdpManager *mgr, udp_msg_callback cb, void *data)
{
	if (mgr) { mgr->msg_cb = cb; mgr->msg_data = data; }
}

void UdpManager_SetDataCallback(UdpManager *mgr, udp_data_callback cb, void *data)
{
	if (mgr) { mgr->data_cb = cb; mgr->data_data = data; }
}
