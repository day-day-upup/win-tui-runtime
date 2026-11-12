#include "tui/core/clock.h"

#include "tui/core/platform.h"

namespace tui::core {

SystemClock::SystemClock() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq_ = f.QuadPart;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    start_ = c.QuadPart;
}

int64_t SystemClock::NowMs() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // 先乘 1000 再除，减少整数截断误差；用 __int128 避免 128 位溢出风险，
    // 但 QPC 计数在数十年内不会让 int64 溢出，这里用普通 int64 即可。
    const int64_t delta = c.QuadPart - start_;
    return (delta * 1000) / freq_;
}

}  // namespace tui::core
