#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tui/console/input_decoder.h"
#include "tui/console/renderer.h"
#include "tui/core/ring_buffer.h"

// 阶段 6：SerialNetAssistant —— capstone 应用逻辑（串口 + 网络助手）
//
// 严格遵循阶段 2 定下的分层：本类只做「状态 + 绘制」，不碰线程、不碰句柄。
// 于是同一个类既能被真实控制台模式驱动，也能被 --selftest 的 headless 模式
// 驱动 —— 保证「无串口/无控制台环境也能验证出口条件」。
//
// 功能边界（约束 E：第一版砍到最简）：连上、收、发、显示。
//   键盘输入 → 编辑命令行 → Enter 发送（串口 + TCP 双发）
//   收到的数据 → RingBuffer 日志（背压）→ 差分渲染显示
//
// 交互：
//   ESC / Ctrl+C : 退出
//   Enter        : 发送当前输入行
//   Backspace    : 删一个字符
//   Ctrl+S       : 切换「发送目标」（串口 / 网络 / 两者）
//   Ctrl+L       : 清屏（清日志，保留标题栏）
//   其他可打印   : 追加到输入行
namespace tui::console {

class SerialNetAssistant {
public:
    enum class SendTarget { kSerial, kNetwork, kBoth };

    SerialNetAssistant(Renderer& renderer, std::wstring title);

    // 外部把数据喂进来（loop 线程调用）。source 标签用于显示来源。
    void OnSerialData(const std::vector<uint8_t>& data);
    void OnNetworkData(const std::vector<uint8_t>& data);
    void OnSystemLine(const std::wstring& line);

    // 处理一个输入事件（loop 线程）。返回是否请求退出。
    bool OnInputEvent(const InputEvent& e);

    // 把当前状态画到 renderer 的 back 缓冲（不 Present）。
    void Draw();

    bool quit_requested() const { return quit_; }
    SendTarget target() const { return target_; }
    const std::wstring& input_line() const { return input_; }
    size_t log_count() const { return logs_.Size(); }
    size_t dropped_count() const { return logs_.Dropped(); }
    size_t serial_rx() const { return serial_rx_; }
    size_t net_rx() const { return net_rx_; }
    size_t tx_count() const { return tx_count_; }

    // 取出「本 tick 要发送的字节」——由外部真正调用 source->Send()。
    // 这样本类不依赖具体 source 实例，保持可测。
    std::vector<uint8_t> TakePendingSend();

private:
    void PushLog(const std::wstring& line);
    void DrawFrame(Size s);
    static std::wstring HexPreview(const std::vector<uint8_t>& data, size_t max_bytes);
    static std::wstring AsciiPreview(const std::vector<uint8_t>& data, size_t max_bytes);

    Renderer& renderer_;
    std::wstring title_;
    core::RingBuffer<std::wstring> logs_{512};  // 背压：满时丢最旧
    std::wstring input_;
    SendTarget target_ = SendTarget::kBoth;
    bool quit_ = false;

    size_t serial_rx_ = 0;
    size_t net_rx_ = 0;
    size_t tx_count_ = 0;
    std::vector<uint8_t> pending_send_;
};

}  // namespace tui::console
