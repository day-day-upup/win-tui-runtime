# win-tui-runtime

用**纯 C++ 与 Windows API** 从零实现的一套事件驱动 TUI 运行时：
自己实现终端输入、事件分发、定时器、任务调度、I/O 通知与界面刷新。

本仓库的首要目标是**学习事件循环与 Windows 系统编程**，而不是快速产出一个工具。
因此不依赖 libuv / asio / ncurses 等任何「替你实现事件循环」的框架。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/00-大纲.md](docs/00-大纲.md) | 总纲：目标、架构、里程碑、验收标准 |
| [docs/01-约束.md](docs/01-约束.md) | 设计约束（不可逾越的规则） |
| [docs/阶段文档模板.md](docs/阶段文档模板.md) | 每阶段记录的统一格式 |
| [docs/stages/](docs/stages/) | 各阶段实施记录（做了什么 / 为什么 / 怎么做） |

## 三项核心决策

1. **控制台**：legacy `ReadConsoleInput` + `WriteConsoleOutput` 为主路径，VT/ANSI 仅作可选后端。
2. **线程**：主循环单线程独占状态；I/O 线程只投递 completion；额外 worker 线程池只做无状态计算。
3. **范围**：第一版只做「连上、收发、显示」；hex 编辑、脚本、日志落盘、IME 等一律延后。

## 里程碑

| 阶段 | 名称 |
|---|---|
| 0 | 仓库与规范 ✅ |
| 1 | Loop 骨架（EventLoop / 任务队列 / 定时器堆 / 唤醒） |
| 2 | 输入与渲染（输入线程 / 差分渲染 / 按键查看器） |
| 3 | I/O 后端（Overlapped / IOCP / close 状态机） |
| 4 | 更多 source + worker 线程池（管道 / 目录 / socket） |
| 5 | 双后端对照（`WaitForMultipleObjectsEx` / 64 句柄上限 / APC） |
| 6 | Capstone（串口网络助手 / 聚合控制台） |

## 构建

C++20 / MSVC / CMake；仅链接 Windows 系统库。具体构建方式在阶段 1 落地后补充。

## 许可

MIT
