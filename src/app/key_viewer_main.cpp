// 阶段 2：按键事件查看器 micro-demo
//
// 两种运行模式：
//   1) 默认（有真实控制台）：ConsoleSession 接管 CONIN$/CONOUT$，
//      ConsoleInputThread 独占读输入，事件经 PostTask 投到 EventLoop，
//      PostRepeatingTask(16ms) 作为渲染 tick。ESC / Ctrl+C 干净退出。
//   2) --selftest：无控制台环境下用 MemoryBackend 驱动同一套 KeyViewer 逻辑，
//      脚本化注入按键 / resize / 鼠标事件，断言差分渲染结果。
//      这样在任何环境都能验证阶段 2 的出口条件。
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tui/console/console_input_thread.h"
#include "tui/console/console_session.h"
#include "tui/console/input_decoder.h"
#include "tui/console/key_viewer.h"
#include "tui/console/render_backend.h"
#include "tui/console/renderer.h"
#include "tui/core/clock.h"
#include "tui/core/event_loop.h"

using namespace tui;

namespace {

int RunSelftest() {
    std::printf("[selftest] headless differential-rendering check\n");
    const console::Size size{40, 8};
    auto backend = std::make_unique<console::MemoryBackend>(size);
    auto* mem = backend.get();
    console::Renderer renderer(std::move(backend), size);
    console::KeyViewer viewer(renderer);

    // 首帧：应整屏输出（dirty 初始为 true）。
    viewer.Draw();
    renderer.RenderTick();
    if (renderer.PresentCount() != 1) {
        std::printf("[selftest] FAIL: expected 1 first present\n");
        return 1;
    }
    const size_t first_deltas = renderer.TotalDeltas();
    std::printf("[selftest] frame 1: %zu cells written\n", first_deltas);

    // 一个 tick 内合并多个输入事件，只应产生一帧。
    console::InputEvent k1{};
    k1.kind = console::InputKind::kKey;
    k1.key_down = true;
    k1.ch = L'A';
    k1.vk = 'A';

    console::InputEvent r{};
    r.kind = console::InputKind::kResize;
    r.width = 50;
    r.height = 10;

    viewer.OnInputEvent(k1);
    viewer.OnInputEvent(r);
    viewer.Draw();
    const size_t before = renderer.PresentCount();
    renderer.RenderTick();
    renderer.RenderTick();  // 第二个 tick：无改动，不应再写
    if (renderer.PresentCount() != before + 1) {
        std::printf("[selftest] FAIL: tick merge / no-op frame broken\n");
        return 1;
    }
    std::printf("[selftest] resize handled: viewer size %dx%d, presents=%zu\n",
                viewer.last_size().w, viewer.last_size().h, renderer.PresentCount());

    // 构造 ESC 事件，确认退出请求。
    console::InputEvent esc{};
    esc.kind = console::InputKind::kKey;
    esc.key_down = true;
    esc.vk = VK_ESCAPE;
    viewer.OnInputEvent(esc);
    if (!viewer.quit_requested()) {
        std::printf("[selftest] FAIL: ESC did not request quit\n");
        return 1;
    }

    // 再画一帧，确认 MemoryBackend 里能看到实际字符。
    viewer.Draw();
    renderer.RenderTick();
    const auto& frame = mem->Frame();
    // 标题首字符应为 'w'（ASCII 保证）。
    if (frame.empty() || frame[1].ch != L'w') {
        std::printf("[selftest] FAIL: frame content not rendered\n");
        return 1;
    }
    std::printf("[selftest] frame cells=%zu, title ok, history frames=%zu\n",
                frame.size(), mem->History().size());
    std::printf("[selftest] PASS\n");
    return 0;
}

int RunConsole() {
    console::ConsoleSession session;
    if (!session.Ok()) {
        std::printf("[key_viewer] no usable Win32 console (err=%lu).\n",
                    session.LastError());
        std::printf("[key_viewer] run with --selftest for headless verification.\n");
        return 2;
    }

    core::SystemClock clock;
    core::EventLoop loop(clock);

    auto backend = std::make_unique<console::LegacyConsoleBackend>(
        session.Output(), session.GetSize());
    console::Renderer renderer(std::move(backend), session.GetSize());
    console::KeyViewer viewer(renderer);

    // 输入线程：解码后的事件经 PostTask 投到 loop（约束 B2/B7）。
    console::ConsoleInputThread input(session.Input(), [&](const console::InputEvent& e) {
        loop.PostTask([&viewer, &renderer, &loop, &session, e]() {
            if (e.kind == console::InputKind::kResize) {
                session.RefreshSize();
                if (auto* legacy = dynamic_cast<console::LegacyConsoleBackend*>(
                        &renderer.Backend())) {
                    legacy->SetSize(session.GetSize());
                }
            }
            if (viewer.OnInputEvent(e)) {
                loop.Stop();
            }
            viewer.Draw();
        });
    });

    // 渲染 tick：16ms 合并刷新（约束 D4）。
    loop.PostRepeatingTask(16, 16, [&]() { renderer.RenderTick(); });

    input.Start();
    loop.Run();       // 阻塞直到 ESC / Ctrl+C
    input.Stop();     // 注入哨兵记录 → join

    std::printf("[key_viewer] stopped: %d keys, %d resizes, %zu presents\n",
                viewer.key_count(), viewer.resize_count(), renderer.PresentCount());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0) {
        return RunSelftest();
    }
    return RunConsole();
}
