#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tui/core/platform.h"

// 阶段 2：InputDecoder —— INPUT_RECORD → InputEvent（纯函数，可单测）
//
// 约束 B7：输入线程独占 ReadConsoleInput；但「解码」不该绑死在线程上，
// 因此单独抽成无状态的 Decode()，输入线程只负责读 + 投递。
// 约束 D7：第一版交互只保证 ASCII；非 ASCII 的 UnicodeChar 仍被保留在
// ch 里（UTF-16 单元），但不做 IME/组合键处理。
namespace tui::console {

enum class InputKind {
    kKey,
    kMouse,
    kResize,
    kFocus,
    kUnknown,
};

// 修饰键位（对应 KEY_EVENT_RECORD.dwControlKeyState 的常用位）。
namespace Mod {
constexpr uint32_t kShift = 0x01;
constexpr uint32_t kCtrl = 0x02;
constexpr uint32_t kAlt = 0x04;
constexpr uint32_t kCapsLock = 0x80;
constexpr uint32_t kNumLock = 0x20;
constexpr uint32_t kScrollLock = 0x40;
constexpr uint32_t kEnhanced = 0x100;
}  // namespace Mod

struct InputEvent {
    InputKind kind = InputKind::kUnknown;

    // --- kKey ---
    bool key_down = false;
    wchar_t ch = 0;             // 可打印字符（ASCII 为主）
    uint16_t vk = 0;            // 虚拟键码
    uint16_t repeat = 0;
    uint32_t mods = 0;          // Mod:: 位组合

    // --- kMouse ---
    int16_t mouse_x = 0;
    int16_t mouse_y = 0;
    uint32_t mouse_buttons = 0;
    uint32_t mouse_event_flags = 0;

    // --- kResize ---
    int16_t width = 0;
    int16_t height = 0;

    // --- kFocus ---
    bool focus = false;

    // 便于日志显示
    std::wstring Describe() const;
};

// 单个 INPUT_RECORD → InputEvent。对不支持的记录返回 kUnknown。
InputEvent Decode(const INPUT_RECORD& rec);

// 批量解码（例如 ReadConsoleInput 一次读到多个）。
std::vector<InputEvent> DecodeAll(const INPUT_RECORD* recs, size_t count);

}  // namespace tui::console
