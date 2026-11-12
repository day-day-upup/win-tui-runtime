#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

// 阶段 4：RingBuffer<T> —— 有上限的背压容器
//
// 事件驱动下多个 source 的日志/数据可能瞬间涌来；若无上限会无限增长。
// Push 在满时丢弃最旧元素并累计 dropped 计数，让「背压」可观测
// （对应大纲阶段 4 出口条件「backpressure 生效」、约束 D4 的刷新合并）。
//
// 注意：本类不自己加锁，只在 loop 线程访问（约束 B1）。
// worker 线程不直接 Push，结果经 EventLoop::PostTask 回投后在 loop 线程入队。
namespace tui::core {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity) {}

    void Push(T value) {
        if (capacity_ == 0) { ++dropped_; return; }
        if (buf_.size() >= capacity_) {
            buf_.pop_front();
            ++dropped_;
        }
        buf_.push_back(std::move(value));
    }

    std::optional<T> Pop() {
        if (buf_.empty()) return std::nullopt;
        T v = std::move(buf_.front());
        buf_.pop_front();
        return v;
    }

    size_t Size() const { return buf_.size(); }
    size_t Capacity() const { return capacity_; }
    size_t Dropped() const { return dropped_; }
    bool Empty() const { return buf_.empty(); }

    const std::deque<T>& Items() const { return buf_; }
    void Clear() { buf_.clear(); }

private:
    size_t capacity_;
    std::deque<T> buf_;
    size_t dropped_ = 0;
};

}  // namespace tui::core
