#include "test_framework.h"

#include <string>
#include <thread>
#include <vector>

#include "tui/core/clock.h"
#include "tui/core/event_loop.h"
#include "tui/core/timer_heap.h"

using namespace tui::core;

// ---------------------------------------------------------------------------
// TimerHeap 直接测试：排序、同刻 FIFO、任意删除、重复定时器
// ---------------------------------------------------------------------------

TEST(timer_heap_orders_by_deadline) {
    TimerHeap h;
    std::vector<int> out;
    h.Add(1, 100, 0, [&] { out.push_back(1); });
    h.Add(2, 30, 0, [&] { out.push_back(2); });
    h.Add(3, 70, 0, [&] { out.push_back(3); });

    CHECK_EQ(h.NextDeadlineMs(), static_cast<int64_t>(30));

    auto due = h.PopDue(100);
    CHECK_EQ(due.size(), static_cast<size_t>(3));
    for (auto& t : due) t();
    CHECK_EQ(out.size(), static_cast<size_t>(3));
    CHECK_EQ(out[0], 2);
    CHECK_EQ(out[1], 3);
    CHECK_EQ(out[2], 1);
}

TEST(timer_heap_same_deadline_is_fifo) {
    TimerHeap h;
    std::vector<int> out;
    for (int i = 0; i < 5; ++i) {
        h.Add(static_cast<TimerId>(i + 1), 50, 0, [i, &out] { out.push_back(i); });
    }
    auto due = h.PopDue(50);
    for (auto& t : due) t();
    CHECK_EQ(out.size(), static_cast<size_t>(5));
    for (int i = 0; i < 5; ++i) CHECK_EQ(out[i], i);
}

TEST(timer_heap_cancel_removes_and_keeps_heap_valid) {
    TimerHeap h;
    std::vector<int> out;
    h.Add(1, 10, 0, [&] { out.push_back(1); });
    h.Add(2, 20, 0, [&] { out.push_back(2); });
    h.Add(3, 30, 0, [&] { out.push_back(3); });
    h.Add(4, 40, 0, [&] { out.push_back(4); });
    h.Add(5, 50, 0, [&] { out.push_back(5); });

    CHECK_TRUE(h.Cancel(3));   // 中间节点
    CHECK_TRUE(h.Cancel(1));   // 堆顶
    CHECK_FALSE(h.Cancel(999));
    CHECK_FALSE(h.Contains(3));
    CHECK_FALSE(h.Contains(1));
    CHECK_TRUE(h.Contains(5));

    auto due = h.PopDue(100);
    for (auto& t : due) t();
    CHECK_EQ(out.size(), static_cast<size_t>(3));
    CHECK_EQ(out[0], 2);
    CHECK_EQ(out[1], 4);
    CHECK_EQ(out[2], 5);
}

TEST(timer_heap_repeating_keeps_same_id) {
    TimerHeap h;
    int count = 0;
    h.Add(7, 10, 10, [&] { ++count; });

    auto d1 = h.PopDue(10);
    CHECK_EQ(d1.size(), static_cast<size_t>(1));
    d1[0]();
    CHECK_EQ(count, 1);
    CHECK_TRUE(h.Contains(7));
    CHECK_EQ(h.NextDeadlineMs(), static_cast<int64_t>(20));

    auto d2 = h.PopDue(20);
    d2[0]();
    CHECK_EQ(count, 2);
    CHECK_TRUE(h.Contains(7));

    CHECK_TRUE(h.Cancel(7));  // 同一个 id 仍能取消
    auto d3 = h.PopDue(1000);
    CHECK_EQ(d3.size(), static_cast<size_t>(0));
}

TEST(timer_heap_popdue_only_returns_due) {
    TimerHeap h;
    int a = 0, b = 0;
    h.Add(1, 10, 0, [&] { ++a; });
    h.Add(2, 100, 0, [&] { ++b; });
    auto due = h.PopDue(50);
    for (auto& t : due) t();
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 0);
    CHECK_EQ(h.NextDeadlineMs(), static_cast<int64_t>(100));
}

// ---------------------------------------------------------------------------
// EventLoop + FakeClock：定时器精度、postTask 顺序、取消、idle
// ---------------------------------------------------------------------------

TEST(fakeclock_timer_fires_at_exact_deadline) {
    FakeClock clock(1000);
    EventLoop loop(clock);
    std::vector<std::string> log;

    loop.PostDelayedTask(50, [&] { log.push_back("t50"); });
    loop.PostDelayedTask(20, [&] { log.push_back("t20"); });
    loop.PostDelayedTask(35, [&] { log.push_back("t35"); });

    // 先跑一轮把「Add 定时器」的控制任务落地（此时 now=1000，无到期）。
    loop.RunOneIterationForTest();
    CHECK_EQ(log.size(), static_cast<size_t>(0));

    clock.Set(1019);
    loop.RunOneIterationForTest();
    CHECK_EQ(log.size(), static_cast<size_t>(0));  // 1019 < 1020

    clock.Set(1020);
    loop.RunOneIterationForTest();
    CHECK_EQ(log.size(), static_cast<size_t>(1));
    CHECK_EQ(log[0], std::string("t20"));

    clock.Set(1035);
    loop.RunOneIterationForTest();
    CHECK_EQ(log.size(), static_cast<size_t>(2));
    CHECK_EQ(log[1], std::string("t35"));

    clock.Set(1050);
    loop.RunOneIterationForTest();
    CHECK_EQ(log.size(), static_cast<size_t>(3));
    CHECK_EQ(log[2], std::string("t50"));
}

