# 阶段 4：更多 source + worker 线程池

| 项 | 内容 |
|---|---|
| 阶段号 | 4 |
| 名称 | 更多 source 与线程池 |
| 日期 | 2026-11-12 |
| 对应大纲出口条件 | 子进程管道、TCP/UDP、worker 线程池；六个子系统全部可跑；backpressure 生效 |
| 关联 commit | 阶段 4 提交 |
| 状态 | 已完成 |

---

## 一、做了什么（What）

**增强阶段 3 的 I/O 抽象（阶段 3 的 13 个测试零改动、零回归）：**

| 文件 | 改动 |
|---|---|
| `include/tui/io/io_context.h` | 新增 `OpKind`（kGeneric/kRead/kWrite/kAccept）、`auto_rearm`、`on_complete` 钩子、`armed` 标志 |
| `src/io/watcher.cpp` | `OnCompletion` 支持 `on_complete` 自定义完成后处置；新增 `Watcher::ReArm(ctx)` 标准重挂；过期包判定兼容多 ctx |
| `include/tui/io/event_source.h` | `CloseAll()` 改为 `virtual`，允许派生类追加清理 |

**新增模块：**

| 文件 | 作用 |
|---|---|
| `include/tui/core/ring_buffer.h` | `RingBuffer<T>`：有上限容器，满时丢最旧 + 累计 `Dropped()`（背压可观测） |
| `include/tui/pool/thread_pool.h`、`src/pool/thread_pool.cpp` | `ThreadPool`：Win32 `SRWLOCK` + `CONDITION_VARIABLE`，worker 只做无状态计算，返回「续延」经 `post_cb` 回投 loop |
| `include/tui/io/pipe_source.h`、`src/io/pipe_source.cpp` | `PipeSource`：命名管道 + `CreateProcess` 捕获子进程 stdout + overlapped `ReadFile` |
| `include/tui/io/tcp_source.h`、`src/io/tcp_source.cpp` | `TcpSource`：Winsock `WSA_FLAG_OVERLAPPED`，**读/写各一个独立 IoContext**（全双工） |
| `include/tui/io/winsock_platform.h` | 集中 Winsock 头文件（避免 winsock.h / winsock2.h 冲突） |

**验证与工程：**

| 文件 | 作用 |
|---|---|
| `tests/test_stage4.cpp` | 7 个单元测试（含真实管道、真实 TCP 端到端） |
| `src/app/aggregate_demo.cpp` | 六子系统聚合控制台（headless） |
| `CMakeLists.txt` | 新增 `tui_pool` 库、`test_stage4`、`aggregate_demo`；`tui_io` 链接 `ws2_32` |
| `README.md` | 补充阶段 4 构建与运行说明 |

**可观察效果：**
- `ctest` → **4/4**（15 + 14 + 13 + 7 = **49 个测试**），阶段 1/2/3 零回归。
- `aggregate_demo.exe` → `beats=11 log_lines=22 dropped=0 presents=72`；
  `sources: dir=0 pipe=0 tcp=0`（全部 Reap 回收）；`tcp recv=4 sent=4`；`state=3(stopped)`。
- `test_stage4`：`pipe_source_captures_child_stdout` 与 `tcp_source_full_duplex_recv_and_send` 真实通过，非跳过。

---

## 二、为什么这么做（Why）

**动机**：阶段 3 只有一个目录源、一个 `IoContext`、一个完成处置路径。阶段 4 要证明这套抽象能扩展到「多种 I/O 源 + 全双工 + 额外线程池」，同时不破坏已有的 close 状态机与差分渲染。

