#include "tui/io/pipe_source.h"

#include <algorithm>
#include <cstdio>

#include "tui/io/iocp_backend.h"

namespace tui::io {

// ---- PipeWatcher -----------------------------------------------------------

PipeWatcher::PipeWatcher(IocpBackend& iocp, HANDLE pipe, PipeDataCallback cb)
    : Watcher(iocp, pipe, nullptr), data_cb_(std::move(cb)) {}

PipeWatcher::~PipeWatcher() = default;

bool PipeWatcher::Start() { return BeginOperation(ctx_); }

bool PipeWatcher::Arm(IoContext& ctx) {
    if (!handle_ || handle_ == INVALID_HANDLE_VALUE) return false;
    ctx.buffer.resize(4096);
    ctx.op_kind = OpKind::kRead;
    ctx.callback = [this, &ctx](uint32_t b, uint32_t e) {
        OnCompletion(ctx, b, e);
    };
    // 读完成后的处置：把数据交给回调，并重新挂读（auto_rearm 会处理）。
    // 这里返回 false，让 Watcher 走默认的「ResetForReuse + BeginOperation」。
    ctx.on_complete = nullptr;
    ctx.auto_rearm = true;

    DWORD read = 0;
    BOOL ok = ReadFile(handle_, ctx.buffer.data(),
                       static_cast<DWORD>(ctx.buffer.size()), &read, &ctx.ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        ctx.callback = {};
        return false;
    }
    ctx.generation = generation_;
    return true;
}

void PipeWatcher::Notify(uint32_t bytes, uint32_t error,
                         const std::vector<uint8_t>& data) {
    if (error != 0 && error != ERROR_BROKEN_PIPE) return;
    if (bytes == 0) return;  // EOF：子进程关闭了管道
    std::vector<uint8_t> chunk(data.begin(), data.begin() + std::min<size_t>(bytes, data.size()));
    if (data_cb_) data_cb_(chunk);
}

// ---- PipeSource ------------------------------------------------------------

PipeSource::PipeSource(IocpBackend& iocp, const std::wstring& command,
                       PipeDataCallback cb)
    : EventSource(iocp), command_(command), cb_(std::move(cb)) {
    // 用唯一名字避免与系统里其它管道冲突。
    wchar_t buf[128];
    swprintf(buf, 128, L"\\\\.\\pipe\\tui_stage4_%lu", GetCurrentProcessId());
    pipe_name_ = buf;
}

PipeSource::~PipeSource() {
    if (process_ && process_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(process_, 1);
        CloseHandle(process_);
    }
    if (thread_ && thread_ != INVALID_HANDLE_VALUE) CloseHandle(thread_);
    if (pipe_ && pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
}

bool PipeSource::Start() {
    // 1) 建 overlapped 命名管道服务端（单实例、字节流）。
    pipe_ = CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    iocp_.Associate(pipe_, kKeyWatcher);

    // 2) 客户端写端：可继承句柄，给子进程当 stdout。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE client = CreateFileW(pipe_name_.c_str(), GENERIC_WRITE, 0, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (client == INVALID_HANDLE_VALUE) return false;

    // 3) CreateProcess：把 client 作为 stdout/stderr。
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = client;
    si.hStdError = client;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                             /*bInheritHandles=*/TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // 父进程不再需要 client 写端；关掉后，子进程退出时服务端会收到 EOF。
    CloseHandle(client);
    if (!ok) return false;

    process_ = pi.hProcess;
    thread_ = pi.hThread;

    // 4) 连接状态：客户端 CreateFileW 已同步连上服务端，因此
    // ConnectNamedPipe 此刻通常返回 FALSE + ERROR_PIPE_CONNECTED。
    // 这里刻意**不使用 overlapped ConnectNamedPipe**：那样会产生一个
    // lpOverlapped 指向非 IoContext 的完成包，被 Dispatch 误解释（真实坑）。
    // 用同步 ConnectNamedPipe 并接受「已连接」即可，连接建立不占用 inflight。
    BOOL conn = ConnectNamedPipe(pipe_, nullptr);
    if (!conn) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) return false;
    }

    // 5) 建立 PipeWatcher 并发起第一次 overlapped ReadFile。
    auto w = std::make_unique<PipeWatcher>(iocp_, pipe_, cb_);
    watchers_.push_back(std::move(w));
    auto* raw = static_cast<PipeWatcher*>(watchers_.back().get());
    return raw->Start();
}

void PipeSource::CloseAll() {
    EventSource::CloseAll();
    // 子进程若还在跑，优雅起见先等一小段；真实场景可 TerminateProcess。
    // 这里不强制杀，避免测试里的 cmd 子进程变成僵尸。
}

DWORD PipeSource::ExitCode() const {
    if (!process_ || process_ == INVALID_HANDLE_VALUE) return 0xFFFFFFFF;
    DWORD code = 0;
    if (GetExitCodeProcess(process_, &code)) return code;
    return 0xFFFFFFFF;
}

}  // namespace tui::io
