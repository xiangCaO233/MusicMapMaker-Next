#include "ui/workspace/MobileDrawerMenus.h"

#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/MobileButton.h"

#include <algorithm>
#include <array>
#include <imgui.h>
#include <span>

namespace MMM::UI
{
namespace
{
/// @brief 启用条件只描述入口能力，执行时公共动作还会再次复核。
enum class Requirement {
    None,     ///< 不要求打开谱面，例如插件列表和关于。
    Project,  ///< BPM 测量允许项目存在但尚未加载谱面。
    Beatmap,  ///< 编辑和导出要求活动谱面，不能把欢迎页当作目标。
    Desktop   ///< 平台能力缺失时保留说明，不执行失败的系统命令。
};
/// @brief 移动入口只借用稳定字符串，不持有桌面视图或后台谱面。
/// @details 描述表属于移动布局，不能传给桌面菜单改变其注册顺序。
/// 动作名称必须与公共注册表逐字一致，翻译只用于显示而不用于查找。
/// 插件可用性和导出任务状态仍由执行目标最终校验。
/// 新增项目应同时提供语义图标和翻译键，不依赖 Hover 说明用途。
struct Entry {
    const char* m_action;  ///< 与桌面注册表一致的语义身份。
    const char* m_label;   ///< 可见文本按当前语言解析。
    const char* m_icon;    ///< 已加载图标字体中的语义字形。
    Requirement m_requirement{ Requirement::None };  ///< 入口前置条件。
};

/// @brief 两列按钮共用整行高度，长英文不挤压相邻按钮。
/// @param entries 固定入口表，不在绘制时创建或注册动作。
/// @param manager 只借用当前帧的应用上下文。
/// @param scale 最小指尖命中高度的缩放。
/// @return 任一普通动作成功派发时返回真。
/// @warning 热路径不复制共享所有权；谱面检查使用现有轻量查询。
/// @details 所有条目保持固定顺序，灰色项目不能被折叠后替换为别的动作。
/// 第一列和第二列均通过统一触摸按钮，手指从标签起滑仍能交给滚动器。
/// 该函数不会保存已注册处理器的指针，生命周期由主菜单注册表保证。
/// 不在这里调用工具的 update 或 renderDeferred，避免后台任务重复推进。
/// 用户选中不同谱面后下一帧重新查询条件，不能缓存项目观察指针。
/// 返回值仅表示派发成功，不表示后台导出或更新下载已经完成。
bool actions(std::span<const Entry> entries, UIManager& manager, float scale)
{
    // 父窗口已经扣除内边距与滚动条，这里只扣两列之间的一次间距。
    // 不使用桌面菜单的内容宽度，避免长标题撑大移动抽屉。
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width =
        std::max(1.0f, (ImGui::GetContentRegionAvail().x - gap) * .5f);
    // 不从窗口焦点猜测编辑目标；上下文检查与桌面工具入口保持一致。
    const bool project =
        Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    const bool beatmap   = MenuUtil::hasActiveBeatmap(true);
    bool       activated = false;
    for ( std::size_t i = 0; i < entries.size(); i += 2 ) {
        // 最小高度保留指尖目标；同行中较长标签决定两侧共同高度。
        // 奇数末项保持半宽，下一分组仍从完整新行开始。
        ImVec2 size{ width, 48 * scale };
        // 同行先完成测量，再绘制任一按钮，保持下一行起点稳定。
        for ( std::size_t j = i; j < std::min(i + 2, entries.size()); ++j )
            size.y = std::max(
                size.y,
                mobileActionButtonSize(TR(entries[j].m_label).data(), size).y);
        for ( std::size_t j = i; j < std::min(i + 2, entries.size()); ++j ) {
            const auto& entry = entries[j];
            const bool  enabled =
                entry.m_requirement == Requirement::None ||
                (entry.m_requirement == Requirement::Project && project) ||
                (entry.m_requirement == Requirement::Beatmap && beatmap);
            // 桌面目录与停靠专用入口显式禁用，不能在安卓调用 xdg-open。
            // 禁用区域只包围一个按钮，不能让项目门禁泄漏到插件与帮助入口。
            // 描述表不拥有动作，不能为检测可用性重复创建动作处理器。
            ImGui::BeginDisabled(!enabled);
            if ( MobileActionButton(entry.m_action,
                                    entry.m_icon,
                                    TR(entry.m_label).data(),
                                    size) )
                activated |=
                    Interaction::executeAction(entry.m_action, &manager);
            // 执行失败时保留抽屉，让用户能切换项目后重试。
            // 成功结果在整组布局结束后交给外壳，不在按钮作用域内销毁宿主。
            ImGui::EndDisabled();
            if ( j == i && j + 1 < entries.size() ) ImGui::SameLine(0, gap);
        }
    }
    return activated;
}

/// @brief 分组开合状态放在当前抽屉的局部存储，避免引入全局状态。
/// @param id 固定身份独立于翻译和语言切换。
/// @param key 不含桌面键盘助记符的移动分组翻译键。
/// @param scale 触摸高度倍率。
/// @return 当前分组展开时返回真。
/// @warning 每帧仅访问窗口存储，不保存文件或持有字体指针。
bool section(const char* id, const char* key, float scale)
{
    // 状态存在当前窗口中；关闭抽屉后仍可保持分组展开位置。
    // 标题取翻译键，内部 ID 不随语言变化，因此切换语言不会折叠内容。
    auto*      storage = ImGui::GetStateStorage();
    const auto stateId = ImGui::GetID(id);
    bool       open    = storage->GetBool(stateId, false);
    // 使用普通触摸按钮，轻点释放与拖动滚动共用既有仲裁。
    if ( MobileActionButton(id,
                            open ? ICON_MMM_ARROW_DOWN : ICON_MMM_BARS,
                            TR(key).data(),
                            { ImGui::GetContentRegionAvail().x, 48 * scale },
                            open) ) {
        // 只在释放确认后更新，不把按住分组标题解释为连续切换。
        // 保持同一窗口存储键，手势中途滚动不会重置折叠状态。
        open = !open;
        storage->SetBool(stateId, open);
    }
    return open;
}
}  // namespace

/// @brief 仅组合移动菜单，工具生命周期与弹窗仍由公共动作拥有。
/// @details 返回激活结果后外壳收起抽屉，避免遮罩盖住新打开的工具。
/// 各分组独立折叠，主窗口开关始终留在抽屉顶部。
/// 目录入口受安卓私有存储限制，明确展示说明而不伪造打开成功。
/// @warning 每帧固定表遍历，不扫描插件、访问目录或触发更新请求。
bool drawMobileDrawerMenus(UIManager& manager, float scale)
{
    // 普通工具成功激活才解除背景遮罩；显示偏好切换可在抽屉中连续操作。
    // 打开请求仍由公共菜单的延迟阶段处理，移动绘制不自行 Begin 工具窗口。
    bool activated = false;
    if ( section("MobileTools", "ui.mobile.tools", scale) ) {
        // 测量读取项目音频，但不要求当前已有活动谱面。
        // 检测和元数据入口允许先打开空工具，再由原视图提示选择谱面。
        // 数据源替换与节拍对齐会修改谱面，必须保留原业务的活动目标检查。
        // 对齐沿用同一撤销事务，移动端不能另行取整后绕过历史系统。
        // 模板工具通过公共动作定位时间线，不依赖时间线面板是否可见。
        // 倍速导出沿用后台任务状态，禁止在绘制时直接转码音频。
        // 插件列表与重载沿用应用级实例，不能因抽屉关闭重新创建插件。
        // 列表展示只借用文字与图标，不在此扫描磁盘或重载脚本。
        const std::array entries{ Entry{ "OpenBpmMeasurementAction",
                                         "ui.tools.bpm_measure",
                                         ICON_MMM_MUSIC,
                                         Requirement::Project },
                                  Entry{ "ToggleOverlapCheckWindowAction",
                                         "ui.tools.overlap_check",
                                         ICON_MMM_SELECT_ALL },
                                  Entry{ "ToggleMetadataEditorWindowAction",
                                         "ui.tools.metadata_editor",
                                         ICON_MMM_PEN },
                                  Entry{ "OpenDataSourceReplaceWindowAction",
                                         "ui.tools.data_source_replace",
                                         ICON_MMM_EXCHANGE,
                                         Requirement::Beatmap },
                                  Entry{ "AlignSelectedToCommonBeatsAction",
                                         "ui.tools.format",
                                         ICON_MMM_MAGNET,
                                         Requirement::Beatmap },
                                  Entry{ "OpenTimingTemplateAction",
                                         "ui.tools.timing_template",
                                         ICON_MMM_CLOCK,
                                         Requirement::Beatmap },
                                  Entry{ "OpenBeatmapSpeedExportAction",
                                         "ui.tools.speed_export",
                                         ICON_MMM_SPEEDOMETER,
                                         Requirement::Beatmap },
                                  Entry{ "OpenPluginListAction",
                                         "ui.tools.plugin_list",
                                         ICON_MMM_PUZZLE },
                                  Entry{ "ReloadPluginsAction",
                                         "ui.tools.reload_plugins",
                                         ICON_MMM_REFRESH } };
        activated |= actions(entries, manager, scale);
    }
    if ( section("MobileView", "ui.mobile.view", scale) ) {
        // 直接读取配置中的当前值，其他设置入口的变化下一帧即可反映。
        // 仅点击时通过正式开关动作保存，绘制本身不写配置。
        auto& settings = Config::AppConfig::instance().getEditorSettings();
        // 窗口开关已在顶部提供，此处补充内容显示偏好，不重复维护显隐值。
        if ( MobileActionButton(
                 "AnnotationDetailsToggleAction",
                 ICON_MMM_COMMENT,
                 TR("ui.view.annotation_details").data(),
                 { ImGui::GetContentRegionAvail().x, 48 * scale },
                 settings.showAnnotationDetails) )
            Interaction::executeAction("AnnotationDetailsToggleAction",
                                       &manager);
        // 工具栏内容由移动工作台和参数组分别组合，桌面停靠配置不应用于移动布局。
        ImGui::TextWrapped("%s", TR("ui.mobile.desktop_view_options").data());
    }
    if ( section("MobileHelp", "ui.mobile.help", scale) ) {
        // 欢迎页复用主区标签；更新动作沿用证书校验及更新弹窗。
        // 关于动作拥有跨帧模态状态，不能在抽屉内创建同名第二实例。
        // 安卓应用私有目录对系统文件管理器不可见，不能当成普通路径传出。
        // 明确禁用三个目录按钮并常驻说明，避免给用户无响应的假入口。
        // 该限制只属于移动呈现层，桌面帮助菜单继续正常打开原生目录。
        const std::array entries{
            Entry{ "OpenWelcomeAction", "ui.welcome.title", ICON_MMM_BOOK },
            Entry{ "CheckUpdateAction",
                   "ui.help.check_update",
                   ICON_MMM_DOWNLOAD },
            Entry{ "ShowAboutAction", "ui.help.about", ICON_MMM_INFO_CIRCLE },
            Entry{ "OpenConfigurationAction",
                   "ui.help.open_software_configuration",
                   ICON_MMM_COG,
                   Requirement::Desktop },
            Entry{ "OpenSkinsDirectoryAction",
                   "ui.help.open_skins_directory",
                   ICON_MMM_PAINT_BRUSH,
                   Requirement::Desktop },
            Entry{ "OpenPluginsDirectoryAction",
                   "ui.help.open_plugins_directory",
                   ICON_MMM_PUZZLE,
                   Requirement::Desktop }
        };
        activated |= actions(entries, manager, scale);
        // 私有目录不能交给安卓系统文件管理器，说明始终可见且不依赖 Hover。
        ImGui::TextWrapped(
            "%s", TR("ui.mobile.private_directory_unavailable").data());
    }
    // 此处不结束抽屉窗口，外壳需要先完成自己的 Begin/End 配对。
    // 打开工具后收起只改变外壳遮罩，不关闭工具自身的会话状态。
    return activated;
}
}  // namespace MMM::UI
