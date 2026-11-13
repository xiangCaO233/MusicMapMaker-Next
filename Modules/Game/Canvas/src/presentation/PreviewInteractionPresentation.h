#pragma once
#include <imgui.h>
#include <memory>
#include <optional>

namespace MMM::Common::Render
{
struct RenderSnapshot;
}
namespace MMM::Canvas
{
/// @brief 预览平台输入只借用当前帧的显示几何与不可变快照。
/// @details 密度栏内部轴与内容矩形分开，首触点只能选择一个区域。
struct PreviewInteractionFrame {
    const Common::Render::RenderSnapshot*
           m_snapshot{};              ///< 不跨帧保存快照地址。
    ImVec2 m_origin{}, m_size{};      ///< 预览内容的屏幕矩形。
    ImVec2 m_railMin{}, m_railMax{};  ///< 独立全谱定位轴的屏幕矩形。
    float  m_yOffset{};   ///< 本帧已经应用于离屏内容的纵向视觉补偿。
    float  m_scale{ 1 };  ///< 当前内容缩放，不修改系统 DPI。
    bool   m_hovered{}, m_available{};  ///< 首触点归属和持续输入许可分开。
};
/// @brief 预览独立于主画布的输入边界；桌面默认不接管原鼠标路径。
/// @details 平台解释器归视图独占，不持有会话或图形资源。
class PreviewInteractionPresentation
{
public:
    /// @brief 析构只释放本地状态，调用方须先完成导航收尾。
    virtual ~PreviewInteractionPresentation() = default;
    /// @brief 接管触控后由平台处理内容和密度栏，避免回放合成鼠标。
    /// @warning 每帧只分类固定触点；不得等待逻辑线程或访问文件。
    // 返回 true 表示本平台独占整张预览，包括右侧密度轴。
    // 默认 false 保留旧鼠标的点击、拖动和滚轮顺序。
    // 调用方仍负责所有谱面绘制，不在这里复制离屏渲染器。
    // 输入解释器只发布命令，不能直接修改谱面模型。
    virtual bool update(const PreviewInteractionFrame&) { return false; }
    /// @brief 隐藏或外部方案接管时结束已显示的浏览，吞掉残留触点。
    virtual void suspend() {}
    /// @brief 待定触点也阻止方案切换，不移交半次输入。
    virtual bool hasGesture() const { return false; }
    /// @brief 密度游标使用即时本地目标，不等待逻辑快照往返。
    virtual std::optional<double> densityTarget() const { return std::nullopt; }
};
/// @brief CMake 选择的平台工厂；只在视图创建时分配一次。
std::unique_ptr<PreviewInteractionPresentation>
createPreviewInteractionPresentation();
/// @brief CPU 测试显式构造与 Android 相同的生产解释器。
std::unique_ptr<PreviewInteractionPresentation>
createMobilePreviewInteractionPresentation();
}  // namespace MMM::Canvas
