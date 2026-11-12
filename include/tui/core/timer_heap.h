#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "tui/core/types.h"

// 阶段 1：TimerHeap —— 手写二叉最小堆
//
// 为什么手写而不用 std::priority_queue？
//   1) 需要 CancelTimer(id)：std::priority_queue 不支持按 id 删除；
//   2) 需要「同 deadline 按插入序 FIFO」：用 (deadline, seq) 复合键；
//   3) 这是事件循环里定时器的标准实现，手写一遍才能看清「堆顶 = 下次 wait 的 timeout」。
//
// 排序键：(deadline_ms, seq)。seq 单调递增，故同一时刻的定时器严格按插入序触发。
//
// id 由 EventLoop 分配并显式传入（因为跨线程投递时要先在调用线程同步拿到 id）。
// 重复定时器在 PopDue 里以「同一个 id」重新入堆，因此 CancelTimer(id) 始终能
// 找到它当前的堆条目——id 是「定时器的身份」，不是「某次堆插入」的编号。
//
// 删除：Cancel(id) 线性定位节点后调用 RemoveAt——尾节点补位，再按需上浮/下沉。
// 阶段 1 定时器数量不大，O(n) 定位可接受；规模变大时加 id->index 映射即可。
//
// 注意：本类不自己加锁，只在 loop 线程访问（约束 B1）。
namespace tui::core {

class TimerHeap {
public:
    // deadline_ms 为绝对毫秒（相对 EventLoop 所用 IClock 的零点）。
    // interval_ms > 0 表示重复；重排基准是触发时的 now，不是原 deadline。
    void Add(TimerId id, int64_t deadline_ms, int64_t interval_ms, Task task);
    bool Cancel(TimerId id);

    // 堆顶有效 deadline（绝对毫秒）；无定时器返回 -1。
    int64_t NextDeadlineMs() const;

    // 取出所有 deadline <= now_ms 的到期任务，按 (deadline, seq) 顺序。
    // 重复定时器会按 interval_ms 重新入堆（保留同一 id）。
    std::vector<Task> PopDue(int64_t now_ms);

    size_t Size() const { return heap_.size(); }
    bool Empty() const { return heap_.empty(); }
    bool Contains(TimerId id) const;

private:
    struct Node {
        TimerId id;
        int64_t deadline_ms;
        int64_t interval_ms;
        uint64_t seq;
        Task task;
    };

    static bool Less(const Node& a, const Node& b) {
        if (a.deadline_ms != b.deadline_ms) return a.deadline_ms < b.deadline_ms;
        return a.seq < b.seq;
    }

    void SiftUp(size_t i);
    void SiftDown(size_t i);
    void RemoveAt(size_t i);

    std::vector<Node> heap_;
    uint64_t next_seq_ = 1;
};

}  // namespace tui::core
