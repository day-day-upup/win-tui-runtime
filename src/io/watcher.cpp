#include "tui/io/watcher.h"

#include "tui/io/iocp_backend.h"

namespace tui::io {

Watcher::Watcher(IocpBackend& iocp, HANDLE handle, WatchCallback cb)
    : iocp_(iocp), handle_(handle), user_cb_(std::move(cb)) {}

Watcher::~Watcher() {
    // 析构只做兜底：正常路径应已走过 Close() + 排空 completion（约束 C4）。
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool Watcher::BeginOperation(IoContext& ctx) {
    if (state_ != WatcherState::kActive) return false;
    ctx.owner = this;
    ctx.error = 0;
    ctx.cancelled = false;
    ctx.generation = generation_;
    ctx.armed = true;
    if (!Arm(ctx)) {
        ctx.armed = false;
        return false;
    }
    ++inflight_;
    return true;
}

bool Watcher::ReArm(IoContext& ctx) {
    if (state_ != WatcherState::kActive) return false;
    ++generation_;
    ctx.ResetForReuse();
    ctx.generation = generation_;
    return BeginOperation(ctx);
}

void Watcher::OnCompletion(IoContext& ctx, uint32_t bytes, uint32_t error) {
    // 过期完成包判定：
    //   - 阶段 3 单 ctx：generation 不匹配 → 丢弃（保留原语义）。
    //   - 阶段 4 多 ctx 全双工：每个 ctx 有自己的生命周期，不能用全局
    //     generation_ 比较（读 ctx 的 ReArm 会递增它，导致写 ctx 的
    //     完成包被误判为过期）。改用 per-ctx 的 armed 标志。
    //     只有「owner 是本 watcher 且 armed==false」才是已消费/重复的旧包。
    if (ctx.owner == this && !ctx.armed &&
        ctx.generation != generation_ && ctx.op_kind == OpKind::kGeneric) {
        return;
    }
    if (ctx.owner == this) ctx.armed = false;  // 本次完成已消费
    if (inflight_ > 0) --inflight_;

    const bool was_cancelled = (error == ERROR_OPERATION_ABORTED) || ctx.cancelled;

    if (state_ == WatcherState::kActive) {
        if (!was_cancelled) {
            // 正常完成：把通知交给具体 source 解析，再回调用户。
            Notify(bytes, error, ctx.buffer);
            if (user_cb_) user_cb_(bytes, error, ctx.buffer);

            // 阶段 4：给 source 一个「自定义完成后处置」的机会。
            // 返回 true 表示 source 已自行处理（例如重新挂读 / 释放写 ctx）。
            bool handled = false;
            if (ctx.on_complete) handled = ctx.on_complete(ctx, bytes, error);

            // 回调里可能调用了 Close()（例如读到 EOF、用户退出）。
            // 此时不能再重新 Arm，直接进入收尾。
            if (!handled && state_ == WatcherState::kActive &&
                !close_requested_ && ctx.auto_rearm) {
                ReArm(ctx);
                return;
            }
            // 否则 fall through 到收尾分支
        }
        // 正常态收到取消包：通常是外部 CancelIoEx，转入收尾。
    }

    // kClosing 或 active 收到取消：推进状态机。
    if (state_ == WatcherState::kClosing ||
        (state_ == WatcherState::kActive && was_cancelled)) {
        if (!close_requested_) close_requested_ = true;
        if (inflight_ == 0) {
            state_ = WatcherState::kClosed;
            if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
                handle_ = nullptr;
            }
            // 注意：这里不 delete this / 不 delete ctx。
            // 释放由 EventSource 在下一次 loop tick 回收（见 EventSource::Reap），
            // 保证「回调里关闭」不会造成 use-after-free。
        }
    }
}

bool Watcher::Close() {
    if (state_ == WatcherState::kClosed) return true;
    if (state_ == WatcherState::kClosing) return true;  // 已发起

    close_requested_ = true;
    state_ = WatcherState::kClosing;

    // 约束 C4 第一步：取消该句柄上所有未完成的 overlapped 操作。
    // 每个被取消的操作仍会产生一个 completion，因此不能在这里释放内存。
    bool ok = true;
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        ok = CancelIoEx(handle_, nullptr) != FALSE;
        // ERROR_NOT_FOUND 表示当前没有挂起操作，也视为成功。
        if (!ok && GetLastError() == ERROR_NOT_FOUND) ok = true;
    }

    // 没有 inflight 操作时，无需等 completion，直接推进到 kClosed。
    if (inflight_ == 0) {
        state_ = WatcherState::kClosed;
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }
    return ok;
}

}  // namespace tui::io
