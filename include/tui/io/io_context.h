#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "tui/core/platform.h"

// 阶段 3：IoContext —— 每个 Overlapped 操作的独立上下文（约束 C2）
//
// 真实事件循环最难的问题：completion 到达时，发起该操作的对象可能已经被关闭，
// 甚至 buffer 已经释放。Windows 的解决方式是「OVERLAPPED 结构内嵌在 context 里，
// 通过 CONTAINING_RECORD 反查」，这里用等价做法：
//   - 每个操作独占一个 IoContext（含 OVERLAPPED + buffer + callback + generation）；
//   - completion 通过 OVERLAPPED* 找回 IoContext；
//   - generation 用于识别「过期完成包」（见 Watcher::OnCompletion）。
//
// 生命周期：IoContext 由 Watcher 持有，直到 close 状态机进入 kClosed 才释放。
// CancelIoEx 取消的操作仍会产生一个 completion（ERROR_OPERATION_ABORTED），
// 因此「排空 completion」之前绝不能 delete context —— 这正是 C4 顺序的原因。
namespace tui::io {

// completion 回调：bytes=传输字节数，error=GetLastError 风格错误码（0 表示成功）。
using IoCallback = std::function<void(uint32_t bytes, uint32_t error)>;

// 完成包类型的 CompletionKey 取值（PostQueuedCompletionStatus 用）。
enum : ULONG_PTR {
    kKeyWatcher = 1,   // 普通 I/O 完成（lpOverlapped 指向 IoContext）
    kKeyWake = 2,      // 唤醒 loop 的自定义包（self-pipe 等价物）
    kKeyTask = 3,      // 跨线程投递任务（阶段 4 worker 用）
};

struct IoContext {
    OVERLAPPED ov{};            // 必须是第一个成员：便于由 lpOverlapped 反查
    std::vector<uint8_t> buffer;
    IoCallback callback;
    uint64_t generation = 0;   // 操作代数，用于丢弃过期完成包
    void* owner = nullptr;      // 指向所属 Watcher（close 状态机校验用）
    uint32_t error = 0;        // completion 的错误码；投递前由 Watcher/测试设置
    bool cancelled = false;    // 标记这是 CancelIoEx 导致的取消完成包

    IoContext() = default;
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;

    void ResetForReuse() {
        // 重新发起 overlapped 操作前必须清零 OVERLAPPED（除 hEvent 外），
        // 否则内核会复用上次的状态。
        ov = {};
        buffer.clear();
        callback = {};
        generation = 0;
        owner = nullptr;
        error = 0;
        cancelled = false;
    }
};

// 由 completion 的 lpOverlapped 反查 IoContext（等价于 CONTAINING_RECORD）。
inline IoContext* ContextFromOverlapped(LPOVERLAPPED ov) {
    return reinterpret_cast<IoContext*>(ov);
}

}  // namespace tui::io