**对应约束编号：**
- `B3`/`B4` worker 只做无状态计算、不持有 loop 状态指针、结果 post 回 loop → `ComputeTask` 返回「续延」`core::Task`，worker 经 `post_cb`（接 `EventLoop::PostTask`）回投，续延才在 loop 线程用 `log()`。
- `C7` overlapped + IOCP → 管道与 socket 都 `FILE_FLAG_OVERLAPPED` / `WSA_FLAG_OVERLAPPED` + `Associate()`。
- `C2` 每操作独立上下文 → 全双工**读/写各一个 `IoContext`**（阶段 3 遗留要求）。
- `C3`/`C4` close 状态机 → 两个 ctx 的 `inflight_` 统一计数，`Close()` 等两个取消包都回来才 `kClosed`。
- `D4`/大纲阶段 4 出口条件 → `RingBuffer` 背压 + `Renderer::RenderTick` 按 tick 合并。
- `E` 第一版功能最简 → 管道/socket 只做「连上、收、发、显示」，不做协议/hex 编辑器。
- `A1` → Win32 原生 `CONDITION_VARIABLE`/`SRWLOCK`，零第三方依赖。
- `F4`/`F5` → 决策登记（见第五节）+ 7 个测试 + 真实端到端 + demo。

**被放弃的方案及原因：**

| 放弃 | 原因 |
|---|---|
| `CreatePipe` 匿名管道 + overlapped | **Windows 匿名管道不支持 overlapped I/O**（MSDN: Anonymous Pipe Operations），无法关联 IOCP。改用 `CreateNamedPipe(FILE_FLAG_OVERLAPPED)` |
| `AcceptEx` 接受 TCP 连接 | 需要额外地址缓冲 + `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)` 解析扩展函数，会淹没本阶段「读/写分离 + 完成回投」的主题。改为 `Start()` 一次性同步建立 loopback socket 对（等价「打开文件句柄」），**所有数据 I/O 仍走 overlapped**。已在第五节登记 |
| overlapped `ConnectNamedPipe` | 它的完成包 `lpOverlapped` 指向非 `IoContext` 的 `OVERLAPPED`，会被 `IocpBackend::Dispatch` 误解释成 context（真实坑）。客户端 `CreateFileW` 已同步连上，改用同步 `ConnectNamedPipe` 并接受 `ERROR_PIPE_CONNECTED` |
| worker 直接 `Renderer::Put` / 碰 `Watcher` 状态 | 违反 B1/B3/B4。改为「计算 → 续延 → `PostTask` → loop 线程执行」 |
| 单一全局 `generation_` 判定所有 ctx 是否过期 | 全双工下读 ctx 的 `ReArm` 会递增 `generation_`，导致**写 ctx 的完成包被误判为过期丢弃**（实测 `sent=0`）。改为 per-ctx `armed` 标志 |
| 无上限日志 deque | 高吞吐下无限增长；`RingBuffer` 满时丢最旧并累计 `Dropped()`，让背压可观测 |

**这个决定让哪件事变得可见：**
1. **「同一套 close 状态机如何扩展到全双工」**：`inflight_` 不区分读写，天然要求「两个取消包都排空」才能释放 —— 这是阶段 3 设计正确性的最好证明。
2. **worker 线程池的正确边界**：不是「worker 直接更新 UI」，而是「算完把续延投回 loop」。续延在 loop 线程执行，所以能安全 `log()`；worker 全程不碰状态。
3. **Windows 系统编程的"暗坑"**：匿名管道不支持 overlapped、命名管道连接的完成包陷阱、socket `CancelIoEx` 的取消语义 —— 这些不亲手撞一次记不住。

---

## 三、怎么做的（How）

### 3.1 关键数据结构

**`IoContext` 阶段 4 增量**
```cpp
OpKind op_kind;        // 读/写分流（TcpWatcher::Arm 按它选 WSARecv / WSASend）
bool auto_rearm;       // 默认 true（阶段 3 行为）；全双工读写设 false
bool armed;            // per-ctx：BeginOperation 置位、OnCompletion 清除
on_complete(ctx, bytes, error) -> bool;  // 返回 true = source 已自行重挂，Watcher 不再 auto_rearm
```

**`Watcher::ReArm(ctx)`**（阶段 4 新增）
```
++generation_ → ctx.ResetForReuse() → ctx.generation = generation_ → BeginOperation(ctx)
```
把「重新挂起」的 generation / inflight 维护收拢到一处，source 不用自己碰计数。

**`ThreadPool`**
```
SubmitCompute(ComputeTask) → queue_（SRWLOCK 保护）
worker: SleepConditionVariableSRW 等待 → 取任务 → 执行 task() 得到续延 core::Task
        → post_cb_(续延)   // 典型接 EventLoop::PostTask → PostQCS(kKeyWake) 唤醒 loop
Shutdown(): stopping_=true → WakeAllConditionVariable → join 所有 worker
```

