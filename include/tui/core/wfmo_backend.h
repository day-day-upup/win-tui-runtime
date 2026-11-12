#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "tui/core/wait_backend.h"
#include "tui/core/waker.h"

// 阶段 5：WfmoBackend —— WaitForMultipleObjectsEx 对照后端（约束 C1）
//
// 目的不是「再实现一遍能用的后端」，而是亲手撞上 Windows 事件循环的
// 两个根本差异，从而理解「为什么 Windows 需要 IOCP」：
//
//   1) 句柄数量上限：WaitForMultipleObjectsEx 一次最多等
//      MAXIMUM_WAIT_OBJECTS = 64 个句柄。本后端把 slot 0 留给唤醒 event，
//      因此用户最多注册 63 个；Register() 超限会明确失败并返回 kInvalidSlot。
//      而 IOCP 是「一个端口 + 任意多个完成包」，没有这个上限。
//
//   2) 完成语义不同：IOCP 的 completion 自带 lpOverlapped / CompletionKey，
//      内核告诉你「哪个操作完成了」；WFMO 只告诉你「第 N 个句柄 signaled」，
//      上层必须自己维护 slot -> 上下文 的映射。这就是为什么 overlapped
//      操作要走 WFMO 必须给每个 OVERLAPPED 配一个 hEvent。
//
//   3) APC：只有 bAlertable=TRUE 的可警报等待才会执行 QueueUserAPC 回调。
//      本后端用 WaitForMultipleObjectsEx(..., bAlertable=TRUE)，并提供
//      QueueApc() 做对照实验；回调在 loop 线程执行（约束 B2 仍成立）。
//
// 契约与 IocpBackend 完全一致：Wait() 只收集「哪些 slot 被触发」，
// Dispatch() 才在 loop 线程跑对应回调。EventLoop 的 RunIteration 不用改。
namespace tui::core {

class WfmoBackend final : public IWaitBackend {
public:
    using Slot = uint32_t;
    static constexpr Slot kInvalidSlot = 0xFFFFFFFF;
    static constexpr size_t kMaxUserHandles = MAXIMUM_WAIT_OBJECTS - 1;  // 63

    WfmoBackend();
    ~WfmoBackend() override;

    WfmoBackend(const WfmoBackend&) = delete;
    WfmoBackend& operator=(const WfmoBackend&) = delete;

    // 注册一个「可等待句柄被 signaled 时要做什么」。仅 loop 线程调用。
    // 句柄通常是某个 OVERLAPPED.hEvent（auto-reset event）。
    // 超过 63 个返回 kInvalidSlot（显式暴露 64 上限，不静默截断）。
    Slot Register(HANDLE handle, std::function<void()> on_signal);
    void Unregister(Slot slot);
    size_t RegisteredCount() const { return count_; }

    // --- IWaitBackend ------------------------------------------------
    void Wake() override;
    size_t Dispatch() override;
    bool Wait(DWORD timeout_ms) override;
    void Shutdown() override;
    const char* Name() const override { return "wfmo"; }

    // --- APC 对照实验 -------------------------------------------------
    // 必须在 loop 线程（Wait 所在线程）调用，内部记录线程句柄。
    // 之后任意线程可用 QueueApc() 投递回调。
    void BindLoopThread();
    bool QueueApc(std::function<void()> fn);  // 任意线程可调用
    size_t ApcRanCount() const { return apc_ran_; }

    // 诊断：上一次 Wait 返回的索引（WAIT_OBJECT_0 / WAIT_IO_COMPLETION / WAIT_TIMEOUT）
    DWORD LastWaitResult() const { return last_wait_result_; }

    // APC 桥接用（由 .cpp 的 trampoline 调用，不应被外部直接使用）。
    struct ApcItem {
        WfmoBackend* self = nullptr;
        std::function<void()> fn;
    };
    void OnApc(ApcItem* item);

private:
    static constexpr size_t kMax = MAXIMUM_WAIT_OBJECTS;  // 64
    HANDLE handles_[kMax]{};
    std::function<void()> callbacks_[kMax];  // slot 0 是 wake，不存用户回调
    bool used_[kMax]{};
    size_t count_ = 0;  // 已用槽位数（含 slot 0）

    Waker waker_;  // slot 0 的 auto-reset event（复用阶段 1 的 self-pipe）
    std::vector<Slot> signaled_;  // Wait 收集到的被触发 slot，供 Dispatch
    std::vector<ApcItem*> pending_apc_;  // APC 期间收集，Dispatch 时执行
    size_t total_dispatched_ = 0;

    HANDLE loop_thread_ = nullptr;  // DuplicateHandle 得到的真实句柄
    bool shutdown_ = false;
    size_t apc_ran_ = 0;
    DWORD last_wait_result_ = WAIT_FAILED;
};

}  // namespace tui::core
