#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tui/io/event_source.h"
#include "tui/io/io_context.h"
#include "tui/io/winsock_platform.h"

// 阶段 4：TcpSource —— Winsock overlapped TCP（约束 C7）
//
// 关键设计（阶段 3 遗留的「全双工」要求）：
//   每个连接持有**读/写各一个独立的 IoContext**，两个 overlapped 操作
//   绝不共用同一个 OVERLAPPED，否则内核状态会互相覆盖。
//
// 形态：一个 loopback 连接。Start() 阶段同步建立一对已连接的 TCP socket
//   （listener + connect，等价于「打开文件句柄」这种一次性准备动作），
//   随后**所有数据 I/O 都走 overlapped WSARecv / WSASend + IOCP**。
//   刻意不用 AcceptEx：它要求额外的地址缓冲与扩展函数解析，会淹没本阶段
//   真正要学的「读/写分离 + 完成回投」。这点在阶段 4 文档「决策与取舍」登记。
//
// 第一版功能边界（约束 E）：只做「连上、收、发、显示」。
namespace tui::io {

using TcpDataCallback = std::function<void(const std::vector<uint8_t>& data)>;

class TcpSource final : public EventSource {
public:
    TcpSource(IocpBackend& iocp, TcpDataCallback cb);
    ~TcpSource() override;

    bool Start() override;

    // 连接的「对端」socket（测试/demo 用它发数据；不属于 IOCP 侧）。
    SOCKET PeerSocket() const { return peer_; }
    uint16_t Port() const { return port_; }

    // 全双工写路径：从 IOCP 侧发一段数据（overlapped WSASend）。
    void Send(const std::vector<uint8_t>& data);

    // 供内部 TcpWatcher 访问（避免友元模板复杂度）。
    SOCKET IoSocket() const { return sock_; }

    // 诊断：已收字节 / 已发字节。
    size_t RecvBytes() const { return recv_bytes_; }
    size_t SentBytes() const { return sent_bytes_; }

private:
    // 内部 Watcher：把 read/write 两个 ctx 纳入同一套 close 状态机（约束 C3）。
    class TcpWatcher;
    bool ArmRecv();
    bool ArmSend(const std::vector<uint8_t>& data);

    TcpDataCallback cb_;
    SOCKET listener_ = INVALID_SOCKET;
    SOCKET sock_ = INVALID_SOCKET;   // IOCP 侧（已连接）
    SOCKET peer_ = INVALID_SOCKET;   // 对端（测试/demo 用）
    uint16_t port_ = 0;
    bool wsa_started_ = false;
    size_t recv_bytes_ = 0;
    size_t sent_bytes_ = 0;
    TcpWatcher* watcher_ = nullptr;  // 指向 watchers_ 里的对象，非所有权
};

}  // namespace tui::io
