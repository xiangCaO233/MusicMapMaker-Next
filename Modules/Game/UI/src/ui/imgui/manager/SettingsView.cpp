#include "ui/imgui/manager/SettingsView.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::UI
{

SettingsView::SettingsView(const std::string& subViewName)
    : IUIView(subViewName), ISubView(subViewName), ITextureLoader(subViewName)
{
}

void SettingsView::update(UIManager* sourceManager)
{
    // SettingsView 作为 SubView 时由 FloatingManagerUI 调用 onUpdate，
    // 这里作为 IUIView 实现 update 仅为满足非抽象类要求。
    // 如果将其作为独立 IUIView 注册，可以开启下方代码。
    // LayoutContext ctx{ m_layoutCtx, getSubViewName() };
    // onUpdate(ctx, sourceManager);
}

void SettingsView::onUpdate(LayoutContext& layoutContext,
                            UIManager*     sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float sidebarWidth = std::stof(skinCfg.getLayoutConfig("side_bar.width"));
    float sidebarIconSize =
        std::stof(skinCfg.getLayoutConfig("side_bar.icon_size"));

    CLayHBox rootHBox;
    rootHBox.setPadding(0, 0, 0, 0).setSpacing(0);

    // 左侧图标侧边栏 (宽度与主侧边栏一致)
    rootHBox.addElement(
        "SettingsCategoryList",
        Sizing::Fixed(sidebarWidth),
        Sizing::Grow(),
        [this, sidebarWidth, sidebarIconSize](Clay_BoundingBox r,
                                              bool             isHovered) {
            ImGui::BeginChild(
                "SettingsCategories", { r.width, r.height }, false);

            // 强制 Flat 风格：无圆角、无边框、无间距
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            auto DrawCategoryIcon = [&](SettingsTab tab, const char* tooltip) {
                bool isActive = (m_currentTab == tab);

                // 获取纹理
                Graphic::VKTexture* tex = nullptr;
                if ( m_tabIcons.count(tab) ) tex = m_tabIcons[tab].get();

                if ( isActive ) {
                    // 激活态：深灰色背景（VS Code 风格，与 SideBarUI 一致）
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
                } else {
                    // 非激活态：全透明，仅悬停反馈
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                          ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
                }

                std::string btnId = "##setting_tab_" + std::to_string((int)tab);
                if ( ImGui::Button(btnId.c_str(),
                                   { sidebarWidth, sidebarWidth }) ) {
                    m_currentTab = tab;
                }

                if ( tex ) {
                    ImTextureID imTexId = (ImTextureID)tex->getImTextureID();
                    ImVec2      p_min   = ImGui::GetItemRectMin();
                    ImVec2      p_max   = ImGui::GetItemRectMax();

                    float  iconSize = sidebarIconSize;
                    ImVec2 img_p1   = {
                        p_min.x + (sidebarWidth - iconSize) * 0.5f,
                        p_min.y + (sidebarWidth - iconSize) * 0.5f
                    };
                    ImVec2 img_p2 = { img_p1.x + iconSize,
                                      img_p1.y + iconSize };

                    ImU32 tint = isActive ? IM_COL32_WHITE
                                          : IM_COL32(200, 200, 200, 255);
                    ImGui::GetWindowDrawList()->AddImage(
                        imTexId, img_p1, img_p2, { 0, 0 }, { 1, 1 }, tint);
                }

                if ( ImGui::IsItemHovered() ) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tooltip);
                    ImGui::EndTooltip();
                }

                ImGui::PopStyleColor(3);
            };

            DrawCategoryIcon(SettingsTab::Software,
                             TR_CACHE("ui.settings.software").data());
            DrawCategoryIcon(SettingsTab::Visual,
                             TR_CACHE("ui.settings.visual").data());
            DrawCategoryIcon(SettingsTab::Project,
                             TR_CACHE("ui.settings.project").data());
            DrawCategoryIcon(SettingsTab::Editor,
                             TR_CACHE("ui.settings.editor").data());

            ImGui::PopStyleVar(4);
            ImGui::EndChild();
        });

    // 右侧内容区
    rootHBox.addElement(
        "SettingsContentArea",
        Sizing::Grow(),
        Sizing::Grow(),
        [this](Clay_BoundingBox r, bool isHovered) {
            ImGui::BeginChild("SettingsContent", { r.width, r.height }, false);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

            switch ( m_currentTab ) {
            case SettingsTab::Software: drawSoftwareSettings(); break;
            case SettingsTab::Visual: drawVisualSettings(); break;
            case SettingsTab::Project: drawProjectSettings(); break;
            case SettingsTab::Editor: drawEditorSettings(); break;
            }

            ImGui::PopStyleVar();
            ImGui::EndChild();
        });

    rootHBox.render(layoutContext);
}

