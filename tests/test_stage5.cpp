#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/core/wait_backend.h"
#include "tui/core/wfmo_backend.h"
#include "tui/core/waker_backend.h"
#include "tui/io/iocp_backend.h"

using namespace tui;

// ---------------------------------------------------------------------------
// 64 句柄上限（约束 C1 的核心对照点）
// ---------------------------------------------------------------------------

TEST(wfmo_maximum_wait_objects_is_64) {
    CHECK_EQ(static_cast<size_t>(MAXIMUM_WAIT_OBJECTS), static_cast<size_t>(64));
    CHECK_EQ(core::WfmoBackend::kMaxUserHandles, static_cast<size_t>(63));
}

TEST(wfmo_register_caps_at_63_user_handles) {
    core::WfmoBackend backend;
    // 造 70 个 auto-reset event，验证第 64 个起显式失败。
    std::vector<HANDLE> events;
    for (int i = 0; i < 70; ++i) {
        events.push_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    }
    int ok = 0;
    for (HANDLE h : events) {
        auto slot = backend.Register(h, []() {});
        if (slot != core::WfmoBackend::kInvalidSlot) ++ok;
    }
    CHECK_EQ(ok, 63);  // 64 - 1(wake slot) = 63，超出不静默截断
    CHECK_EQ(backend.RegisteredCount(), static_cast<size_t>(64));  // 含 slot 0
    for (HANDLE h : events) CloseHandle(h);
}

TEST(wfmo_unregister_frees_slot) {
    core::WfmoBackend backend;
    HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto s = backend.Register(h, []() {});
    CHECK_TRUE(s != core::WfmoBackend::kInvalidSlot);
    CHECK_EQ(backend.RegisteredCount(), static_cast<size_t>(2));
    backend.Unregister(s);
    CHECK_EQ(backend.RegisteredCount(), static_cast<size_t>(1));
    HANDLE h2 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto s2 = backend.Register(h2, []() {});
    CHECK_TRUE(s2 != core::WfmoBackend::kInvalidSlot);  // 空洞被复用
    CloseHandle(h);
    CloseHandle(h2);
}

// ---------------------------------------------------------------------------
// WFMO 的分发：句柄 signaled → Dispatch 在 loop 线程跑回调
// ---------------------------------------------------------------------------

TEST(wfmo_signaled_handle_dispatches_callback) {
    core::WfmoBackend backend;
    HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int count = 0;
    backend.Register(h, [&]() { ++count; });

    SetEvent(h);
    CHECK_TRUE(backend.Wait(1000));
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(1));
    CHECK_EQ(count, 1);
    // auto-reset event 已复位，第二轮不应再触发。
    CHECK_FALSE(backend.Wait(50));
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(0));
    CloseHandle(h);
}

TEST(wfmo_wake_returns_without_user_callback) {
    core::WfmoBackend backend;
    backend.Wake();
    CHECK_TRUE(backend.Wait(1000));       // slot 0 被 signaled
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(0));  // 纯唤醒，无用户回调
}

// ---------------------------------------------------------------------------
// APC：只有 alertable wait 才执行 QueueUserAPC（阶段 5 对照点）
// ---------------------------------------------------------------------------

TEST(wfmo_apc_runs_during_alertable_wait_and_dispatches) {
    core::WfmoBackend backend;
    backend.BindLoopThread();
    std::atomic<int> ran{0};
    CHECK_TRUE(backend.QueueApc([&]() { ++ran; }));

    // APC 已在队列；进入可警报等待会立即执行它（WAIT_IO_COMPLETION）。
    CHECK_TRUE(backend.Wait(1000));
    CHECK_EQ(backend.ApcRanCount(), static_cast<size_t>(1));
    CHECK_EQ(backend.LastWaitResult(), static_cast<DWORD>(WAIT_IO_COMPLETION));
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(1));  // 回调在 Dispatch 执行
    CHECK_EQ(ran.load(), 1);
}

TEST(wfmo_apc_from_another_thread) {
    core::WfmoBackend backend;
    backend.BindLoopThread();
    std::atomic<int> ran{0};
    std::thread poster([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        backend.QueueApc([&]() { ++ran; });
        backend.Wake();  // 确保 wait 被唤醒
    });
    // 可警报等待会执行 APC；若 APC 尚未投递，wake event 也会让它返回。
    backend.Wait(1000);
    backend.Dispatch();
    poster.join();
    CHECK_EQ(ran.load(), 1);
}

// ---------------------------------------------------------------------------
// 同一套 EventLoop 接口切换后端（约束 C1 出口条件）
// ---------------------------------------------------------------------------

// 用「同一个测试体」分别跑三种后端，验证 EventLoop 逻辑与后端无关。
template <typename MakeBackend>
void RunCommonLoopContract(const char* label, MakeBackend make) {
    core::SystemClock clock;
    core::EventLoop loop(clock, make());
    CHECK_TRUE(std::string(loop.WaitBackend().Name()).size() > 0);

    std::atomic<int> tasks{0};
    std::atomic<int> timers{0};
    for (int i = 0; i < 5; ++i) {
        loop.PostTask([&]() { ++tasks; });
    }
    loop.PostDelayedTask(20, [&]() {
        ++timers;
        loop.Stop();
    });
    loop.Run();
    CHECK_EQ(tasks.load(), 5);
    CHECK_EQ(timers.load(), 1);
    CHECK_EQ(loop.State(), core::LoopState::kStopped);
    (void)label;
}

TEST(eventloop_contract_waker_backend) {
    RunCommonLoopContract("waker", []() -> std::unique_ptr<core::IWaitBackend> {
        return std::make_unique<core::WakerBackend>();
    });
}

TEST(eventloop_contract_wfmo_backend) {
    RunCommonLoopContract("wfmo", []() -> std::unique_ptr<core::IWaitBackend> {
        return std::make_unique<core::WfmoBackend>();
    });
}

TEST(eventloop_contract_iocp_backend) {
    RunCommonLoopContract("iocp", []() -> std::unique_ptr<core::IWaitBackend> {
        return std::make_unique<io::IocpBackend>();
    });
}

TEST(wfmo_and_iocp_backend_names_differ) {
    core::WfmoBackend wfmo;
    io::IocpBackend iocp;
    CHECK_EQ(std::string(wfmo.Name()), std::string("wfmo"));
    CHECK_EQ(std::string(iocp.Name()), std::string("iocp"));
}

int main() { return tui::test::RunAll(); }
