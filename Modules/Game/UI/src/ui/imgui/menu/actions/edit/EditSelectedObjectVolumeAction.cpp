#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 当前选中物件中可批量编辑的音量摘要。
/// @details 只统计具有音频绑定且允许编辑的对象，供窗口初始化与启用判断使用。
struct SelectedVolumeState {
    /// @brief 至少包含一个音频绑定的选中物件数量。
    /// @note 一个音符包含多个子音时仍只计为一个选中物件。
    std::size_t objectCount{ 0U };

    /// @brief 全部音频绑定一致时的共同音量。
    /// @note 尚未遇到任何音频绑定时保持空值。
    std::optional<float> commonVolume;

    /// @brief 是否存在两个不同的音量值或非法旧值。
    /// @note mixed 置位后仍继续统计对象数量，但共同值不再适合作为默认输入。
    bool mixed{ false };
};

/// @brief 读取当前选中索引中的可编辑音量摘要。
/// @return 当前活动会话不存在时返回空摘要。
/// @warning UI 低频路径：只在打开或显示编辑窗口时遍历已选实体索引；
/// `shared_ptr` 用于保护会话切换期间的生命周期，现有接口没有稳定观察句柄。
/// @note 只遍历选择索引，不扫描完整 entt registry。
SelectedVolumeState inspectSelectedVolumeState()
{
    SelectedVolumeState state;
    auto&               engine = Logic::EditorEngine::instance();
    // 会话锁覆盖活动 Session 获取和全部选择读取，防止切换时引用失效。
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    // shared_ptr 在低频窗口路径保护 Session 跨越本次摘要计算。
    const auto session = engine.getActiveSession();
    // 没有活动会话时返回默认空摘要，调用方据此禁用应用按钮。
    if ( !session ) return state;

    const auto& context = session->getContext();
    // 局部累加器统一处理主音、子音与独立采样的浮点比较规则。
    const auto accumulateVolume = [&state](float volume) {
        if ( !std::isfinite(volume) ) {
            // 非有限旧值无法作为输入初值，同时标记选择包含混合状态。
            state.mixed = true;
            return;
        }
        if ( !state.commonVolume ) {
            // 首个合法绑定建立后续比较基准。
            state.commonVolume = volume;
            return;
        }
        // 使用小容差吸收序列化和浮点运算产生的微小误差。
        if ( std::abs(*state.commonVolume - volume) > 1e-6F ) {
            state.mixed = true;
        }
    };

    // 音符选择索引通常很小，按索引访问而非遍历完整音符注册表。
    for ( const auto entity : context.selectedNoteEntities ) {
        // 选择索引可能在命令边界短暂含有过期实体，读取前必须验证。
        if ( !context.noteRegistry.valid(entity) ||
             !context.noteRegistry.all_of<Logic::NoteComponent>(entity) ) {
            continue;
        }
        // 组件引用仅在会话锁和本次循环内使用。
        const auto& note =
            context.noteRegistry.get<const Logic::NoteComponent>(entity);
        if ( note.m_isSubNote || !Logic::SessionUtils::isNoteEditable(
                                     note, context.lastConfig.settings) ) {
            // 子音由所属主音统一统计，不可编辑音符不应接受批量修改。
            continue;
        }

        // 对象计数只在主音或任一子音确实具有采样绑定时增加。
        bool hasAudio = false;
        if ( note.m_sampleBinding ) {
            // 主音采样参与共同音量判定。
            accumulateVolume(note.m_sampleBinding->m_volume);
            hasAudio = true;
        }
        // 一个折线或组合音符的所有子音绑定都必须具有一致音量。
        for ( const auto& subNote : note.m_subNotes ) {
            // 无采样绑定的子音不影响音量摘要。
            if ( !subNote.sampleBinding ) continue;
            accumulateVolume(subNote.sampleBinding->m_volume);
            hasAudio = true;
        }
        // 即使一个音符有多个绑定，窗口中的对象数量仍只增加一次。
        if ( hasAudio ) ++state.objectCount;
    }

    // 独立采样使用单独选择索引，同样避免完整 registry 遍历。
    for ( const auto entity : context.selectedSampleEntities ) {
        // 对删除后残留索引执行与音符相同的有效性防御。
        if ( !context.sampleRegistry.valid(entity) ||
             !context.sampleRegistry.all_of<Logic::SampleComponent>(entity) ) {
            continue;
        }
        // 独立采样总是具有直接音量字段，可立即纳入摘要。
        const auto& sample =
            context.sampleRegistry.get<const Logic::SampleComponent>(entity);
        accumulateVolume(sample.m_volume);
        ++state.objectCount;
    }

    // 返回值按值传递，离开会话锁后不保留 ECS 引用。
    return state;
}

