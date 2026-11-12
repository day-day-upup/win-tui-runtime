#pragma once

#include <deque>
#include <vector>

#include "tui/core/wait_backend.h"
#include "tui/io/io_context.h"

// 阶段 3：IocpBackend —— IOCP 主等待后端（约束 C1）
//
// 用 CreateIoCompletionPort 建端口，GetQueuedCompletionStatusEx 批量取完成包。
//
// 关键分离（约束 B2）：
//   - Wait()   ：从内核取包，存入 pending_，绝不执行业务回调；
//   - Dispatch()：在 loop 线程把 pending_ 里的包逐个分发回调。
//   这样回调里可以安全 PostTask / 关 watcher / 重入分发，全程无锁。
//
// Wake()：PostQueuedCompletionStatus(kKeyWake) —— Windows 版 self-pipe。
// 与阶段 1 的 SetEvent 等价，但走的是同一个 completion port，
// 因此 loop 只需等待一个内核对象。
//
// PostCompletion()：供测试「可控注入完成包」与阶段 4 I/O 线程使用，
// 模拟内核投递，驱动 close 状态机验证。
namespace tui::io {

class IocpBackend final : public tui::core::IWaitBackend {
public:
    IocpBackend();
    ~IocpBackend() override;

    IocpBackend(const IocpBackend&) = delete;
    IocpBackend& operator=(const IocpBackend&) = delete;

    bool Ok() const { return iocp_ != nullptr && iocp_ != INVALID_HANDLE_VALUE; }
    HANDLE Handle() const { return iocp_; }

    // 关联一个以 overlapped 方式打开的句柄到本端口（约束 C7）。
    bool Associate(HANDLE file, ULONG_PTR key);

    // 任意线程：投递一个自定义完成包（Wake / 任务 / 测试注入）。
    bool PostCompletion(ULONG_PTR key, uint32_t bytes, LPOVERLAPPED ov);

    // --- IWaitBackend ------------------------------------------------
    void Wake() override;
    size_t Dispatch() override;
    bool Wait(DWORD timeout_ms) override;
    void Shutdown() override;
    const char* Name() const override { return "iocp"; }

    // 诊断
    size_t PendingCount() const { return pending_.size(); }
    size_t TotalCompletions() const { return total_completions_; }

private:
    struct Entry {
        ULONG_PTR key = 0;
        uint32_t bytes = 0;
        LPOVERLAPPED ov = nullptr;
        uint32_t error = 0;
    };

    void DrainOne(const Entry& e);

    HANDLE iocp_ = nullptr;
    bool shutdown_ = false;
    std::deque<Entry> pending_;     // 仅 loop 线程访问
    size_t total_completions_ = 0;
};

}  // namespace tui::io
