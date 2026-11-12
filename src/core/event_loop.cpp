#include "tui/core/event_loop.h"

#include <algorithm>
#include <utility>

namespace tui::core {

EventLoop::EventLoop(IClock& clock) : clock_(clock) {}

EventLoop::~EventLoop() = default;

// 计算本次等待窗口。
//   - 无定时器：INFINITE（只靠 Waker 唤醒）；
//   - 定时器已到期：0（不阻塞，立即处理）；
//   - 否则：deadline - now，截到 DWORD（最大约 49.7 天）。
DWORD EventLoop::ComputeWaitMs(int64_t now_ms) const {
    int64_t next = timers_.NextDeadlineMs();
    if (next < 0) return INFINITE;
    int64_t delta = next - now_ms;
    if (delta <= 0) return 0;
    const int64_t kMax = static_cast<int64_t>(0xFFFFFFFEu);
    return static_cast<DWORD>(std::min(delta, kMax));
}

void EventLoop::PostTask(Task task) {
    tasks_.Post(std::move(task));
    waker_.Wake();  // 约束 B5：跨线程投递必须唤醒阻塞的 loop
}

TimerId EventLoop::PostDelayedTask(int64_t delay_ms, Task task) {
    TimerId id = next_timer_id_.fetch_add(1);
    int64_t deadline = clock_.NowMs() + delay_ms;  // 调用线程读时钟，仅做加法
    // 真正的堆操作序列化到 loop 线程执行（约束 B1）。
    tasks_.Post([this, id, deadline, task = std::move(task)]() mutable {
        timers_.Add(id, deadline, 0, std::move(task));
    });
    waker_.Wake();
    return id;
}

TimerId EventLoop::PostRepeatingTask(int64_t delay_ms, int64_t interval_ms, Task task) {
    TimerId id = next_timer_id_.fetch_add(1);
    int64_t deadline = clock_.NowMs() + delay_ms;
    tasks_.Post([this, id, deadline, interval_ms, task = std::move(task)]() mutable {
        timers_.Add(id, deadline, interval_ms, std::move(task));
    });
    waker_.Wake();
    return id;
}

void EventLoop::CancelTimer(TimerId id) {
    tasks_.Post([this, id]() { timers_.Cancel(id); });
    waker_.Wake();
}

void EventLoop::SetIdleTask(Task task) {
    tasks_.Post([this, task = std::move(task)]() mutable {
        idle_task_ = std::move(task);
        has_idle_task_ = true;
    });
    waker_.Wake();
}

void EventLoop::Stop() {
    stop_requested_.store(true);
    waker_.Wake();
}

// 返回本轮实际执行的任务数（用于 idle 判定）。
size_t EventLoop::DispatchPostedTasks() {
    auto q = tasks_.Drain();
    size_t n = 0;
    for (auto& t : q) {
        if (t) {
            t();
            ++n;
        }
    }
    return n;
}

size_t EventLoop::DispatchDueTimers(int64_t now_ms) {
    auto due = timers_.PopDue(now_ms);
    size_t n = 0;
    for (auto& t : due) {
        if (t) {
            t();
            ++n;
        }
    }
    return n;
}

void EventLoop::DispatchIdle() {
    if (has_idle_task_ && idle_task_) idle_task_();
}

// 一轮分发。
//   - 先执行投递任务（含定时器 Add/Cancel 控制任务）；
//   - 再执行到期定时器；
//   - 只有「本轮既没投递任务、也没定时器触发、且没有待处理工作」时才跑 idle。
// blocking=true 时，在分发完成后按堆顶 deadline 阻塞等待；
// blocking=false 供 FakeClock 测试逐轮精确驱动。
void EventLoop::RunIteration(bool blocking) {
    const size_t posted = DispatchPostedTasks();

    int64_t now = clock_.NowMs();
    const size_t fired = DispatchDueTimers(now);

    const bool no_pending_work = tasks_.Empty() && timers_.Empty();
    if (posted == 0 && fired == 0 && no_pending_work) {
        DispatchIdle();
    }

    // 若 Stop 在本轮分发中被请求（例如某个任务里调用了 Stop），
    // 不要再进入阻塞等待，直接回到 Run() 的循环判定退出。
    if (blocking && !stop_requested_.load()) {
        waker_.Wait(ComputeWaitMs(now));
    }
}

void EventLoop::RunOneIterationForTest() {
    state_.store(LoopState::kRunning);
    RunIteration(/*blocking=*/false);
}

void EventLoop::Run() {
    state_.store(LoopState::kRunning);

    while (!stop_requested_.load()) {
        RunIteration(/*blocking=*/true);
    }

    state_.store(LoopState::kStopping);
    // 退出前排空剩余投递任务（例如日志 flush），但不处理新定时器。
    DispatchPostedTasks();
    state_.store(LoopState::kStopped);
}

}  // namespace tui::core
