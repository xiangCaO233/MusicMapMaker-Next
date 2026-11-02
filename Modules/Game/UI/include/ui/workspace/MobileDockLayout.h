#pragma once
#include <imgui.h>

namespace MMM::UI
{
/// @brief 移动工作区的三个受控停靠分区，资源抽屉不参与此布局。
enum class MobileDockPanel { Main, Timing, Preview };

/// @brief 同一根节点提供等高标签区和原生可调宽分隔条。
/// @details 只在辅助窗口显隐变化时重建分割，普通帧保留用户调整的宽度。
class MobileDockLayout
{
public:
    /// @brief 提交工作区宿主及固定的水平分区，不创建业务视图。
    /// @warning 每帧绘制固定节点；重建仅发生在显隐切换的低频路径。
    void render(ImVec2 position, ImVec2 size, float scale, bool timing,
                bool preview);
    /// @brief 在业务窗口 Begin 前指定所属分区，并恢复重建前活动标签。
    void target(MobileDockPanel panel, const char* title);
    /// @brief 当前主画布、欢迎和设置共用的中心叶节点。
    ImGuiID center() const { return m_center; }
    /// @brief 返回指定分区的节点身份，隐藏分区返回零。
    ImGuiID node(MobileDockPanel panel) const;
    /// @brief 返回一致的触摸标签内边距，宿主和业务 Begin 必须共用。
    /// @warning 每帧只读取当前样式与字号，不分配或查询平台状态。
    static ImVec2 tabPadding(float scale);
    /// @brief 首次建树前恢复客户端比例和中心活动标签，不消费桌面 ini。
    void restore(float timingRatio, float previewRatio, ImGuiID selectedTab);
    /// @brief 在显式提交边沿读取实际比例和中心标签，隐藏区保留原比例。
    /// @warning 只查询固定数量节点，不构造 JSON 或执行文件访问。
    void capture(float& timingRatio, float& previewRatio,
                 ImGuiID& selectedTab) const;
    /// @brief 帧末恢复启动活动窗口，首次用户按下后停止覆盖启动焦点请求。
    /// @warning 固定 ID 查询；不抢业务弹窗焦点，不等待视图创建。
    void finishFrame();
    /// @brief 长按当前窗口的原生停靠标签后提供关闭菜单，不绘制标签 X。
    /// @param canClose 调用方的生命周期策略，最后一个主画布必须为 false。
    /// @param enabled 抽屉或其他模态独占输入时禁止开始新手势。
    /// @return 用户在菜单中明确选择关闭，而不是仅松开长按触点。
    /// @warning 每个停靠窗口每帧调用；只查询当前标签，不等待或扫描业务对象。
    bool tabCloseRequested(const char* closeLabel, float scale, bool canClose,
                           bool enabled);
    /// @brief 菜单和关闭释放帧拦截画布原始输入，防止菜单触点穿透。
    bool tabMenuOwnsPointer() const { return m_menuWindow || m_releaseGuard; }

private:
    /// @brief 仅保存稳定 ID，不跨帧缓存可能因分割失效的节点指针。
    ImGuiID m_root{}, m_center{}, m_timing{}, m_preview{}, m_restoreFocus{};
    /// @brief 首次交互前保留已保存窗口，防止惰性设置页的初始聚焦覆盖它。
    ImGuiID m_startupFocus{};
    /// @brief 最近显隐组合，不把旋转或宿主变宽误判为需要重建。
    int m_visibility{ -1 };
    /// @brief 各辅助分区占根宽度的比例，隐藏前保留用户实际调整值。
    float m_timingRatio{ .28f }, m_previewRatio{ .2f };
    /// @brief 长按仅保存窗口身份与起点；移动超过容差后本次触点永久取消。
    ImGuiID m_holdWindow{};
    ImVec2  m_holdOrigin{};
    double  m_holdStarted{};
    /// @brief 菜单窗口身份不拥有视图，关闭原窗口后由帧末清理。
    ImGuiID m_menuWindow{};
    /// @brief 长按松手不能点中菜单；关闭菜单的释放不能落入后方画布。
    bool m_waitMenuRelease{}, m_releaseGuard{};
};
}  // namespace MMM::UI
