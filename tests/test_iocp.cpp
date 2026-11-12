#include "test_framework.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/io/dir_watcher.h"
#include "tui/io/event_source.h"
#include "tui/io/iocp_backend.h"
#include "tui/io/io_context.h"
#include "tui/io/watcher.h"

using namespace tui;

// ---------------------------------------------------------------------------
// 一个「可控注入 completion」的假 Watcher，用来精确驱动 close 状态机
// （不依赖真实内核 I/O，可在任何环境验证约束 C3/C4）。
// ---------------------------------------------------------------------------

class FakeWatcher final : public io::Watcher {
public:
    explicit FakeWatcher(io::IocpBackend& iocp)
        : io::Watcher(iocp, reinterpret_cast<HANDLE>(0x1234), nullptr) {}

    bool Start() { return BeginOperation(ctx_); }

    // 模拟内核投递一个完成包（可控注入）。cancelled=true 模拟 CancelIoEx 的取消包。
    void Inject(uint32_t bytes, bool cancelled) {
        ctx_.error = cancelled ? ERROR_OPERATION_ABORTED : 0;
        ctx_.cancelled = cancelled;
        iocp_.PostCompletion(io::kKeyWatcher, bytes, &ctx_.ov);
    }

    // 供测试读取状态
    size_t inflight() const { return inflight_; }
    uint64_t gen() const { return generation_; }

protected:
    bool Arm(io::IoContext& ctx) override {
        // 假操作：不碰内核，只把回调与 generation 设好。
        ctx.callback = [this, &ctx](uint32_t b, uint32_t e) {
            OnCompletion(ctx, b, e);
        };
        ctx.generation = generation_;
        return true;
    }
    void Notify(uint32_t, uint32_t, const std::vector<uint8_t>&) override {
        ++notify_count_;
    }

private:
    io::IoContext ctx_;
    int notify_count_ = 0;
};

// ---------------------------------------------------------------------------
// close 状态机
// ---------------------------------------------------------------------------

TEST(watcher_starts_active_with_one_inflight) {
    io::IocpBackend backend;
    FakeWatcher w(backend);
    CHECK_EQ(static_cast<int>(w.State()), static_cast<int>(io::WatcherState::kActive));
    CHECK_TRUE(w.Start());
    CHECK_EQ(w.inflight(), static_cast<size_t>(1));
}

TEST(watcher_close_transitions_to_closing_then_closed_after_drain) {
    io::IocpBackend backend;
    FakeWatcher w(backend);
    w.Start();

    // Close()：CancelIoEx 语义 → 有 inflight 时停留在 kClosing。
    // 假句柄的 CancelIoEx 会失败（ERROR_INVALID_HANDLE），但我们仍应进入 closing。
    w.Close();
    CHECK_EQ(static_cast<int>(w.State()), static_cast<int>(io::WatcherState::kClosing));

    // 注入取消完成包并让 backend 收集 + 分发。
    w.Inject(0, true);
    backend.Wait(1000);
    backend.Dispatch();  // 回调在「loop 线程」执行（此处就是测试线程）
    CHECK_EQ(static_cast<int>(w.State()), static_cast<int>(io::WatcherState::kClosed));
    CHECK_EQ(w.inflight(), static_cast<size_t>(0));
}

TEST(close_from_inside_callback_is_safe) {
    // 出口条件：在 completion 回调里关闭 watcher，不崩溃、状态机正确收尾。
    io::IocpBackend backend;
    bool closed_in_callback = false;

    // 给 FakeWatcher 加一个「正常完成时调用」的 hook：在回调内部请求 Close。
    class ClosingWatcher final : public io::Watcher {
    public:
        explicit ClosingWatcher(io::IocpBackend& b, bool& flag)
            : io::Watcher(b, reinterpret_cast<HANDLE>(0x1234), nullptr), flag_(flag) {}
        bool Start() { return BeginOperation(ctx_); }
        void Inject(uint32_t bytes, bool cancelled) {
            ctx_.error = cancelled ? ERROR_OPERATION_ABORTED : 0;
            ctx_.cancelled = cancelled;
            iocp_.PostCompletion(io::kKeyWatcher, bytes, &ctx_.ov);
        }
        size_t inflight() const { return inflight_; }
        uint64_t gen() const { return generation_; }
    protected:
        bool Arm(io::IoContext& ctx) override {
            ctx.callback = [this, &ctx](uint32_t b, uint32_t e) {
                OnCompletion(ctx, b, e);
            };
            ctx.generation = generation_;
            return true;
        }
        void Notify(uint32_t, uint32_t, const std::vector<uint8_t>&) override {
            // 模拟「读到 EOF / 用户按退出」：在回调内部关闭自己。
            Close();
            flag_ = true;
        }
    private:
        io::IoContext ctx_;
        bool& flag_;
    } w(backend, closed_in_callback);
    w.Start();

    // 注入一个「正常完成」包 → OnCompletion 调 Notify → 内部 Close()
    w.Inject(10, false);
    backend.Wait(1000);
    backend.Dispatch();  // 回调在 loop 线程执行，内部 Close() 必须安全
    CHECK_TRUE(closed_in_callback);
    // OnCompletion 进入时 inflight 已减到 0（本次 completion 已被消费），
    // 因此 Close() 无需等待排空，直接到 kClosed —— 这是正确的快路径。
    CHECK_EQ(static_cast<int>(w.State()), static_cast<int>(io::WatcherState::kClosed));
    CHECK_EQ(w.inflight(), static_cast<size_t>(0));
    // 且回调里请求关闭后，不应再重新发起新的 overlapped 操作。
    CHECK_EQ(w.gen(), static_cast<uint64_t>(1));  // generation 未递增
}

