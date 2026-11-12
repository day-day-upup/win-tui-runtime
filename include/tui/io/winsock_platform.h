#pragma once

// 阶段 4：集中管理 Winsock 头文件。
// 必须放在 windows.h 之后（platform.h 已定义 WIN32_LEAN_AND_MEAN，
// 否则 winsock.h 与 winsock2.h 会冲突）。
#include "tui/core/platform.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>   // AcceptEx / LPFN_ACCEPTEX / WSAID_ACCEPTEX

#pragma comment(lib, "ws2_32.lib")
