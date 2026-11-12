# 阶段 1：Loop 骨架

| 项 | 内容 |
|---|---|
| 阶段号 | 1 |
| 名称 | Loop 骨架（headless 事件循环） |
| 日期 | 2026-11-12 |
| 对应大纲出口条件 | 定时器精度/排序/postTask 顺序有测试覆盖 |
| 关联 commit | 阶段 1 提交 |
| 状态 | 已完成 |

---

## 一、做了什么（What）

**新增文件（代码）：**

| 文件 | 作用 |
|---|---|
| `include/tui/core/platform.h` | 统一 `windows.h` 引入，定义 `NOMINMAX` / `WIN32_LEAN_AND_MEAN` |
| `include/tui/core/types.h` | `Task` / `TimerId` / `TimerSpec` / `LoopState` 基础类型 |
| `include/tui/core/clock.h`、`src/core/clock.cpp` | `IClock` 接口、`SystemClock`(QPC)、`FakeClock`(手动推进) |
| `include/tui/core/task_queue.h` | 基于 `SRWLOCK` 的跨线程任务队列，任意线程 `Post`、loop 线程 `Drain` |
| `include/tui/core/timer_heap.h`、`src/core/timer_heap.cpp` | 手写二叉最小堆：按 `(deadline, seq)` 排序、按 id 删除、重复定时器 |
| `include/tui/core/waker.h`、`src/core/waker.cpp` | Windows 版 self-pipe：auto-reset event + `WaitForSingleObjectEx`(可警报) |
| `include/tui/core/event_loop.h`、`src/core/event_loop.cpp` | 事件循环主体：`Run()` / 任务调度 / 定时器 / idle / 优雅停止 |

**新增文件（验证与工程）：**

| 文件 | 作用 |
|---|---|
| `tests/test_framework.h` | 零依赖极简测试框架（`TEST` / `CHECK_*` / `RunAll`） |
| `tests/test_event_loop.cpp` | 15 个单元测试 |
| `src/app/loop_demo.cpp` | headless 演示（真实时钟跑 3 秒） |
| `CMakeLists.txt` | `tui_core` 静态库 + 测试 + demo |
| `README.md` | 补充构建说明 |

**可观察效果：**
- `build\test_event_loop.exe` → **15/15 passed, 0 failures**。
- `build\loop_demo.exe` → 乱序投递的 50/150/250ms 定时器按 deadline 依次触发；100ms 心跳稳定；worker 线程在 120ms 投递的任务被唤醒执行；3000ms 准时 `Stop()`，以 `state=kStopped` 干净退出。

---

## 二、为什么这么做（Why）

**动机**：阶段 1 的目标是把「事件循环」这件事从概念变成可运行、可测试的最小内核，且**先不碰任何 I/O 与 UI**——这样后面加 IOCP、输入线程、渲染时，问题能定位到具体某一层。对应 `00-大纲.md` 学习目标 1（理解事件循环）与 4（生命周期直觉）。

**对应约束编号：**
- `B1` 唯一 loop 线程拥有状态 → `TimerHeap` / `idle_task_` 无锁，只在 `Run()` 线程访问。
- `B2`/`B5` 跨线程只投递 + Waker 唤醒 → `PostTask`/`PostDelayedTask`/`Stop` 全部「写 TaskQueue / 置原子标志 / SetEvent」。
- `B6` loop 线程不阻塞（除等待本身）→ 唯一阻塞点是 `Waker::Wait`。
- `C5`/`C6` 定时器用最小堆、不用 TimerQueueTimer → 手写 `TimerHeap`。
- `A1`/`A4` 零第三方依赖、离线可构建 → 自建测试框架、手写堆。
- `F5` 出口可验证 → 15 个测试 + demo。

**被放弃的方案及原因：**

