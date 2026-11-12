#pragma once

#include <cstddef>
#include <memory>

#include "tui/console/render_backend.h"
#include "tui/console/screen_buffer.h"

// 阶段 2：Renderer —— 绘图门面 + 按 tick 合并的 Present（约束 D4）
//
// 用法（在 loop 线程里）：
//   1) 一个或多个事件回调里调用 Draw/Put/Clear 改 back_，并 MarkDirty()；
//   2) 一个 PostRepeatingTask(约 16ms) 触发 RenderTick()；
//   3) RenderTick() 只在「脏」时 Commit + backend.Present，一次 tick 合并所有改动。
//
// 关键点：输入数据不直接触发刷屏；多个 source 在一 tick 内的改动被合并成一帧，
// 且 Commit 只产出变化单元格 → 既不撕裂，也不浪费系统调用。
namespace tui::console {

class Renderer {
public:
    Renderer(std::unique_ptr<IRenderBackend> backend, Size size);

    Size GetSize() const { return buffer_.GetSize(); }
    IRenderBackend& Backend() { return *backend_; }
    ScreenBuffer& Buffer() { return buffer_; }

    // 标记本 tick 有内容改动。Resize 内部会自动标脏。
    void MarkDirty() { dirty_ = true; }
    void Resize(Size s);

    // 绘制便捷方法（都作用于 back_ 并自动 MarkDirty）。
    void Clear(Cell fill);
    void Put(int16_t x, int16_t y, Cell cell);
    void Put(int16_t x, int16_t y, const std::wstring& text, WORD attr);
    void FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Cell cell);

    // 由渲染 tick 调用。返回 true 表示确实向后端写了一帧。
    bool RenderTick();

    // 统计：便于验证「按 tick 合并」与「差分」确实生效。
    size_t PresentCount() const { return present_count_; }
    size_t TotalDeltas() const { return total_deltas_; }

private:
    std::unique_ptr<IRenderBackend> backend_;
    ScreenBuffer buffer_;
    bool dirty_ = true;  // 首帧强制输出
    size_t present_count_ = 0;
    size_t total_deltas_ = 0;
};

}  // namespace tui::console
