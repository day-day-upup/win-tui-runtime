#pragma once

#include "tui/core/platform.h"

// 阶段 1：Waker —— Windows 版 self-pipe
//
// Unix 事件循环常用 pipe 唤醒阻塞的 epoll/select；Windows 上没有等价物，
// 标准做法是「一个可等待的内核对象 + 跨线程 SetEvent」。
//
// 这里用 auto-reset event：
//   - Wake() 可在任意线程调用（阶段 4 的 I/O 线程 / worker 线程池会用）；
//   - loop 线程在 WaitForMultipleObjectsEx 上等待它；
//   - 阶段 3 引入 IOCP 后，Waker 会退化为 GetQueuedCompletionStatusEx 的
//     timeout 计算 + 一个保留 completion key，但「跨线程唤醒」的语义不变。
//
// 为什么用「可警报等待」(Ex / bAlertable=TRUE)？
//   为阶段 5 的 APC 对照实验留口子：ReadFileEx/WriteFileEx 的完成回调
//   以 APC 形式投递，只有当线程进入 alertable wait 才会执行。
namespace tui::core {

class Waker {
public:
    Waker();
    ~Waker();

    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    // 任意线程：唤醒正在等待的 loop。
    void Wake();

    // loop 线程：等待唤醒或超时。
    // timeout_ms == INFINITE 表示无限等；返回 TRUE 表示 event 被置位（有活要干）。
    // 用 WaitForSingleObjectEx 且 bAlertable=TRUE，便于后续 APC 实验。
    bool Wait(DWORD timeout_ms);

    HANDLE Handle() const { return event_; }

private:
    HANDLE event_;  // auto-reset, 初始无信号
};

}  // namespace tui::core
