#include "tui/console/screen_buffer.h"

namespace tui::console {

ScreenBuffer::ScreenBuffer(Size size) : size_(size) {
    const size_t n = static_cast<size_t>(size.w) * size.h;
    front_.assign(n, Cell{});
    back_.assign(n, Cell{});
    dirty_.assign(n, 0);
}

void ScreenBuffer::Resize(Size s) {
    if (s == size_) return;
    size_ = s;
    const size_t n = static_cast<size_t>(s.w) * s.h;
    // 尺寸变化 → 内容不可复用，整屏标脏，确保新区域与旧区域都被推送。
    front_.assign(n, Cell{});
    back_.assign(n, Cell{});
    dirty_.assign(n, 1);
    force_full_ = true;
}

void ScreenBuffer::Clear(Cell fill) {
    for (int16_t y = 0; y < size_.h; ++y) {
        for (int16_t x = 0; x < size_.w; ++x) {
            Put(x, y, fill);
        }
    }
}

void ScreenBuffer::Put(int16_t x, int16_t y, Cell cell) {
    if (!InRange(x, y)) return;
    const size_t i = Index(x, y);
    if (back_[i] == cell && dirty_[i]) return;  // 写出相同值且已标脏，跳过
    back_[i] = cell;
    dirty_[i] = 1;  // 即使值相同也标脏：front_ 可能不同（例如上一帧是别的值）
}

void ScreenBuffer::Put(int16_t x, int16_t y, const std::wstring& text, WORD attr) {
    Cell c;
    c.attr = attr;
    for (size_t k = 0; k < text.size(); ++k) {
        const int16_t px = static_cast<int16_t>(x + k);
        if (px >= size_.w) break;
        c.ch = text[k];
        Put(px, y, c);
    }
}

void ScreenBuffer::FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Cell cell) {
    for (int16_t y = y0; y <= y1; ++y) {
        for (int16_t x = x0; x <= x1; ++x) {
            Put(x, y, cell);
        }
    }
}

std::vector<CellDelta> ScreenBuffer::Commit() {
    std::vector<CellDelta> deltas;
    const size_t n = static_cast<size_t>(size_.w) * size_.h;
    for (size_t i = 0; i < n; ++i) {
        // force_full_：整屏强制输出（Resize 后清掉真实控制台上的旧内容）。
        // 其余情况只检查本帧标脏的单元格。
        if (!force_full_ && !dirty_[i]) continue;
        dirty_[i] = 0;
        if (!force_full_ && front_[i] == back_[i]) continue;  // 标脏但内容没变
        front_[i] = back_[i];
        CellDelta d;
        d.pos.x = static_cast<int16_t>(i % size_.w);
        d.pos.y = static_cast<int16_t>(i / size_.w);
        d.cell = back_[i];
        deltas.push_back(d);
    }
    force_full_ = false;
    return deltas;
}

Cell ScreenBuffer::Back(int16_t x, int16_t y) const {
    if (!InRange(x, y)) return Cell{};
    return back_[Index(x, y)];
}

Cell ScreenBuffer::Front(int16_t x, int16_t y) const {
    if (!InRange(x, y)) return Cell{};
    return front_[Index(x, y)];
}

}  // namespace tui::console
