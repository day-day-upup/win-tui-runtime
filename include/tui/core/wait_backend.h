#pragma once

#include <cstddef>

#include "tui/core/platform.h"
#include "tui/core/types.h"

// 阶段 3：IWaitBackend —— 等待后端抽象
//
// 把阶段 1 的 Waker 泛化成可替换的「等待 + 完成分发」接口，是约束 C1 的落点：
//   - WakerBackend     ：默认后端（阶段 1/2 行为，auto-reset event + 可警报等待）
//   - IocpBackend      ：阶段 3 主后端（GetQueuedCompletionStatusEx）
//   - 阶段 5 再实现 WaitForMultipleObjectsEx 后端作对照
//
// 一次循环迭代的契约：
//   1) backend.Dispatch() —— 在 loop 线程执行「上一轮 Wait 收集到的完成包回调」；
//   2) 分发投递任务 / 定时器 / idle；
//   3) backend.Wait(timeout) —— 阻塞等待，收集完成包供下一轮 Dispatch。
//
// 为什么要拆开 Dispatch 与 Wait：
//   约束 B2 要求「I/O 线程只投递 completion，回调必须在 loop 线程执行」。
//   Wait 负责从内核取包（不跑业务回调），Dispatch 才在 loop 线程跑回调。
//   这样回调里可以安全地 PostTask / CancelTimer / 关闭 watcher，无锁、无竞态。
//
// Wake() 保留阶段 1 的语义：任意线程调用，唤醒阻塞中的 loop（self-pipe 等价物）。
namespace tui::core {

class IWaitBackend {
public:
    virtual ~IWaitBackend() = default;

    // 任意线程：唤醒阻塞的 loop。
    virtual void Wake() = 0;

    // 仅 loop 线程：执行已收集完成包的回调，返回执行数量。
    virtual size_t Dispatch() = 0;

    // 仅 loop 线程：阻塞最多 timeout_ms，收集完成包。返回是否有「活」需要处理。
    // INFINITE 表示无限等待。
    virtual bool Wait(DWORD timeout_ms) = 0;

    // 仅 loop 线程：关闭后端（排空完成包、释放句柄）。由 EventLoop 在退出时调用。
    virtual void Shutdown() = 0;

    virtual const char* Name() const = 0;
};

}  // namespace tui::core
