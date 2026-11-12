// 阶段 5：backend_compare —— 同一套 EventLoop 接口切换两种等待后端
//
// 目的：把「为什么 Windows 需要 IOCP」变成可运行的对照实验。
//   1) 相同的工作负载（N 个任务 + 1 个定时器 + I/O 完成）分别跑在
//      WfmoBackend 与 IocpBackend 上，逻辑完全一致（约束 C1 出口条件）。
//   2) 打印 WFMO 的句柄上限：注册 70 个事件，只有 63 个成功；
//      而 IOCP 一个端口就能承载任意多个完成包。
//   3) 演示 APC：只有可警报等待（WFMO alertable=TRUE）才会执行 QueueUserAPC。
#include <cstdio>
#include <memory>
#include <vector>

#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/core/wait_backend.h"
#include "tui/core/wfmo_backend.h"
#include "tui/io/iocp_backend.h"

using namespace tui;

namespace {

// 同一份「应用逻辑」：N 个投递任务 + 周期心跳 + 一个停止定时器。
// 通过传入不同的后端，证明 EventLoop 与等待模型解耦。
int RunWorkload(const char* label, std::unique_ptr<core::IWaitBackend> backend) {
    core::SystemClock clock;
    core::EventLoop loop(clock, std::move(backend));
    const char* name = loop.WaitBackend().Name();

    int tasks = 0;
    int beats = 0;
    for (int i = 0; i < 20; ++i) {
        loop.PostTask([&]() { ++tasks; });
    }
    loop.PostRepeatingTask(15, 15, [&]() { ++beats; });
    loop.PostDelayedTask(120, [&]() { loop.Stop(); });

    loop.Run();
    std::printf("[%s] backend=%-6s tasks=%d beats=%d state=%d\n",
                label, name, tasks, beats, static_cast<int>(loop.State()));
    return tasks + beats;
}

}  // namespace

int main() {
    std::printf("[compare] Stage 5: IOCP vs WaitForMultipleObjectsEx\n\n");

    // 1) 同一工作负载换后端。
    int a = RunWorkload("run1", std::make_unique<core::WfmoBackend>());
    int b = RunWorkload("run2", std::make_unique<io::IocpBackend>());
    std::printf("[compare] identical logic -> wfmo=%d iocp=%d (same=%s)\n\n",
                a, b, a == b ? "yes" : "no(time jitter)");

    // 2) 64 句柄上限。
    core::WfmoBackend wfmo;
    std::vector<HANDLE> events;
    for (int i = 0; i < 70; ++i) {
        events.push_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    }
    int registered = 0;
    for (HANDLE h : events) {
        if (wfmo.Register(h, []() {}) != core::WfmoBackend::kInvalidSlot) ++registered;
    }
    std::printf("[compare] WFMO: MAXIMUM_WAIT_OBJECTS=%d, wake slot=1, "
                "registered=%d/70 (limit=%zu)\n",
                MAXIMUM_WAIT_OBJECTS, registered, core::WfmoBackend::kMaxUserHandles);
    io::IocpBackend iocp;
    std::printf("[compare] IOCP: one port, no handle-count cap (completion packets unlimited)\n");
    for (HANDLE h : events) CloseHandle(h);

    // 3) APC 对照。
    core::WfmoBackend apc_backend;
    apc_backend.BindLoopThread();
    int apc_ran = 0;
    apc_backend.QueueApc([&]() { ++apc_ran; });
    apc_backend.Wait(500);      // alertable=TRUE → APC 执行
    apc_backend.Dispatch();     // 回调在 Dispatch 统一执行
    std::printf("[compare] APC during alertable wait: ran=%d, last_wait=0x%lx "
                "(WAIT_IO_COMPLETION=0x%lx)\n",
                apc_ran, static_cast<unsigned long>(apc_backend.LastWaitResult()),
                static_cast<unsigned long>(WAIT_IO_COMPLETION));

    std::printf("\n[compare] done\n");
    return 0;
}
