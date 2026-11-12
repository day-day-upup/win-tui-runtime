// 阶段 3：IOCP demo —— 把目录监视事件画进 TUI（headless 也可跑）
//
// 串联阶段 1/2/3：
//   EventLoop(IocpBackend) + DirSource(ReadDirectoryChangesW/IOCP)
//   + Renderer(MemoryBackend) 把 DirEvent 画成日志
//   + 定时器在 2 秒后 CloseAll / Reap / Stop，验证优雅退出。
//
// 用 MemoryBackend 而非真实控制台，保证无控制台环境也能验证。
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "tui/console/render_backend.h"
#include "tui/console/renderer.h"
#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/io/dir_watcher.h"
#include "tui/io/iocp_backend.h"

using namespace tui;

int main() {
    namespace fs = std::filesystem;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path dir = fs::path(tmp) / L"tui_iocp_demo";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    std::printf("[iocp_demo] watching %ls\n", dir.c_str());

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    console::Size size{60, 12};
    auto rbackend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(rbackend), size);
    int log_line = 2;
    int event_count = 0;

    auto put_line = [&](const std::wstring& s) {
        if (log_line < size.h - 1) {
            renderer.Put(1, static_cast<int16_t>(log_line), s,
                         console::MakeAttr(console::Color::kBrightCyan,
                                           console::Color::kBlack));
            ++log_line;
        }
        renderer.MarkDirty();
    };

    renderer.Clear(console::Cell{});
    renderer.Put(1, 0,
                 std::wstring(L" win-tui-runtime | Stage 3: IOCP DirWatcher "),
                 console::MakeAttr(console::Color::kBrightWhite, console::Color::kBlue));
    put_line(std::wstring(L"backend = iocp (GetQueuedCompletionStatusEx)"));

    io::DirSource source(iocp, dir.wstring(), [&](const io::DirEvent& e) {
        ++event_count;
        const wchar_t* act = L"?";
        switch (e.action) {
            case io::DirEvent::Action::kAdded: act = L"ADDED   "; break;
            case io::DirEvent::Action::kRemoved: act = L"REMOVED "; break;
            case io::DirEvent::Action::kModified: act = L"MODIFIED"; break;
            case io::DirEvent::Action::kRenamedOld: act = L"REN_OLD "; break;
            case io::DirEvent::Action::kRenamedNew: act = L"REN_NEW "; break;
            default: break;
        }
        put_line(std::wstring(L"  ") + act + L"  " + e.name);
    });

    if (!source.Start()) {
        std::printf("[iocp_demo] failed to start DirSource\n");
        return 1;
    }
    put_line(std::wstring(L"watcher armed, inflight=1"));

    // 渲染 tick：16ms 合并刷新（约束 D4）。
    loop.PostRepeatingTask(16, 16, [&]() { renderer.RenderTick(); });

    // 从别的线程制造几个文件事件。
    std::thread creator([dir]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        for (int i = 0; i < 3; ++i) {
            std::ofstream f(dir / ("file" + std::to_string(i) + ".txt"));
            f << "data";
        }
    });

    // 约束 C4 顺序：t=600ms CloseAll（CancelIoEx → kClosing），
    //             t=900ms 取消包已排空 → Reap 回收 → Stop。
    loop.PostDelayedTask(600, [&]() {
        put_line(std::wstring(L"CloseAll() -> CancelIoEx..."));
        source.CloseAll();
    });
    loop.PostDelayedTask(900, [&]() {
        source.Reap();
        put_line(std::wstring(L"Reap() done, watchers=0"));
        loop.Stop();
    });

    loop.Run();
    creator.join();
    renderer.RenderTick();  // 补最后一帧

    std::printf("[iocp_demo] events=%d, presents=%zu, watchers_left=%zu\n",
                event_count, renderer.PresentCount(), source.WatcherCount());
    std::printf("[iocp_demo] loop state=%d (3=stopped)\n",
                static_cast<int>(loop.State()));

    fs::remove_all(dir, ec);
    return event_count > 0 ? 0 : 1;
}
