#pragma once

#include <memory>
#include <vector>

#include "tui/io/watcher.h"

// 阶段 3：EventSource —— 可插拔事件源基类
//
// 持有若干 Watcher，负责：
//   - 注册 / 启动（Start）；
//   - 在 loop 线程「回收」已 kClosed 的 watcher（Reap），
//     这是「回调里安全关闭 watcher」的最后一环：Close() 只标记，
//     真正的 delete 延迟到所有 completion 排空后的 Reap；
//   - 关闭全部（CloseAll，约束 C4 顺序）。
//
// 阶段 4 会给它派生 PipeSource / SocketSource / SerialSource，
// 阶段 6 capstone 再加两个 source 即可拼出串口网络助手。
namespace tui::io {

class IocpBackend;

class EventSource {
public:
    explicit EventSource(IocpBackend& iocp) : iocp_(iocp) {}
    virtual ~EventSource() = default;

    EventSource(const EventSource&) = delete;
    EventSource& operator=(const EventSource&) = delete;

    virtual bool Start() = 0;
    virtual void CloseAll();  // 阶段 4：派生类可追加清理（如子进程）
    void Reap();  // 仅 loop 线程调用
    size_t WatcherCount() const { return watchers_.size(); }

protected:
    IocpBackend& iocp_;
    std::vector<std::unique_ptr<Watcher>> watchers_;
};

}  // namespace tui::io
