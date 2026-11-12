#pragma once

#include <vector>

#include "tui/console/console_types.h"
#include "tui/core/platform.h"

// 阶段 2：IRenderBackend —— 渲染后端抽象
//
// 约束 D1：主路径 legacy WriteConsoleOutputW。
// 约束 D2：VT/ANSI 只能作可选后端，不是默认。
// 这里再提供 MemoryBackend：把「本帧写了什么」记下来，供单元测试与
// 无控制台环境（selftest）验证 dirty-cell 差分的正确性。
//
// 后端只做一件事：把一批 CellDelta 落地。差分由 ScreenBuffer/Renderer 负责，
// 后端不关心「哪些该写」——保持抽象薄。
namespace tui::console {

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual bool Present(const std::vector<CellDelta>& deltas) = 0;
    virtual Size GetSize() const = 0;
    virtual const char* Name() const = 0;
};

// legacy 主路径：对每行连续的脏单元格合并成一次 WriteConsoleOutputW。
// 这就是「差分 + run-length 合并」减少系统调用的地方。
class LegacyConsoleBackend final : public IRenderBackend {
public:
    // 不接管 h 的所有权；由 ConsoleSession 负责生命周期。
    explicit LegacyConsoleBackend(HANDLE h, Size size) : h_(h), size_(size) {}
    bool Present(const std::vector<CellDelta>& deltas) override;
    Size GetSize() const override { return size_; }
    void SetSize(Size s) { size_ = s; }
    const char* Name() const override { return "legacy"; }

    size_t LastCallCount() const { return last_calls_; }

private:
    HANDLE h_;
    Size size_;
    size_t last_calls_ = 0;
};

// 内存后端：记录每次 Present 收到的 deltas。用于测试与 selftest。
class MemoryBackend final : public IRenderBackend {
public:
    explicit MemoryBackend(Size size) : size_(size) {}
    bool Present(const std::vector<CellDelta>& deltas) override;
    Size GetSize() const override { return size_; }
    void SetSize(Size s) { size_ = s; }
    const char* Name() const override { return "memory"; }

    // 把已落地的单元格累积成一个完整画面，便于断言。
    const std::vector<Cell>& Frame() const { return frame_; }
    const std::vector<std::vector<CellDelta>>& History() const { return history_; }

private:
    Size size_;
    std::vector<Cell> frame_;  // 大小为 w*h
    std::vector<std::vector<CellDelta>> history_;
};

}  // namespace tui::console
