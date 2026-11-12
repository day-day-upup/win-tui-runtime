#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace tui::core {

// 一次可调度的回调。用 std::function 便于阶段推进；若后续要优化可换成
// 带 inline storage 的 move-only task，但阶段 1 先保持简单可读。
using Task = std::function<void()>;

// 定时器句柄。返回给调用方用于 CancelTimer。
// 0 是非法句柄，保证有效 id 从 1 开始。
using TimerId = uint64_t;
constexpr TimerId kInvalidTimerId = 0;

// 定时器触发后的返回值决定它是否重复。
// 一次性定时器用 kOneShot；周期定时器由回调返回「下一次间隔」。
// 阶段 1 只实现一次性与「固定间隔重复」两种，interval_ms <= 0 视为一次性。
struct TimerSpec {
    int64_t delay_ms = 0;     // 首次触发前等待
    int64_t interval_ms = 0;  // >0 时按该间隔重复
};

// Loop 的运行状态。阶段 1 只用到 Idle/Running/Stopping/Stopped，
// 阶段 3 引入 IOCP 后会扩展等待逻辑，但状态机保持不变。
enum class LoopState {
    kIdle,
    kRunning,
    kStopping,
    kStopped,
};

}  // namespace tui::core
