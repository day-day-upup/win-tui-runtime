#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

#include "tui/core/clock.h"
#include "tui/core/task_queue.h"
#include "tui/core/timer_heap.h"
#include "tui/core/types.h"
#include "tui/core/wait_backend.h"
#include "tui/core/waker_backend.h"

// 阶段 1：EventLoop —— 事件循环骨架（headless）
//
// 一次 Run() 的每一轮做四件事：
//   1) 计算等待窗口 = 堆顶 deadline - now；无定时器则 INFINITE；
//   2) 在 Waker 上可警报等待该窗口（阶段 3 换成 GetQueuedCompletionStatusEx）；
//   3) 分发：投递任务 → 到期定时器 → idle 任务；
//   4) 若 Stop() 被调用，排空剩余投递任务后退出。
//
// 线程不变量（约束 B1/B2/B5）：
//   - Run() 在唯一 loop 线程执行；TimerHeap / idle_task_ 只在该线程访问。
//   - 其它线程只能调用 PostTask / PostDelayedTask / PostRepeatingTask /
//     CancelTimer / Stop：它们只写 TaskQueue、置原子标志、SetEvent。
//   - 定时器 id 在调用线程用原子自增分配；真正的 TimerHeap::Add/Cancel
//     被包成 Task 投递，在 loop 线程里执行 → 无需给堆加锁。
//
// 时钟注入：IClock& 由外部传入。测试用 FakeClock 手动 Advance()，
// 精确验证定时器排序与触发时机，不依赖真实时间。
namespace tui::core {

class EventLoop {
public:
    // 默认用 WakerBackend（阶段 1/2 行为）；传入其它后端即可切换等待模型
    // （约束 C1：同一套 EventLoop 接口，阶段 3 注入 IocpBackend，阶段 5 注入 WFMO 后端）。
    explicit EventLoop(IClock& clock);
    EventLoop(IClock& clock, std::unique_ptr<IWaitBackend> backend);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void Run();
    void Stop();

    // 仅诊断 / 后端接入用。IocpBackend 需要它来投递「自定义完成包」。
    IWaitBackend& WaitBackend() { return *backend_; }

    // --- 任务调度（任意线程）-------------------------------------
    void PostTask(Task task);
    TimerId PostDelayedTask(int64_t delay_ms, Task task);
    TimerId PostRepeatingTask(int64_t delay_ms, int64_t interval_ms, Task task);
    void CancelTimer(TimerId id);
    void SetIdleTask(Task task);

    // --- 诊断 / 测试钩子 -------------------------------------------
    bool IsRunning() const { return state_.load() == LoopState::kRunning; }
    LoopState State() const { return state_.load(); }

    // 只跑一轮分发（不进入阻塞等待）。供单元测试在 FakeClock 下精确驱动。
    // 前置条件：未调用 Run()。测试专用，不要与 Run() 混用。
    void RunOneIterationForTest();

private:
    DWORD ComputeWaitMs(int64_t now_ms) const;
    size_t DispatchPostedTasks();  // 返回执行的任务数
    size_t DispatchDueTimers(int64_t now_ms);  // 返回触发的定时器数
    void DispatchIdle();
    void RunIteration(bool blocking);

    IClock& clock_;
    TaskQueue tasks_;
    TimerHeap timers_;   // 仅 loop 线程
    std::unique_ptr<IWaitBackend> backend_;

    std::atomic<LoopState> state_{LoopState::kIdle};
    std::atomic<bool> stop_requested_{false};
    std::atomic<TimerId> next_timer_id_{1};

    Task idle_task_;          // 仅 loop 线程
    bool has_idle_task_ = false;
};

}  // namespace tui::core
