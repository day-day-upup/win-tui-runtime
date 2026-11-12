#include "tui/io/serial_source.h"

#include <algorithm>

#include "tui/io/iocp_backend.h"

namespace tui::io {

namespace {

// 串口设备路径必须用 \\.\COMx 形式，否则 "COM10" 以上会解析失败。
std::wstring DevicePath(const std::wstring& port) {
    if (port.rfind(L"\\\\.\\", 0) == 0) return port;
    return L"\\\\.\\" + port;
}

}  // namespace

// SerialWatcher：三个独立 IoContext（wait / read / write），全双工互不干扰。
// 这是约束 C2「每操作独立上下文」在串口上的直接应用，也是阶段 4 全双工
// 教训（绝不共用 OVERLAPPED）的延续。
class SerialSource::SerialWatcher final : public Watcher {
public:
    SerialWatcher(IocpBackend& iocp, HANDLE h, SerialSource& owner)
        : Watcher(iocp, h, nullptr), owner_(owner) {}

    bool StartWait() {
        wait_ctx_.op_kind = OpKind::kGeneric;  // WaitCommEvent 不是读也不是写
        return BeginOperation(wait_ctx_);
    }

    bool StartRead() {
        read_ctx_.op_kind = OpKind::kRead;
        return BeginOperation(read_ctx_);
    }

    bool StartWrite(const std::vector<uint8_t>& data) {
        write_ctx_.buffer.assign(data.begin(), data.end());
        write_ctx_.op_kind = OpKind::kWrite;
        return BeginOperation(write_ctx_);
    }

    IoContext& WaitCtx() { return wait_ctx_; }
    IoContext& ReadCtx() { return read_ctx_; }
    IoContext& WriteCtx() { return write_ctx_; }

    // 供 owner 在 Arm 里设置 lpEvtMask 输出变量。
    DWORD& EventMaskOut() { return evt_mask_; }

protected:
    bool Arm(IoContext& ctx) override {
        if (!owner_.IsOpen()) return false;
        ctx.callback = [this, &ctx](uint32_t b, uint32_t e) { OnCompletion(ctx, b, e); };

        switch (ctx.op_kind) {
            case OpKind::kGeneric: {
                // overlapped WaitCommEvent：lpEvtMask 必须指向有效内存，
                // 且该内存在操作完成前不能失效 → 用成员 evt_mask_。
                evt_mask_ = 0;
                BOOL ok = WaitCommEvent(owner_.CommHandle(), &evt_mask_, &ctx.ov);
                if (!ok && GetLastError() != ERROR_IO_PENDING) {
                    ctx.callback = {};
                    return false;
                }
                return true;
            }
            case OpKind::kRead: {
                ctx.buffer.resize(4096);
                DWORD got = 0;
                BOOL ok = ReadFile(owner_.CommHandle(), ctx.buffer.data(),
                                   static_cast<DWORD>(ctx.buffer.size()), &got, &ctx.ov);
                if (!ok && GetLastError() != ERROR_IO_PENDING) {
                    ctx.callback = {};
                    return false;
                }
                return true;
            }
            case OpKind::kWrite: {
                DWORD put = 0;
                BOOL ok = WriteFile(owner_.CommHandle(), ctx.buffer.data(),
                                    static_cast<DWORD>(ctx.buffer.size()), &put, &ctx.ov);
                if (!ok && GetLastError() != ERROR_IO_PENDING) {
                    ctx.callback = {};
                    return false;
                }
                return true;
            }
            default:
                return false;
        }
    }

