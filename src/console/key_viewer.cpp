#include "tui/console/key_viewer.h"

#include <algorithm>

namespace tui::console {

namespace {

Cell HdrCell() { return Cell{L' ', MakeAttr(Color::kBrightWhite, Color::kBlue)}; }
Cell BodyCell() { return Cell{L' ', MakeAttr(Color::kGray, Color::kBlack)}; }
Cell LogCell() { return Cell{L' ', MakeAttr(Color::kBrightCyan, Color::kBlack)}; }
Cell StatusCell() { return Cell{L' ', MakeAttr(Color::kBlack, Color::kCyan)}; }

}  // namespace

KeyViewer::KeyViewer(Renderer& renderer) : renderer_(renderer) {
    last_size_ = renderer_.GetSize();
}

void KeyViewer::PushLog(const std::wstring& line) {
    log_.push_back(line);
    while (log_.size() > kMaxLog) log_.pop_front();  // 背压：丢弃最旧
}

bool KeyViewer::OnInputEvent(const InputEvent& e) {
    switch (e.kind) {
        case InputKind::kKey: {
            if (e.key_down) ++key_count_;
            PushLog(e.Describe());
            // Esc 或 Ctrl+C 退出（第一版功能边界：只做查看 + 退出）。
            if (e.key_down && (e.vk == VK_ESCAPE ||
                               (e.ch == 3 && (e.mods & Mod::kCtrl)))) {
                quit_ = true;
            }
            break;
        }
        case InputKind::kResize: {
            ++resize_count_;
            last_size_ = {e.width, e.height};
            renderer_.Resize(last_size_);
            PushLog(e.Describe());
            break;
        }
        case InputKind::kMouse:
            ++mouse_count_;
            PushLog(e.Describe());
            break;
        case InputKind::kFocus:
            PushLog(e.Describe());
            break;
        default:
            break;
    }
    return quit_;
}

void KeyViewer::Draw() {
    Size s = renderer_.GetSize();
    if (s.w <= 0 || s.h <= 0) return;
    DrawFrame(s);
    renderer_.MarkDirty();  // 标记本 tick 有改动
}

void KeyViewer::DrawFrame(Size s) {
    const int16_t w = s.w;
    const int16_t h = s.h;

    // 全屏底色（差分后只有变化单元格会被推送）。
    renderer_.Clear(BodyCell());

    // 标题栏
    std::wstring title = L" win-tui-runtime | Stage 2: Key Event Viewer ";
    renderer_.Put(0, 0, title, MakeAttr(Color::kBrightWhite, Color::kBlue));
    if (w > static_cast<int16_t>(title.size())) {
        Cell hc = HdrCell();
        renderer_.FillRect(static_cast<int16_t>(title.size()), 0, w - 1, 0, hc);
    }

    // 提示
    if (h > 1) {
        renderer_.Put(0, 1, L" Press keys / resize / move mouse. ESC or Ctrl+C to quit.",
                      MakeAttr(Color::kBrightYellow, Color::kBlack));
    }

    // 日志区：从第 3 行到倒数第 2 行。
    const int16_t log_top = 3;
    const int16_t log_bottom = std::max<int16_t>(log_top, h - 2);
    const int16_t rows = log_bottom - log_top + 1;
    size_t start = log_.size() > static_cast<size_t>(rows)
                       ? log_.size() - static_cast<size_t>(rows)
                       : 0;
    int16_t y = log_top;
    for (size_t i = start; i < log_.size() && y <= log_bottom; ++i, ++y) {
        renderer_.Put(0, y, log_[i], MakeAttr(Color::kBrightCyan, Color::kBlack));
    }

    // 状态栏（最后一行）
    if (h >= 1) {
        std::wstring status =
            L" keys=" + std::to_wstring(key_count_) +
            L"  resize=" + std::to_wstring(resize_count_) +
            L"  mouse=" + std::to_wstring(mouse_count_) +
            L"  size=" + std::to_wstring(w) + L"x" + std::to_wstring(h) +
            L"  presents=" + std::to_wstring(renderer_.PresentCount());
        // 先用状态色铺满，再叠加文字。
        Cell sc = StatusCell();
        renderer_.FillRect(0, h - 1, w - 1, h - 1, sc);
        renderer_.Put(0, h - 1, status, MakeAttr(Color::kBlack, Color::kCyan));
    }
}

}  // namespace tui::console
