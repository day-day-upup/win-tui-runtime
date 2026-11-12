#include "tui/console/renderer.h"

namespace tui::console {

Renderer::Renderer(std::unique_ptr<IRenderBackend> backend, Size size)
    : backend_(std::move(backend)), buffer_(size) {
    dirty_ = true;  // 首帧整屏输出
}

void Renderer::Resize(Size s) {
    buffer_.Resize(s);
    if (auto* mem = dynamic_cast<MemoryBackend*>(backend_.get())) {
        mem->SetSize(s);
    } else if (auto* legacy = dynamic_cast<LegacyConsoleBackend*>(backend_.get())) {
        legacy->SetSize(s);
    }
    dirty_ = true;  // resize 后必须整屏刷新
}

void Renderer::Clear(Cell fill) {
    buffer_.Clear(fill);
    dirty_ = true;
}

void Renderer::Put(int16_t x, int16_t y, Cell cell) {
    buffer_.Put(x, y, cell);
    dirty_ = true;
}

void Renderer::Put(int16_t x, int16_t y, const std::wstring& text, WORD attr) {
    buffer_.Put(x, y, text, attr);
    dirty_ = true;
}

void Renderer::FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Cell cell) {
    buffer_.FillRect(x0, y0, x1, y1, cell);
    dirty_ = true;
}

bool Renderer::RenderTick() {
    if (!dirty_) return false;  // 本 tick 无改动 → 不刷屏（合并的关键）
    auto deltas = buffer_.Commit();
    if (deltas.empty()) {
        dirty_ = false;
        return false;  // 标了脏但内容与上帧相同 → 不写
    }
    bool ok = backend_->Present(deltas);
    total_deltas_ += deltas.size();
    if (ok) ++present_count_;
    dirty_ = false;
    return ok;
}

}  // namespace tui::console
