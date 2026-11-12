#include "tui/io/tcp_source.h"

#include <algorithm>

#include "tui/io/iocp_backend.h"

namespace tui::io {

// TcpWatcher：把「读 ctx」和「写 ctx」都挂在同一个 Watcher 上（全双工）。
// inflight_ 同时统计两个 ctx；Close() 时两个取消完成包都回来才 kClosed。
class TcpSource::TcpWatcher final : public Watcher {
public:
    TcpWatcher(IocpBackend& iocp, SOCKET s, TcpSource& owner)
        : Watcher(iocp, reinterpret_cast<HANDLE>(s), nullptr), owner_(owner) {}

    bool StartRead() {
        read_ctx_.op_kind = OpKind::kRead;
        return BeginOperation(read_ctx_);
    }

    bool StartWrite(const std::vector<uint8_t>& data) {
        write_ctx_.buffer.assign(data.begin(), data.end());
        write_ctx_.op_kind = OpKind::kWrite;
        return BeginOperation(write_ctx_);
    }

    IoContext& ReadCtx() { return read_ctx_; }
    IoContext& WriteCtx() { return write_ctx_; }

protected:
    // 由 op_kind 分流读/写（约束 C7：overlapped WSARecv / WSASend）。
    bool Arm(IoContext& ctx) override {
        SOCKET s = owner_.IoSocket();
        if (s == INVALID_SOCKET) return false;
        ctx.callback = [this, &ctx](uint32_t b, uint32_t e) {
            OnCompletion(ctx, b, e);
        };
        if (ctx.op_kind == OpKind::kRead) {
            ctx.buffer.resize(4096);
            WSABUF buf{static_cast<ULONG>(ctx.buffer.size()),
                       reinterpret_cast<char*>(ctx.buffer.data())};
            DWORD flags = 0, received = 0;
            int r = WSARecv(s, &buf, 1, &received, &flags, &ctx.ov, nullptr);
            if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                ctx.callback = {};
                return false;
            }
        } else {
            WSABUF buf{static_cast<ULONG>(ctx.buffer.size()),
                       reinterpret_cast<char*>(ctx.buffer.data())};
            DWORD sent = 0;
            int r = WSASend(s, &buf, 1, &sent, 0, &ctx.ov, nullptr);
            if (r == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                ctx.callback = {};
                return false;
            }
        }
        return true;
    }

    void Notify(uint32_t bytes, uint32_t error,
                const std::vector<uint8_t>& data) override {
        // 由 on_complete 钩子按 op_kind 精确分流，这里不重复解析。
    }

private:
    TcpSource& owner_;
    IoContext read_ctx_;
    IoContext write_ctx_;
};

TcpSource::TcpSource(IocpBackend& iocp, TcpDataCallback cb)
    : EventSource(iocp), cb_(std::move(cb)) {}

TcpSource::~TcpSource() {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
    if (peer_ != INVALID_SOCKET) closesocket(peer_);
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    if (wsa_started_) WSACleanup();
}

bool TcpSource::Start() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsa_started_ = true;

    // 一次性同步准备：建 listener + connect/accept 出一对已连接 socket。
    // 等价于「打开文件句柄」，不属于事件循环里的 I/O 分发。
    listener_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                           WSA_FLAG_OVERLAPPED);
    peer_ = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                       WSA_FLAG_OVERLAPPED);
    if (listener_ == INVALID_SOCKET || peer_ == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        return false;
    int len = sizeof(addr);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR)
        return false;
    port_ = ntohs(addr.sin_port);
    if (listen(listener_, 1) == SOCKET_ERROR) return false;

    if (WSAConnect(peer_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr),
                   nullptr, nullptr, nullptr, nullptr) == SOCKET_ERROR)
        return false;
    SOCKET accepted = WSAAccept(listener_, nullptr, nullptr, nullptr, 0);
    if (accepted == INVALID_SOCKET) return false;
    sock_ = accepted;  // IOCP 侧（服务端）；peer_ 是客户端（测试/demo 发数据）

    iocp_.Associate(reinterpret_cast<HANDLE>(sock_), kKeyWatcher);

    auto w = std::make_unique<TcpWatcher>(iocp_, sock_, *this);
    watcher_ = w.get();
    watchers_.push_back(std::move(w));

    // 读完成：把数据交给回调，并用 ReArm 重新挂读（读永远在监听）。
    auto& rctx = watcher_->ReadCtx();
    rctx.on_complete = [this](IoContext& ctx, uint32_t bytes, uint32_t error) -> bool {
        if (error == 0 && bytes > 0) {
            recv_bytes_ += bytes;
            std::vector<uint8_t> chunk(
                ctx.buffer.begin(),
                ctx.buffer.begin() + std::min<size_t>(bytes, ctx.buffer.size()));
            if (cb_) cb_(chunk);
        }
        watcher_->ReArm(ctx);  // 正确维护 generation / inflight
        return true;           // 已自行处理，Watcher 不再 auto_rearm
    };
    rctx.auto_rearm = false;

    // 写完成：只累计字节数，不重挂（一次性发送）。
    auto& wctx = watcher_->WriteCtx();
    wctx.on_complete = [this](IoContext& ctx, uint32_t bytes, uint32_t error) -> bool {
        (void)ctx;
        if (error == 0) sent_bytes_ += bytes;
        return true;
    };
    wctx.auto_rearm = false;

    return watcher_->StartRead();
}

bool TcpSource::ArmRecv() { return watcher_ ? watcher_->ReArm(watcher_->ReadCtx()) : false; }

bool TcpSource::ArmSend(const std::vector<uint8_t>& data) {
    return watcher_ ? watcher_->StartWrite(data) : false;
}

void TcpSource::Send(const std::vector<uint8_t>& data) { ArmSend(data); }

}  // namespace tui::io
