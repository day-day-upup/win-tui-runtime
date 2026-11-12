#include "tui/console/serial_net_assistant.h"

#include <algorithm>

namespace tui::console {

namespace {

constexpr size_t kMaxPreview = 48;  // 单行最多展示多少字节，避免超宽

const wchar_t* TargetName(SerialNetAssistant::SendTarget t) {
    switch (t) {
        case SerialNetAssistant::SendTarget::kSerial: return L"serial";
        case SerialNetAssistant::SendTarget::kNetwork: return L"network";
        default: return L"both";
    }
}

}  // namespace

SerialNetAssistant::SerialNetAssistant(Renderer& renderer, std::wstring title)
    : renderer_(renderer), title_(std::move(title)) {}

std::wstring SerialNetAssistant::HexPreview(const std::vector<uint8_t>& data,
                                            size_t max_bytes) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    std::wstring out;
    size_t n = std::min(data.size(), max_bytes);
    for (size_t i = 0; i < n; ++i) {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 0x0F];
        out += L' ';
    }
    if (data.size() > n) out += L"...";
    return out;
}

std::wstring SerialNetAssistant::AsciiPreview(const std::vector<uint8_t>& data,
                                              size_t max_bytes) {
    std::wstring out;
    size_t n = std::min(data.size(), max_bytes);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = data[i];
        // 约束 D7：只保证 ASCII；不可打印字符用 '.' 占位。
        out += (b >= 0x20 && b < 0x7F) ? static_cast<wchar_t>(b) : L'.';
    }
    if (data.size() > n) out += L"...";
    return out;
}

void SerialNetAssistant::PushLog(const std::wstring& line) {
    logs_.Push(line);  // RingBuffer 满时自动丢最旧（背压）
    renderer_.MarkDirty();
}

void SerialNetAssistant::OnSerialData(const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    serial_rx_ += data.size();
    PushLog(L"[rx serial  " + std::to_wstring(data.size()) + L"B] " +
            AsciiPreview(data, kMaxPreview) + L"  | " + HexPreview(data, 16));
}

void SerialNetAssistant::OnNetworkData(const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    net_rx_ += data.size();
    PushLog(L"[rx network " + std::to_wstring(data.size()) + L"B] " +
            AsciiPreview(data, kMaxPreview) + L"  | " + HexPreview(data, 16));
}

void SerialNetAssistant::OnSystemLine(const std::wstring& line) {
    PushLog(L"[sys] " + line);
}

std::vector<uint8_t> SerialNetAssistant::TakePendingSend() {
    std::vector<uint8_t> out;
    out.swap(pending_send_);
    return out;
}

bool SerialNetAssistant::OnInputEvent(const InputEvent& e) {
    if (e.kind == InputKind::kResize) {
        renderer_.MarkDirty();
        return false;
    }
    if (e.kind != InputKind::kKey || !e.key_down) return false;

    const bool ctrl = (e.mods & Mod::kCtrl) != 0;

    // 退出：ESC 或 Ctrl+C
    if (e.vk == VK_ESCAPE || (ctrl && e.vk == 'C')) {
        quit_ = true;
        return true;
    }

    // Ctrl+S：切换发送目标
    if (ctrl && e.vk == 'S') {
        switch (target_) {
            case SendTarget::kSerial: target_ = SendTarget::kNetwork; break;
            case SendTarget::kNetwork: target_ = SendTarget::kBoth; break;
            default: target_ = SendTarget::kSerial; break;
        }
        PushLog(std::wstring(L"[sys] send target -> ") + TargetName(target_));
        return false;
    }

    // Ctrl+L：清日志
    if (ctrl && e.vk == 'L') {
        logs_.Clear();
        PushLog(L"[sys] log cleared");
        return false;
    }

    // Backspace：删一个字符
    if (e.vk == VK_BACK) {
        if (!input_.empty()) {
            input_.pop_back();
            renderer_.MarkDirty();
        }
        return false;
    }

    // Enter：把输入行发出去（交给外部真正调用 source->Send）
    if (e.vk == VK_RETURN) {
        if (!input_.empty()) {
            pending_send_.assign(input_.begin(), input_.end());
            pending_send_.push_back('\r');  // 串口惯例：CR 结尾
            ++tx_count_;
            PushLog(std::wstring(L"[tx ") + TargetName(target_) + L" " +
                    std::to_wstring(pending_send_.size()) + L"B] " + input_);
            input_.clear();
            renderer_.MarkDirty();
        }
        return false;
    }

    // 其他可打印 ASCII：追加（约束 D7 只保证 ASCII）
    if (e.ch >= 0x20 && e.ch < 0x7F) {
        if (input_.size() < 240) {  // 防止输入行无限增长
            input_.push_back(e.ch);
            renderer_.MarkDirty();
        }
    }
    return false;
}

