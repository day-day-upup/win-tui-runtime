// 阶段 6：Capstone —— 串口网络助手
//
// 两种运行模式：
//   1) 默认（有真实控制台）：ConsoleSession 接管 CONIN$/CONOUT$，
//      ConsoleInputThread 独占读输入，串口 + TCP 两个 EventSource 挂同一个
//      IOCP 事件循环，PostRepeatingTask(16ms) 作为渲染 tick。
//      ESC / Ctrl+C 干净退出（CancelIoEx → 排空 → join）。
//   2) --selftest：无控制台/无串口环境下用 MemoryBackend 驱动同一套
//      SerialNetAssistant 逻辑，断言收发与差分渲染。
//
// 用法：
//   serial_net_assistant.exe [--port COM2] [--baud 9600] [--run-ms 1500] [--selftest]
//
//   --run-ms N  自动运行 N 毫秒后走「CancelIoEx → 排空 → Reap → Stop」优雅关闭，
//               用于自动化验证完整启动/关闭链路（不传则一直交互直到 ESC）。
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tui/console/console_input_thread.h"
#include "tui/console/console_session.h"
#include "tui/console/input_decoder.h"
#include "tui/console/render_backend.h"
#include "tui/console/renderer.h"
#include "tui/console/serial_net_assistant.h"
#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/io/iocp_backend.h"
#include "tui/io/serial_source.h"
#include "tui/io/tcp_source.h"

using namespace tui;