**`TcpSource` 全双工**
```
一个 Watcher（TcpWatcher）持有两个 IoContext：
  read_ctx_  : op_kind=kRead,  auto_rearm=false, on_complete=收数据+ReArm重挂读
  write_ctx_ : op_kind=kWrite, auto_rearm=false, on_complete=累计 sent_bytes_
Arm(ctx) 按 op_kind 选 WSARecv / WSASend（都是 overlapped，关联同一 IOCP）
```

### 3.2 核心流程 / 调用链

```
[EventLoop(IocpBackend)]  ← 一个 completion port 等所有源
   │
   ├─ DirSource   ReadDirectoryChangesW → completion → 日志
   ├─ PipeSource  CreateNamedPipe + CreateProcess → overlapped ReadFile → completion
   ├─ TcpSource   WSARecv(read ctx) / WSASend(write ctx) → 各自 completion
   ├─ 定时器      心跳 / 渲染 tick（PostRepeatingTask）
   ├─ PostTask    worker 续延回投（kKeyWake 唤醒）
   └─ ThreadPool  worker 算 hex → 续延 → PostTask → loop 线程 log()

[backpressure]
   source 数据 → RingBuffer<wstring>(256) 满则丢最旧 + Dropped++
   Renderer::RenderTick(16ms) → 只画可见行 → dirty-cell 差分 → Present

[优雅关闭，约束 C4]
   t=900ms  dsrc/psrc/tsrc.CloseAll() → CancelIoEx → 各 watcher kClosing
            取消包经 IOCP 排空 → OnCompletion → inflight 归零 → kClosed
   t=1200ms dsrc/psrc/tsrc.Reap() → delete 已 kClosed watchers
            pool.Shutdown() → join worker（先停 worker 避免再 PostTask）
            loop.Stop()
```

### 3.3 用到的 Win32 API

| API | 用途 |
|---|---|
| `SRWLOCK` / `CONDITION_VARIABLE` / `SleepConditionVariableSRW` / `WakeAllConditionVariable` | ThreadPool 的等待与唤醒（约束 A1） |
| `CreateNamedPipeW(FILE_FLAG_OVERLAPPED)` / `ConnectNamedPipe` | 子进程 stdout 管道服务端 |
| `CreateFileW(..., FILE_ATTRIBUTE_NORMAL, 可继承)` | 管道客户端写端（给子进程当 stdout） |
| `CreateProcessW(STARTUPINFOW.hStdOutput, bInheritHandles=TRUE)` | 启动子进程并重定向 stdout |
| `ReadFile(..., &ov)` | overlapped 管道读 |
| `WSAStartup` / `WSASocketW(WSA_FLAG_OVERLAPPED)` / `bind` / `listen` / `WSAConnect` / `WSAAccept` | loopback TCP 连接建立（一次性准备） |
| `WSARecv` / `WSASend`（overlapped） | 全双工数据 I/O |
| `WSAGetLastError() == WSA_IO_PENDING` | overlapped 提交成功的判定 |
| `CancelIoEx` | 关闭时取消读/写两个挂起操作 |

### 3.4 遇到的坑与处理（真实发生）