| 放弃 | 原因 |
|---|---|
| `std::priority_queue` 存定时器 | 不支持按 id 删除（`CancelTimer` 需要），也无法表达「同刻 FIFO」的复合键 |
| 给 `TimerHeap` 加锁让任意线程直接 Add | 违反 `B1`；会引入锁与竞态。改为「id 在调用线程原子分配，堆操作包成 Task 回投 loop」 |
| `CreateTimerQueueTimer` / `SetTimer` 回调 | 回调跑在线程池线程上，破坏单线程分发（`C5`） |
| `std::condition_variable` 做唤醒 | 目标是学 Windows 系统编程；auto-reset event + 可警报等待更贴 Win32，且为阶段 5 的 APC 对照留口子 |
| gtest / catch2 | 违反 `A4`（离线、零下载依赖）；自建 60 行框架足够 |

**这个决定让哪件事变得可见：**
1. **「堆顶 deadline = 下一次 wait 的 timeout」** 这条事件循环核心公式，在 `ComputeWaitMs()` 里是一行减法——定时器不是「另一个线程在计时」，而是 loop 主动算出该睡多久。
2. **Windows 没有 epoll 的第一个后果**：用内核 event 对象做 self-pipe，且必须用可警报等待（`bAlertable=TRUE`）才能承接后续 APC。
3. **跨线程控制定时器的正确姿势**：不是加锁，而是把「Add/Cancel」也当成事件投回 loop。这正是阶段 4 worker 线程池、阶段 3 watcher 关闭要复用的模式。

---

## 三、怎么做的（How）

### 3.1 关键数据结构

**`TimerHeap::Node`**
```cpp
struct Node {
    TimerId id;            // 身份；重复定时器重排时保持不变
    int64_t deadline_ms;   // 绝对毫秒，相对 IClock 零点
    int64_t interval_ms;   // >0 重复
    uint64_t seq;          // 插入序号 → 同 deadline 时 FIFO
    Task task;
};
```
排序键 `(deadline_ms, seq)`。`Less()` 里先比 deadline、再比 seq，保证相同到期时刻严格按投递顺序触发（测试 `fakeclock_same_deadline_fifo` 覆盖）。

**`EventLoop` 成员**
```cpp
IClock& clock_;            // 注入的时间源
TaskQueue tasks_;          // 跨线程，SRWLOCK 保护
TimerHeap timers_;         // 仅 loop 线程
Waker waker_;              // 唤醒用 event
std::atomic<LoopState> state_;
std::atomic<bool> stop_requested_;
std::atomic<TimerId> next_timer_id_;  // 调用线程分配 id
Task idle_task_;           // 仅 loop 线程
```

### 3.2 核心流程 / 调用链

`Run()` 每轮 = `RunIteration(blocking=true)`：

```
DispatchPostedTasks()          // 先落地控制任务（定时器 Add/Cancel、SetIdleTask、普通 PostTask）
        │
        ▼
now = clock_.NowMs()
fired = DispatchDueTimers(now) // PopDue(now) 取出所有 deadline<=now，按序遍历
        │                      // 重复定时器：复制 task，以 now+interval 同 id 重新入堆
        ▼
if (posted==0 && fired==0 && 无待处理工作) DispatchIdle()
        │
        ▼
wait = ComputeWaitMs(now)      // 堆顶 deadline - now；无定时器 → INFINITE；已到期 → 0
Waker::Wait(wait)              // 可警报等待；被 SetEvent / 超时 / APC 唤醒
```

`Stop()` 只做 `stop_requested_=true` + `Wake()`；`Run()` 退出前 `DispatchPostedTasks()` 排空剩余任务，再置 `kStopped`。

**跨线程投递定时器的时序**（关键设计）：
```
[任意线程] PostDelayedTask(50, task)
   ├─ id = next_timer_id_.fetch_add(1)        // 同步拿到 id，供 CancelTimer 用
   ├─ deadline = clock_.NowMs() + 50          // 只做加法，不碰 heap
   ├─ tasks_.Post( [=]{ timers_.Add(id,deadline,0,task); } )  // 控制任务
   └─ waker_.Wake()
[loop 线程] 下一轮 DispatchPostedTasks() 执行该 lambda → 真正入堆
```
这样 `TimerHeap` 永远只有一个线程碰，无需加锁（`B1`）。取消同理，`CancelTimer(id)` 投一个 `timers_.Cancel(id)` 任务。