void SettingsView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                  vk::Device&         logicalDevice,
                                  vk::CommandPool& cmdPool, vk::Queue& queue)
{
    auto&          skin     = Config::SkinManager::instance();
    const uint32_t iconSize = 48;

    m_tabIcons[SettingsTab::Software] =
        loadTextureResource(skin.getAssetPath("settings.software"),
                            iconSize,
                            physicalDevice,
                            logicalDevice,
                            cmdPool,
                            queue,
                            { { .83f, .83f, .83f, .83f } });

    m_tabIcons[SettingsTab::Visual] =
        loadTextureResource(skin.getAssetPath("settings.visual"),
                            iconSize,
                            physicalDevice,
                            logicalDevice,
                            cmdPool,
                            queue,
                            { { .83f, .83f, .83f, .83f } });

    m_tabIcons[SettingsTab::Project] =
        loadTextureResource(skin.getAssetPath("settings.project"),
                            iconSize,
                            physicalDevice,
                            logicalDevice,
                            cmdPool,
                            queue,
                            { { .83f, .83f, .83f, .83f } });

    m_tabIcons[SettingsTab::Editor] =
        loadTextureResource(skin.getAssetPath("settings.editor"),
                            iconSize,
                            physicalDevice,
                            logicalDevice,
                            cmdPool,
                            queue,
                            { { .83f, .83f, .83f, .83f } });

    XINFO("SettingsView textures reloaded.");
    m_needReload = false;
}

void SettingsView::drawSoftwareSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();

    ImGui::SeparatorText(TR_CACHE("ui.settings.software.general").data());

    // 1. 语言选择 (示例，目前只有 zh_cn 和 en_us)
    const char* langs[]     = { "简体中文 (zh_cn)", "English (en_us)" };
    static int  currentLang = 0;  // TODO: 从配置中获取
    if ( ImGui::Combo(TR_CACHE("ui.settings.software.language").data(),
                      &currentLang,
                      langs,
                      IM_ARRAYSIZE(langs)) ) {
        // TODO: 切换语言逻辑
    }

    // 2. 文件选择器样式
    int pickerStyle = (int)settings.filePickerStyle;
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.picker_native").data(),
             pickerStyle == (int)Config::FilePickerStyle::Native) ) {
        settings.filePickerStyle = Config::FilePickerStyle::Native;
        Config::AppConfig::instance().save();
    }
    ImGui::SameLine();
    if ( ImGui::RadioButton(
             TR_CACHE("ui.settings.software.picker_unified").data(),
             pickerStyle == (int)Config::FilePickerStyle::Unified) ) {
        settings.filePickerStyle = Config::FilePickerStyle::Unified;
        Config::AppConfig::instance().save();
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.software.sync").data());
    // 3. 同步模式
    int         syncMode    = (int)settings.syncConfig.mode;
    const char* syncModes[] = { "None", "Integral", "WaterTank" };
    if ( ImGui::Combo(TR_CACHE("ui.settings.software.sync_mode").data(),
                      &syncMode,
                      syncModes,
                      IM_ARRAYSIZE(syncModes)) ) {
        settings.syncConfig.mode = (Config::SyncMode)syncMode;
        Config::AppConfig::instance().save();
    }

    if ( settings.syncConfig.mode == Config::SyncMode::Integral ) {
        ImGui::SliderFloat(TR_CACHE("ui.settings.software.sync_factor").data(),
                           &settings.syncConfig.integralFactor,
                           0.0f,
                           1.0f);
    } else if ( settings.syncConfig.mode == Config::SyncMode::WaterTank ) {
        ImGui::SliderFloat(TR_CACHE("ui.settings.software.sync_buffer").data(),
                           &settings.syncConfig.waterTankBuffer,
                           0.0f,
                           0.5f);
    }
}

