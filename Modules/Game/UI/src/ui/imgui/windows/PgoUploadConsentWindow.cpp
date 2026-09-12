#include "ui/imgui/windows/PgoUploadConsentWindow.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>
#include <string>

/// @file PgoUploadConsentWindow.cpp
/// @brief PGO profile 自动上传的一次性授权弹窗实现。
/// @details 编译开关隔离常规产物与插桩产物；授权结果写入软件配置，窗口自身
/// 不负责上传、网络访问或 profile 文件管理。
/// @note 弹窗必须保留稳定 ImGui ID，避免本地化标题变化破坏模态状态。

namespace MMM::UI
{

/// @brief 在尚未记录授权选择时渲染 PGO 上传授权窗口。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：每帧执行；仅在 PGO 插桩构建且尚未授权时绘制。
/// @details 非 PGO 构建编译为空操作；插桩构建中弹窗不可绕过选择，接受与
/// 拒绝都会持久化“已询问”状态，防止后续启动反复提示。
void PgoUploadConsentWindow::render(float dpiScale) const
{
#ifndef MMM_PGO_INSTRUMENT
    // 非插桩产物不收集 PGO 数据，也不读取或修改授权配置。
    (void)dpiScale;
#else
    // PGO 构建从全局软件配置读取授权状态。
    auto& appConfig = Config::AppConfig::instance();
    auto& settings  = appConfig.getEditorSettings();
    // 已记录明确选择后常规帧立即返回。
    if ( settings.pgoProfileUploadConsentAsked ) return;

    // 可见标题本地化，### 后内部 ID 保持稳定。
    const std::string popupId =
        TR("ui.pgo.consent.title").toString() + "###PgoUploadConsentModal";
    // 在未选择期间持续确保模态弹窗保持打开。
    ::MMM::UI::FeedbackOpenPopup(popupId.c_str());

    // 居中作用域统一处理位置、圆角和 DPI 样式。
    Utils::CenteredModalPopupScope popupStyle(dpiScale);
    if ( !popupStyle.begin(popupId.c_str(),
                           nullptr,
                           ImGuiWindowFlags_None,
                           ImVec2(620.0f * dpiScale, 0.0f)) ) {
        // 弹窗本帧未实际开始时不绘制内容，也不调用 EndPopup。
        return;
    }

    // 主说明和详细说明均自动换行以适配 620 逻辑像素宽度。
    ImGui::TextWrapped("%s", TR("ui.pgo.consent.message").data());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", TR("ui.pgo.consent.detail").data());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 两个按钮共享原子式配置更新与关闭逻辑。
    const auto applyConsent = [&](bool allowUpload) {
        // 先保存实际选择，再标记已经询问。
        settings.autoUploadPgoProfiles        = allowUpload;
        settings.pgoProfileUploadConsentAsked = true;
        // 选择需要跨启动保留；保存失败由配置系统统一记录。
        appConfig.save();
        ImGui::CloseCurrentPopup();
    };

    // 两个等宽按钮组成居中操作行，尺寸按 DPI 缩放。
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2      buttonSize(128.0f * dpiScale, 0.0f);
    const float buttonRowWidth = buttonSize.x * 2.0f + style.ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    if ( availableWidth > buttonRowWidth ) {
        // 只在空间充足时右移到居中位置，窄区域保持左侧可见。
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (availableWidth - buttonRowWidth) * 0.5f);
    }

    if ( ::MMM::UI::FeedbackButton(TR("ui.pgo.consent.accept").data(),
                                   buttonSize) ) {
        // 接受允许后续自动上传 PGO profile。
        applyConsent(true);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(TR("ui.pgo.consent.decline").data(),
                                   buttonSize) ) {
        // 拒绝仍记录已询问，但关闭自动上传。
        applyConsent(false);
    }

    // 与 CenteredModalPopupScope::begin 成功路径严格配对。
    ImGui::EndPopup();
#endif
}

}  // namespace MMM::UI
