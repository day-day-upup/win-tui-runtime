#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tui/console/console_types.h"

// 阶段 2：ScreenBuffer —— 双缓冲 + dirty-cell 差分
//
// 这是约束 D3 的落点。两个缓冲：
//   - front_：上一次 Present 到后端的内容（「屏幕现在是什么」）；
//   - back_ ：本帧应用绘制后的目标内容（「屏幕应该是什么」）。
//
// 流程：Clear/DrawXxx 只改 back_ 并标脏；Commit() 比较 front_ 与 back_，
// 只把不同的单元格产出 CellDelta，然后交换 front_/back_。
//
// 为什么要「双缓冲 + 差分」而不是直接写：
//   1) 事件驱动下多个 source 在一 tick 内各自改画面，若来一个字节就刷屏
//      会产生大量 WriteConsoleOutput 调用和闪烁；按 tick 合并 + 差分把它压到最小；
//   2) 差分天然保证「不撕裂」：一帧只有一次 Present，且只写变化单元格。
//
// Resize：尺寸变化时重建两个缓冲，并整屏标脏（否则新增区域不会刷新）。
namespace tui::console {

class ScreenBuffer {
public:
    explicit ScreenBuffer(Size size);

    Size GetSize() const { return size_; }
    void Resize(Size s);

    // --- 绘制 API（只改 back_，标脏）---------------------------------
    void Clear(Cell fill);
    void Put(int16_t x, int16_t y, Cell cell);
    void Put(int16_t x, int16_t y, const std::wstring& text, WORD attr);
    void FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Cell cell);

    // 计算 front_ 与 back_ 的差异，返回 deltas，并把 front_ 更新为 back_。
    // 返回的 deltas 是「本帧需要写入后端的全部变更」。
    std::vector<CellDelta> Commit();

    // 调试/测试：读取 back_ / front_ 某单元格。
    Cell Back(int16_t x, int16_t y) const;
    Cell Front(int16_t x, int16_t y) const;

private:
    size_t Index(int16_t x, int16_t y) const {
        return static_cast<size_t>(y) * size_.w + x;
    }
    bool InRange(int16_t x, int16_t y) const {
        return x >= 0 && y >= 0 && x < size_.w && y < size_.h;
    }

    Size size_;
    std::vector<Cell> front_;
    std::vector<Cell> back_;
    // 标记 back_ 中「本帧被写过」的单元格。Commit 时只需检查这些位置，
    // 避免每帧 O(w*h) 全扫描（大缓冲下有意义）。
    std::vector<uint8_t> dirty_;
    // Resize 后置位：强制把整屏写入后端，清除真实控制台上的旧内容残留。
    bool force_full_ = false;
};

}  // namespace tui::console
