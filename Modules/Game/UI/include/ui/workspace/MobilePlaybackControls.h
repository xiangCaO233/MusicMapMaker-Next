#pragma once

#include <imgui.h>

namespace MMM::UI
{
class UIManager;

/// @brief 触屏工具栏的播放、分拍、键数、速度与缩放入口，不维护独立业务状态。
/// @details 弹层与桌面使用相同预设来源和命令，手势由移动按钮解释。
class MobilePlaybackControls
{
public:
    /// @brief 推进弹层关闭后的触点释放保护，不访问编辑器会话。
    /// @warning 每帧固定次数读取输入，不等待线程或执行文件操作。
    void beginFrame();
    /// @brief 按当前字号测量八个工具按钮需要的宽度。
    /// @warning 每帧只测量固定数量短标签，不缓存字体资源。
    static float buttonWidth(float scale);
    /// @brief 绘制五个参数按钮，播放与参数命令只在用户操作时提交。
    /// @warning 每帧会话读取只尝试一次 try_lock；失败沿用标量显示，不阻塞更新。
    void drawButtons(UIManager& manager, float width, float scale);
    /// @brief 在工具栏结束后绘制弹层，避免继承窄工具栏的裁剪区。
    /// @param enabled 工具栏隐藏时关闭所属弹层，不残留输入拦截。
    /// @warning 每帧仅处理可见弹层；速度持久化只在预设点击或拖动结束发生。
    void drawPopup(UIManager& manager, float scale, bool enabled);
    /// @brief 弹层及其关闭释放帧独占画布输入，防止触点穿透绘制音符。
    bool blocksCanvasInput() const
    {
        return m_popup != Popup::None || m_releaseGuard;
    }

private:
    /// @brief 同时只允许调整一项，打开新项不会叠加三个浮层。
    enum class Popup { None, Divisor, Keys, Speed, Zoom };
    /// @brief 请求打开的参数类型，关闭后不保留旧会话参数草稿。
    Popup m_popup{ Popup::None };
    /// @brief 按钮与弹层在不同窗口提交，打开请求推迟到统一 Popup ID 作用域。
    bool m_openRequested{};
    /// @brief 关闭弹层的按下及释放都不交给后方画布。
    bool m_releaseGuard{};
    /// @brief 仅缓存轨数标量；活动标签改变时立即失效，不能显示上一谱面键数。
    int m_sessionIndex{ -1 }, m_trackCount{};
    /// @brief 弹层绑定打开时的标签，不能随工具栏缓存自动换谱。
    int m_popupSessionIndex{ -1 };
    /// @brief 拖动沿用本地草稿，命令回读不能在手指按住时拉回滑块。
    bool m_editingTracks{}, m_editingSpeed{};
    /// @brief 倍速只保存交互草稿，空闲时重新读取音频权威状态。
    float m_speedDraft{ 1.0f };
    /// @brief 请求显示对应参数的触摸弹层。
    void open(Popup popup);
};
}  // namespace MMM::UI
