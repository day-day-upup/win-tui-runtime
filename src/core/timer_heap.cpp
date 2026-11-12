#include "tui/core/timer_heap.h"

namespace tui::core {

void TimerHeap::Add(TimerId id, int64_t deadline_ms, int64_t interval_ms, Task task) {
    Node n;
    n.id = id;
    n.deadline_ms = deadline_ms;
    n.interval_ms = interval_ms;
    n.seq = next_seq_++;
    n.task = std::move(task);
    heap_.push_back(std::move(n));
    SiftUp(heap_.size() - 1);
}

void TimerHeap::SiftUp(size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!Less(heap_[i], heap_[parent])) break;
        std::swap(heap_[i], heap_[parent]);
        i = parent;
    }
}

void TimerHeap::SiftDown(size_t i) {
    const size_t n = heap_.size();
    for (;;) {
        size_t l = 2 * i + 1;
        size_t r = 2 * i + 2;
        size_t smallest = i;
        if (l < n && Less(heap_[l], heap_[smallest])) smallest = l;
        if (r < n && Less(heap_[r], heap_[smallest])) smallest = r;
        if (smallest == i) break;
        std::swap(heap_[i], heap_[smallest]);
        i = smallest;
    }
}

void TimerHeap::RemoveAt(size_t i) {
    heap_[i] = std::move(heap_.back());
    heap_.pop_back();
    if (i >= heap_.size()) return;
    if (i > 0 && Less(heap_[i], heap_[(i - 1) / 2])) {
        SiftUp(i);
    } else {
        SiftDown(i);
    }
}

bool TimerHeap::Cancel(TimerId id) {
    for (size_t i = 0; i < heap_.size(); ++i) {
        if (heap_[i].id == id) {
            RemoveAt(i);
            return true;
        }
    }
    return false;
}

bool TimerHeap::Contains(TimerId id) const {
    for (const auto& n : heap_) {
        if (n.id == id) return true;
    }
    return false;
}

int64_t TimerHeap::NextDeadlineMs() const {
    if (heap_.empty()) return -1;
    return heap_.front().deadline_ms;
}

std::vector<Task> TimerHeap::PopDue(int64_t now_ms) {
    std::vector<Task> due;
    for (;;) {
        if (heap_.empty()) break;
        if (heap_.front().deadline_ms > now_ms) break;

        Node n = std::move(heap_[0]);
        RemoveAt(0);

        if (n.interval_ms > 0) {
            // 重复定时器：复制 task（std::function 可拷贝），以 now 为基准重排，
            // 保留同一 id，使 CancelTimer 依然有效。
            Task next = n.task;
            Add(n.id, now_ms + n.interval_ms, n.interval_ms, std::move(next));
        }
        due.push_back(std::move(n.task));
    }
    return due;
}

}  // namespace tui::core