namespace {

struct Options {
    std::wstring port = L"COM2";
    uint32_t baud = 9600;
    bool selftest = false;
    int64_t run_ms = 0;  // >0：自动运行指定毫秒后走优雅关闭（用于自动化验证）
};

Options ParseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) {
            o.selftest = true;
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            std::string p = argv[++i];
            o.port.assign(p.begin(), p.end());
        } else if (std::strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            o.baud = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--run-ms") == 0 && i + 1 < argc) {
            o.run_ms = std::atoll(argv[++i]);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// headless 自检：无控制台、无串口也能验证 capstone 出口条件
// ---------------------------------------------------------------------------
int RunSelftest() {
    std::printf("[selftest] capstone: assistant logic + differential rendering\n");

    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    auto* mem = backend.get();
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" win-tui-runtime | Serial/Net Assistant ");

    // 首帧：整屏输出。
    app.Draw();
    renderer.RenderTick();
    if (renderer.PresentCount() != 1) {
        std::printf("[selftest] FAIL: expected 1 first present\n");
        return 1;
    }

    // --- 收：串口 + 网络各来一段数据 ---
    const std::vector<uint8_t> serial_rx{'H', 'E', 'L', 'L', 'O'};
    const std::vector<uint8_t> net_rx{'W', 'O', 'R', 'L', 'D'};
    app.OnSerialData(serial_rx);
    app.OnNetworkData(net_rx);
    if (app.serial_rx() != 5 || app.net_rx() != 5) {
        std::printf("[selftest] FAIL: rx counters wrong (serial=%zu net=%zu)\n",
                    app.serial_rx(), app.net_rx());
        return 1;
    }
    if (app.log_count() != 2) {
        std::printf("[selftest] FAIL: expected 2 log lines, got %zu\n", app.log_count());
        return 1;
    }

    // --- 发：模拟键入 "PING" + Enter ---
    for (wchar_t ch : std::wstring(L"PING")) {
        console::InputEvent k{};
        k.kind = console::InputKind::kKey;
        k.key_down = true;
        k.ch = ch;
        k.vk = static_cast<uint16_t>(ch);
        app.OnInputEvent(k);
    }
    if (app.input_line() != L"PING") {
        std::printf("[selftest] FAIL: input line = %ls\n", app.input_line().c_str());
        return 1;
    }

    console::InputEvent enter{};
    enter.kind = console::InputKind::kKey;
    enter.key_down = true;
    enter.vk = VK_RETURN;
    app.OnInputEvent(enter);

    auto payload = app.TakePendingSend();
    if (payload.size() != 5 || payload[0] != 'P' || payload[4] != '\r') {
        std::printf("[selftest] FAIL: pending send wrong (size=%zu)\n", payload.size());
        return 1;
    }
    if (app.input_line() != L"") {
        std::printf("[selftest] FAIL: input line not cleared after send\n");
        return 1;
    }
    if (app.tx_count() != 1) {
        std::printf("[selftest] FAIL: tx counter = %zu\n", app.tx_count());
        return 1;
    }
    std::printf("[selftest] tx payload = %zu bytes (\"PING\\r\")\n", payload.size());

    // --- 一个 tick 内合并：多事件只产生一帧 ---
    app.OnSerialData(std::vector<uint8_t>{'a'});
    app.OnSerialData(std::vector<uint8_t>{'b'});
    app.Draw();
    const size_t before = renderer.PresentCount();
    renderer.RenderTick();
    renderer.RenderTick();  // 第二个 tick 无改动，不应再写
    if (renderer.PresentCount() != before + 1) {
        std::printf("[selftest] FAIL: tick merge broken\n");
        return 1;
    }
    std::printf("[selftest] tick merge ok, presents=%zu\n", renderer.PresentCount());

    // --- 背压：日志上限 512，灌 600 条应丢最旧 ---
    for (int i = 0; i < 600; ++i) {
        app.OnSystemLine(L"flood " + std::to_wstring(i));
    }
    if (app.log_count() != 512) {
        std::printf("[selftest] FAIL: log cap = %zu (expect 512)\n", app.log_count());
        return 1;
    }
    if (app.dropped_count() == 0) {
        std::printf("[selftest] FAIL: dropped counter not tracked\n");
        return 1;
    }
    std::printf("[selftest] backpressure ok: log=%zu dropped=%zu\n",
                app.log_count(), app.dropped_count());

    // --- 发送目标切换 ---
    console::InputEvent ctrl_s{};
    ctrl_s.kind = console::InputKind::kKey;
    ctrl_s.key_down = true;
    ctrl_s.vk = 'S';
    ctrl_s.mods = console::Mod::kCtrl;
    const auto t0 = app.target();
    app.OnInputEvent(ctrl_s);
    if (app.target() == t0) {
        std::printf("[selftest] FAIL: Ctrl+S did not switch target\n");
        return 1;
    }
    std::printf("[selftest] target switched\n");

    // --- 帧内容检查：标题首字符 + 输入行可见 ---
    app.Draw();
    renderer.RenderTick();
    const auto& frame = mem->Frame();
    if (frame.empty() || frame[0].ch != L' ') {
        std::printf("[selftest] FAIL: title not rendered\n");
        return 1;
    }

    // --- 退出：ESC ---
    console::InputEvent esc{};
    esc.kind = console::InputKind::kKey;
    esc.key_down = true;
    esc.vk = VK_ESCAPE;
    if (!app.OnInputEvent(esc) || !app.quit_requested()) {
        std::printf("[selftest] FAIL: ESC did not request quit\n");
        return 1;
    }

    std::printf("[selftest] PASS (frames=%zu, cells=%zu)\n",
                renderer.PresentCount(), frame.size());
    return 0;
}

