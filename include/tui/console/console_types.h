#pragma once

#include <cstdint>

#include "tui/core/platform.h"

// 阶段 2：控制台基础类型
//
// 内部统一用 UTF-16 单元（CHAR_INFO 的粒度，约束 D6）。
// 第一版交互只保证 ASCII（约束 D7），因此颜色/字符都用简单值即可。
namespace tui::console {

// 前景/背景色，直接对应 Win32 控制台属性低 4 位 / 高 4 位。
enum class Color : uint8_t {
    kBlack = 0,
    kBlue = 1,
    kGreen = 2,
    kCyan = 3,
    kRed = 4,
    kMagenta = 5,
    kYellow = 6,
    kWhite = 7,
    kGray = 8,
    kBrightBlue = 9,
    kBrightGreen = 10,
    kBrightCyan = 11,
    kBrightRed = 12,
    kBrightMagenta = 13,
    kBrightYellow = 14,
    kBrightWhite = 15,
};

inline WORD MakeAttr(Color fg, Color bg) {
    return static_cast<WORD>((static_cast<WORD>(bg) << 4) | static_cast<WORD>(fg));
}

inline Color FgOf(WORD a) { return static_cast<Color>(a & 0x0F); }
inline Color BgOf(WORD a) { return static_cast<Color>((a >> 4) & 0x0F); }

// 一个单元格。与 CHAR_INFO 语义一致，但用我们自己的 POD 表示，
// 便于内存后端与单元测试不依赖真实控制台。
struct Cell {
    wchar_t ch = L' ';
    WORD attr = MakeAttr(Color::kGray, Color::kBlack);

    bool operator==(const Cell& o) const { return ch == o.ch && attr == o.attr; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

struct Size {
    int16_t w = 0;
    int16_t h = 0;
    bool operator==(const Size& o) const { return w == o.w && h == o.h; }
    bool operator!=(const Size& o) const { return !(*this == o); }
};

struct Point {
    int16_t x = 0;
    int16_t y = 0;
};

// 单个脏单元格的变更（用于差分与内存后端记录）。
struct CellDelta {
    Point pos;
    Cell cell;
};

}  // namespace tui::console
