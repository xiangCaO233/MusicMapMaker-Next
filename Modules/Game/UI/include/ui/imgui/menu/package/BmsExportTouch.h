#pragma once

#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <imgui.h>

namespace MMM::UI
{
/// @brief 导出模态的局部触控尺寸，不改主题或桌面控件密度。
/// @details 在 Begin 前保留紧凑标题，成功 Begin 后才放大内容行。
/// endContent 必须在 EndPopup 前调用，避免 ImGui 把正常样式当作泄漏恢复。
/// 标题与正文分开，防止高 DPI 横屏键盘把文本输入区域挤成零高度。
class ExportTouchLayout
{
public:
    /// @brief 条件仅决定呈现，导出格式与业务状态仍由原动作拥有。
    /// @param scale 当前窗口倍率，只用于逻辑尺寸，不访问 Android density。
    /// @param enabled 只让支持的导出阶段采用移动尺寸，不扩展其它格式业务。
    /// @warning 弹窗可见期间逐帧构造，只有固定数量样式栈操作。
    explicit ExportTouchLayout(float scale, bool enabled = true)
        : m_scale(scale), m_mobile(enabled && !workspaceAllowsHoverFeedback())
    {
        if ( !m_mobile ) return;
        const auto padding = ImGui::GetStyle().FramePadding;
        // 原生标题没有触摸动作，不让父表单的大 padding 放大标题栏。
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            { padding.x, std::min(padding.y, 4.f * scale) });
    }
    /// @brief 样式栈由单个作用域拥有，不允许复制后重复弹栈。
    ExportTouchLayout(const ExportTouchLayout&) = delete;
    /// @brief 禁止把活动样式所有者赋给另一个作用域。
    ExportTouchLayout& operator=(const ExportTouchLayout&) = delete;
    /// @brief 析构只恢复 Begin 之前的标题样式，不关闭窗口或提交业务。
    ~ExportTouchLayout()
    {
        // 先退出内容层再退出标题层，保持与压入顺序严格相反。
        // 正常调用方已在 EndPopup 前退出内容，此处只处理未开始窗口的路径。
        endContent();
        if ( m_mobile ) ImGui::PopStyleVar();
    }
    /// @brief 成功开始模态后扩大输入行，底部按钮与复选项共用触摸高度。
    /// @warning 可见模态每帧调用，只读样式标量，无平台查询或文件操作。
    void beginContent()
    {
        if ( !m_mobile || m_content ) return;
        // 内容在 Begin 后才能读取正确字号，标题专用字体不能参与行高计算。
        // 主题本来更高时不缩小，避免把用户字号或可访问性设置裁掉。
        const auto padding = ImGui::GetStyle().FramePadding;
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            { padding.x,
              std::max(padding.y,
                       (48.f * m_scale - ImGui::GetFontSize()) * .5f) });
        m_content = true;
    }
    /// @brief 在 EndPopup 前配对正文样式；多次结束不重复弹栈。
    void endContent()
    {
        if ( !m_content ) return;
        ImGui::PopStyleVar();
        m_content = false;
    }
    /// @brief 判断是否采用移动尺寸，不按单次事件来源猜测设备。
    /// @warning 每帧读取本对象标量，不保存窗口或输入设备指针。
    bool mobile() const { return m_mobile; }
    /// @brief 两枚操作按钮平分可用宽度，桌面保留原固定宽度和自动高度。
    /// @warning 每帧固定几何计算，宽度来自当前父窗，不能缓存前一个模态尺寸。
    ImVec2 actionSize() const
    {
        // 两按钮总宽度包含一次间隔，不能按屏幕宽度忽略窗口 padding。
        // 桌面继续使用原固定逻辑尺寸，不将触控下限传播到桌面快捷菜单。
        if ( !m_mobile ) return { 120.f * m_scale, 0 };
        return { std::max(1.f,
                          (ImGui::GetContentRegionAvail().x -
                           ImGui::GetStyle().ItemSpacing.x) *
                              .5f),
                 48.f * m_scale };
    }

private:
    float m_scale{};                ///< 本窗口内容倍率，不持有平台对象。
    bool  m_mobile{}, m_content{};  ///< 标题与内容样式分别配对。
};

/// @brief 输出选择器按当前文本安全区域换算尺寸，桌面保留原像素约定。
/// @param scale 当前内容倍率，与字体和其它导出控件使用同一坐标空间。
/// @warning 每帧只读外壳几何，不查询 IME、不枚举目录或保存窗口配置。
inline ImVec2 exportFileDialogSize(float scale)
{
    // 移动窗口必须跟随键盘缩小，不能把完整主视口高度作为最小尺寸。
    // 小窗口来自桌面固定像素，不能通过保存 INI 或放大字体来弥补。
    if ( workspaceAllowsHoverFeedback() ) return { 600, 400 };
    const auto available = workspaceTextInputWindowArea().m_size;
    return { std::min(600.f * scale, available.x * .92f),
             std::min(400.f * scale, available.y * .88f) };
}

/// @brief 单文件和资源包共用的触控 BMS 选项，修改的只是本次导出草稿。
/// @param package 资源包允许处理音乐副本，单文件只写谱面且不生成音频。
/// @param lossy 有损吸附必须由用户明确启用，绘制不重设默认值。
/// @param shift 与另一个选项互斥，资源包 false 表示处理音乐副本。
/// @warning 可见弹窗每帧绘制固定控件，不编码音频、不改项目或触发保存。
inline void drawBmsTouchOptions(bool package, bool& lossy, bool& shift)
{
    // 保真边界常驻，不能依赖鼠标悬浮才能发现 MyO2 会改变时间。
    ImGui::TextWrapped("默认保留变速和物件时间；高精度叠加行可能不兼容 MyO2。");
    FeedbackCheckbox("MyO2 兼容吸附（有损）", &lossy);
    // 风险说明只在用户主动开启有损策略时展开，保真路径不承担额外操作。
    if ( lossy )
        ImGui::TextWrapped(
            "有损吸附可能移动或合并变速事件，复杂变速谱可能明显偏移。");
    ImGui::Separator();
    ImGui::TextUnformatted("首拍对齐方式");
    // 两个单选共享唯一草稿值，重复点击已选项也不能造成全部取消。
    if ( FeedbackRadioButton(package ? "处理音乐副本" : "保持原时间位置",
                             !shift) )
        shift = false;
    if ( FeedbackRadioButton("保留音乐文件，后移事件", shift) ) shift = true;
    // 每个选项的影响独立说明，手机不需要猜测桌面 Tooltip 的内容。
    // 同一个 false 在两种流程中的业务含义不同，不能显示误导性的统一描述。
    // 音频处理仅允许资源包；单文件分支绝不许诺生成裁剪后的音乐。
    if ( package && !shift )
        ImGui::TextWrapped("裁剪或补静音到音乐副本，不改源音乐和 Key 采样。");
    else if ( shift )
        ImGui::TextWrapped(
            "统一后移 BGM、物件及初始 BPM 以外的红线；不重编码音乐。");
    else
        ImGui::TextWrapped(
            "仅写出谱面文件，保持现有时间位置，不修改或生成音频。");
    // 不把导出选项混入编辑器磁铁或全局配置，取消后仍是原项目。
    ImGui::TextWrapped("这些选项只作用于导出副本，项目源文件保持不变。");
}
}  // namespace MMM::UI