/// @brief 打开选中物件批量音量编辑器动作。
/// @details 统一快捷键、选择摘要、窗口输入状态与批量更新命令。
class EditSelectedObjectVolumeAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的批量音量编辑快捷键提示。
    /// @param context 单帧主菜单上下文，本查询无需读取。
    /// @param fallbackShortcut 配置无法格式化时使用的静态提示。
    /// @return 在下一次缓存刷新前有效的字符串指针。
    /// @warning UI 热路径：只读取内存配置并格式化一个快捷键。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        // 引用快捷键配置，避免复制完整编辑器设置。
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        // 成员字符串为返回的 c_str 提供菜单帧内稳定存储。
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.editSelectedVolume);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 打开编辑窗口并用共同音量或 1.0 初始化输入。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不影响窗口初始化。
    /// @warning 低频路径：会锁定活动会话并遍历当前选择索引。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 在打开瞬间拍摄选择摘要，使输入与用户触发时的状态一致。
        const auto state = inspectSelectedVolumeState();

        // 混合或空选择使用中性 1.0，避免任意选取其中一个对象值。
        m_volume     = state.mixed ? 1.0F : state.commonVolume.value_or(1.0F);
        m_mixedInput = state.mixed;
        if ( !m_showWindow ) {
            // 已打开窗口的重复命令只刷新输入，不重复播放弹窗反馈。
            ::MMM::UI::PlayPopupOpenFeedback();
        }
        m_showWindow = true;
    }

    /// @brief 消费用户配置的批量音量编辑快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    /// @note 画布编辑快捷键被抑制时不检查用户绑定。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        // 每帧引用最新配置，使运行时重绑无需重建菜单动作。
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(
                 shortcutConfig.editSelectedVolume) ) {
            // 复用菜单执行入口，保证快捷键同样刷新选择摘要。
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

    /// @brief 渲染选中物件批量音量编辑窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在窗口打开时检查已选索引并绘制常量数量控件。
    /// @note 对象数量每帧刷新，选择变化会立即影响应用按钮可用性。
    void renderDeferred(MainMenuContext& context) override
    {
        // 窗口关闭时不锁会话，也不遍历选择索引。
        if ( !m_showWindow ) return;

        // 重新摘要只更新对象可用性，不覆盖用户正在编辑的输入值。
        const auto state = inspectSelectedVolumeState();
        // FirstUseEver 保留用户调整后的窗口尺寸。
        ImGui::SetNextWindowSize(ImVec2(420.0F * context.dpiScale, 0.0F),
                                 ImGuiCond_FirstUseEver);
        if ( ImGui::Begin(TR("ui.edit.selected_volume.title").data(),
                          &m_showWindow,
                          ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoCollapse) ) {
            if ( state.objectCount == 0U ) {
                // 选择变空时明确提示，并在下方禁用应用按钮。
                ImGui::TextUnformatted(
                    TR("ui.edit.selected_volume.none").data());
            } else {
                // 本地化计数文本帮助用户确认批量操作范围。
                const auto countText =
                    TR_FMT("ui.edit.selected_volume.count", state.objectCount);
                ImGui::TextUnformatted(countText.c_str());
            }
            if ( m_mixedInput ) {
                // 混合提示描述打开窗口时的初始状态，不随输入编辑消失。
                ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.25F, 1.0F),
                                   "%s",
                                   TR("ui.edit.selected_volume.mixed").data());
            }

            // 输入宽度按 DPI 缩放，数值步进保持业务单位不变。
            ImGui::Spacing();
            ImGui::SetNextItemWidth(220.0F * context.dpiScale);
            ImGui::InputFloat(TR("ui.edit.sample_properties.volume").data(),
                              &m_volume,
                              0.05F,
                              0.25F,
                              "%.3f");

            // 音量允许超过 1.0，但必须为有限非负值。
            const bool validVolume =
                std::isfinite(m_volume) && m_volume >= 0.0F;
            if ( !validVolume ) {
                // 无效输入立即显示危险色提示，且不会生成更新命令。
                ImGui::TextColored(
                    ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                    "%s",
                    TR("ui.edit.sample_properties.invalid_volume").data());
            }

            ImGui::Spacing();
            // 没有可编辑对象或输入无效时同时禁用应用按钮交互。
            ImGui::BeginDisabled(state.objectCount == 0U || !validVolume);
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.apply").data()) ) {
                // 命令在执行时解析当前选择，保持与最新会话状态一致。
                MenuUtil::dispatchCommand(
                    Logic::CmdUpdateSelectedObjectSampleVolume{ m_volume });
                // 成功投递后关闭窗口，避免同一输入被重复应用。
                m_showWindow = false;
            }
            ImGui::EndDisabled();
            // 取消按钮不修改选择或音量，只关闭当前窗口。
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data()) ) {
                m_showWindow = false;
            }
        }
        // 与 Begin 无条件配对，包括折叠或不可见内容分支。
        ImGui::End();
    }

private:
    /// @brief 是否显示批量音量编辑窗口。
    /// @note execute 和窗口关闭按钮共同维护该值。
    bool m_showWindow{ false };

    /// @brief 打开窗口时选择是否包含多个不同音量。
    /// @note 只用于提示初始选择状态，不影响应用命令参数。
    bool m_mixedInput{ false };

    /// @brief 待写入全部受支持选中物件的音量倍率。
    /// @note 允许大于 1.0，提交前验证为有限非负值。
    float m_volume{ 1.0F };

    /// @brief 当前帧快捷键显示缓存。
    /// @note 只为 shortcut 返回值提供稳定存储。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建打开选中物件批量音量编辑器动作处理器。
/// @return 独占所有权的批量音量编辑处理器。
/// @warning 处理器必须在 EditorEngine 与 ImGui 所属 UI 线程使用。
std::unique_ptr<IMainMenuItemActionHandler>
createEditSelectedObjectVolumeAction()
{
    return std::make_unique<EditSelectedObjectVolumeAction>();
}

}  // namespace MMM::UI