TEST(fakeclock_same_deadline_fifo) {
    FakeClock clock(0);
    EventLoop loop(clock);
    std::vector<int> out;
    loop.PostDelayedTask(10, [&] { out.push_back(1); });
    loop.PostDelayedTask(10, [&] { out.push_back(2); });
    loop.PostDelayedTask(10, [&] { out.push_back(3); });
    loop.RunOneIterationForTest();  // 落地 Add

    clock.Set(10);
    loop.RunOneIterationForTest();
    CHECK_EQ(out.size(), static_cast<size_t>(3));
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 2);
    CHECK_EQ(out[2], 3);
}

TEST(posttask_runs_before_timers_of_same_iteration) {
    FakeClock clock(0);
    EventLoop loop(clock);
    std::vector<std::string> out;
    loop.PostDelayedTask(0, [&] { out.push_back("timer"); });
    loop.PostTask([&] { out.push_back("post"); });
    loop.RunOneIterationForTest();  // 落地定时器的 Add 控制任务 + post
    loop.RunOneIterationForTest();  // 触发定时器
    CHECK_EQ(out.size(), static_cast<size_t>(2));
    CHECK_EQ(out[0], std::string("post"));   // 先投递
    CHECK_EQ(out[1], std::string("timer"));  // 后定时器
}

TEST(posttask_order_is_fifo) {
    FakeClock clock(0);
    EventLoop loop(clock);
    std::vector<int> out;
    for (int i = 0; i < 10; ++i) loop.PostTask([i, &out] { out.push_back(i); });
    loop.RunOneIterationForTest();
    CHECK_EQ(out.size(), static_cast<size_t>(10));
    for (int i = 0; i < 10; ++i) CHECK_EQ(out[i], i);
}

TEST(cancel_timer_prevents_fire) {
    FakeClock clock(0);
    EventLoop loop(clock);
    int fired = 0;
    TimerId id = loop.PostDelayedTask(10, [&] { ++fired; });
    loop.RunOneIterationForTest();  // 落地 Add
    loop.CancelTimer(id);
    loop.RunOneIterationForTest();  // 落地 Cancel
    clock.Set(100);
    loop.RunOneIterationForTest();
    CHECK_EQ(fired, 0);
}

TEST(repeating_timer_fires_each_interval) {
    FakeClock clock(0);
    EventLoop loop(clock);
    std::vector<int64_t> times;
    loop.PostRepeatingTask(10, 10, [&] { times.push_back(clock.NowMs()); });
    loop.RunOneIterationForTest();  // 落地 Add

    clock.Set(10);
    loop.RunOneIterationForTest();
    clock.Set(20);
    loop.RunOneIterationForTest();
    clock.Set(30);
    loop.RunOneIterationForTest();
    CHECK_EQ(times.size(), static_cast<size_t>(3));
    CHECK_EQ(times[0], static_cast<int64_t>(10));
    CHECK_EQ(times[1], static_cast<int64_t>(20));
    CHECK_EQ(times[2], static_cast<int64_t>(30));
}

TEST(idle_task_runs_only_when_nothing_pending) {
    FakeClock clock(0);
    EventLoop loop(clock);
    int idle = 0;
    loop.SetIdleTask([&] { ++idle; });
    loop.RunOneIterationForTest();  // 落地 SetIdleTask
    CHECK_EQ(idle, 0);             // 该轮有控制任务，不算 idle
    loop.RunOneIterationForTest();  // 真空闲
    CHECK_EQ(idle, 1);
    loop.RunOneIterationForTest();
    CHECK_EQ(idle, 2);
}

// ---------------------------------------------------------------------------
// 真实 Run()：跨线程投递唤醒、Stop 排空
// ---------------------------------------------------------------------------

TEST(cross_thread_posttask_wakes_blocked_loop) {
    SystemClock clock;
    EventLoop loop(clock);
    std::atomic<int> count{0};

    std::thread t([&] { loop.Run(); });

    // 从另一线程投递 5 个任务，最后一个负责 Stop。
    std::thread poster([&] {
        for (int i = 0; i < 4; ++i) {
            loop.PostTask([&] { ++count; });
        }
        loop.PostTask([&] { loop.Stop(); });
    });

    poster.join();
    t.join();
    CHECK_EQ(count.load(), 4);
    CHECK_EQ(loop.State(), LoopState::kStopped);
}

TEST(realtime_timer_fires_and_stops) {
    SystemClock clock;
    EventLoop loop(clock);
    std::atomic<int> ticks{0};

    loop.PostDelayedTask(30, [&] {
        ++ticks;
        loop.Stop();
    });

    loop.Run();  // 阻塞直到定时器触发并 Stop
    CHECK_EQ(ticks.load(), 1);
    CHECK_EQ(loop.State(), LoopState::kStopped);
}

TEST(stop_drains_pending_posted_tasks) {
    SystemClock clock;
    EventLoop loop(clock);
    std::atomic<int> count{0};

    std::thread t([&] { loop.Run(); });
    // 同等优先级下，先投递普通任务，再立刻 Stop。
    // Stop 只置标志+wake，退出前的 DispatchPostedTasks 应把这些任务跑完。
    for (int i = 0; i < 3; ++i) loop.PostTask([&] { ++count; });
    loop.Stop();
    t.join();
    CHECK_EQ(count.load(), 3);
}

int main() {
    return tui::test::RunAll();
}
