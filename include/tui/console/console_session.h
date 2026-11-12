#pragma once

#include "tui/console/console_types.h"
#include "tui/core/platform.h"

// 阶段 2：ConsoleSession —— 控制台资源的 RAII 管理
//
// 职责：
//   - 打开 CONIN$ / CONOUT$（或退化用 GetStdHandle）；
//   - 保存并恢复原 console mode；
//   - 设置 ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT（约束 D5）；
//   - 隐藏光标、创建自己的活动屏幕缓冲（避免破坏调用者的滚动历史）；
//   - 退出时恢复一切。
//
// 约束 D1：主路径 legacy console API；VT 作为可选后端由编译开关决定，
// 本阶段不启用。这里始终走 legacy。
namespace tui::console {

class ConsoleSession {
public:
    ConsoleSession();
    ~ConsoleSession();

    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;

    bool Ok() const { return ok_; }
    HANDLE Input() const { return conin_; }
    HANDLE Output() const { return conout_; }
    Size GetSize() const;
    DWORD LastError() const { return last_error_; }

    // 窗口尺寸变化时刷新缓存尺寸（由 resize 事件触发）。
    void RefreshSize();
    void SetCursorVisible(bool visible);

private:
    void Restore();

    HANDLE conin_ = nullptr;
    HANDLE conout_ = nullptr;
    HANDLE saved_out_ = nullptr;       // 进程原活动缓冲
    HANDLE own_buffer_ = nullptr;      // 我们创建的活动缓冲
    DWORD saved_in_mode_ = 0;
    DWORD saved_out_mode_ = 0;
    bool owns_buffer_ = false;
    bool ok_ = false;
    DWORD last_error_ = 0;
    Size cached_size_;
};

}  // namespace tui::console
