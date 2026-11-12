#include "tui/io/dir_watcher.h"

#include <algorithm>
#include <cstddef>

#include "tui/io/iocp_backend.h"

namespace tui::io {

namespace {

DirEvent::Action MapAction(DWORD a) {
    switch (a) {
        case FILE_ACTION_ADDED: return DirEvent::Action::kAdded;
        case FILE_ACTION_REMOVED: return DirEvent::Action::kRemoved;
        case FILE_ACTION_MODIFIED: return DirEvent::Action::kModified;
        case FILE_ACTION_RENAMED_OLD_NAME: return DirEvent::Action::kRenamedOld;
        case FILE_ACTION_RENAMED_NEW_NAME: return DirEvent::Action::kRenamedNew;
        default: return DirEvent::Action::kUnknown;
    }
}

}  // namespace

DirWatcher::DirWatcher(IocpBackend& iocp, const std::wstring& dir, DirCallback cb)
    : Watcher(iocp, nullptr, nullptr), dir_(dir), dir_cb_(std::move(cb)) {
    // 目录句柄必须用 FILE_FLAG_BACKUP_SEMANTICS 打开，且用 FILE_FLAG_OVERLAPPED
    // 才能关联 IOCP（约束 C7）。
    HANDLE h = CreateFileW(dir_.c_str(),
                           FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        handle_ = h;
        iocp_.Associate(handle_, kKeyWatcher);
    }
}

DirWatcher::~DirWatcher() = default;

bool DirWatcher::Start() {
    // 通过 Watcher::BeginOperation 提交，统一走 inflight 计数（约束 C3）。
    return BeginOperation(ctx_);
}

bool DirWatcher::Arm(IoContext& ctx) {
    if (!handle_ || handle_ == INVALID_HANDLE_VALUE) return false;
    ctx.buffer.resize(4096);

    // completion 回调：IocpBackend::Dispatch 在 loop 线程调用它。
    // 转发给 Watcher::OnCompletion，由 close 状态机处理 inflight / 重投 / 关闭。
    ctx.callback = [this, &ctx](uint32_t bytes, uint32_t error) {
        OnCompletion(ctx, bytes, error);
    };

    DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                  FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    // lpBytesReturned 必须给有效指针；overlapped 下该值在调用时无意义，
    // 真正的字节数来自 completion。用成员避免指针悬空。
    bytes_returned_ = 0;
    BOOL ok = ReadDirectoryChangesW(
        handle_, ctx.buffer.data(), static_cast<DWORD>(ctx.buffer.size()),
        /*bWatchSubtree=*/FALSE, flags,
        &bytes_returned_, &ctx.ov, /*lpCompletionRoutine=*/nullptr);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        ctx.callback = {};
        return false;
    }
    ctx.generation = generation_;
    return true;
}

void DirWatcher::Notify(uint32_t bytes, uint32_t error,
                        const std::vector<uint8_t>& data) {
    if (error != 0 || bytes == 0) return;
    auto events = Parse(data, bytes);
    for (const auto& e : events) {
        if (dir_cb_) dir_cb_(e);
    }
}

std::vector<DirEvent> DirWatcher::Parse(const std::vector<uint8_t>& buffer, uint32_t bytes) {
    std::vector<DirEvent> out;
    if (bytes == 0 || buffer.empty()) return out;
    const uint8_t* p = buffer.data();
    const uint8_t* end = p + std::min<size_t>(bytes, buffer.size());
    for (;;) {
        if (p + offsetof(FILE_NOTIFY_INFORMATION, FileName) > end) break;
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        DirEvent e;
        e.action = MapAction(info->Action);
        const size_t name_bytes = info->FileNameLength;
        const wchar_t* name = reinterpret_cast<const wchar_t*>(
            p + offsetof(FILE_NOTIFY_INFORMATION, FileName));
        size_t name_chars = name_bytes / sizeof(wchar_t);
        const uint8_t* name_end = reinterpret_cast<const uint8_t*>(name) + name_bytes;
        if (name_end > end) break;
        e.name.assign(name, name_chars);
        out.push_back(e);
        if (info->NextEntryOffset == 0) break;
        p += info->NextEntryOffset;
        if (p >= end) break;
    }
    return out;
}

// ---- DirSource -------------------------------------------------------------

DirSource::DirSource(IocpBackend& iocp, const std::wstring& dir, DirCallback cb)
    : EventSource(iocp), dir_(dir), cb_(std::move(cb)) {}

bool DirSource::Start() {
    auto w = std::make_unique<DirWatcher>(iocp_, dir_, cb_);
    if (!w->IsOpen()) return false;
    raw_ = w.get();
    watchers_.push_back(std::move(w));
    return raw_->Start();
}

}  // namespace tui::io
