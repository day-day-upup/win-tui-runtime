// 阶段 1：headless demo —— 用真实时钟跑 3 秒，直观看到事件循环在做什么。
//
// 这个 demo 没有任何控制台 UI（符合阶段 1「纯 headless」的边界），
// 只往 stdout 打印：
//   - 一个 100ms 的周期定时器（心跳）
//   - 几个一次性定时器（观察排序）
//   - 一个跨线程投递的任务（观察 Waker 唤醒）
//   - 到时间后由定时器调用 Stop()，干净退出
#include <chrono>
#include <cstdio>
#include <thread>

#include "tui/core/clock.h"
#include "tui/core/event_loop.h"

using namespace tui::core;

int main() {
    SystemClock clock;
    EventLoop loop(clock);
    const int64_t t0 = clock.NowMs();

    std::printf("[demo] start at t=%lldms\n", static_cast<long long>(t0));

    // 心跳：每 100ms 一次，共约 3 秒。
    loop.PostRepeatingTask(100, 100, [&] {
        std::printf("[heartbeat] t=+%lldms\n",
                    static_cast<long long>(clock.NowMs() - t0));
    });

    // 一次性任务，故意乱序投递，验证堆排序。
    loop.PostDelayedTask(250, [&] { std::printf("[timer] 250ms fired\n"); });
    loop.PostDelayedTask(50, [&] { std::printf("[timer] 50ms fired\n"); });
    loop.PostDelayedTask(150, [&] { std::printf("[timer] 150ms fired\n"); });

    // 从其它线程投递任务：验证 Waker 跨线程唤醒。
    std::thread feeder([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        loop.PostTask([&] { std::printf("[posted] from worker thread\n"); });
    });

    // 3 秒后停止。
    loop.PostDelayedTask(3000, [&] {
        std::printf("[demo] stopping\n");
        loop.Stop();
    });

    loop.Run();
    feeder.join();
    std::printf("[demo] stopped, state=%d, elapsed=%lldms\n",
                static_cast<int>(loop.State()),
                static_cast<long long>(clock.NowMs() - t0));
    return 0;
}
