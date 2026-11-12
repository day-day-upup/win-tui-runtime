#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "tui/console/input_decoder.h"
#include "tui/console/render_backend.h"
#include "tui/console/renderer.h"
#include "tui/console/screen_buffer.h"

using namespace tui::console;

// ---------------------------------------------------------------------------
// ScreenBuffer：dirty-cell 差分
// ---------------------------------------------------------------------------

TEST(screenbuffer_first_commit_emits_full_frame) {
    ScreenBuffer sb({4, 2});
    // 用非默认填充，确保与 front 的初始值不同。
    Cell fill{L'.', MakeAttr(Color::kGreen, Color::kBlack)};
    sb.Clear(fill);
    auto d = sb.Commit();
    // 首帧：所有单元格都写过且与 front 默认值不同 → 全 8 个。
    CHECK_EQ(d.size(), static_cast<size_t>(8));
    CHECK_EQ(sb.Back(0, 0).ch, L'.');
    CHECK_EQ(sb.Front(0, 0).ch, L'.');
}

TEST(screenbuffer_commit_only_dirty_cells) {
    ScreenBuffer sb({4, 1});
    sb.Clear(Cell{});
    sb.Commit();  // 建立 front 基线

    Cell a{L'A', MakeAttr(Color::kGreen, Color::kBlack)};
    sb.Put(1, 0, a);
    auto d = sb.Commit();
    CHECK_EQ(d.size(), static_cast<size_t>(1));
    CHECK_EQ(d[0].pos.x, static_cast<int16_t>(1));
    CHECK_EQ(d[0].cell.ch, L'A');
}

TEST(screenbuffer_no_change_no_delta) {
    ScreenBuffer sb({4, 1});
    sb.Clear(Cell{});
    sb.Commit();

    // 写出与当前相同的值 → 标脏但 front==back，不应产出 delta。
    sb.Put(0, 0, Cell{});
    auto d = sb.Commit();
    CHECK_EQ(d.size(), static_cast<size_t>(0));
}

TEST(screenbuffer_text_writes_in_order) {
    ScreenBuffer sb({10, 1});
    sb.Put(0, 0, std::wstring(L"HELLO"), MakeAttr(Color::kWhite, Color::kBlack));
    auto d = sb.Commit();
    CHECK_EQ(d.size(), static_cast<size_t>(5));
    CHECK_EQ(sb.Back(0, 0).ch, L'H');
    CHECK_EQ(sb.Back(4, 0).ch, L'O');
    CHECK_EQ(sb.Back(5, 0).ch, L' ');  // 未写区域仍是默认
}

TEST(screenbuffer_resize_forces_full_refresh) {
    ScreenBuffer sb({4, 1});
    sb.Clear(Cell{});
    sb.Commit();

    sb.Resize({6, 2});
    // 即使新缓冲内容全是默认值，resize 后首帧也必须整屏输出（清旧内容）。
    auto d = sb.Commit();
    CHECK_EQ(d.size(), static_cast<size_t>(12));
}

TEST(screenbuffer_out_of_range_is_ignored) {
    ScreenBuffer sb({4, 2});
    sb.Put(99, 99, Cell{L'Z', 0});
    sb.Put(-1, 0, Cell{L'Z', 0});
    auto d = sb.Commit();
    CHECK_EQ(d.size(), static_cast<size_t>(0));
}

// ---------------------------------------------------------------------------
// InputDecoder：纯函数解码
// ---------------------------------------------------------------------------

TEST(decode_key_event_ascii) {
    INPUT_RECORD r{};
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = TRUE;
    r.Event.KeyEvent.uChar.UnicodeChar = L'Q';
    r.Event.KeyEvent.wVirtualKeyCode = 'Q';
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.dwControlKeyState = SHIFT_PRESSED;

    auto e = Decode(r);
    CHECK_EQ(static_cast<int>(e.kind), static_cast<int>(InputKind::kKey));
    CHECK_TRUE(e.key_down);
    CHECK_EQ(e.ch, L'Q');
    CHECK_EQ(e.vk, static_cast<uint16_t>('Q'));
    CHECK_TRUE((e.mods & Mod::kShift) != 0);
    CHECK_TRUE(e.Describe().find(L'Q') != std::wstring::npos);
}

TEST(decode_resize_event) {
    INPUT_RECORD r{};
    r.EventType = WINDOW_BUFFER_SIZE_EVENT;
    r.Event.WindowBufferSizeEvent.dwSize = {120, 40};
    auto e = Decode(r);
    CHECK_EQ(static_cast<int>(e.kind), static_cast<int>(InputKind::kResize));
    CHECK_EQ(e.width, static_cast<int16_t>(120));
    CHECK_EQ(e.height, static_cast<int16_t>(40));
}

