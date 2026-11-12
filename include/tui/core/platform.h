#pragma once

// 阶段 1：集中管理 Win32 头文件。
// 统一在这里定义 NOMINMAX / WIN32_LEAN_AND_MEAN，避免 windows.h 的
// min/max 宏污染 std::min/std::max 与 std::numeric_limits。
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
