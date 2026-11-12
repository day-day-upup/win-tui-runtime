#include "tui/core/wfmo_backend.h"

#include <algorithm>

namespace tui::core {

namespace {

// QueueUserAPC 的回调签名是 C 函数指针 void CALLBACK(ULONG_PTR)。
// 传一个堆上 ApcItem* 进去，回调在「可警报等待的线程」上执行。
VOID CALLBACK ApcTrampoline(ULONG_PTR param) {
    auto* item = reinterpret_cast<WfmoBackend::ApcItem*>(param);
    if (!item || !item->self) return;
    item->self->OnApc(item);
}

}  // namespace

WfmoBackend::WfmoBackend() {
    // slot 0 固定为唤醒 event（阶段 1 的 self-pipe 等价物）。
    handles_[0] = waker_.Handle();
    used_[0] = true;
    count_ = 1;
}

WfmoBackend::~WfmoBackend() { Shutdown(); }

WfmoBackend::Slot WfmoBackend::Register(HANDLE handle, std::function<void()> on_signal) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return kInvalidSlot;
    // 显式暴露 64 句柄上限：slot 0 已被 wake 占用，故最多 63 个用户句柄。
    if (count_ >= kMax) return kInvalidSlot;
    // 复用空洞槽位（Unregister 后留下的）。
    for (size_t i = 1; i < kMax; ++i) {
        if (!used_[i]) {
            handles_[i] = handle;
            callbacks_[i] = std::move(on_signal);
            used_[i] = true;
            ++count_;
            return static_cast<Slot>(i);
        }
    }
    return kInvalidSlot;
}

void WfmoBackend::Unregister(Slot slot) {
    if (slot == 0 || slot >= kMax || !used_[slot]) return;
    handles_[slot] = nullptr;
    callbacks_[slot] = {};
    used_[slot] = false;
    --count_;
}

void WfmoBackend::Wake() { waker_.Wake(); }

void WfmoBackend::BindLoopThread() {
    // 用伪句柄（GetCurrentThread 返回 -2），无需 CloseHandle，跨线程
    // QueueUserAPC 需要用真实句柄：这里 DuplicateHandle 得到真实句柄。
    HANDLE pseudo = GetCurrentThread();
    HANDLE real = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), pseudo, GetCurrentProcess(), &real,
                        0, FALSE, DUPLICATE_SAME_ACCESS)) {
        loop_thread_ = real;
    }
}

bool WfmoBackend::QueueApc(std::function<void()> fn) {
    if (!loop_thread_) return false;
    auto* item = new ApcItem{this, std::move(fn)};
    if (QueueUserAPC(ApcTrampoline, loop_thread_, reinterpret_cast<ULONG_PTR>(item)) == 0) {
        delete item;
        return false;
    }
    return true;
}

void WfmoBackend::OnApc(ApcItem* item) {
    // 此函数运行在 loop 线程的可警报等待中（阶段 5 的 APC 对照点）。
    ++apc_ran_;
    // 收集到待分发队列，交给 Dispatch() 在 loop 线程执行（保持 B2 契约）。
    pending_apc_.push_back(item);
}

bool WfmoBackend::Wait(DWORD timeout_ms) {
    if (shutdown_) return false;

    // 只把 used_ 的句柄压缩进连续数组（WFMO 要求连续）。
    HANDLE wait_handles[kMax];
    Slot slot_of_index[kMax];
    DWORD n = 0;
    for (size_t i = 0; i < kMax; ++i) {
        if (used_[i] && handles_[i]) {
            wait_handles[n] = handles_[i];
            slot_of_index[n] = static_cast<Slot>(i);
            ++n;
        }
    }

    // bWaitAll=FALSE（任一触发即可）；bAlertable=TRUE（APC 对照的关键）。
    DWORD r = WaitForMultipleObjectsEx(n, wait_handles, FALSE, timeout_ms, TRUE);
    last_wait_result_ = r;

    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) {
        DWORD idx = r - WAIT_OBJECT_0;
        signaled_.push_back(slot_of_index[idx]);
        return true;
    }
    if (r == WAIT_IO_COMPLETION) {
        // APC 已执行（OnApc 里已入队），返回 true 让 Dispatch 处理。
        return true;
    }
    // WAIT_TIMEOUT / WAIT_FAILED：无 I/O 完成，但定时器窗口可能已到。
    return false;
}

size_t WfmoBackend::Dispatch() {
    size_t n = 0;
    // 1) 处理被 signaled 的句柄回调（slot 0 = wake，无回调）。
    for (Slot s : signaled_) {
        if (s == 0) continue;  // 纯唤醒
        if (s < kMax && used_[s] && callbacks_[s]) {
            callbacks_[s]();
            ++n;
        }
    }
    signaled_.clear();

    // 2) 处理 APC 期间收集到的回调（在 loop 线程执行，约束 B2）。
    for (auto* item : pending_apc_) {
        if (item && item->fn) {
            item->fn();
            ++n;
        }
        delete item;
    }
    pending_apc_.clear();

    total_dispatched_ += n;
    return n;
}

void WfmoBackend::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    waker_.Wake();
    for (auto* item : pending_apc_) delete item;
    pending_apc_.clear();
    signaled_.clear();
    if (loop_thread_) {
        CloseHandle(loop_thread_);
        loop_thread_ = nullptr;
    }
}

}  // namespace tui::core
