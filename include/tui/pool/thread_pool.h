#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "tui/core/platform.h"
#include "tui/core/types.h"

// 阶段 4：ThreadPool —— 额外 worker 线程池（约束 B3/B4）
//
// 定位：只做「无状态计算」（帧解析、编码转换、日志格式化、hex 转储）。
// 它不持有指向 loop 状态的指针；计算任务返回一个「续延」(core::Task)，
// worker 通过 post_cb_ 把它投回 loop 线程执行。
//
// 典型接法（应用侧）：
//   ThreadPool pool(4, [&loop](core::Task t){ loop.PostTask(std::move(t)); });
//   pool.SubmitCompute([data = std::move(bytes)]() mutable -> core::Task {
//       std::string hex = ToHex(data);              // 无状态计算
//       return [s = std::move(hex)]() { /* 在 loop 线程用 s 更新 UI */ };
//   });
//
// 用 Win32 原生同步原语（约束 A1）：SRWLOCK + CONDITION_VARIABLE，
// 不引入 std::mutex / std::condition_variable / 第三方库。
namespace tui::pool {

// 计算任务：在 worker 线程执行，返回「要在 loop 线程执行的续延」。
using ComputeTask = std::function<core::Task()>;

class ThreadPool {
public:
    // post_cb：把续延投回 loop 的函数，必须线程安全且「只投递」。
    explicit ThreadPool(size_t threads, std::function<void(core::Task)> post_cb);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 任意线程：提交一个无状态计算任务。
    void SubmitCompute(ComputeTask task);

    // 等待所有已提交任务执行完并停止线程（优雅关闭，约束 C4 的 join 步骤）。
    void Shutdown();

    size_t Pending() const;
    size_t Completed() const;

private:
    void WorkerMain();

    mutable SRWLOCK lock_;  // const 成员函数（Pending/Completed）也要取锁
    CONDITION_VARIABLE cv_;
    std::vector<std::thread> workers_;
    std::deque<ComputeTask> queue_;  // guarded by lock_
    std::function<void(core::Task)> post_cb_;
    bool stopping_ = false;
    size_t completed_ = 0;
};

}  // namespace tui::pool
