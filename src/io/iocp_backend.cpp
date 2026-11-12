#include "tui/io/iocp_backend.h"

#include <utility>

namespace tui::io {

IocpBackend::IocpBackend() {
    // NumberOfConcurrentThreads=1：与「单 loop 线程」语义一致（约束 B1）。
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (iocp_ == INVALID_HANDLE_VALUE) iocp_ = nullptr;
}

IocpBackend::~IocpBackend() { Shutdown(); }

bool IocpBackend::Associate(HANDLE file, ULONG_PTR key) {
    if (!Ok() || !file || file == INVALID_HANDLE_VALUE) return false;
    HANDLE r = CreateIoCompletionPort(file, iocp_, key, 0);
    return r == iocp_;
}

bool IocpBackend::PostCompletion(ULONG_PTR key, uint32_t bytes, LPOVERLAPPED ov) {
    if (!Ok()) return false;
    return PostQueuedCompletionStatus(iocp_, bytes, key, ov) != FALSE;
}

void IocpBackend::Wake() {
    // 唤醒包不携带 OVERLAPPED；Dispatch 里按 key 区分。
    PostCompletion(kKeyWake, 0, nullptr);
}

bool IocpBackend::Wait(DWORD timeout_ms) {
    if (!Ok() || shutdown_) return false;

    OVERLAPPED_ENTRY entries[64];
    ULONG removed = 0;
    BOOL ok = GetQueuedCompletionStatusEx(
        iocp_, entries, 64, &removed, timeout_ms, /*fAlertable=*/FALSE);

    if (ok && removed > 0) {
        for (ULONG i = 0; i < removed; ++i) {
            Entry e;
            e.key = entries[i].lpCompletionKey;
            e.bytes = entries[i].dwNumberOfBytesTransferred;
            e.ov = entries[i].lpOverlapped;
            e.error = 0;  // GQCSEx 不直接给 error；失败包由 ov/Internal 表达，
                          // 本项目 Watcher 层用 PostCompletion 注入 error 更直观。
            pending_.push_back(e);
        }
        total_completions_ += removed;
        return true;
    }

    // WAIT_TIMEOUT / 无包：返回 false 表示「没有 I/O 完成需要处理」，
    // 但定时器窗口可能已到，loop 下一轮会重新计算 timeout。
    return false;
}

void IocpBackend::DrainOne(const Entry& e) {
    switch (e.key) {
        case kKeyWake:
        case kKeyTask:
            // 唤醒 / 任务包：无回调，存在的意义就是让 Wait 返回。
            // 对应阶段 1 的「Waker 被置位」。
            break;
        case kKeyWatcher:
        default: {
            if (!e.ov) break;
            IoContext* ctx = ContextFromOverlapped(e.ov);
            if (ctx && ctx->callback) {
                // 回调在 loop 线程执行（约束 B2）。
                // 真实 CancelIoEx 的取消包会把 ov.Internal 设为 STATUS_CANCELLED；
                // 测试注入的取消包显式置 ctx->cancelled。两者统一在此转成
                // ERROR_OPERATION_ABORTED 传给 Watcher 的 close 状态机。
                bool cancelled = ctx->cancelled ||
                                 ctx->ov.Internal == static_cast<ULONG_PTR>(0xC0000120L);  // STATUS_CANCELLED
                uint32_t error = ctx->error;
                if (cancelled) error = ERROR_OPERATION_ABORTED;
                ctx->callback(e.bytes, error);
            }
            break;
        }
    }
}

size_t IocpBackend::Dispatch() {
    size_t n = 0;
    while (!pending_.empty()) {
        Entry e = pending_.front();
        pending_.pop_front();
        DrainOne(e);
        ++n;
    }
    return n;
}

void IocpBackend::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    // 排空残留完成包（约束 C4）：此处不执行业务回调，只丢弃，
    // 因为 Run() 已经在退出前调用过 Dispatch()。
    OVERLAPPED_ENTRY entries[64];
    ULONG removed = 0;
    while (Ok() && GetQueuedCompletionStatusEx(iocp_, entries, 64, &removed, 0, FALSE) &&
           removed > 0) {
        // 丢弃
    }
    pending_.clear();
    if (Ok()) {
        CloseHandle(iocp_);
        iocp_ = nullptr;
    }
}

}  // namespace tui::io