void SettingsView::drawVisualSettings()
{
    auto& visual = Config::AppConfig::instance().getVisualConfig();

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.judgeline").data());
    ImGui::SliderFloat(TR_CACHE("ui.settings.visual.judgeline_pos").data(),
                       &visual.judgeline_pos,
                       0.0f,
                       1.0f);
    ImGui::SliderFloat(TR_CACHE("ui.settings.visual.judgeline_width").data(),
                       &visual.judgelineWidth,
                       1.0f,
                       10.0f);

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.note").data());
    ImGui::SliderFloat(TR_CACHE("ui.settings.visual.note_scale_x").data(),
                       &visual.noteScaleX,
                       0.5f,
                       3.0f);
    ImGui::SliderFloat(TR_CACHE("ui.settings.visual.note_scale_y").data(),
                       &visual.noteScaleY,
                       0.5f,
                       3.0f);

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.background").data());
    ImGui::SliderFloat(TR_CACHE("ui.settings.visual.bg_darken").data(),
                       &visual.background.darken_ratio,
                       0.0f,
                       1.0f);

    ImGui::SeparatorText(TR_CACHE("ui.settings.visual.offset").data());
    if ( ImGui::DragFloat(TR_CACHE("ui.settings.visual.visual_offset").data(),
                          &visual.visualOffset,
                          0.001f,
                          -0.5f,
                          0.5f,
                          "%.3f s") ) {
        Config::AppConfig::instance().save();
    }
}

void SettingsView::drawProjectSettings()
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    if ( !project ) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1),
                           "%s",
                           TR_CACHE("ui.settings.project.no_project").data());
        return;
    }

    ImGui::SeparatorText(TR_CACHE("ui.settings.project.info").data());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.name").data(),
                project->m_metadata.m_title.c_str());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.artist").data(),
                project->m_metadata.m_artist.c_str());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.mapper").data(),
                project->m_metadata.m_mapper.c_str());
    ImGui::Text("%s: %s",
                TR_CACHE("ui.settings.project.path").data(),
                project->m_projectRoot.string().c_str());

    // TODO: 允许修改项目元数据
}

void SettingsView::drawEditorSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();

    ImGui::SeparatorText(TR_CACHE("ui.settings.editor.sfx").data());

    int         strategy     = (int)settings.sfxConfig.polylineStrategy;
    const char* strategies[] = {
        "Exact", "InternalAsNormal", "OnlyTailExact", "AllAsNormal"
    };
    if ( ImGui::Combo(TR_CACHE("ui.settings.editor.sfx_strategy").data(),
                      &strategy,
                      strategies,
                      IM_ARRAYSIZE(strategies)) ) {
        settings.sfxConfig.polylineStrategy =
            (Config::PolylineSfxStrategy)strategy;
        Config::AppConfig::instance().save();
    }

    ImGui::Checkbox(TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
                    &settings.sfxConfig.enableFlickWidthVolumeScaling);
    if ( settings.sfxConfig.enableFlickWidthVolumeScaling ) {
        ImGui::SliderFloat(TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
                           &settings.sfxConfig.flickWidthVolumeMultiplier,
                           0.0f,
                           1.0f);
    }
}

}  // namespace MMM::UI
