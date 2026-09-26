#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/theme/ImGuiThemeRegistry.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/plugin/ToolPluginView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace MMM::UI
{
namespace
{

/// @brief 用户在插件列表中请求的单次热开关操作。
/// @details 遍历阶段只记录请求，清单重建延迟到所有行绘制结束后执行。
struct PendingPluginToggle {
    /// @brief 配置根目录相对插件 ID。
    /// @note 使用稳定 ID 而不是 ThemePluginInfo 地址，避免重载后悬空。
    std::string pluginId;
    /// @brief 操作后的启用状态。
    /// @note true 表示保存启用配置并尝试立即加载插件。
    bool enabled{ true };
};

/// @brief 打开插件列表并提供持久化热开关。
/// @details 窗口每帧只读取注册表快照，用户切换后才执行配置保存与热重载。
class OpenPluginListAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 打开插件列表窗口。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    /// @note 插件目录文本首次打开时缓存，避免窗口绘制阶段反复转换路径。
    /// @warning 路径转换只允许发生在显式打开动作中，不得移入逐帧渲染。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        if ( m_pluginDirectoryLabel.empty() ) {
            // AppPaths 返回原生路径，此处一次性转换为 ImGui 使用的 UTF-8 文本。
            m_pluginDirectoryLabel =
                Config::pathToUtf8(Config::AppPaths::themePluginsRootPath());
        }
        // 重复执行只保持窗口打开，不会重新载入插件或重置清单。
        m_showWindow = true;
    }

    /// @brief 渲染插件列表窗口并在行绘制结束后执行热重载。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：窗口打开时只读取内存清单；文件系统访问、配置保存
    /// 和 Lua 执行仅在用户切换复选框时发生。
    /// @note 开关请求在 ImGui::End 后应用，防止遍历中的注册表失效。
    void renderDeferred(MainMenuContext& context) override
    {
        // 窗口关闭时不获取图形上下文，也不构造标题字符串。
        if ( !m_showWindow ) return;

        // FirstUseEver 仅提供初始尺寸，保留用户后续调整结果。
        ImGui::SetNextWindowSize(
            ImVec2(620.0f * context.dpiScale, 440.0f * context.dpiScale),
            ImGuiCond_FirstUseEver);
        // 标题可本地化，### 后的管理窗口 ID 才是持久化停靠身份。
        const std::string windowTitle =
            TR("ui.tools.plugin_list.title").toString() + "###PluginListWindow";
        if ( m_pendingInitialDock ) {
            // 旧版浮动记录不会响应 FirstUseEver，打开时迁回主 DockSpace。
            // 既有 DockId 则保留用户在项目工作区创建的标签或分栏。
            const ImGuiID centerDockId = MainDockSpaceUI::getCenterDockId();
            if ( centerDockId != 0 ) {
                const auto* saved = ImGui::FindWindowSettingsByID(
                    ImHashStr(windowTitle.c_str()));
                const auto* existing =
                    ImGui::FindWindowByName(windowTitle.c_str());
                // 仅初次显示时迁回浮动窗口；之后不覆盖用户拖放的结果。
                // 如果节点本帧尚不可用，保留请求至实际停靠成功。
                // 旧配置常只有 Pos/Size，这仍是已保存的窗口；FirstUseEver
                // 对它无效，因此以缺少 DockId 作为迁移依据。
                const bool floating = saved ? saved->DockId == 0
                                            : existing && existing->DockId == 0;
                ImGui::SetNextWindowDockID(
                    centerDockId,
                    floating ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
            }
        }
        // 即使从 DockSpace 拖出，也继续使用应用主视口而非平台独立窗口。
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        const bool wasOpenBeforeBegin = m_showWindow;
        // Begin 返回折叠状态；无论返回值如何都必须调用 End。
        const bool opened = ImGui::Begin(windowTitle.c_str(), &m_showWindow);
        // 真实 DockNode 建立后才撤销初始停靠请求。
        // 这样首次打开时主 DockSpace 尚在构建，也不会把窗口永久留在浮动层。
        if ( m_pendingInitialDock && ImGui::IsWindowDocked() )
            m_pendingInitialDock = false;
        // 统一关闭按钮反馈只比较 Begin 前后的可见性值。
        FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &m_showWindow);

        // 每帧最多保存一个开关请求，避免重载过程中使用旧清单元素。
        std::optional<PendingPluginToggle> pendingToggle;
        // 获取轻量上下文引用包装，不复制底层图形资源所有权。
        auto graphicContext = Graphic::VKContext::get();
        if ( opened ) {
            // 工具插件清单来自启动或显式重载的内存快照；打开按钮只修改窗口位。
            if ( context.sourceManager ) {
                if ( auto* toolView =
                         context.sourceManager->getView<ToolPluginView>(
                             "ToolPluginView") ) {
                    ImGui::TextUnformatted("工具插件");
                    // 工具状态来自内存快照；此窗口每帧不会重新扫描用户目录。
                    for ( const auto& tool : toolView->plugins() ) {
                        ImGui::PushID(tool.id.c_str());
                        if ( tool.available ) {
                            if ( FeedbackButton(tool.name.c_str()) ) {
                                (void)toolView->openPlugin(tool.id);
                            }
                        } else {
                            // 无效脚本保留来源与诊断，避免提供无效果的打开按钮。
                            // 点击重载后会重新校验，修复脚本无需重启应用。
                            ImGui::TextUnformatted(tool.name.c_str());
                        }
                        if ( !tool.error.empty() ) {
                            ImGui::SameLine();
                            ImGui::TextWrapped("%s", tool.error.c_str());
                        }
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                }
            }
            if ( !graphicContext ) {
                // 图形上下文未就绪时只显示不可用提示，窗口仍可安全关闭。
                ImGui::TextDisabled(
                    "%s", TR("ui.tools.plugin_list.unavailable").data());
            } else {
                // 清单来自最近一次扫描结果，渲染函数不得自行刷新文件系统。
                renderPluginList(graphicContext->get(),
                                 m_pluginDirectoryLabel,
                                 pendingToggle);
            }
        }
        // 与 Begin 无条件配对，包括折叠和上下文不可用分支。
        ImGui::End();

        // 离开清单遍历后才允许开关插件并重建主题注册表。
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
    /// @note pluginDirectoryLabel 已在打开动作中转换，不触发逐帧路径操作。
    static void renderPluginList(
        Graphic::VKContext&                 graphicContext,
        const std::string&                  pluginDirectoryLabel,
        std::optional<PendingPluginToggle>& pendingToggle)
    {
        // 目录作为只读上下文显示，帮助用户确认插件扫描位置。
        ImGui::TextDisabled("%s", pluginDirectoryLabel.c_str());
        ImGui::Separator();

        // 引用注册表内部稳定快照，只在本次渲染调用期间使用。
        const auto& plugins = graphicContext.getThemeRegistry().plugins();
        if ( plugins.empty() ) {
            // 空清单使用禁用文本，不提供会产生无效操作的控件。
            ImGui::TextDisabled("%s", TR("ui.tools.plugin_list.empty").data());
            return;
        }

        // 子区域承担长列表滚动，避免扩张外层工具窗口。
        if ( ImGui::BeginChild("PluginListEntries",
                               ImVec2(0.0f, 0.0f),
                               false,
                               ImGuiWindowFlags_AlwaysVerticalScrollbar) ) {
            for ( const auto& plugin : plugins ) {
                // 插件 ID 形成行级 ImGui 标识，允许所有复选框共享可见标签。
                ImGui::PushID(plugin.id.c_str());
                // 先复制启用值，只有用户修改后才生成待处理请求。
                bool enabled = plugin.enabled;
                if ( FeedbackCheckbox("##PluginEnabled", &enabled) ) {
                    // 后续点击覆盖前一请求，使当前帧最多触发一次注册表重建。
                    pendingToggle = PendingPluginToggle{ plugin.id, enabled };
                }
                // ID 与状态同行显示，构成清晰的插件主标题。
                ImGui::SameLine();
                ImGui::TextUnformatted(plugin.id.c_str());

                // 加载状态缩进到插件 ID 下方，表达从属关系。
                ImGui::Indent();
                renderPluginStatus(plugin);
                ImGui::Unindent();
                // 分隔相邻插件行，错误详情较长时仍能辨识边界。
                ImGui::Separator();
                // 每次循环恢复 ID 栈，防止污染后续插件或外层控件。
                ImGui::PopID();
            }
        }
        // BeginChild 无论内容可见性返回值如何都必须配对 EndChild。
        ImGui::EndChild();
    }

    /// @brief 渲染单个插件最近一次加载状态。
    /// @param plugin 插件文件状态。
    /// @warning UI 热路径：只读取内存状态并构造短翻译文本。
    /// @note 禁用、完整加载、完全失败和部分加载四种状态互斥显示。
    static void renderPluginStatus(const Graphic::ThemePluginInfo& plugin)
    {
        if ( !plugin.enabled ) {
            // 用户禁用优先于历史加载计数，避免呈现过期错误。
            ImGui::TextDisabled(
                "%s", TR("ui.tools.plugin_list.state.disabled").data());
            return;
        }

        if ( plugin.errorCount == 0 ) {
            // 零错误时显示成功加载的主题数量并提前结束。
            ImGui::TextDisabled("%s",
                                TR_FMT("ui.tools.plugin_list.state.loaded",
                                       plugin.loadedThemeCount)
                                    .c_str());
            return;
        }

        // 所有失败状态统一使用主题危险色。
        const ImVec4 dangerColor = Utils::UIThemeUtils::getDangerColor();
        if ( plugin.loadedThemeCount == 0 ) {
            // 没有任何主题载入时展示完全失败文案。
            ImGui::TextColored(
                dangerColor,
                "%s",
                TR_FMT("ui.tools.plugin_list.state.failed", plugin.errorCount)
                    .c_str());
        } else {
            // 同时存在已载入主题与错误时展示部分成功统计。
            ImGui::TextColored(dangerColor,
                               "%s",
                               TR_FMT("ui.tools.plugin_list.state.partial",
                                      plugin.loadedThemeCount,
                                      plugin.errorCount)
                                   .c_str());
        }
        if ( !plugin.firstError.empty() ) {
            // 只展示首条错误作为摘要，完整错误由日志或诊断入口承担。
            ImGui::PushStyleColor(ImGuiCol_Text, dangerColor);
            // 使用换行文本避免长 Lua 错误横向撑大窗口。
            ImGui::TextWrapped("%s", plugin.firstError.c_str());
            // 恢复文本颜色，防止影响下一插件行。
            ImGui::PopStyleColor();
        }
    }

    /// @brief 应用用户请求并发布持久化或加载结果。
    /// @param context 单帧主菜单上下文。
    /// @param graphicContext 图形上下文。
    /// @param toggle 待执行开关。
    /// @warning 低频插件管理路径：会保存配置、访问文件系统并执行 Lua。
    /// @note 调用发生在插件列表遍历结束后，允许注册表安全重建。
    static void applyPendingToggle(MainMenuContext&    context,
                                   Graphic::VKContext& graphicContext,
                                   PendingPluginToggle toggle)
    {
        // 图形上下文统一保存启用状态，并在需要时加载或卸载插件。
        const bool saved =
            graphicContext.setPluginEnabled(toggle.pluginId, toggle.enabled);
        // 重载可能替换元素地址，因此必须用稳定 ID 重新查询结果。
        const auto* updatedPlugin =
            graphicContext.getThemeRegistry().findPlugin(toggle.pluginId);

        std::string message;
        if ( !saved ) {
            // 配置未保存时优先报告持久化失败，不能宣称状态已生效。
            message = TR_FMT("ui.tools.plugin_list.toggle.save_failed",
                             toggle.pluginId);
        } else if ( toggle.enabled && updatedPlugin &&
                    updatedPlugin->errorCount != 0 ) {
            // 启用成功但加载有错误时给出独立失败摘要。
            message = TR_FMT("ui.tools.plugin_list.toggle.load_failed",
                             toggle.pluginId);
        } else {
            // 禁用或无错误启用均使用目标状态对应的成功消息。
            message =
                TR_FMT(toggle.enabled ? "ui.tools.plugin_list.toggle.enabled"
                                      : "ui.tools.plugin_list.toggle.disabled",
                       toggle.pluginId);
        }
        // 状态栏接管消息生命周期，菜单动作不持有跨帧反馈文本。
        context.statusMessageSink.showStatusMessage(std::move(message), 4.0f);
    }

    /// @brief 插件列表窗口是否显示。
    /// @note execute 置位，ImGui 窗口关闭按钮直接清除。
    bool m_showWindow{ false };

    /// @brief 管理面板首次显示时等待有效主 DockSpace 节点。
    bool m_pendingInitialDock{ true };

    /// @brief 打开窗口时缓存的主题插件目录显示文本，避免每帧触发路径创建。
    /// @note 目录在应用运行期间稳定，关闭窗口时无需清空。
    std::string m_pluginDirectoryLabel;
};

}  // namespace

/// @brief 创建插件列表窗口动作处理器。
/// @return 独占所有权的插件列表处理器。
/// @warning 处理器及其窗口必须在图形上下文所属 UI 线程使用。
std::unique_ptr<IMainMenuItemActionHandler> createOpenPluginListAction()
{
    return std::make_unique<OpenPluginListAction>();
}

}  // namespace MMM::UI
