#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "tui/console/input_decoder.h"
#include "tui/core/platform.h"

// 阶段 2：ConsoleInputThread —— 独占输入线程（约束 B7）
//
// 该线程阻塞在 ReadConsoleInput，解析出 InputEvent 后通过回调投递到
// EventLoop（由外部接成 PostTask）。它不碰任何渲染状态（约束 B1）。
//
// 停止的难点：ReadConsoleInput 是阻塞调用，没有 CancelSynchronousIo 之外的
// 干净取消手段。采用的 Win32 技巧是 WriteConsoleInput 注入一条「哨兵记录」：
//   - 用一个自定义的 KEY_EVENT（VK=0, UnicodeChar=0）作为唤醒标记；
//   - 线程读到它后检查 stop_ 标志并退出；
//   - 若注入失败（例如句柄是管道），退化用 CancelSynchronousIo。
// 这样 join 一定能返回，满足优雅退出要求。
namespace tui::console {

using InputCallback = std::function<void(const InputEvent&)>;

class ConsoleInputThread {
public:
    ConsoleInputThread(HANDLE conin, InputCallback cb);
    ~ConsoleInputThread();

    ConsoleInputThread(const ConsoleInputThread&) = delete;
    ConsoleInputThread& operator=(const ConsoleInputThread&) = delete;

    void Start();
    void Stop();  // 请求停止并 join
    bool IsRunning() const { return running_.load(); }

private:
    void ThreadMain();
    void InjectWakeupRecord();

    HANDLE conin_;
    InputCallback cb_;
    std::thread thread_;
    HANDLE thread_handle_ = nullptr;  // 仅用于 CancelSynchronousIo
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
};

}  // namespace tui::console