void SerialNetAssistant::Draw() {
    DrawFrame(renderer_.GetSize());
}

void SerialNetAssistant::DrawFrame(Size s) {
    // --- 标题栏（第 0 行）---
    renderer_.Put(0, 0, title_,
                  MakeAttr(Color::kBrightWhite, Color::kBlue));
    // --- 状态栏（第 1 行）---
    std::wstring status = L" serial=" + std::to_wstring(serial_rx_) +
                          L"B  net=" + std::to_wstring(net_rx_) +
                          L"B  tx=" + std::to_wstring(tx_count_) +
                          L"  target=" + TargetName(target_) +
                          L"  log=" + std::to_wstring(logs_.Size()) +
                          L"/" + std::to_wstring(logs_.Capacity()) +
                          L" dropped=" + std::to_wstring(logs_.Dropped());
    renderer_.Put(0, 1, status,
                  MakeAttr(Color::kBrightYellow, Color::kBlack));

    // --- 日志区（第 2 行 ~ 倒数第 3 行）---
    const int16_t top = 2;
    const int16_t bottom = static_cast<int16_t>(s.h - 3);  // 留输入行 + 帮助行
    const int16_t rows = bottom > top ? static_cast<int16_t>(bottom - top) : 0;

    // 只画最后 rows 条（滚到底），避免整屏重排。
    const auto& items = logs_.Items();
    size_t start = items.size() > static_cast<size_t>(rows)
                       ? items.size() - static_cast<size_t>(rows)
                       : 0;
    int16_t y = top;
    for (size_t i = start; i < items.size() && y <= bottom; ++i, ++y) {
        WORD attr = MakeAttr(Color::kBrightCyan, Color::kBlack);
        if (items[i].rfind(L"[tx", 0) == 0) attr = MakeAttr(Color::kBrightGreen, Color::kBlack);
        else if (items[i].rfind(L"[sys]", 0) == 0) attr = MakeAttr(Color::kBrightYellow, Color::kBlack);
        else if (items[i].rfind(L"[rx network", 0) == 0) attr = MakeAttr(Color::kBrightMagenta, Color::kBlack);
        renderer_.Put(0, y, items[i], attr);
    }

    // --- 输入行（倒数第 2 行）---
    if (s.h >= 2) {
        const int16_t iy = static_cast<int16_t>(s.h - 2);
        std::wstring line = L"> " + input_;
        renderer_.Put(0, iy, line, MakeAttr(Color::kBrightWhite, Color::kBlack));
        // 光标示意（用 '_' 占位，避免真的移动控制台光标）
        if (line.size() < static_cast<size_t>(s.w)) {
            renderer_.Put(static_cast<int16_t>(line.size()), iy, L"_",
                          MakeAttr(Color::kBrightWhite, Color::kBlack));
        }
    }

    // --- 帮助行（最后一行）---
    if (s.h >= 1) {
        renderer_.Put(0, static_cast<int16_t>(s.h - 1),
                      L" Enter=send  Ctrl+S=target  Ctrl+L=clear  ESC=quit",
                      MakeAttr(Color::kBlack, Color::kCyan));
    }
}

}  // namespace tui::console