TEST(decode_mouse_and_focus) {
    INPUT_RECORD m{};
    m.EventType = MOUSE_EVENT;
    m.Event.MouseEvent.dwMousePosition = {5, 9};
    m.Event.MouseEvent.dwButtonState = 1;
    auto em = Decode(m);
    CHECK_EQ(static_cast<int>(em.kind), static_cast<int>(InputKind::kMouse));
    CHECK_EQ(em.mouse_x, static_cast<int16_t>(5));
    CHECK_EQ(em.mouse_y, static_cast<int16_t>(9));

    INPUT_RECORD f{};
    f.EventType = FOCUS_EVENT;
    f.Event.FocusEvent.bSetFocus = TRUE;
    auto ef = Decode(f);
    CHECK_EQ(static_cast<int>(ef.kind), static_cast<int>(InputKind::kFocus));
    CHECK_TRUE(ef.focus);
}

TEST(decode_all_batch_and_unknown) {
    INPUT_RECORD recs[3]{};
    recs[0].EventType = MENU_EVENT;  // 未支持 → kUnknown
    recs[1].EventType = WINDOW_BUFFER_SIZE_EVENT;
    recs[1].Event.WindowBufferSizeEvent.dwSize = {80, 25};
    recs[2].EventType = KEY_EVENT;
    recs[2].Event.KeyEvent.bKeyDown = TRUE;
    recs[2].Event.KeyEvent.uChar.UnicodeChar = L'a';
    auto v = DecodeAll(recs, 3);
    CHECK_EQ(v.size(), static_cast<size_t>(3));
    CHECK_EQ(static_cast<int>(v[0].kind), static_cast<int>(InputKind::kUnknown));
    CHECK_EQ(static_cast<int>(v[1].kind), static_cast<int>(InputKind::kResize));
    CHECK_EQ(static_cast<int>(v[2].kind), static_cast<int>(InputKind::kKey));
}

// ---------------------------------------------------------------------------
// Renderer：按 tick 合并 + 差分
// ---------------------------------------------------------------------------

TEST(renderer_merges_multiple_draws_into_one_present) {
    auto backend = std::make_unique<MemoryBackend>(Size{10, 2});
    auto* mem = backend.get();
    Renderer r(std::move(backend), Size{10, 2});

    r.Clear(Cell{});
    r.Put(0, 0, Cell{L'A', 0});
    r.Put(1, 0, Cell{L'B', 0});
    r.Put(2, 0, Cell{L'C', 0});
    // 三次 Draw 都在同一个 tick 内，只应 Present 一次。
    CHECK_TRUE(r.RenderTick());
    CHECK_EQ(r.PresentCount(), static_cast<size_t>(1));
    CHECK_EQ(mem->History().size(), static_cast<size_t>(1));

    // 第二个 tick 无改动 → 不再写。
    CHECK_FALSE(r.RenderTick());
    CHECK_EQ(r.PresentCount(), static_cast<size_t>(1));
}

TEST(renderer_only_writes_changed_cells_on_second_frame) {
    auto backend = std::make_unique<MemoryBackend>(Size{10, 1});
    auto* mem = backend.get();
    Renderer r(std::move(backend), Size{10, 1});

    r.Put(0, 0, Cell{L'A', 0});
    r.RenderTick();

    // 第二帧只改 (0,0)：应只产出 1 个 delta。
    r.Put(0, 0, Cell{L'B', 0});
    r.RenderTick();
    CHECK_EQ(mem->History().size(), static_cast<size_t>(2));
    CHECK_EQ(mem->History()[1].size(), static_cast<size_t>(1));
    CHECK_EQ(mem->Frame()[0].ch, L'B');
}

TEST(renderer_resize_reemits_full_frame) {
    auto backend = std::make_unique<MemoryBackend>(Size{4, 1});
    auto* mem = backend.get();
    Renderer r(std::move(backend), Size{4, 1});
    // 首帧写点实在的东西，确保真的 Present 了一次。
    r.Put(0, 0, Cell{L'X', 0});
    r.RenderTick();
    CHECK_EQ(mem->History().size(), static_cast<size_t>(1));

    r.Resize(Size{6, 2});
    r.RenderTick();
    CHECK_EQ(mem->History().size(), static_cast<size_t>(2));
    // resize 后必须整屏输出（6x2=12），清除真实控制台上的旧内容残留。
    CHECK_EQ(mem->History()[1].size(), static_cast<size_t>(12));
}

TEST(renderer_memory_backend_accumulates_frame) {
    auto backend = std::make_unique<MemoryBackend>(Size{5, 1});
    auto* mem = backend.get();
    Renderer r(std::move(backend), Size{5, 1});
    r.Put(0, 0, std::wstring(L"HELLO"), 0);
    r.RenderTick();
    CHECK_EQ(mem->Frame().size(), static_cast<size_t>(5));
    CHECK_EQ(mem->Frame()[0].ch, L'H');
    CHECK_EQ(mem->Frame()[4].ch, L'O');
}

int main() { return tui::test::RunAll(); }