### 3.3 用到的 Win32 API

| API | 用途 |
|---|---|
| `QueryPerformanceFrequency` / `QueryPerformanceCounter` | `SystemClock` 单调毫秒时钟；频率构造时缓存，`NowMs()` 是纯算术 |
| `CreateEventW` / `SetEvent` / `CloseHandle` | `Waker`：auto-reset、初始无信号 |
| `WaitForSingleObjectEx(event, timeout, TRUE)` | 可警报等待；`bAlertable=TRUE` 为阶段 5 APC 预留 |
| `InitializeSRWLock` / `AcquireSRWLockExclusive` / `Shared` / `ReleaseSRWLock*` | `TaskQueue` 的无递归轻量锁 |

> 阶段 1 只用到 `kernel32`，MSVC 默认链接，无需显式 `target_link_libraries`。

### 3.4 遇到的坑与处理（真实发生）

| 坑 | 现象 | 处理 |
|---|---|---|
| **枚举类型无法流式打印** | 测试里 `CHECK_EQ(loop.State(), LoopState::kStopped)` 编译失败：`Str<T>()` 里 `ostringstream << enum class` 无匹配 | `Str()` 用 `if constexpr (std::is_enum_v<T>)` 转 `underlying_type_t` 再输出 |
| **idle 语义偏差**（唯一一次测试失败） | 首轮 `SetIdleTask` 本身是控制任务，分发后队列即空，误判为「空闲」导致多跑一次 idle（14/15 → 3 个断言失败） | 让 `DispatchPostedTasks` / `DispatchDueTimers` 返回执行数量；只有 `posted==0 && fired==0 && 无待处理工作` 才跑 idle。改后 15/15 |
| **重复定时器 task 被 move 走** | 初版在 `PopDue` 里先 move 出 task 才发现要重排，写了一段「Add 空 task 再 Cancel 回滚」的丑代码 | 重排前先 `Task next = n.task;` 复制一份（`std::function` 可拷贝），再 `Add(同 id, now+interval, ..., std::move(next))` |
| **auto-reset event 的「合并唤醒」** | 多次 `SetEvent` 可能只唤醒一次 | 这是期望行为：loop 醒来会 `Drain()` 整个队列，一次唤醒处理所有任务。文档化而非改逻辑 |
| **MSVC 输出噪音** | 编译时刷屏 `注意: 包含文件:`（`/showIncludes`） | 与本阶段代码无关，来自 MSVC/CMake 环境；清 `CL`/`_CL_` 环境变量、构建输出重定向到文件后不影响结果 |
| **工具链不在 PATH** | `cl` / `cmake` / `ninja` 直接调用找不到 | 用 `vcvars64.bat` 导入环境 + VS 自带 `CMake\bin\cmake.exe` 与 `Ninja\ninja.exe` 的绝对路径 |

### 3.5 线程 / 生命周期时序

```
主线程                loop 线程              worker/post 线程
  │ 构造 EventLoop(IClock&)
  │ PostDelayedTask ─────────────────────────► (可从任意线程)
  │ Run() ─────────► 进入循环
  │                    │ Wait(INFINITE)
  │ PostTask ─────────┼────────────────────────► SetEvent 唤醒
  │                    │◄── WAIT_OBJECT_0
  │                    │ Drain → 执行任务
  │                    │ 若有定时器：wait=deadline-now
  │                    │ WAIT_TIMEOUT → PopDue → 执行
  │ Stop() ───────────┼────────────────────────► SetEvent 唤醒
  │                    │ 检查 stop_requested_ → 退出循环
  │                    │ DispatchPostedTasks() 排空
  │                    │ state = kStopped
  │◄───────────────────┘ Run() 返回
```

`Waker` 在阶段 3 会被 IOCP 的 completion key 取代，但「跨线程只投递 + 唤醒 loop」的契约不变；阶段 4 的 worker 线程池直接复用 `PostTask` 回投结果。

---

## 四、验证（出口条件核对）

