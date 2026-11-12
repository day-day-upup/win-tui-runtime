#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tui/io/event_source.h"
#include "tui/io/io_context.h"

// 阶段 3：DirWatcher —— 第一个真实事件源（目录变更监视）
//
// 选它作首个 source 的理由：ReadDirectoryChangesW 是纯内核通知、
// overlapped 语义清晰、无需网络/硬件、可写自动化端到端测试。
// 覆盖的 Win32 知识点：CreateFileW(FILE_FLAG_BACKUP_SEMANTICS |
// FILE_FLAG_OVERLAPPED)、ReadDirectoryChangesW、FILE_NOTIFY_INFORMATION 解析。
namespace tui::io {

struct DirEvent {
    enum class Action { kAdded, kRemoved, kModified, kRenamedOld, kRenamedNew, kUnknown };
    Action action = Action::kUnknown;
    std::wstring name;
};

using DirCallback = std::function<void(const DirEvent&)>;

class DirWatcher final : public Watcher {
public:
    DirWatcher(IocpBackend& iocp, const std::wstring& dir, DirCallback cb);
    ~DirWatcher() override;

    bool IsOpen() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    // 发起第一次 overlapped 读（提交到 IOCP）。仅 loop 线程调用。
    bool Start();

    // 解析 FILE_NOTIFY_INFORMATION 缓冲（纯函数，可单测）。
    static std::vector<DirEvent> Parse(const std::vector<uint8_t>& buffer, uint32_t bytes);

protected:
    bool Arm(IoContext& ctx) override;
    void Notify(uint32_t bytes, uint32_t error,
                const std::vector<uint8_t>& data) override;

private:
    // 单个固定的 IoContext（一次只有一个挂起的 ReadDirectoryChangesW）。
    // completion 通过它的 ov 找回，回调在 loop 线程转发给 Watcher::OnCompletion。
    IoContext ctx_;
    DWORD bytes_returned_ = 0;  // ReadDirectoryChangesW 的输出参数（overlapped 下也要给）
    std::wstring dir_;
    DirCallback dir_cb_;
};

// 把 DirWatcher 包成一个 EventSource（便于 CloseAll / Reap 统一管理）。
class DirSource final : public EventSource {
public:
    DirSource(IocpBackend& iocp, const std::wstring& dir, DirCallback cb);
    bool Start() override;
    DirWatcher* Get() const { return raw_; }

private:
    std::wstring dir_;
    DirCallback cb_;
    DirWatcher* raw_ = nullptr;  // 指向 watchers_ 里的对象，非所有权
};

}  // namespace tui::io
