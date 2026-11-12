#include "tui/console/console_input_thread.h"

namespace tui::console {

ConsoleInputThread::ConsoleInputThread(HANDLE conin, InputCallback cb)
    : conin_(conin), cb_(std::move(cb)) {}

ConsoleInputThread::~ConsoleInputThread() { Stop(); }

void ConsoleInputThread::Start() {
    if (running_.load()) return;
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { ThreadMain(); });
    // 取原生线程句柄，供 CancelSynchronousIo 用（不接管所有权，CloseHandle 前不要 join）。
    thread_handle_ = thread_.native_handle();
}

void ConsoleInputThread::Stop() {
    if (!running_.exchange(false)) return;  // 已经停了
    stop_.store(true);
    InjectWakeupRecord();  // 唤醒阻塞的 ReadConsoleInput
    // 双保险：若注入未生效，取消该线程正在进行的同步 I/O。
    if (thread_handle_) CancelSynchronousIo(thread_handle_);
    if (thread_.joinable()) thread_.join();
    thread_handle_ = nullptr;
}

void ConsoleInputThread::InjectWakeupRecord() {
    if (!conin_ || conin_ == INVALID_HANDLE_VALUE) return;
    INPUT_RECORD rec{};
    rec.EventType = KEY_EVENT;
    rec.Event.KeyEvent.bKeyDown = TRUE;
    rec.Event.KeyEvent.wVirtualKeyCode = 0;     // 哨兵：VK=0
    rec.Event.KeyEvent.uChar.UnicodeChar = 0;  // 且 ch=0
    DWORD written = 0;
    WriteConsoleInputW(conin_, &rec, 1, &written);
    // 无论注入是否成功，Stop() 都会额外调用 CancelSynchronousIo 兜底。
}

void ConsoleInputThread::ThreadMain() {
    constexpr DWORD kBatch = 32;
    INPUT_RECORD recs[kBatch];
    while (!stop_.load()) {
        DWORD read = 0;
        BOOL ok = ReadConsoleInputW(conin_, recs, kBatch, &read);
        if (!ok || read == 0) {
            // 被 CancelSynchronousIo 打断或句柄失效 → 退出。
            break;
        }
        for (DWORD i = 0; i < read; ++i) {
            // 识别哨兵记录：VK=0 且 ch=0 的按键事件，且已请求停止。
            if (stop_.load() && recs[i].EventType == KEY_EVENT &&
                recs[i].Event.KeyEvent.wVirtualKeyCode == 0 &&
                recs[i].Event.KeyEvent.uChar.UnicodeChar == 0) {
                return;
            }
            InputEvent e = Decode(recs[i]);
            if (cb_) cb_(e);
        }
    }
    running_.store(false);
}

}  // namespace tui::console