| 出口条件 | 是否达成 | 验证方式 |
|---|---|---|
| 定时器精度 | 是 | `fakeclock_timer_fires_at_exact_deadline`：FakeClock 在 1019 不触发、1020/1035/1050 精确触发；`realtime_timer_fires_and_stops`：真实 30ms 定时器 + demo 的 100ms 心跳 |
| 定时器排序 | 是 | `timer_heap_orders_by_deadline`（乱序入堆按 deadline 出）、`timer_heap_same_deadline_fifo`（同刻 FIFO）、demo 中 50/150/250ms 依次触发 |
| postTask 顺序 | 是 | `posttask_order_is_fifo`（10 个任务保序）、`posttask_runs_before_timers_of_same_iteration`（投递先于定时器） |
| 取消 / 重复 / 删除堆 | 是 | `timer_heap_cancel_removes_and_keeps_heap_valid`、`timer_heap_repeating_keeps_same_id`、`cancel_timer_prevents_fire`、`repeating_timer_fires_each_interval` |
| 跨线程唤醒（Waker） | 是 | `cross_thread_posttask_wakes_blocked_loop`、demo 中 worker 线程 120ms 投递 |
| 优雅停止与排空 | 是 | `stop_drains_pending_posted_tasks`、`realtime_timer_fires_and_stops`、demo `state=kStopped` |
| idle 语义 | 是 | `idle_task_runs_only_when_nothing_pending` |
| headless demo 可跑 | 是 | `loop_demo.exe` 输出符合预期，exit 0 |

**构建命令与结果**：`cmake -S . -B build -G Ninja` + `cmake --build build` → exit 0；`test_event_loop.exe` → `15/15 passed, 0 failure(s)`；`ctest` → 1/1。

---

## 五、决策与取舍（偏离约束时必填）

| 约束编号 | 偏离 | 理由 | 是否修订约束文件 |
|---|---|---|---|
| 无 | 无 | 阶段 1 严格遵守 `01-约束.md` B/C/F 各条 | — |

> 说明两处「提前实现」，属**主动预留**而非偏离：
> 1. `Waker::Wait` 用 `WaitForSingleObjectEx(..., bAlertable=TRUE)`：为阶段 5 的 APC 对照实验留口子，当前并无 APC 逻辑。
> 2. `LoopState` 已定义 `kStopping` 等状态：为阶段 3 的 close 状态机预留，阶段 1 只用到 `kIdle/kRunning/kStopped`。

---

## 六、遗留 / 下一阶段衔接

**遗留：**
- `TimerHeap::Cancel` 是 O(n) 线性定位；阶段 1 规模足够，定时器变多时可加 `id -> index` 的 `unordered_map`，对外接口不变。
- 重复定时器重排基准是「触发时的 now」，因此卡顿后不会补跑多次（这是有意选择，已在代码注释说明）。
- `/showIncludes` 编译噪音来自本机 MSVC/CMake 环境，未查明根因，不影响构建；后续若持续可在 CMake 里显式处理。
- 心跳存在 +1ms 抖动（`WaitForSingleObjectEx` 超时 + 线程调度），这是真实事件循环的固有特征，阶段 2 引入渲染 tick 时需要考虑。

**下一阶段（阶段 2：输入与渲染）需要本阶段提供的接口：**
- `EventLoop::PostTask` —— 输入线程解析出按键后投递。
- `EventLoop::PostRepeatingTask` —— 渲染 tick（按固定间隔合并刷新）。
- `EventLoop::Stop()` —— 退出键触发干净退出。
- `IClock` —— 渲染节流与帧率统计可复用。

**需要提前注意的坑：**
- 输入线程会阻塞在 `ReadConsoleInput`，它投递任务时必须走 `PostTask`（已具备 Waker 唤醒），**不要**直接碰渲染状态（约束 B7/B1）。
- 阶段 3 把 `Waker` 换成 IOCP completion key 时，`RunIteration` 的「分发 → 计算 timeout → 等待」结构保持不变，只需替换等待后端。
- `RunOneIterationForTest()` 与 `Run()` 不要混用；测试继续用 FakeClock + 前者。
