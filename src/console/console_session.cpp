#include "tui/console/console_session.h"

namespace tui::console {

namespace {

HANDLE OpenConsole(const wchar_t* name, DWORD access) {
    HANDLE h = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) h = nullptr;
    return h;
}

}  // namespace

ConsoleSession::ConsoleSession() {
    // 输入句柄：优先 CONIN$，失败退化到 STD_INPUT_HANDLE。
    conin_ = OpenConsole(L"CONIN$", GENERIC_READ | GENERIC_WRITE);
    if (!conin_) conin_ = GetStdHandle(STD_INPUT_HANDLE);

    // 输出句柄：优先 CONOUT$，失败退化到 STD_OUTPUT_HANDLE。
    HANDLE out = OpenConsole(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
    if (!out) out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!conin_ || !out || conin_ == INVALID_HANDLE_VALUE ||
        out == INVALID_HANDLE_VALUE) {
        last_error_ = GetLastError();
        return;
    }

    // 保存输入/输出 mode 以便退出时恢复。
    if (!GetConsoleMode(conin_, &saved_in_mode_)) { last_error_ = GetLastError(); return; }
    if (!GetConsoleMode(out, &saved_out_mode_)) { last_error_ = GetLastError(); return; }

    // 创建自己的屏幕缓冲并设为活动缓冲，这样 WriteConsoleOutputW 写的是我们的画布，
    // 不会污染调用者已有的滚动内容。
    own_buffer_ = CreateConsoleScreenBuffer(
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);
    if (own_buffer_ != INVALID_HANDLE_VALUE) {
        saved_out_ = out;
        if (SetConsoleActiveScreenBuffer(own_buffer_)) {
            conout_ = own_buffer_;
            owns_buffer_ = true;
        } else {
            CloseHandle(own_buffer_);
            own_buffer_ = nullptr;
        }
    }
    if (!owns_buffer_) {
        conout_ = out;  // 退化：直接用活动缓冲
    }

    // 输入模式：窗口尺寸 + 鼠标事件；关掉行输入/回显，让按键即时到达。
    DWORD in_mode = ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    if (!SetConsoleMode(conin_, in_mode)) { last_error_ = GetLastError(); return; }

    // 输出模式：关闭自动换行（差分渲染自己控制边界）。
    DWORD out_mode = saved_out_mode_ & ~static_cast<DWORD>(ENABLE_WRAP_AT_EOL_OUTPUT);
    SetConsoleMode(conout_, out_mode);

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(conout_, &csbi)) {
        cached_size_ = {csbi.dwSize.X, csbi.dwSize.Y};
    } else {
        cached_size_ = {80, 25};
    }

    // 隐藏光标：避免闪烁干扰。
    SetCursorVisible(false);
    ok_ = true;
}

ConsoleSession::~ConsoleSession() { Restore(); }

void ConsoleSession::Restore() {
    if (!conin_ || conin_ == INVALID_HANDLE_VALUE) return;
    SetConsoleMode(conin_, saved_in_mode_);
    if (owns_buffer_ && saved_out_ && saved_out_ != INVALID_HANDLE_VALUE) {
        SetConsoleActiveScreenBuffer(saved_out_);
    }
    if (conout_ && conout_ != INVALID_HANDLE_VALUE) {
        SetConsoleMode(conout_, saved_out_mode_);
    }
    SetCursorVisible(true);
    if (own_buffer_ && own_buffer_ != INVALID_HANDLE_VALUE) {
        CloseHandle(own_buffer_);
        own_buffer_ = nullptr;
    }
    ok_ = false;
}

Size ConsoleSession::GetSize() const { return cached_size_; }

void ConsoleSession::RefreshSize() {
    if (!conout_ || conout_ == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(conout_, &csbi)) {
        cached_size_ = {csbi.dwSize.X, csbi.dwSize.Y};
        // 新建缓冲时也同步一下尺寸（SetConsoleScreenBufferSize 可能因窗口更小而失败）。
        SetConsoleScreenBufferSize(conout_, csbi.dwSize);
    }
}

void ConsoleSession::SetCursorVisible(bool visible) {
    if (!conout_ || conout_ == INVALID_HANDLE_VALUE) return;
    CONSOLE_CURSOR_INFO ci{};
    if (!GetConsoleCursorInfo(conout_, &ci)) return;
    ci.bVisible = visible ? TRUE : FALSE;
    SetConsoleCursorInfo(conout_, &ci);
}

}  // namespace tui::console
