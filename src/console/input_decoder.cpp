#include "tui/console/input_decoder.h"

#include <cwchar>

namespace tui::console {

namespace {

uint32_t TranslateMods(DWORD state) {
    uint32_t m = 0;
    if (state & (RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED)) m |= Mod::kAlt;
    if (state & (RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED)) m |= Mod::kCtrl;
    if (state & SHIFT_PRESSED) m |= Mod::kShift;
    if (state & CAPSLOCK_ON) m |= Mod::kCapsLock;
    if (state & NUMLOCK_ON) m |= Mod::kNumLock;
    if (state & SCROLLLOCK_ON) m |= Mod::kScrollLock;
    if (state & ENHANCED_KEY) m |= Mod::kEnhanced;
    return m;
}

std::wstring VkName(uint16_t vk) {
    // 第一版只给常见键一个可读名字，其余用 VK 号。
    switch (vk) {
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_BACK: return L"Backspace";
        case VK_TAB: return L"Tab";
        case VK_SPACE: return L"Space";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"PgUp";
        case VK_NEXT: return L"PgDn";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_F1: return L"F1";
        case VK_F2: return L"F2";
        case VK_F3: return L"F3";
        case VK_F4: return L"F4";
        case VK_F5: return L"F5";
        case VK_F6: return L"F6";
        case VK_F7: return L"F7";
        case VK_F8: return L"F8";
        case VK_F9: return L"F9";
        case VK_F10: return L"F10";
        case VK_F11: return L"F11";
        case VK_F12: return L"F12";
        default: return L"VK" + std::to_wstring(vk);
    }
}

}  // namespace

InputEvent Decode(const INPUT_RECORD& rec) {
    InputEvent e;
    switch (rec.EventType) {
        case KEY_EVENT: {
            const auto& k = rec.Event.KeyEvent;
            e.kind = InputKind::kKey;
            e.key_down = k.bKeyDown != FALSE;
            e.ch = k.uChar.UnicodeChar;
            e.vk = k.wVirtualKeyCode;
            e.repeat = k.wRepeatCount;
            e.mods = TranslateMods(k.dwControlKeyState);
            break;
        }
        case MOUSE_EVENT: {
            const auto& m = rec.Event.MouseEvent;
            e.kind = InputKind::kMouse;
            e.mouse_x = m.dwMousePosition.X;
            e.mouse_y = m.dwMousePosition.Y;
            e.mouse_buttons = m.dwButtonState;
            e.mouse_event_flags = m.dwEventFlags;
            e.mods = TranslateMods(m.dwControlKeyState);
            break;
        }
        case WINDOW_BUFFER_SIZE_EVENT: {
            const auto& w = rec.Event.WindowBufferSizeEvent;
            e.kind = InputKind::kResize;
            e.width = w.dwSize.X;
            e.height = w.dwSize.Y;
            break;
        }
        case FOCUS_EVENT: {
            e.kind = InputKind::kFocus;
            e.focus = rec.Event.FocusEvent.bSetFocus != FALSE;
            break;
        }
        case MENU_EVENT:
        default:
            e.kind = InputKind::kUnknown;
            break;
    }
    return e;
}

std::vector<InputEvent> DecodeAll(const INPUT_RECORD* recs, size_t count) {
    std::vector<InputEvent> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.push_back(Decode(recs[i]));
    return out;
}

std::wstring InputEvent::Describe() const {
    std::wstring s;
    switch (kind) {
        case InputKind::kKey: {
            s = key_down ? L"KEY down " : L"KEY up   ";
            if (ch >= 32 && ch < 127) {
                s += L"'" + std::wstring(1, ch) + L"' ";
            }
            s += VkName(vk);
            if (mods & Mod::kCtrl) s += L" +Ctrl";
            if (mods & Mod::kAlt) s += L" +Alt";
            if (mods & Mod::kShift) s += L" +Shift";
            if (repeat > 1) s += L" x" + std::to_wstring(repeat);
            break;
        }
        case InputKind::kMouse:
            s = L"MOUSE (" + std::to_wstring(mouse_x) + L"," +
                std::to_wstring(mouse_y) + L") buttons=" +
                std::to_wstring(mouse_buttons);
            break;
        case InputKind::kResize:
            s = L"RESIZE " + std::to_wstring(width) + L"x" + std::to_wstring(height);
            break;
        case InputKind::kFocus:
            s = focus ? L"FOCUS gained" : L"FOCUS lost";
            break;
        default:
            s = L"(unknown event)";
            break;
    }
    return s;
}

}  // namespace tui::console
