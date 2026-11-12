#pragma once

#include <deque>
#include <string>
#include <vector>

#include "tui/console/input_decoder.h"
#include "tui/console/renderer.h"

// 阶段 2：KeyViewer —— 「按键事件查看器」micro-demo 的应用逻辑
//
// 只负责「状态 + 绘制」，不碰线程、不碰控制台句柄：
//   - OnInputEvent()：接收解码后的输入事件，更新状态，标脏；
//   - Draw()：把当前状态画到 Renderer 的 back 缓冲；
//   - 由外部决定何时 Draw + RenderTick（渲染 tick）。
//
// 这样同一个类既能被真实控制台模式驱动，也能被 headless selftest 驱动，
// 保证「无控制台环境也能验证出口条件」。
namespace tui::console {

class KeyViewer {
public:
    explicit KeyViewer(Renderer& renderer);

    // 处理一个输入事件（在 loop 线程调用）。返回是否请求退出。
    bool OnInputEvent(const InputEvent& e);
    void Draw();

    bool quit_requested() const { return quit_; }
    int key_count() const { return key_count_; }
    int resize_count() const { return resize_count_; }
    Size last_size() const { return last_size_; }

private:
    void PushLog(const std::wstring& line);
    void DrawFrame(Size s);

    Renderer& renderer_;
    std::deque<std::wstring> log_;
    int key_count_ = 0;
    int resize_count_ = 0;
    int mouse_count_ = 0;
    Size last_size_{80, 25};
    bool quit_ = false;

    static constexpr size_t kMaxLog = 256;  // ring buffer 上限，防止无限增长（背压）
};

}  // namespace tui::console
