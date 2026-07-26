#include "ui/imgui/menu/actions/MainMenuToolsActions.h"

#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKContext.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace MMM::UI
{
namespace
{

/// @brief 用户在插件列表中请求的单次热开关操作。
struct PendingPluginToggle {
    /// @brief 配置根目录相对插件 ID。
    std::string pluginId;
    /// @brief 操作后的启用状态。
    bool enabled{ true };
};

/// @brief 打开插件列表并提供持久化热开关。
class OpenPluginListAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 打开插件列表窗口。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        if ( m_pluginDirectoryLabel.empty() ) {
            m_pluginDirectoryLabel =
                Config::pathToUtf8(Config::AppPaths::themePluginsRootPath());
        }
        m_showWindow = true;
    }

    /// @brief 渲染插件列表窗口并在行绘制结束后执行热重载。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：窗口打开时只读取内存清单；文件系统访问、配置保存
    /// 和 Lua 执行仅在用户切换复选框时发生。
    void renderDeferred(MainMenuContext& context) override
    {
        if ( !m_showWindow ) return;

        ImGui::SetNextWindowSize(
            ImVec2(620.0f * context.dpiScale, 440.0f * context.dpiScale),
            ImGuiCond_FirstUseEver);
        const std::string windowTitle =
            std::string(TR("ui.tools.plugin_list.title").data()) +
            "###PluginListWindow";
        const bool wasOpenBeforeBegin = m_showWindow;
        const bool opened = ImGui::Begin(windowTitle.c_str(), &m_showWindow);
        FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &m_showWindow);

        std::optional<PendingPluginToggle> pendingToggle;
        auto graphicContext = Graphic::VKContext::get();
        if ( opened ) {
            if ( !graphicContext ) {
                ImGui::TextDisabled(
                    "%s", TR("ui.tools.plugin_list.unavailable").data());
            } else {
                renderPluginList(graphicContext->get(),
                                 m_pluginDirectoryLabel,
                                 pendingToggle);
            }
        }
        ImGui::End();

        if ( pendingToggle && graphicContext ) {
            applyPendingToggle(
                context, graphicContext->get(), std::move(*pendingToggle));
        }
    }

private:
    /// @brief 渲染最近一次扫描得到的插件文件清单。
    /// @param graphicContext 图形上下文。
    /// @param pluginDirectoryLabel 已缓存的主题插件目录显示文本。
    /// @param pendingToggle 接收本帧用户请求，避免遍历时重建清单。
    /// @warning UI 热路径：窗口打开时每帧执行；禁止文件系统访问和 Lua 执行。
    static void renderPluginList(
        Graphic::VKContext&                 graphicContext,
        const std::string&                  pluginDirectoryLabel,
        std::optional<PendingPluginToggle>& pendingToggle)
    {
        ImGui::TextDisabled("%s", pluginDirectoryLabel.c_str());
        ImGui::Separator();

        const auto& plugins = graphicContext.getThemeRegistry().plugins();
        if ( plugins.empty() ) {
            ImGui::TextDisabled("%s", TR("ui.tools.plugin_list.empty").data());
            return;
        }

        if ( ImGui::BeginChild("PluginListEntries",
                               ImVec2(0.0f, 0.0f),
                               false,
                               ImGuiWindowFlags_AlwaysVerticalScrollbar) ) {
            for ( const auto& plugin : plugins ) {
                ImGui::PushID(plugin.id.c_str());
                bool enabled = plugin.enabled;
                if ( FeedbackCheckbox("##PluginEnabled", &enabled) ) {
                    pendingToggle = PendingPluginToggle{ plugin.id, enabled };
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(plugin.id.c_str());

                ImGui::Indent();
                renderPluginStatus(plugin);
                ImGui::Unindent();
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    /// @brief 渲染单个插件最近一次加载状态。
    /// @param plugin 插件文件状态。
    static void renderPluginStatus(const Graphic::ThemePluginInfo& plugin)
    {
        if ( !plugin.enabled ) {
            ImGui::TextDisabled(
                "%s", TR("ui.tools.plugin_list.state.disabled").data());
            return;
        }

        if ( plugin.errorCount == 0 ) {
            ImGui::TextDisabled("%s",
                                TR_FMT("ui.tools.plugin_list.state.loaded",
                                       plugin.loadedThemeCount)
                                    .c_str());
            return;
        }

        const ImVec4 dangerColor = Utils::UIThemeUtils::getDangerColor();
        if ( plugin.loadedThemeCount == 0 ) {
            ImGui::TextColored(
                dangerColor,
                "%s",
                TR_FMT("ui.tools.plugin_list.state.failed", plugin.errorCount)
                    .c_str());
        } else {
            ImGui::TextColored(dangerColor,
                               "%s",
                               TR_FMT("ui.tools.plugin_list.state.partial",
                                      plugin.loadedThemeCount,
                                      plugin.errorCount)
                                   .c_str());
        }
        if ( !plugin.firstError.empty() ) {
            ImGui::PushStyleColor(ImGuiCol_Text, dangerColor);
            ImGui::TextWrapped("%s", plugin.firstError.c_str());
            ImGui::PopStyleColor();
        }
    }

    /// @brief 应用用户请求并发布持久化或加载结果。
    /// @param context 单帧主菜单上下文。
    /// @param graphicContext 图形上下文。
    /// @param toggle 待执行开关。
    /// @warning 低频插件管理路径：会保存配置、访问文件系统并执行 Lua。
    static void applyPendingToggle(MainMenuContext&    context,
                                   Graphic::VKContext& graphicContext,
                                   PendingPluginToggle toggle)
    {
        const bool saved =
            graphicContext.setPluginEnabled(toggle.pluginId, toggle.enabled);
        const auto* updatedPlugin =
            graphicContext.getThemeRegistry().findPlugin(toggle.pluginId);

        std::string message;
        if ( !saved ) {
            message = TR_FMT("ui.tools.plugin_list.toggle.save_failed",
                             toggle.pluginId);
        } else if ( toggle.enabled && updatedPlugin &&
                    updatedPlugin->errorCount != 0 ) {
            message = TR_FMT("ui.tools.plugin_list.toggle.load_failed",
                             toggle.pluginId);
        } else {
            message =
                TR_FMT(toggle.enabled ? "ui.tools.plugin_list.toggle.enabled"
                                      : "ui.tools.plugin_list.toggle.disabled",
                       toggle.pluginId);
        }
        context.statusMessageSink.showStatusMessage(std::move(message), 4.0f);
    }

    /// @brief 插件列表窗口是否显示。
    bool m_showWindow{ false };

    /// @brief 打开窗口时缓存的主题插件目录显示文本，避免每帧触发路径创建。
    std::string m_pluginDirectoryLabel;
};

}  // namespace

std::unique_ptr<IMainMenuItemActionHandler> createOpenPluginListAction()
{
    return std::make_unique<OpenPluginListAction>();
}

}  // namespace MMM::UI
