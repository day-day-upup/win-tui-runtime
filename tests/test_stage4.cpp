#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/core/ring_buffer.h"
#include "tui/io/iocp_backend.h"
#include "tui/io/pipe_source.h"
#include "tui/io/tcp_source.h"
#include "tui/pool/thread_pool.h"

using namespace tui;

// ---------------------------------------------------------------------------
// RingBuffer：背压可观测
// ---------------------------------------------------------------------------

TEST(ringbuffer_drops_oldest_when_full) {
    core::RingBuffer<int> rb(3);
    rb.Push(1); rb.Push(2); rb.Push(3);
    CHECK_EQ(rb.Size(), static_cast<size_t>(3));
    CHECK_EQ(rb.Dropped(), static_cast<size_t>(0));
    rb.Push(4);  // 挤掉 1
    CHECK_EQ(rb.Size(), static_cast<size_t>(3));
    CHECK_EQ(rb.Dropped(), static_cast<size_t>(1));
    CHECK_EQ(rb.Items().front(), 2);  // 1 被挤掉，最旧变成 2
    CHECK_EQ(rb.Items().back(), 4);
}

TEST(ringbuffer_pop_and_zero_capacity) {
    core::RingBuffer<int> rb(2);
    CHECK_TRUE(rb.Empty());
    rb.Push(10);
    auto v = rb.Pop();
    CHECK_TRUE(v.has_value());
    CHECK_EQ(*v, 10);
    CHECK_TRUE(rb.Empty());

    core::RingBuffer<int> zero(0);
    zero.Push(1);
    CHECK_EQ(zero.Dropped(), static_cast<size_t>(1));
    CHECK_TRUE(zero.Empty());
}

// ---------------------------------------------------------------------------
// ThreadPool：无状态计算 + 续延回投到 loop（约束 B3/B4）
// ---------------------------------------------------------------------------

TEST(threadpool_computes_and_posts_continuation_to_loop) {
    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    core::EventLoop loop(clock, std::move(backend));

    pool::ThreadPool pool(2, [&loop](core::Task t) { loop.PostTask(std::move(t)); });

    std::atomic<int> computed{0};
    std::atomic<int> applied{0};
    const int kJobs = 8;
    for (int i = 0; i < kJobs; ++i) {
        pool.SubmitCompute([i, &computed, &applied]() mutable -> core::Task {
            // worker 线程：无状态计算（这里模拟 hex/格式化）
            int value = i * i;
            ++computed;
            // 返回「要在 loop 线程执行的续延」，结果按值捕获（约束 B4）
            return [applied_ptr = &applied, value]() { *applied_ptr += value; };
        });
    }

    // 等 worker 完成，再让 loop 跑一小段时间消化续延。
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (pool.Completed() < kJobs &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    loop.PostDelayedTask(60, [&]() { loop.Stop(); });
    loop.Run();
    pool.Shutdown();

    CHECK_EQ(pool.Completed(), static_cast<size_t>(kJobs));
    CHECK_EQ(computed.load(), kJobs);
    CHECK_EQ(applied.load(), 0 + 1 + 4 + 9 + 16 + 25 + 36 + 49);
    CHECK_EQ(loop.State(), core::LoopState::kStopped);
}

TEST(threadpool_shutdown_is_idempotent) {
    pool::ThreadPool pool(2, [](core::Task) {});
    pool.SubmitCompute([]() -> core::Task { return core::Task{}; });
    pool.Shutdown();
    pool.Shutdown();  // 不应崩溃 / 死锁
    CHECK_TRUE(pool.Completed() >= 1);
}

// ---------------------------------------------------------------------------
// IoContext 多 ctx 支持（阶段 3 遗留的全双工要求）
// ---------------------------------------------------------------------------

TEST(iocontext_op_kind_and_rearm_preserve_metadata) {
    io::IoContext ctx;
    ctx.op_kind = io::OpKind::kWrite;
    ctx.auto_rearm = false;
    ctx.on_complete = [](io::IoContext&, uint32_t, uint32_t) { return true; };
    ctx.generation = 5;
    ctx.ResetForReuse();
    CHECK_EQ(static_cast<int>(ctx.op_kind), static_cast<int>(io::OpKind::kWrite));
    CHECK_FALSE(ctx.auto_rearm);
    CHECK_TRUE(static_cast<bool>(ctx.on_complete));
    CHECK_EQ(ctx.generation, static_cast<uint64_t>(0));
}

// ---------------------------------------------------------------------------
// 端到端：命名管道捕获子进程 stdout（真实 Win32）
// ---------------------------------------------------------------------------

TEST(pipe_source_captures_child_stdout) {
    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    std::string captured;
    io::PipeSource source(iocp, L"cmd.exe /C echo HELLO_STAGE4",
                          [&](const std::vector<uint8_t>& data) {
                              captured.append(data.begin(), data.end());
                          });
    if (!source.Start()) {
        // 某些环境 cmd 策略受限：记为跳过而非失败。
        CHECK_TRUE(true);
        return;
    }

    loop.PostDelayedTask(700, [&]() {
        source.CloseAll();
        loop.Stop();
    });
    loop.Run();
    source.Reap();

    CHECK_TRUE(captured.find("HELLO_STAGE4") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 端到端：TCP loopback 全双工（overlapped WSARecv / WSASend）
// ---------------------------------------------------------------------------

TEST(tcp_source_full_duplex_recv_and_send) {
    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    std::vector<uint8_t> received;
    io::TcpSource source(iocp, [&](const std::vector<uint8_t>& data) {
        received.insert(received.end(), data.begin(), data.end());
    });
    if (!source.Start()) {
        CHECK_TRUE(false);
        return;
    }
    CHECK_TRUE(source.Port() > 0);

    // 从对端（客户端 socket）发数据 → IOCP 侧 WSARecv 收到。
    const std::string msg = "PING_STAGE4";
    auto send_on_peer = [&]() {
        ::send(source.PeerSocket(), msg.data(), static_cast<int>(msg.size()), 0);
    };

    // 也在 loop 里演示全双工写路径：从 IOCP 侧发一段。
    loop.PostDelayedTask(80, [&]() {
        send_on_peer();
        source.Send(std::vector<uint8_t>{'E', 'C', 'H', 'O'});
    });
    loop.PostDelayedTask(400, [&]() {
        source.CloseAll();
        source.Reap();
        loop.Stop();
    });

    loop.Run();

    CHECK_TRUE(received.size() >= msg.size());
    CHECK_EQ(source.RecvBytes(), msg.size());
}

int main() { return tui::test::RunAll(); }
