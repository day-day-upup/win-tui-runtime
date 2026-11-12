#pragma once

#include "tui/core/wait_backend.h"
#include "tui/core/waker.h"

// 阶段 3：WakerBackend —— 阶段 1/2 行为的默认后端
//
// 包装现有 Waker，Dispatch 永远返回 0（事件唤醒只靠 TaskQueue）。
// 有了它，现有 EventLoop 用户与 15+14 个测试无需改动即可继续跑。
namespace tui::core {

class WakerBackend final : public IWaitBackend {
public:
    void Wake() override { waker_.Wake(); }
    size_t Dispatch() override { return 0; }
    bool Wait(DWORD timeout_ms) override { return waker_.Wait(timeout_ms); }
    void Shutdown() override { waker_.Wake(); }
    const char* Name() const override { return "waker"; }

private:
    Waker waker_;
};

}  // namespace tui::core
