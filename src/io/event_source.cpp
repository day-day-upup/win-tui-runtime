#include "tui/io/event_source.h"

#include <algorithm>

namespace tui::io {

void EventSource::CloseAll() {
    // 约束 C4：先对每个 watcher 发起取消，completion 排空由 loop 继续驱动。
    for (auto& w : watchers_) {
        if (w) w->Close();
    }
}

void EventSource::Reap() {
    // 仅回收已 kClosed 的 watcher；kClosing 的还要等 completion，不能释放。
    watchers_.erase(
        std::remove_if(watchers_.begin(), watchers_.end(),
                       [](const std::unique_ptr<Watcher>& w) {
                           return !w || w->State() == WatcherState::kClosed;
                       }),
        watchers_.end());
}

}  // namespace tui::io
