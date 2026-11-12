#pragma once

#include <cstddef>
#include <deque>
#include <utility>

#include "tui/core/platform.h"
#include "tui/core/types.h"

// 阶段 1：TaskQueue
//
// 跨线程投递的任务队列。这是「B2 I/O 线程只投递、B3 worker 只回投」的落点：
// 任何线程都可以 Post()，只有 loop 线程调用 Drain()。
//
// 用 SRWLOCK 保护 deque（Win32 原生轻量锁，比 std::mutex 更贴学习目标）。
// 阶段 1 先用 std::mutex 的可移植写法亦可，但为了早点接触 Win32 同步原语，
// 这里直接用 SRWLOCK，封装成 RAII 的 Guard。
namespace tui::core {

class TaskQueue {
public:
    TaskQueue() { InitializeSRWLock(&lock_); }
    ~TaskQueue() = default;

    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // 任意线程调用：把任务放进队列。不负责唤醒，唤醒由 Waker 完成
    // （见 EventLoop::PostTask 的组合动作）。
    void Post(Task task) {
        AcquireSRWLockExclusive(&lock_);
        queue_.push_back(std::move(task));
        ReleaseSRWLockExclusive(&lock_);
    }

    // 仅 loop 线程调用：一次性取空队列并返回，避免「边执行边持锁」。
    // 在锁外执行回调，防止回调里再次 Post 造成死锁或迭代器失效。
    std::deque<Task> Drain() {
        std::deque<Task> out;
        AcquireSRWLockExclusive(&lock_);
        out.swap(queue_);
        ReleaseSRWLockExclusive(&lock_);
        return out;
    }

    bool Empty() const {
        AcquireSRWLockShared(&lock_);
        bool e = queue_.empty();
        ReleaseSRWLockShared(&lock_);
        return e;
    }

    size_t Size() const {
        AcquireSRWLockShared(&lock_);
        size_t n = queue_.size();
        ReleaseSRWLockShared(&lock_);
        return n;
    }

private:
    mutable SRWLOCK lock_;
    std::deque<Task> queue_;
};

}  // namespace tui::core