| 坑 | 现象 | 处理 |
|---|---|---|
| **匿名管道不支持 overlapped** | 设计推演：`CreatePipe` 句柄无法关联 IOCP | 改用 `CreateNamedPipe(FILE_FLAG_OVERLAPPED)`；文档登记为 Win32 事实 |
| **overlapped ConnectNamedPipe 的完成包陷阱** | 其 `lpOverlapped` 指向非 `IoContext`，会被 `Dispatch` 用 `ContextFromOverlapped` 误解释 | 客户端已同步连上，改用同步 `ConnectNamedPipe` 并接受 `ERROR_PIPE_CONNECTED` |
| **多 ctx 共享全局 `generation_` 误判过期** | `aggregate_demo` 首跑 `sent=0`、`tcp=1`（写完成包被丢弃、watcher 没回收） | 引入 per-ctx `armed`；`OnCompletion` 只在「owner 是本 watcher 且 armed==false 且 kGeneric」时按 generation 丢弃。修后 `sent=4`、`tcp=0` |
| **`Watcher::OnCompletion` 无条件 auto_rearm** | 全双工读要重挂、写不重挂，单一路径不够 | 加 `on_complete` 钩子 + `auto_rearm`；`ReArm()` 收拢重挂逻辑 |
| **`EventSource::CloseAll` 改 virtual 后的 LNK2019** | 把 `CloseAll` 改成 `virtual`（`UEAA` 符号），但 build 里残留旧 obj（`QEAA` 符号）→ 链接找不到 | 删除整个 `build/` 全量重编译；符号一致后 4/4 通过 |
| **`ThreadPool` const 成员函数取 SRWLOCK** | `Pending()/Completed()` 是 const，`AcquireSRWLockShared` 要非 const 指针 → `C2664` | `mutable SRWLOCK lock_` |
| **测试续延 lambda 捕获** | 内层续延用 `&applied` 但外层 lambda 没捕获 → `C3493/C2326` | 外层补 `&applied`，续延用 `[applied_ptr=&applied, value]` 传值 |
| **worker 不能持有 loop 局部 `log` lambda** | 「续延在 loop 线程执行」所以**可以**捕获 `&log`；初版误以为不行，写了「worker 算 hex + loop 再算一次」的冗余代码 | 精简为：worker 算 hex 返回续延，续延在 loop 线程 `log(tag + hex)` |

### 3.5 线程 / 生命周期时序

```
主线程(loop)        worker 线程        内核/IOCP         子进程
  │ ThreadPool(4) ──► SleepCV 等待
  │ DirSource/PipeSource/TcpSource Start（提交 overlapped 操作）
  │ CreateProcess ─────────────────────────────────────► stdout 写管道
  │ EventLoop::Run ──► GQCSEx 阻塞
  │                                  文件/管道/socket 事件 ──► completion
  │   ◄─────────────────────────── Dispatch → 各 source 回调 → log() 入 RingBuffer
  │   worker: PostTask(续延) ─────► kKeyWake 包 → loop 唤醒 → 续延执行 log()
  │   16ms tick: RenderTick → 差分帧
  │   t=120ms: peer send(PING) + tsrc.Send(PONG)（全双工 WSARecv/WSASend）
  │   t=900ms: CloseAll → CancelIoEx（读+写两个 ctx）
  │   ◄─────────────────────────── 两个取消包 → inflight 2→1→0 → kClosed
  │   t=1200ms: Reap×3 → delete；pool.Shutdown → join；loop.Stop()
  ▼ state=kStopped，sources 全为 0
```

---

## 四、验证（出口条件核对）

| 出口条件 | 是否达成 | 验证方式 |
|---|---|---|
| 子进程管道 | 是 | `pipe_source_captures_child_stdout`：真实 `cmd.exe /C echo` + 命名管道 + 捕获 `HELLO_STAGE4` |
| TCP 全双工 | 是 | `tcp_source_full_duplex_recv_and_send`：真实 loopback，`WSARecv` 收 PING、`WSASend` 发 ECHO；`aggregate_demo` `recv=4 sent=4` |
| worker 线程池 | 是 | `threadpool_computes_and_posts_continuation_to_loop`：8 个计算任务，续延在 loop 线程累加结果（0+1+4+...+49=140） |
| 六个子系统全部可跑 | 是 | `aggregate_demo`：dir/pipe/tcp/timer/task/pool 同时挂在一个 IOCP loop，`state=3` 退出 |
| backpressure 生效 | 是 | `ringbuffer_drops_oldest_when_full`（满时丢最旧 + Dropped++）、`ringbuffer_pop_and_zero_capacity`；demo `dropped=0` 且 `presents=72` 体现 tick 合并 |
| 多 ctx 全双工 + 关闭不崩溃 | 是 | 修复 `armed` 后 `aggregate_demo` `tcp=0`（全部 Reap）；阶段 3 的 13 个测试零回归 |
| Watcher close 状态机扩展 | 是 | `TcpWatcher` 两个 ctx 共用 `inflight_`，两个取消包都到齐才 `kClosed` |
| 阶段 1/2/3 零回归 | 是 | `ctest` 4/4（15+14+13+7 = 49 个测试） |