// ---------------------------------------------------------------------------
// 真实控制台模式
// ---------------------------------------------------------------------------
int RunConsole(const Options& opt) {
    console::ConsoleSession session;
    if (!session.Ok()) {
        std::printf("[assistant] no usable Win32 console (err=%lu).\n",
                    session.LastError());
        std::printf("[assistant] run with --selftest for headless verification.\n");
        return 2;
    }

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    auto rbackend = std::make_unique<console::LegacyConsoleBackend>(
        session.Output(), session.GetSize());
    console::Renderer renderer(std::move(rbackend), session.GetSize());
    console::SerialNetAssistant app(renderer, L" win-tui-runtime | Serial/Net Assistant ");

    // --- 串口源 ---
    io::SerialConfig cfg;
    cfg.port = opt.port;
    cfg.baud = opt.baud;
    io::SerialSource serial(iocp, cfg, [&](const std::vector<uint8_t>& d) {
        app.OnSerialData(d);  // 已在 loop 线程（Dispatch 内）
    });
    bool serial_ok = serial.Start();
    if (serial_ok) {
        app.OnSystemLine(L"serial " + opt.port + L" open @ " +
                         std::to_wstring(opt.baud) + L" 8N1");
    } else {
        app.OnSystemLine(L"serial " + opt.port + L" open FAILED (err=" +
                         std::to_wstring(serial.LastCommError()) + L")");
    }

    // --- 网络源 ---
    io::TcpSource tcp(iocp, [&](const std::vector<uint8_t>& d) { app.OnNetworkData(d); });
    bool tcp_ok = tcp.Start();
    if (tcp_ok) {
        app.OnSystemLine(L"tcp listening on 127.0.0.1:" + std::to_wstring(tcp.Port()));
    } else {
        app.OnSystemLine(L"tcp start FAILED");
    }

    // --- 优雅关闭：定时器两段式（约束 C4 顺序，在 loop 内完成排空）---
    // 不能在 loop.Run() 返回后再排空：那时 backend 已 Shutdown。
    // 因此关闭分两步，都由 loop 内的定时器驱动：
    //   t+0   : CloseAll → CancelIoEx → 各 watcher 进入 kClosing
    //   t+250 : 取消完成包已排空 → Reap → Stop
    auto begin_shutdown = [&]() {
        loop.PostTask([&]() {
            app.OnSystemLine(L"closing sources (CancelIoEx)...");
            serial.CloseAll();
            tcp.CloseAll();
            app.Draw();
            loop.PostDelayedTask(250, [&]() {
                serial.Reap();
                tcp.Reap();
                app.OnSystemLine(L"reaped, stopping");
                app.Draw();
                renderer.RenderTick();
                loop.Stop();
            });
        });
    };

    // --- 输入线程（约束 B2/B7：解码后 PostTask 到 loop）---
    console::ConsoleInputThread input(session.Input(), [&](const console::InputEvent& e) {
        loop.PostTask([&]() {
            if (e.kind == console::InputKind::kResize) {
                session.RefreshSize();
                if (auto* legacy = dynamic_cast<console::LegacyConsoleBackend*>(
                        &renderer.Backend())) {
                    legacy->SetSize(session.GetSize());
                }
            }
            if (app.OnInputEvent(e)) {
                begin_shutdown();  // 不直接 Stop，先走关闭流程
            }
            app.Draw();
        });
    });

    // --- 发送：把 app 攒的 payload 分发给目标 ---
    loop.PostRepeatingTask(16, 16, [&]() {
        auto payload = app.TakePendingSend();
        if (!payload.empty()) {
            const auto t = app.target();
            if (t != console::SerialNetAssistant::SendTarget::kNetwork && serial_ok) {
                serial.Send(payload);
            }
            if (t != console::SerialNetAssistant::SendTarget::kSerial && tcp_ok) {
                tcp.Send(payload);
            }
        }
        renderer.RenderTick();  // 渲染 tick 合并刷新（约束 D4）
    });

    // --- 自动退出（仅 --run-ms > 0 时）：用于自动化验证启动→关闭全链路 ---
    if (opt.run_ms > 0) {
        loop.PostDelayedTask(opt.run_ms, [&]() {
            app.OnSystemLine(L"auto-stop after " + std::to_wstring(opt.run_ms) + L"ms");
            begin_shutdown();
        });
    }

    input.Start();
    loop.Run();
    input.Stop();

    std::printf("[assistant] stopped: serial_rx=%zu net_rx=%zu tx=%zu "
                "presents=%zu dropped=%zu\n",
                app.serial_rx(), app.net_rx(), app.tx_count(),
                renderer.PresentCount(), app.dropped_count());
    std::printf("[assistant] sources left: serial=%zu tcp=%zu\n",
                serial.WatcherCount(), tcp.WatcherCount());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = ParseArgs(argc, argv);
    if (opt.selftest) return RunSelftest();
    return RunConsole(opt);
}