TEST(stale_completion_is_dropped_by_generation) {
    io::IocpBackend backend;
    FakeWatcher w(backend);
    w.Start();
    // 制造一个「过期」context：generation 落后于当前，但让它的回调
    // 真的转发给 w.OnCompletion，以验证 generation 检查确实生效。
    io::IoContext stale;
    stale.owner = &w;
    stale.generation = 0;  // 与 w 的 generation_=1 不符
    stale.callback = [&w, &stale](uint32_t b, uint32_t e) {
        w.OnCompletion(stale, b, e);
    };
    backend.PostCompletion(io::kKeyWatcher, 5, &stale.ov);
    backend.Wait(1000);
    backend.Dispatch();  // 应被 Watcher 的 generation 检查丢弃
    CHECK_EQ(static_cast<int>(w.State()), static_cast<int>(io::WatcherState::kActive));
    CHECK_EQ(w.inflight(), static_cast<size_t>(1));  // 未被误减
}

TEST(close_with_zero_inflight_goes_straight_to_closed) {
    io::IocpBackend backend;
    FakeWatcher w(backend);
    // 不 Start，直接 Close：无挂起操作，应立刻 kClosed。
    w.Close();
    CHECK_EQ(static_cast<int>(w.State()), static_cast<int>(io::WatcherState::kClosed));
}

// ---------------------------------------------------------------------------
// EventSource::Reap：延迟回收，保证「回调里关闭」不 use-after-free
// ---------------------------------------------------------------------------

TEST(eventsource_reap_after_close_all_and_drain) {
    io::IocpBackend backend;
    struct Src : io::EventSource {
        using io::EventSource::EventSource;
        bool Start() override { return true; }
        void Add(std::unique_ptr<io::Watcher> w) { watchers_.push_back(std::move(w)); }
    } src(backend);

    auto owned = std::make_unique<FakeWatcher>(backend);
    FakeWatcher* raw = owned.get();
    owned->Start();
    src.Add(std::move(owned));

    src.CloseAll();
    src.Reap();
    CHECK_EQ(src.WatcherCount(), static_cast<size_t>(1));  // 还在等 completion

    raw->Inject(0, true);
    backend.Wait(1000);
    backend.Dispatch();
    src.Reap();
    CHECK_EQ(src.WatcherCount(), static_cast<size_t>(0));  // 已回收
}

// ---------------------------------------------------------------------------
// IocpBackend 自身
// ---------------------------------------------------------------------------

TEST(iocp_wake_and_dispatch) {
    io::IocpBackend backend;
    CHECK_TRUE(backend.Ok());
    backend.Wake();
    CHECK_TRUE(backend.Wait(1000));  // 收到 kKeyWake 包
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(1));
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(0));  // 已排空
}

TEST(iocp_posted_completion_reaches_callback) {
    io::IocpBackend backend;
    io::IoContext ctx;
    ctx.callback = [&](uint32_t bytes, uint32_t) { CHECK_EQ(bytes, 42u); };
    backend.PostCompletion(io::kKeyWatcher, 42, &ctx.ov);
    backend.Wait(1000);
    CHECK_EQ(backend.Dispatch(), static_cast<size_t>(1));
}

TEST(eventloop_with_iocp_backend_dispatches_completions) {
    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend* bp = backend.get();
    core::EventLoop loop(clock, std::move(backend));

    int count = 0;
    io::IoContext ctx;
    ctx.callback = [&](uint32_t, uint32_t) { ++count; };
    bp->PostCompletion(io::kKeyWatcher, 1, &ctx.ov);

    // 让 loop 先 Wait 收集完成包（第 1 轮），再 Dispatch 执行回调（第 2 轮）。
    // 用一个短定时器在第 3 轮附近 Stop，保证 Wait/Dispatch 都发生过。
    loop.PostDelayedTask(30, [&]() { loop.Stop(); });
    loop.Run();
    CHECK_EQ(count, 1);
    CHECK_EQ(loop.State(), core::LoopState::kStopped);
}

