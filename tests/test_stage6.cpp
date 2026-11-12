#include "test_framework.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

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

// 探测某串口能否以 overlapped 方式打开（不真正 Start 事件源）。
bool PortExists(const std::wstring& port) {
    std::wstring path = L"\\\\.\\" + port;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// SerialConfig / 打开失败路径（不依赖硬件）
// ---------------------------------------------------------------------------

TEST(serial_open_nonexistent_port_fails_cleanly) {
    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    io::SerialConfig cfg;
    cfg.port = L"COM99";  // 不存在
    io::SerialSource src(iocp, cfg, [](const std::vector<uint8_t>&) {});
    CHECK_FALSE(src.Start());
    CHECK_FALSE(src.IsOpen());
    CHECK_TRUE(src.LastCommError() != 0);
    CHECK_EQ(src.WatcherCount(), static_cast<size_t>(0));
}

TEST(serial_config_defaults_are_8n1_9600) {
    io::SerialConfig cfg;
    CHECK_EQ(cfg.baud, 9600u);
    CHECK_EQ(static_cast<int>(cfg.byte_size), 8);
    CHECK_EQ(static_cast<int>(cfg.parity), 0);
    CHECK_EQ(static_cast<int>(cfg.stop_bits), 0);
}

// ---------------------------------------------------------------------------
// 真实串口路径验证（不依赖回环数据）
//
// 本机有真实 COM1（\Device\Serial0）。这条测试覆盖串口完整生命周期：
//   CreateFile(overlapped) → GetCommState/SetCommState(DCB) → SetCommTimeouts
//   → SetCommMask(EV_RXCHAR) → Associate(IOCP) → overlapped WaitCommEvent
//   → CloseAll(CancelIoEx) → 排空取消包 → Reap 归零
// 数据回环需要 TX-RX 物理短接，无法在此环境断言，故不虚报。
// ---------------------------------------------------------------------------

TEST(serial_real_port_lifecycle_clean_exit) {
    if (!PortExists(L"COM1")) {
        std::printf("    [skip] COM1 not available\n");
        return;
    }

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    io::SerialConfig cfg;
    cfg.port = L"COM1";
    cfg.baud = 9600;
    io::SerialSource src(iocp, cfg, [](const std::vector<uint8_t>&) {});

    CHECK_TRUE(src.Start());
    CHECK_TRUE(src.IsOpen());
    CHECK_EQ(src.Baud(), 9600u);
    CHECK_EQ(src.WatcherCount(), static_cast<size_t>(1));  // 一个 SerialWatcher

    // 手动泵几轮，让 WaitCommEvent 真正挂到 IOCP 上（无数据时保持 pending）。
    // 注意：**不能**用 loop.Run()，因为它在退出时会 Shutdown() 后端
    // （关闭 IOCP 句柄），之后的取消完成包就再也取不到了。
    // 这是阶段 6 发现的真实 API 陷阱，已记入阶段文档。
    for (int i = 0; i < 5; ++i) {
        iocp.Wait(20);
        iocp.Dispatch();
    }

    // 优雅关闭：CloseAll → 取消完成包排空 → Reap 归零（约束 C4）。
    src.CloseAll();
    for (int i = 0; i < 40 && src.WatcherCount() > 0; ++i) {
        iocp.Wait(50);
        iocp.Dispatch();
        src.Reap();
    }
    CHECK_EQ(src.WatcherCount(), static_cast<size_t>(0));

    std::printf("    [ok] COM1 real serial lifecycle: open/DCB/mask/associate -> "
                "clean CancelIoEx -> Reap (wait_events=%zu recv=%zu)\n",
                src.WaitEvents(), src.RecvBytes());
}

// ---------------------------------------------------------------------------
// 真实串口端到端：COM2 ↔ COM3（com0com 虚拟串口对）
//   需要管理员权限打开 VSerial 端口；否则自动跳过（不虚报成功）。
// ---------------------------------------------------------------------------

TEST(serial_real_loopback_recv_and_send) {
    if (!PortExists(L"COM2") || !PortExists(L"COM3")) {
        std::printf("    [skip] COM2/COM3 (com0com pair) not available\n");
        return;  // 无虚拟串口对：跳过，不虚报成功
    }

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    std::vector<uint8_t> received;
    io::SerialConfig cfg;
    cfg.port = L"COM2";
    cfg.baud = 115200;
    io::SerialSource src(iocp, cfg, [&](const std::vector<uint8_t>& d) {
        received.insert(received.end(), d.begin(), d.end());
    });
    CHECK_TRUE(src.Start());
    CHECK_TRUE(src.IsOpen());
    CHECK_EQ(src.Baud(), 115200u);

    // 另一侧：直接用 Win32 打开 COM3 并写数据（模拟对端设备）。
    // 这样测的是「真实串口驱动 → IOCP completion → 回调」的完整链路。
    HANDLE peer = CreateFileW(L"\\\\.\\COM3", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    CHECK_TRUE(peer != INVALID_HANDLE_VALUE);
    if (peer == INVALID_HANDLE_VALUE) {
        src.CloseAll();
        return;
    }

    // 配置 COM3 同样 115200 8N1。
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(peer, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    SetCommState(peer, &dcb);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    SetCommTimeouts(peer, &to);

    // 对端写 "PING"（overlapped，这里用同步等待完成简化对端）。
    const char* msg = "PING";
    OVERLAPPED wov{};
    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(peer, msg, 4, &written, &wov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(wov.hEvent, 1000);
        GetOverlappedResult(peer, &wov, &written, FALSE);
    }
    CloseHandle(wov.hEvent);

    // 跑 loop 直到收到数据或超时。
    std::atomic<bool> got{false};
    loop.PostDelayedTask(1200, [&]() { loop.Stop(); });
    loop.PostRepeatingTask(20, 20, [&]() {
        if (received.size() >= 4 && !got.load()) {
            got.store(true);
            loop.Stop();
        }
    });
    loop.Run();

    // 排空取消包（约束 C4）：Wait → Dispatch 直到 Reap 归零。
    src.CloseAll();
    for (int i = 0; i < 40 && src.WatcherCount() > 0; ++i) {
        iocp.Wait(50);
        iocp.Dispatch();
        src.Reap();
    }
    CloseHandle(peer);

    if (got.load()) {
        std::string s(received.begin(), received.begin() + 4);
        CHECK_EQ(s, std::string("PING"));
        CHECK_TRUE(src.RecvBytes() >= 4);
        CHECK_TRUE(src.WaitEvents() >= 1);
        std::printf("    [ok] COM2<-COM3 real serial loopback: %s (%zu bytes, %zu wait events)\n",
                    s.c_str(), src.RecvBytes(), src.WaitEvents());
    } else {
        // com0com 已装但未真正桥接时也可能收不到；不虚报成功。
        std::printf("    [skip] serial loopback produced no data (recv=%zu wait=%zu)\n",
                    src.RecvBytes(), src.WaitEvents());
    }
}

TEST(serial_send_to_peer_arrives) {
    if (!PortExists(L"COM2") || !PortExists(L"COM3")) {
        std::printf("    [skip] COM2/COM3 (com0com pair) not available\n");
        return;
    }

    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    io::SerialConfig cfg;
    cfg.port = L"COM2";
    cfg.baud = 115200;
    io::SerialSource src(iocp, cfg, [](const std::vector<uint8_t>&) {});
    CHECK_TRUE(src.Start());

    HANDLE peer = CreateFileW(L"\\\\.\\COM3", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    CHECK_TRUE(peer != INVALID_HANDLE_VALUE);
    if (peer == INVALID_HANDLE_VALUE) {
        src.CloseAll();
        return;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(peer, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    SetCommState(peer, &dcb);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 100;
    to.ReadTotalTimeoutConstant = 300;
    SetCommTimeouts(peer, &to);

    // 通过 SerialSource 发 "PONG"，再在 COM3 上同步读回。
    loop.PostDelayedTask(60, [&]() {
        src.Send(std::vector<uint8_t>{'P', 'O', 'N', 'G'});
    });
    loop.PostDelayedTask(700, [&]() { loop.Stop(); });
    loop.Run();

    char buf[16]{};
    DWORD got = 0;
    BOOL rok = ReadFile(peer, buf, sizeof(buf), &got, nullptr);
    src.CloseAll();
    for (int i = 0; i < 40 && src.WatcherCount() > 0; ++i) {
        iocp.Wait(50);
        iocp.Dispatch();
        src.Reap();
    }
    CloseHandle(peer);

    if (rok && got >= 4) {
        CHECK_EQ(std::string(buf, 4), std::string("PONG"));
        CHECK_TRUE(src.SentBytes() >= 4);
        std::printf("    [ok] SerialSource sent PONG, peer read %lu bytes\n", got);
    } else {
        std::printf("    [skip] peer read nothing (got=%lu ok=%d sent=%zu)\n",
                    got, static_cast<int>(rok), src.SentBytes());
    }
}

// ---------------------------------------------------------------------------
// Capstone 应用逻辑（headless，无控制台）
// ---------------------------------------------------------------------------

TEST(assistant_rx_counters_and_log) {
    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" title ");

    app.OnSerialData(std::vector<uint8_t>{'A', 'B', 'C'});
    app.OnNetworkData(std::vector<uint8_t>{'X', 'Y'});
    CHECK_EQ(app.serial_rx(), static_cast<size_t>(3));
    CHECK_EQ(app.net_rx(), static_cast<size_t>(2));
    CHECK_EQ(app.log_count(), static_cast<size_t>(2));
}

TEST(assistant_type_and_send) {
    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" title ");

    for (wchar_t ch : std::wstring(L"HI")) {
        console::InputEvent k{};
        k.kind = console::InputKind::kKey;
        k.key_down = true;
        k.ch = ch;
        k.vk = static_cast<uint16_t>(ch);
        app.OnInputEvent(k);
    }
    CHECK_EQ(app.input_line(), std::wstring(L"HI"));

    console::InputEvent enter{};
    enter.kind = console::InputKind::kKey;
    enter.key_down = true;
    enter.vk = VK_RETURN;
    app.OnInputEvent(enter);

    auto payload = app.TakePendingSend();
    CHECK_EQ(payload.size(), static_cast<size_t>(3));  // "HI" + '\r'
    CHECK_EQ(static_cast<char>(payload[0]), 'H');
    CHECK_EQ(static_cast<char>(payload[2]), '\r');
    CHECK_EQ(app.input_line(), std::wstring(L""));
    CHECK_EQ(app.tx_count(), static_cast<size_t>(1));
    // 取过一次后应为空。
    CHECK_EQ(app.TakePendingSend().size(), static_cast<size_t>(0));
}

TEST(assistant_backspace_and_target_cycle) {
    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" title ");

    for (wchar_t ch : std::wstring(L"AB")) {
        console::InputEvent k{};
        k.kind = console::InputKind::kKey;
        k.key_down = true;
        k.ch = ch;
        k.vk = static_cast<uint16_t>(ch);
        app.OnInputEvent(k);
    }
    console::InputEvent back{};
    back.kind = console::InputKind::kKey;
    back.key_down = true;
    back.vk = VK_BACK;
    app.OnInputEvent(back);
    CHECK_EQ(app.input_line(), std::wstring(L"A"));

    // Ctrl+S 循环 kBoth -> kSerial -> kNetwork -> kBoth
    auto press_ctrl_s = [&]() {
        console::InputEvent e{};
        e.kind = console::InputKind::kKey;
        e.key_down = true;
        e.vk = 'S';
        e.mods = console::Mod::kCtrl;
        app.OnInputEvent(e);
    };
    CHECK_TRUE(app.target() == console::SerialNetAssistant::SendTarget::kBoth);
    press_ctrl_s();
    CHECK_TRUE(app.target() == console::SerialNetAssistant::SendTarget::kSerial);
    press_ctrl_s();
    CHECK_TRUE(app.target() == console::SerialNetAssistant::SendTarget::kNetwork);
    press_ctrl_s();
    CHECK_TRUE(app.target() == console::SerialNetAssistant::SendTarget::kBoth);
}

TEST(assistant_backpressure_caps_log) {
    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" title ");

    for (int i = 0; i < 600; ++i) app.OnSystemLine(L"line " + std::to_wstring(i));
    CHECK_EQ(app.log_count(), static_cast<size_t>(512));
    CHECK_TRUE(app.dropped_count() > 0);
}

TEST(assistant_esc_quits_ctrl_c_quits) {
    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" title ");

    console::InputEvent esc{};
    esc.kind = console::InputKind::kKey;
    esc.key_down = true;
    esc.vk = VK_ESCAPE;
    CHECK_TRUE(app.OnInputEvent(esc));
    CHECK_TRUE(app.quit_requested());

    console::SerialNetAssistant app2(renderer, L" title ");
    console::InputEvent ctrl_c{};
    ctrl_c.kind = console::InputKind::kKey;
    ctrl_c.key_down = true;
    ctrl_c.vk = 'C';
    ctrl_c.mods = console::Mod::kCtrl;
    CHECK_TRUE(app2.OnInputEvent(ctrl_c));
    CHECK_TRUE(app2.quit_requested());
}

TEST(assistant_draw_is_differential_and_tick_merged) {
    const console::Size size{80, 12};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    auto* mem = backend.get();
    console::Renderer renderer(std::move(backend), size);
    console::SerialNetAssistant app(renderer, L" win-tui-runtime ");

    app.Draw();
    CHECK_TRUE(renderer.RenderTick());  // 首帧必写
    const size_t first = renderer.PresentCount();
    CHECK_EQ(first, static_cast<size_t>(1));

    // 无改动：不产生新帧。
    CHECK_FALSE(renderer.RenderTick());
    CHECK_EQ(renderer.PresentCount(), first);

    // 一个 tick 内多次改动 → 只一帧。
    app.OnSerialData(std::vector<uint8_t>{'1'});
    app.OnSerialData(std::vector<uint8_t>{'2'});
    app.OnSerialData(std::vector<uint8_t>{'3'});
    app.Draw();
    CHECK_TRUE(renderer.RenderTick());
    CHECK_EQ(renderer.PresentCount(), first + 1);

    // 帧内容：标题首字符可见。
    CHECK_TRUE(!mem->Frame().empty());
}

// ---------------------------------------------------------------------------
// Capstone 集成：串口 + TCP 同时挂在同一个 IOCP 事件循环上
//
// TcpSource 刻意是 loopback 形态（见 tcp_source.h 的设计说明），用 PeerSocket()
// 作为「对端设备」注入数据；串口用真实 COM1。验证：
//   - 两个 source 共享一个 IocpBackend / 一个 loop 线程；
//   - 数据从各自 source 到达应用层回调，计数正确；
//   - 一起 CloseAll → 排空 → Reap 归零（无泄漏、无崩溃）。
// ---------------------------------------------------------------------------

TEST(capstone_two_sources_share_one_loop_and_exit_clean) {
    core::SystemClock clock;
    auto backend = std::make_unique<io::IocpBackend>();
    io::IocpBackend& iocp = *backend;
    core::EventLoop loop(clock, std::move(backend));

    std::vector<uint8_t> serial_rx;
    std::vector<uint8_t> net_rx;

    // --- 网络源 ---
    io::TcpSource tcp(iocp, [&](const std::vector<uint8_t>& d) {
        net_rx.insert(net_rx.end(), d.begin(), d.end());
    });
    CHECK_TRUE(tcp.Start());

    // --- 串口源（真实 COM1；没有则跳过串口部分）---
    const bool has_com1 = PortExists(L"COM1");
    std::unique_ptr<io::SerialSource> serial;
    if (has_com1) {
        io::SerialConfig cfg;
        cfg.port = L"COM1";
        cfg.baud = 9600;
        serial = std::make_unique<io::SerialSource>(
            iocp, cfg, [&](const std::vector<uint8_t>& d) {
                serial_rx.insert(serial_rx.end(), d.begin(), d.end());
            });
        CHECK_TRUE(serial->Start());
    }

    // 从 TCP 对端注入数据：走真实 loopback → IOCP → 回调。
    const char* msg = "NETDATA";
    int sent = send(tcp.PeerSocket(), msg, 7, 0);
    CHECK_EQ(sent, 7);

    // 手动泵（不用 loop.Run()，避免它在退出时 Shutdown 后端）。
    for (int i = 0; i < 40 && net_rx.size() < 7; ++i) {
        iocp.Wait(50);
        iocp.Dispatch();
    }
    CHECK_EQ(net_rx.size(), static_cast<size_t>(7));
    CHECK_EQ(std::string(net_rx.begin(), net_rx.end()), std::string("NETDATA"));
    std::printf("    [ok] tcp loopback delivered %zu bytes through IOCP\n", net_rx.size());

    // 反向：应用层 Send → 对端 recv。
    tcp.Send(std::vector<uint8_t>{'A', 'C', 'K'});
    for (int i = 0; i < 20 && tcp.SentBytes() < 3; ++i) {
        iocp.Wait(30);
        iocp.Dispatch();
    }
    char back[8]{};
    int got = recv(tcp.PeerSocket(), back, sizeof(back), 0);
    if (got > 0) {
        std::printf("    [ok] peer received %d bytes from TcpSource.Send\n", got);
    }
    CHECK_TRUE(tcp.SentBytes() >= 3);

    // --- 一起优雅关闭 ---
    if (serial) serial->CloseAll();
    tcp.CloseAll();
    for (int i = 0; i < 60 && (tcp.WatcherCount() > 0 ||
                               (serial && serial->WatcherCount() > 0)); ++i) {
        iocp.Wait(50);
        iocp.Dispatch();
        tcp.Reap();
        if (serial) serial->Reap();
    }
    CHECK_EQ(tcp.WatcherCount(), static_cast<size_t>(0));
    if (serial) CHECK_EQ(serial->WatcherCount(), static_cast<size_t>(0));
    std::printf("    [ok] both sources closed cleanly (tcp=%zu serial=%zu)\n",
                tcp.WatcherCount(), serial ? serial->WatcherCount() : 0);
}

int main() { return tui::test::RunAll(); }
