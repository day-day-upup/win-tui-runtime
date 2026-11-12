#pragma once

#include <cstdint>

// 阶段 1：Loop 骨架
//
// IClock 是可注入的时间源。EventLoop 只依赖这个接口，不直接调用
// QueryPerformanceCounter。这样测试可以用 FakeClock 手动推进时间，
// 精确验证定时器排序与 postDelayedTask 的行为，而不依赖真实时间。
//
// 约定：单调、毫秒、只增不减（SystemClock 用 QPC 保证单调）。
namespace tui::core {

class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t NowMs() const = 0;
};

// 真实时钟：QueryPerformanceCounter / QueryPerformanceFrequency。
// 频率在构造时缓存，之后 NowMs() 是纯算术，无系统调用。
class SystemClock final : public IClock {
public:
    SystemClock();
    int64_t NowMs() const override;

private:
    int64_t freq_;   // 每秒计数
    int64_t start_;  // 构造时的计数，作为零点
};

// 假时钟：由测试显式 Advance() 推进。
// 注意：FakeClock 不自己走动，EventLoop 在它上面不会「自然超时」，
// 这正是我们想要的——测试完全掌控时间。
class FakeClock final : public IClock {
public:
    explicit FakeClock(int64_t initial_ms = 0) : now_(initial_ms) {}
    int64_t NowMs() const override { return now_; }

    void Advance(int64_t delta_ms) { now_ += delta_ms; }
    void Set(int64_t ms) { now_ = ms; }

private:
    int64_t now_;
};

}  // namespace tui::core
