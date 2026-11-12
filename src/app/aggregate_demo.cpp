// 阶段 4：聚合控制台 demo —— 把六类子系统接进同一个 EventLoop
//
// 覆盖：
//   S1 定时器        EventLoop::PostRepeatingTask（心跳 / 渲染 tick）
//   S2 任务队列      PostTask（worker 回投、控制命令）
//   S3 目录监视      DirSource（阶段 3，IOCP）
//   S4 子进程管道    PipeSource（命名管道 + CreateProcess）
//   S5 TCP           TcpSource（Winsock overlapped，全双工）
//   S6 worker 线程池 ThreadPool（无状态 hex 计算，续延回投）
//   + 差分渲染       Renderer(MemoryBackend) 按 tick 合并
//   + 背压           RingBuffer<std::wstring> 上限 256
//
// 用 MemoryBackend 保证无控制台环境也可运行。约 1.2 秒后优雅退出。
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tui/console/render_backend.h"
#include "tui/console/renderer.h"
#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/core/ring_buffer.h"
#include "tui/io/dir_watcher.h"
#include "tui/io/iocp_backend.h"
#include "tui/io/pipe_source.h"
#include "tui/io/tcp_source.h"
#include "tui/pool/thread_pool.h"

using namespace tui;

namespace {
std::wstring ToHex(const std::vector<uint8_t>& data) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    std::wstring out;
    for (uint8_t b : data) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
        out += L' ';
    }
    return out;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path dir = fs::path(tmp) / L"tui_aggregate_demo";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    std::printf("[aggregate] start: 6 subsystems on one IOCP event loop\n");

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    console::Size size{72, 16};
    auto rbackend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(rbackend), size);
    core::RingBuffer<std::wstring> logs(256);  // 背压：满时丢最旧

    auto log = [&](const std::wstring& s) {
        logs.Push(s);
        renderer.MarkDirty();
    };

    renderer.Clear(console::Cell{});
    renderer.Put(1, 0,
                 std::wstring(L" win-tui-runtime | Stage 4: Aggregate Console (IOCP) "),
                 console::MakeAttr(console::Color::kBrightWhite, console::Color::kBlue));
    log(std::wstring(L"backend=iocp  sources=dir,pipe,tcp,timer,task,pool"));

    // --- S6 worker 线程池（约束 B3/B4）---
    pool::ThreadPool pool(4, [&loop](core::Task t) { loop.PostTask(std::move(t)); });

    // 把一段字节交给 worker 做无状态 hex 计算；续延经 post_cb 投回 loop，
    // 在 loop 线程里安全地调用 log()（约束 B3/B4：worker 不碰 loop 状态）。
    auto submit_hex = [&](const std::vector<uint8_t>& data, const std::wstring& tag) {
        pool.SubmitCompute([data, tag, &log]() mutable -> core::Task {
            std::wstring hex = ToHex(data);  // 无状态计算（worker 线程）
            return [tag, hex, &log]() {
                log(tag + L" " + hex);       // 续延在 loop 线程执行
            };
        });
    };

    // --- S3 目录监视 ---
    io::DirSource dsrc(iocp, dir.wstring(), [&](const io::DirEvent& e) {
        std::wstring act = L"?";
        switch (e.action) {
            case io::DirEvent::Action::kAdded: act = L"ADDED"; break;
            case io::DirEvent::Action::kModified: act = L"MODIFIED"; break;
            case io::DirEvent::Action::kRemoved: act = L"REMOVED"; break;
            default: break;
        }
        log(std::wstring(L"[dir ] ") + act + L" " + e.name);
    });
    if (!dsrc.Start()) return 1;

    // --- S4 子进程管道 ---
    io::PipeSource psrc(iocp, L"cmd.exe /C echo STAGE4_PIPE_OK",
                        [&](const std::vector<uint8_t>& data) {
                            submit_hex(data, std::wstring(L"[pipe]"));
                        });
    if (!psrc.Start()) {
        std::printf("[aggregate] pipe source unavailable (skipped)\n");
    }

    // --- S5 TCP 全双工 ---
    io::TcpSource tsrc(iocp, [&](const std::vector<uint8_t>& data) {
        submit_hex(data, std::wstring(L"[tcp ]"));
    });
    if (!tsrc.Start()) return 1;
    // 从对端发数据，触发 IOCP 侧 WSARecv。
    const std::string ping = "PING";
    loop.PostDelayedTask(120, [&]() {
        ::send(tsrc.PeerSocket(), ping.data(), static_cast<int>(ping.size()), 0);
        tsrc.Send(std::vector<uint8_t>{'P', 'O', 'N', 'G'});  // 全双工写
    });

    // --- S1 定时器：心跳 ---
    int beats = 0;
    loop.PostRepeatingTask(100, 100, [&]() {
        ++beats;
        log(std::wstring(L"[tick] heartbeat #") + std::to_wstring(beats));
    });

    // --- 差分渲染 tick（约束 D4）---
    auto draw = [&]() {
        const int16_t h = renderer.GetSize().h;
        int16_t y = 2;
        const auto& items = logs.Items();
        size_t start = items.size() > static_cast<size_t>(h - 4)
                           ? items.size() - static_cast<size_t>(h - 4)
                           : 0;
        for (size_t i = start; i < items.size() && y < h - 2; ++i, ++y) {
            renderer.Put(1, y, items[i],
                         console::MakeAttr(console::Color::kBrightCyan,
                                           console::Color::kBlack));
        }
        renderer.Put(1, h - 1,
                     std::wstring(L" logs=") + std::to_wstring(logs.Size()) +
                         L" dropped=" + std::to_wstring(logs.Dropped()) +
                         L" presents=" + std::to_wstring(renderer.PresentCount()),
                     console::MakeAttr(console::Color::kBlack, console::Color::kCyan));
        renderer.RenderTick();
    };
    loop.PostRepeatingTask(16, 16, draw);  // 渲染 tick

    // 制造目录事件的子线程。
    std::thread creator([dir]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int i = 0; i < 3; ++i) {
            std::ofstream f(dir / ("f" + std::to_string(i) + ".txt"));
            f << "x";
        }
    });

    // --- 优雅关闭（约束 C4 顺序）---
    loop.PostDelayedTask(900, [&]() {
        dsrc.CloseAll();
        psrc.CloseAll();
        tsrc.CloseAll();
        log(std::wstring(L"[stop] CancelIoEx issued..."));
    });
    loop.PostDelayedTask(1200, [&]() {
        dsrc.Reap();
        psrc.Reap();
        tsrc.Reap();
        pool.Shutdown();  // 先停 worker，避免它再 PostTask 到已退出的 loop
        log(std::wstring(L"[stop] reaped, pool joined"));
        loop.Stop();
    });

    loop.Run();
    creator.join();
    draw();  // 补最后一帧

    std::printf("[aggregate] beats=%d log_lines=%zu dropped=%zu presents=%zu\n",
                beats, logs.Size(), logs.Dropped(), renderer.PresentCount());
    std::printf("[aggregate] sources: dir=%zu pipe=%zu tcp=%zu  state=%d\n",
                dsrc.WatcherCount(), psrc.WatcherCount(), tsrc.WatcherCount(),
                static_cast<int>(loop.State()));
    std::printf("[aggregate] tcp recv=%zu sent=%zu\n",
                tsrc.RecvBytes(), tsrc.SentBytes());

    fs::remove_all(dir, ec);
    return 0;
}
