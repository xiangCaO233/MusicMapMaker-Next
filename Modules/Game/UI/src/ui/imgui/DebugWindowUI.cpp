#include "ui/imgui/DebugWindowUI.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "ui/IRenderableView.h"
#include "ui/UIManager.h"
#include "ui/utils/UIThemeUtils.h"
#include <imgui.h>

/// @file DebugWindowUI.cpp
/// @brief 渲染调试窗口的活动画布查询与 Glow Mask 预览实现。
/// @details 本文件只观察画布已经建立的渲染资源，不取得描述符所有权，也不
/// 触发离屏目标重建；资源缺失通过窗口内状态文本反馈。

namespace MMM::UI
{

/// @brief 创建渲染调试视图并注册稳定名称。
/// @param name UIManager 查找该视图时使用的名称。
DebugWindowUI::DebugWindowUI(const std::string& name) : IUIView(name) {}

/// @brief 绘制活动画布的发光遮罩诊断窗口。
/// @param sourceManager 当前 UI 管理器，用于解析活动画布能力。
/// @warning UI 热路径：每帧读取现有画布和描述符，不等待或重建渲染资源。
/// @details 活动相机 ID 为空时查询默认二维画布；只有实现 IRenderableView
/// 能力且已提供 Glow 描述符的视图才会显示纹理预览。
void DebugWindowUI::update(UIManager* sourceManager)
{
    // 保留皮肤管理器入口，便于后续诊断项继续使用统一主题配置。
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    if ( ImGui::Begin("Renderer Debug Window") ) {
        // 优先按活动相机 ID 查找，缺失时回退基础二维画布。
        std::string activeCameraId =
            Logic::EditorEngine::instance().getActiveCameraId();
        auto* view = sourceManager->getView<IUIView>(
            activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);
        // 能力接口避免依赖 RTTI 或具体画布类型。
        auto* canvas = view ? view->asRenderableView() : nullptr;
        if ( canvas ) {
            // 描述符由画布拥有，调试窗口只转换为 ImGui 非拥有纹理 ID。
            ImTextureID glowTex =
                (ImTextureID)(VkDescriptorSet)canvas->getGlowDescriptorSet();

            if ( glowTex ) {
                // 标签明确该纹理是渲染目标的原始几何遮罩。
                ImGui::Text("Main Canvas Glow Mask (RT):");

                // 预览占满窗口可用宽度。
                float availW = ImGui::GetContentRegionAvail().x;
                // 以 16:9 显示，避免调试纹理随窗口宽度变形过度。
                float displayH = availW * (9.0f / 16.0f);

                // 半透明边框帮助在调试窗口背景上辨识纹理边界。
                ImGui::Image(glowTex,
                             ImVec2(availW, displayH),
                             ImVec2(0, 0),
                             ImVec2(1, 1),
                             ImVec4(1, 1, 1, 1),
                             ImVec4(1, 1, 1, 0.5f));

                if ( ImGui::IsItemHovered() ) {
                    // 悬停提示解释遮罩用途，不在主界面长期占用空间。
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "This texture shows the raw geometry mask used for the "
                        "glow effect.");
                    ImGui::EndTooltip();
                }
            } else {
                // 画布存在但尚未生成 Glow 资源时使用危险色诊断。
                ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
                ImGui::TextColored(dangerCol,
                                   "Glow Mask texture not available.");
            }
        } else {
            // 活动画布缺失或不具备渲染能力时使用警告色区分。
            ImVec4 warningCol = Utils::UIThemeUtils::getWarningColor();
            ImGui::TextColored(warningCol,
                               "Basic2DCanvas ('Basic2DCanvas') not found.");
        }
    }
    // Begin 返回 false 时也必须结束窗口。
    ImGui::End();
}

}  // namespace MMM::UI
