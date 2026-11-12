#include "tui/pool/thread_pool.h"

namespace tui::pool {

ThreadPool::ThreadPool(size_t threads, std::function<void(core::Task)> post_cb)
    : post_cb_(std::move(post_cb)) {
    InitializeSRWLock(&lock_);
    InitializeConditionVariable(&cv_);
    if (threads == 0) threads = 1;
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { WorkerMain(); });
    }
}

ThreadPool::~ThreadPool() { Shutdown(); }

void ThreadPool::SubmitCompute(ComputeTask task) {
    AcquireSRWLockExclusive(&lock_);
    if (!stopping_) {
        queue_.push_back(std::move(task));
        WakeConditionVariable(&cv_);
    }
    ReleaseSRWLockExclusive(&lock_);
}

size_t ThreadPool::Pending() const {
    AcquireSRWLockShared(&lock_);
    size_t n = queue_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

size_t ThreadPool::Completed() const {
    AcquireSRWLockShared(&lock_);
    size_t n = completed_;
    ReleaseSRWLockShared(&lock_);
    return n;
}

void ThreadPool::WorkerMain() {
    for (;;) {
        ComputeTask task;
        AcquireSRWLockExclusive(&lock_);
        while (queue_.empty() && !stopping_) {
            // SleepConditionVariableSRW 原子地释放锁并等待，被唤醒后重新持锁。
            SleepConditionVariableSRW(&cv_, &lock_, INFINITE, 0);
        }
        if (stopping_ && queue_.empty()) {
            ReleaseSRWLockExclusive(&lock_);
            break;
        }
        task = std::move(queue_.front());
        queue_.pop_front();
        ReleaseSRWLockExclusive(&lock_);

        // 无状态计算（约束 B3）：不碰 loop 状态，返回一个续延。
        core::Task continuation;
        if (task) continuation = task();

        AcquireSRWLockExclusive(&lock_);
        ++completed_;
        ReleaseSRWLockExclusive(&lock_);

        // 结果回投（约束 B4）：续延经 post_cb 投到 loop 线程执行。
        // post_cb 典型实现是 EventLoop::PostTask，内部 PostQCS(kKeyWake) 唤醒 loop。
        if (continuation && post_cb_) {
            post_cb_(std::move(continuation));
        }
    }
}

void ThreadPool::Shutdown() {
    AcquireSRWLockExclusive(&lock_);
    if (stopping_) {
        ReleaseSRWLockExclusive(&lock_);
        for (auto& t : workers_) if (t.joinable()) t.join();
        return;
    }
    stopping_ = true;
    ReleaseSRWLockExclusive(&lock_);
    WakeAllConditionVariable(&cv_);
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

}  // namespace tui::pool