**构建与结果**：clean `cmake --build build` → exit 0；`ctest` → 100% (4/4)；`aggregate_demo.exe` → exit 0。

---

## 五、决策与取舍（偏离约束时必填）

| 约束编号 | 偏离 | 理由 | 是否修订约束文件 |
|---|---|---|---|
| `C7`（"所有 I/O 句柄以 overlapped 方式打开"） | `TcpSource::Start()` 用同步 `WSAAccept`/`WSAConnect` 建立连接；`PipeSource` 用同步 `ConnectNamedPipe` | 这些是**一次性连接建立**（等价打开文件句柄），不是事件循环里的数据 I/O；所有数据传输仍走 overlapped + IOCP。避免 `AcceptEx` 扩展函数样板代码淹没本阶段主题 | 否（属实现细化，不改约束） |
| 大纲 S5 的"UDP" | 阶段 4 只做了 TCP，未做 UDP | `TcpSource` 已覆盖 `WSA_FLAG_OVERLAPPED` + IOCP + 全双工这些核心知识点；UDP 无连接、`WSARecvFrom` 语义不同，留到阶段 6 capstone 与串口一起补 | 否（大纲阶段 4 出口条以「六个子系统可跑」为准，UDP 归 capstone） |

> **主动补充（非偏离）**：
> 1. `IoContext` 的 `OpKind` / `auto_rearm` / `on_complete` / `armed` 是阶段 3 抽象的自然扩展，默认值保持阶段 3 行为（13 个测试零改动）。
> 2. `Watcher::ReArm()` 把重挂逻辑收拢，供全双工读完成后使用。
> 3. `EventSource::CloseAll()` 改为 `virtual`，让 `PipeSource` 能追加子进程清理。

---

## 六、遗留 / 下一阶段衔接

**遗留：**
- **UDP 未实现**（见第五节），留到阶段 6 capstone。
- `PipeSource` 未把 `ConnectNamedPipe` 的 overlapped 路径纳入 IOCP（当前同步连接，见 3.4 坑）；若要监听多个动态连接需补 `AcceptEx`-式处理。
- `TcpSource` 的 `WSASend` 是「一次性发」：连续 `Send()` 会覆盖同一个 `write_ctx_`。生产级需要写队列，阶段 6 补。
- `aggregate_demo` 里 `submit_hex` 的续延捕获 `&log`（main 局部），依赖 `loop.Run()` 同步执行；若改成后台 loop 需改值捕获。
- `ThreadPool::Shutdown()` 会先置 `stopping_`，已入队但未取出的任务会被丢弃；当前 demo 在 shutdown 前已等 worker 完成。
- Winsock 错误分类（`WSAECONNRESET` 等）尚未细分，`OnCompletion` 统一按 error!=0 跳过。

**下一阶段（阶段 5：双后端对照）需要本阶段提供的接口：**
- `IWaitBackend` 抽象 + `EventLoop(IClock&, unique_ptr<IWaitBackend>)` —— 注入 `WaitForMultipleObjectsEx` 后端即可对照。
- 阶段 4 的多个 source 都绑定到 `IocpBackend::Handle()`；WFMO 后端需要把每个句柄加进 wait 数组，正好撞上 **64 句柄上限**（大纲阶段 5 的学习点）。
- `EventSource` / `Watcher` 的 close 状态机与后端无关，可复用。

**需要提前注意的坑：**
- WFMO 后端下，`ReadDirectoryChangesW` 等 overlapped 操作的完成不能用 `WaitForMultipleObjects` 直接等（那是等句柄信号，不是等 completion）——这正是 C1 要你亲手体会的差异。
- 阶段 5 实现时**不要**改 `EventLoop::RunIteration` 的「Dispatch → tasks → timers → idle → Wait」结构，只替换 `IWaitBackend`。
- 阶段 6 capstone 加串口（`CreateFile("COMx")` + DCB + `WaitCommEvent`）时，直接派生 `EventSource`/`Watcher` 即可，六子系统骨架已就位。
