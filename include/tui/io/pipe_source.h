#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tui/io/event_source.h"
#include "tui/io/io_context.h"

// 阶段 4：PipeSource —— 捕获子进程 stdout 的命名管道源
//
// 一个必须记录的 Win32 事实（阶段 4 文档 3.4 节）：
//   Windows **匿名管道不支持 overlapped I/O**（MSDN: Anonymous Pipe Operations）。
//   CreatePipe 产出的句柄无法关联 IOCP。因此这里用 CreateNamedPipe +
//   FILE_FLAG_OVERLAPPED 建服务端，子进程用 CreateFileW 打开客户端写端，
//   再用 CreateProcess 把该句柄作为 stdout 重定向。
//
// 覆盖知识点：CreateNamedPipe / ConnectNamedPipe(overlapped) /
// CreateProcess(STARTUPINFOW with hStdOutput) / 句柄继承 / overlapped ReadFile。
namespace tui::io {

using PipeDataCallback = std::function<void(const std::vector<uint8_t>& data)>;

class PipeWatcher final : public Watcher {
public:
    PipeWatcher(IocpBackend& iocp, HANDLE pipe, PipeDataCallback cb);
    ~PipeWatcher() override;

    bool Start();  // 发起第一次 overlapped ReadFile
    bool IsOpen() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

protected:
    bool Arm(IoContext& ctx) override;
    void Notify(uint32_t bytes, uint32_t error,
                const std::vector<uint8_t>& data) override;

private:
    IoContext ctx_;
    PipeDataCallback data_cb_;
};

class PipeSource final : public EventSource {
public:
    PipeSource(IocpBackend& iocp, const std::wstring& command, PipeDataCallback cb);
    ~PipeSource() override;

    bool Start() override;
    void CloseAll() override;  // 额外：终止子进程 + Reap

    HANDLE ProcessHandle() const { return process_; }
    DWORD ExitCode() const;

private:
    std::wstring command_;
    PipeDataCallback cb_;
    HANDLE pipe_ = nullptr;      // 命名管道服务端（IOCP 关联）
    HANDLE process_ = nullptr;   // 子进程
    HANDLE thread_ = nullptr;    // 子进程主线程
    std::wstring pipe_name_;
};

}  // namespace tui::io
