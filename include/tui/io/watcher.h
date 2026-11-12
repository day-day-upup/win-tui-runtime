#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "tui/core/platform.h"
#include "tui/io/io_context.h"

// 阶段 3：Watcher —— 异步 I/O 观察者 + close 状态机（约束 C3/C4）
//
// 这是「真实事件循环最难也最值钱的部分」：
//   - 回调里可能要关闭这个 watcher（例如读到 EOF、连接断开、用户按退出）；
//   - 但此时还有「已提交给内核、尚未完成」的 overlapped 操作；
//   - 若直接 delete，那些操作的 completion 会写到已释放内存 → use-after-free。
//
// 状态机（uv_close 式）：
//   kActive  正常，持有句柄，有 inflight 操作
//       │ Close()
//       ▼
//   kClosing 已调用 CancelIoEx，等待所有 inflight 操作的完成包回来
//       │ 每个 completion 到达 → inflight--；归零时
//       ▼
//   kClosed  句柄可关、context 可释放（由 EventSource 回收）
//
// 关键不变量：
//   - Close() 不释放内存，只标记 + CancelIoEx；
//   - completion 回调在 loop 线程（约束 B2）通过 OnCompletion 递减 inflight；
//   - generation 用于丢弃「关闭后仍到达的过期完成包」。
//   - 可在回调内部安全调用 Close()、甚至 delete 之后的间接释放（由 owner 延迟）。
namespace tui::io {

class IocpBackend;

enum class WatcherState {
    kActive,
    kClosing,
    kClosed,
};

// 完成回调：由具体 source 实现，参数 (bytes, error, notify_buffer)。
using WatchCallback = std::function<void(uint32_t bytes, uint32_t error,
                                         const std::vector<uint8_t>& data)>;

class Watcher {
public:
    Watcher(IocpBackend& iocp, HANDLE handle, WatchCallback cb);
    virtual ~Watcher();

    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    WatcherState State() const { return state_; }
    bool IsActive() const { return state_ == WatcherState::kActive; }
    HANDLE Handle() const { return handle_; }
    uint64_t Generation() const { return generation_; }

    // 请求关闭（可从任意线程调用，但真正的状态迁移在 loop 线程的 OnCompletion 完成）。
    // 返回是否成功发起了 CancelIoEx。
    bool Close();

    // 由 IocpBackend::Dispatch 在 loop 线程调用，处理该 watcher 的一个完成包。
    // ctx 是发起操作时用的 IoContext；返回本 watcher 是否仍处于活跃/关闭中。
    void OnCompletion(IoContext& ctx, uint32_t bytes, uint32_t error);

    // 发起一次新的 overlapped 读（具体 source 在 Arm() 里调 ReadDirectoryChangesW 等）。
    // 返回是否成功提交。失败时 inflight 不增。
    bool BeginOperation(IoContext& ctx);

protected:
    // 具体 source 重写：提交一次 overlapped 操作。返回是否成功。
    virtual bool Arm(IoContext& ctx) = 0;
    // 具体 source 重写：解析一次完成的通知数据。
    virtual void Notify(uint32_t bytes, uint32_t error,
                        const std::vector<uint8_t>& data) = 0;

    IocpBackend& iocp_;
    HANDLE handle_ = nullptr;
    WatchCallback user_cb_;
    WatcherState state_ = WatcherState::kActive;
    uint64_t generation_ = 1;   // 每次重新 Arm 递增，用于丢弃过期完成包
    size_t inflight_ = 0;       // 已提交未完成的 overlapped 操作数
    bool close_requested_ = false;
};

}  // namespace tui::io
