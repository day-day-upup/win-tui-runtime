#include "tui/console/render_backend.h"

#include <algorithm>

namespace tui::console {

namespace {

// 把 deltas 按 (y, x) 排序，并按「同一行且 x 连续」分组成 run。
// 每个 run 用一次 WriteConsoleOutputW 写入。
struct Run {
    int16_t y;
    int16_t x0;
    std::vector<CHAR_INFO> cells;
};

std::vector<Run> GroupIntoRuns(const std::vector<CellDelta>& deltas) {
    std::vector<CellDelta> sorted = deltas;
    std::sort(sorted.begin(), sorted.end(), [](const CellDelta& a, const CellDelta& b) {
        if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
        return a.pos.x < b.pos.x;
    });

    std::vector<Run> runs;
    for (const auto& d : sorted) {
        if (!runs.empty() && runs.back().y == d.pos.y &&
            runs.back().x0 + static_cast<int16_t>(runs.back().cells.size()) == d.pos.x) {
            // 同一行且 x 连续 → 并入当前 run
        } else {
            Run r;
            r.y = d.pos.y;
            r.x0 = d.pos.x;
            runs.push_back(std::move(r));
        }
        CHAR_INFO ci{};
        ci.Char.UnicodeChar = d.cell.ch;
        ci.Attributes = d.cell.attr;
        runs.back().cells.push_back(ci);
    }
    return runs;
}

}  // namespace

bool LegacyConsoleBackend::Present(const std::vector<CellDelta>& deltas) {
    last_calls_ = 0;
    if (deltas.empty()) return true;
    if (!h_ || h_ == INVALID_HANDLE_VALUE) return false;

    auto runs = GroupIntoRuns(deltas);
    for (auto& r : runs) {
        COORD buf_size = {static_cast<SHORT>(r.cells.size()), 1};
        COORD buf_pos = {0, 0};
        SMALL_RECT region = {r.x0, r.y,
                             static_cast<SHORT>(r.x0 + r.cells.size() - 1), r.y};
        BOOL ok = WriteConsoleOutputW(h_, r.cells.data(), buf_size, buf_pos, &region);
        ++last_calls_;
        if (!ok) return false;
    }
    return true;
}

bool MemoryBackend::Present(const std::vector<CellDelta>& deltas) {
    const size_t total = static_cast<size_t>(size_.w) * size_.h;
    if (frame_.size() != total) {
        frame_.assign(total, Cell{});
    }
    std::vector<CellDelta> copy = deltas;
    for (const auto& d : copy) {
        if (d.pos.x < 0 || d.pos.y < 0 || d.pos.x >= size_.w || d.pos.y >= size_.h) continue;
        frame_[static_cast<size_t>(d.pos.y) * size_.w + d.pos.x] = d.cell;
    }
    history_.push_back(std::move(copy));
    return true;
}

}  // namespace tui::console