// ---------------------------------------------------------------------------
// DirWatcher::Parse 纯函数（FILE_NOTIFY_INFORMATION 解析）
// ---------------------------------------------------------------------------

TEST(dir_parse_single_entry) {
    // 手工构造一条 FILE_NOTIFY_INFORMATION：NextEntryOffset=0, Action=ADDED, Name="a.txt"
    struct Raw {
        DWORD next;
        DWORD action;
        DWORD name_len;
        wchar_t name[6];
    };
    Raw r{};
    r.next = 0;
    r.action = FILE_ACTION_ADDED;
    const std::wstring nm = L"a.txt";
    r.name_len = static_cast<DWORD>(nm.size() * sizeof(wchar_t));
    std::copy(nm.begin(), nm.end(), r.name);

    std::vector<uint8_t> buf(sizeof(Raw));
    std::memcpy(buf.data(), &r, sizeof(Raw));
    auto evs = io::DirWatcher::Parse(buf, static_cast<uint32_t>(sizeof(Raw)));
    CHECK_EQ(evs.size(), static_cast<size_t>(1));
    CHECK_EQ(static_cast<int>(evs[0].action), static_cast<int>(io::DirEvent::Action::kAdded));
    CHECK_EQ(evs[0].name, std::wstring(L"a.txt"));
}

TEST(dir_parse_two_entries_with_next_offset) {
    // 两条记录，第一条 NextEntryOffset 指向第二条。
    struct Rec1 { DWORD next, action, name_len; wchar_t name[2]; };  // "A" + padding
    struct Rec2 { DWORD next, action, name_len; wchar_t name[2]; };  // "B"
    Rec1 a{}; Rec2 b{};
    a.action = FILE_ACTION_ADDED; a.name_len = 2; a.name[0] = L'A';
    b.action = FILE_ACTION_REMOVED; b.name_len = 2; b.name[0] = L'B';
    const size_t off = (sizeof(Rec1) + 3) & ~static_cast<size_t>(3);  // DWORD 对齐
    a.next = static_cast<DWORD>(off);

    std::vector<uint8_t> buf(off + sizeof(Rec2));
    std::memcpy(buf.data(), &a, sizeof(Rec1));
    std::memcpy(buf.data() + off, &b, sizeof(Rec2));
    auto evs = io::DirWatcher::Parse(buf, static_cast<uint32_t>(buf.size()));
    CHECK_EQ(evs.size(), static_cast<size_t>(2));
    CHECK_EQ(evs[0].name, std::wstring(L"A"));
    CHECK_EQ(static_cast<int>(evs[1].action), static_cast<int>(io::DirEvent::Action::kRemoved));
    CHECK_EQ(evs[1].name, std::wstring(L"B"));
}

TEST(dir_parse_empty_returns_empty) {
    std::vector<uint8_t> buf(64, 0);
    CHECK_EQ(io::DirWatcher::Parse(buf, 0).size(), static_cast<size_t>(0));
}

// ---------------------------------------------------------------------------
// 端到端：真实目录监视（ReadDirectoryChangesW + IOCP + close 状态机）
// ---------------------------------------------------------------------------

TEST(dir_watcher_end_to_end_create_file_and_close_cleanly) {
    namespace fs = std::filesystem;
    // 临时目录
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path dir = fs::path(tmp) / L"tui_stage3_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (!fs::exists(dir)) {
        CHECK_TRUE(false);  // 环境异常
        return;
    }

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& bp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    std::atomic<int> added{0};
    std::atomic<bool> got_event{false};

    io::DirSource source(bp, dir.wstring(), [&](const io::DirEvent& e) {
        if (e.action == io::DirEvent::Action::kAdded) {
            ++added;
            got_event.store(true);
        }
    });

    if (!source.Start()) {
        fs::remove_all(dir, ec);
        CHECK_TRUE(false);
        return;
    }

    // 从另一个线程创建文件，触发目录变更通知。
    std::thread creator([dir]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ofstream f(dir / "hello.txt");
        f << "hi";
        f.close();
    });

    // 分两步验证约束 C4 的顺序：
    //   t=200ms  CloseAll() → CancelIoEx，进入 kClosing；
    //   t=350ms  取消包已被 loop 的 Wait/Dispatch 处理 → kClosed → Reap 回收 → Stop。
    loop.PostDelayedTask(200, [&]() { source.CloseAll(); });
    loop.PostDelayedTask(350, [&]() {
        source.Reap();
        loop.Stop();
    });

    loop.Run();
    creator.join();

    CHECK_TRUE(got_event.load());
    CHECK_TRUE(added.load() >= 1);

    fs::remove_all(dir, ec);
}

int main() { return tui::test::RunAll(); }
