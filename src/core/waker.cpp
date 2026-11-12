#include "tui/core/waker.h"

namespace tui::core {

Waker::Waker() {
    // auto-reset（第二个参数 FALSE）、初始无信号（第三个参数 FALSE）、无名。
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // 阶段 1 不引入异常体系；若创建失败，后续 Wait 会立即返回，
    // 由 EventLoop 的断言暴露。生产代码应在构造后检查并报错。
}

Waker::~Waker() {
    if (event_) CloseHandle(event_);
}

void Waker::Wake() {
    if (event_) SetEvent(event_);
}

bool Waker::Wait(DWORD timeout_ms) {
    if (!event_) return true;
    // bAlertable = TRUE：允许 APC 在此执行（阶段 5 对照用）。
    DWORD r = WaitForSingleObjectEx(event_, timeout_ms, TRUE);
    // WAIT_OBJECT_0：event 被置位（有任务/定时器到期，或纯唤醒）
    // WAIT_IO_COMPLETION：APC 已执行，继续下一轮
    // WAIT_TIMEOUT：定时器窗口到了，需要处理到期定时器
    return r == WAIT_OBJECT_0 || r == WAIT_IO_COMPLETION;
}

}  // namespace tui::core