    void Notify(uint32_t bytes, uint32_t error,
                const std::vector<uint8_t>& data) override {
        (void)bytes;
        (void)error;
        (void)data;
        // 全部由 owner 的 on_complete 钩子按 op_kind 精确分流，这里不重复解析。
    }

private:
    SerialSource& owner_;
    IoContext wait_ctx_;
    IoContext read_ctx_;
    IoContext write_ctx_;
    DWORD evt_mask_ = 0;  // WaitCommEvent 的输出参数，必须活到完成
};

SerialSource::SerialSource(IocpBackend& iocp, const SerialConfig& cfg, SerialDataCallback cb)
    : EventSource(iocp), cfg_(cfg), cb_(std::move(cb)) {}

SerialSource::~SerialSource() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool SerialSource::Start() {
    // 约束 C7：串口必须以 overlapped 方式打开，才能关联 IOCP。
    HANDLE h = CreateFileW(DevicePath(cfg_.port).c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,        // 串口独占，不共享
                           nullptr,  // 无安全属性
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        last_comm_error_ = GetLastError();
        return false;
    }
    handle_ = h;

    // --- DCB：波特率 / 数据位 / 校验 / 停止位 ---
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(handle_, &dcb)) {
        last_comm_error_ = GetLastError();
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }
    dcb.BaudRate = cfg_.baud;
    dcb.ByteSize = cfg_.byte_size;
    dcb.Parity = cfg_.parity;
    dcb.StopBits = cfg_.stop_bits;
    dcb.fBinary = TRUE;   // 串口必须二进制模式
    dcb.fParity = cfg_.parity != 0 ? TRUE : FALSE;
    // 关掉流控，第一版最简（约束 E）。
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(handle_, &dcb)) {
        last_comm_error_ = GetLastError();
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    // --- 超时：读「立即返回已到达的字节」，避免阻塞 loop 线程 ---
    // 关键：既然用 overlapped ReadFile，超时只影响「无数据时是否立刻返回」。
    // 这里让 ReadFile 在有数据时立刻返回、无数据时等 MAXDWORD（由 WaitCommEvent 兜底）。
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 0;
    if (!SetCommTimeouts(handle_, &to)) {
        last_comm_error_ = GetLastError();
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    // --- 事件掩码：只在「有字符到达」时唤醒（第一版最简）---
    if (!SetCommMask(handle_, EV_RXCHAR)) {
        last_comm_error_ = GetLastError();
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    // 关联到 IOCP（约束 C7）。
    if (!iocp_.Associate(handle_, kKeyWatcher)) {
        last_comm_error_ = GetLastError();
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    auto w = std::make_unique<SerialWatcher>(iocp_, handle_, *this);
    watcher_ = w.get();
    watchers_.push_back(std::move(w));

    // --- 读完成：交付数据 → 继续 Pump ---
    auto& rctx = watcher_->ReadCtx();
    rctx.auto_rearm = false;
    rctx.on_complete = [this](IoContext& ctx, uint32_t bytes, uint32_t error) -> bool {
        read_in_flight_ = false;
        if (error == 0 && bytes > 0) {
            ++read_ops_;
            recv_bytes_ += bytes;
            std::vector<uint8_t> chunk(
                ctx.buffer.begin(),
                ctx.buffer.begin() + std::min<size_t>(bytes, ctx.buffer.size()));
            if (cb_) cb_(chunk);
        } else if (error != 0) {
            last_comm_error_ = error;
        }
        Pump();  // 读完了，重新挂 WaitCommEvent 等下一批
        return true;
    };

    // --- WaitCommEvent 完成：有字符到达 → 发 ReadFile ---
    auto& wctx = watcher_->WaitCtx();
    wctx.auto_rearm = false;
    wctx.on_complete = [this](IoContext& ctx, uint32_t bytes, uint32_t error) -> bool {
        (void)ctx;
        (void)bytes;
        wait_armed_ = false;
        if (error != 0) {
            last_comm_error_ = error;
            return true;
        }
        ++wait_events_;
        DWORD mask = watcher_->EventMaskOut();
        if ((mask & EV_RXCHAR) && !read_in_flight_) {
            ArmRead();
        } else {
            Pump();  // 没有可读事件（例如 EV_ERR），重新等
        }
        return true;
    };

    // --- 写完成：累计字节，不重挂 ---
    auto& wrctx = watcher_->WriteCtx();
    wrctx.auto_rearm = false;
    wrctx.on_complete = [this](IoContext& ctx, uint32_t bytes, uint32_t error) -> bool {
        (void)ctx;
        if (error == 0) sent_bytes_ += bytes;
        else last_comm_error_ = error;
        return true;
    };

    // 首次挂起：等「有字符到达」。
    return ArmWait();
}

bool SerialSource::ArmWait() {
    if (!watcher_ || wait_armed_) return false;
    wait_armed_ = true;
    if (!watcher_->StartWait()) {
        wait_armed_ = false;
        return false;
    }
    return true;
}

bool SerialSource::ArmRead() {
    if (!watcher_ || read_in_flight_) return false;
    read_in_flight_ = true;
    if (!watcher_->StartRead()) {
        read_in_flight_ = false;
        return false;
    }
    return true;
}

bool SerialSource::ArmWrite(const std::vector<uint8_t>& data) {
    return watcher_ ? watcher_->StartWrite(data) : false;
}

// 唯一挂起点：保证任何时刻「最多一个 WaitCommEvent + 最多一个 ReadFile」。
void SerialSource::Pump() {
    if (!watcher_) return;
    if (!wait_armed_ && !read_in_flight_) {
        ArmWait();
    }
}

void SerialSource::Send(const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    ArmWrite(data);
}

void SerialSource::CloseAll() {
    // 顺序很重要（阶段 6 实测得出的结论）：
    //   1) EventSource::CloseAll()：先把 watcher 置 kClosing + CancelIoEx，
    //      这样后续到达的完成包一律走「收尾」分支，不会被当成正常完成而重挂。
    //   2) SetCommMask(handle, 0)：让挂起的 WaitCommEvent **立即以成功完成包
    //      返回**（实测 GQCS=1）。这是串口最可靠的唤醒手段。
    //      仅靠 CancelIoEx 在部分驱动/时序下不投递完成包，会导致 inflight
    //      永不归零、watcher 卡在 kClosing —— 本阶段真实踩过这个坑。
    //   3) PurgeComm：丢弃未读数据，避免关闭时残留旧数据。
    wait_armed_ = false;
    read_in_flight_ = false;
    EventSource::CloseAll();  // kClosing + CancelIoEx
    if (IsOpen()) {
        SetCommMask(handle_, 0);  // 强制 WaitCommEvent 产生完成包
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
}

}  // namespace tui::io
