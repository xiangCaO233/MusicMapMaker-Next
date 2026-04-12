#include "ui/imgui/DebugWindowUI.h"
#include "canvas/Basic2DCanvas.h"
#include "config/skin/SkinConfig.h"
#include "ui/UIManager.h"
#include <imgui.h>

namespace MMM::UI
{

DebugWindowUI::DebugWindowUI(const std::string& name) : IUIView(name) {}

void DebugWindowUI::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    if ( ImGui::Begin("Renderer Debug Window") ) {
        // 获取主画布实例
        auto canvas =
            sourceManager->getView<Canvas::Basic2DCanvas>("Basic2DCanvas");
        if ( canvas ) {
            // 获取发光层遮罩的 ImGui 纹理 ID
            ImTextureID glowTex =
                (ImTextureID)(VkDescriptorSet)canvas->getGlowDescriptorSet();

            if ( glowTex ) {
                ImGui::Text("Main Canvas Glow Mask (RT):");

                // 获取窗口可用宽度
                float availW = ImGui::GetContentRegionAvail().x;
                // 按 16:9 比例显示预览图
                float displayH = availW * (9.0f / 16.0f);

                ImGui::Image(glowTex,
                             ImVec2(availW, displayH),
                             ImVec2(0, 0),
                             ImVec2(1, 1),
                             ImVec4(1, 1, 1, 1),
                             ImVec4(1, 1, 1, 0.5f));

                if ( ImGui::IsItemHovered() ) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "This texture shows the raw geometry mask used for the "
                        "glow effect.");
                    ImGui::EndTooltip();
                }
            } else {
                auto dangerCol = skinCfg.getColor("ui.danger");
                ImGui::TextColored(
                    ImVec4(dangerCol.r, dangerCol.g, dangerCol.b, dangerCol.a),
                    "Glow Mask texture not available.");
            }
        } else {
            auto warningCol = skinCfg.getColor("ui.warning");
            ImGui::TextColored(
                ImVec4(warningCol.r, warningCol.g, warningCol.b, warningCol.a),
                "Basic2DCanvas ('Basic2DCanvas') not found.");
        }
    }
    ImGui::End();
}

}  // namespace MMM::UI
