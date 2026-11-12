#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tui/io/event_source.h"
#include "tui/io/io_context.h"

// 阶段 6：SerialSource —— 串口事件源（capstone 的 S6）
//
// 覆盖的 Win32 知识点（大纲 1.2 S6）：
//   - CreateFileW(L"\\\\.\\COMx", ..., FILE_FLAG_OVERLAPPED) 打开串口并关联 IOCP（约束 C7）
//   - DCB + GetCommState/SetCommState 配置波特率/数据位/校验/停止位
//   - SetCommTimeouts 设成「立即返回」语义，配合 overlapped 读
//   - SetCommMask + WaitCommEvent（overlapped）作为事件驱动入口
//   - overlapped ReadFile / WriteFile 做全双工收发
//
// 为什么要 WaitCommEvent 而不是像 TCP 那样直接挂着 overlapped ReadFile：
//   TCP 的 ReadFile/WSARecv 天然「等数据到达才完成」，可以一直挂着；
//   串口的 ReadFile 在「无数据」时的行为由 SetCommTimeouts 决定，若设成
//   立即返回就会空转。所以串口的经典做法是：
//     WaitCommEvent 等「有字符到达」→ 再 ReadFile 取数据 → 重新挂 WaitCommEvent。
//   这正是串口与 socket 最本质的差别，也是本阶段要亲手体会的点。
//
// 状态机（避免双挂 / 空转）：
//   wait_armed_       : 是否有挂起的 WaitCommEvent
//   read_in_flight_   : 是否有挂起的 ReadFile
//   Pump()            : 两者都空闲时才挂 WaitCommEvent（唯一挂起点）
//   Wait 完成         : wait_armed_=false；EV_RXCHAR 且无读在飞 → 发 ReadFile
//   Read 完成         : read_in_flight_=false；交付数据 → Pump()
//
// 全双工：读路径（wait_ctx_ + read_ctx_）与写路径（write_ctx_）完全独立，
// 绝不共用同一个 OVERLAPPED（约束 C2 / 阶段 4 教训）。
namespace tui::io {

using SerialDataCallback = std::function<void(const std::vector<uint8_t>& data)>;

// 串口参数（第一版最简：8N1 + 可配置波特率，约束 E）。
struct SerialConfig {
    std::wstring port = L"COM2";  // 不含 \\.\ 前缀，由实现补
    uint32_t baud = 9600;
    uint8_t byte_size = 8;
    uint8_t parity = 0;      // 0=NOPARITY 1=ODD 2=EVEN
    uint8_t stop_bits = 0;   // 0=ONESTOPBIT 1=ONE5STOPBITS 2=TWOSTOPBITS
};

class SerialSource final : public EventSource {
public:
    SerialSource(IocpBackend& iocp, const SerialConfig& cfg, SerialDataCallback cb);
    ~SerialSource() override;

    bool Start() override;
    void CloseAll() override;  // 追加 PurgeComm（丢弃未读数据，避免关闭时残留）

    // 全双工写路径：从 IOCP 侧发一段数据（overlapped WriteFile）。
    void Send(const std::vector<uint8_t>& data);

    bool IsOpen() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE CommHandle() const { return handle_; }
    const std::wstring& PortName() const { return cfg_.port; }
    uint32_t Baud() const { return cfg_.baud; }

    // 诊断
    size_t RecvBytes() const { return recv_bytes_; }
    size_t SentBytes() const { return sent_bytes_; }
    size_t WaitEvents() const { return wait_events_; }
    size_t ReadOps() const { return read_ops_; }
    DWORD LastCommError() const { return last_comm_error_; }

    // 供内部 SerialWatcher 访问。
    bool IsActiveForPump() const { return watchers_.size() > 0; }

private:
    class SerialWatcher;
    friend class SerialSource::SerialWatcher;

    bool ArmWait();
    bool ArmRead();
    bool ArmWrite(const std::vector<uint8_t>& data);
    void Pump();  // 唯一挂起点

    SerialConfig cfg_;
    SerialDataCallback cb_;
    HANDLE handle_ = nullptr;
    SerialWatcher* watcher_ = nullptr;  // 指向 watchers_ 里的对象，非所有权

    // 挂起状态（仅 loop 线程访问）
    bool wait_armed_ = false;
    bool read_in_flight_ = false;

    size_t recv_bytes_ = 0;
    size_t sent_bytes_ = 0;
    size_t wait_events_ = 0;
    size_t read_ops_ = 0;
    DWORD last_comm_error_ = 0;
};

}  // namespace tui::io
