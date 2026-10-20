#include "logic/EditorEngine.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 打开音频导入选择器动作。
/// @details 菜单与 Ctrl+I 共享 MenuUtil 入口，文件对话框和导入事务不在动作中
/// 重复实现。
class OpenAudioImportAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "OpenAudioImportAction";
    }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "importAudio"; }

    /// @brief 仅在已有项目时允许导入音频。
    /// @param context 统一菜单上下文。
    /// @return 当前项目存在时返回 true。
    /// @warning UI 热路径：只读取项目观察指针。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 打开音频导入选择器。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变对话框选项。
    /// @note 具体格式过滤与导入目标由 MenuUtil 统一配置。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        MenuUtil::openAudioImportPicker();
    }

    /// @brief 消费 Ctrl+I 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        // Shift 修饰保留给其他导入入口，repeat=false 防止重复打开对话框。
        if ( ShortcutUtils::isActionShortcutPressed("importAudio", false) ) {
            Interaction::dispatchAction(
                *this,
                context,
                MainMenuItemActivation{
                    .m_source = MainMenuActivationSource::Shortcut });
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建打开音频导入选择器的菜单项业务处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenAudioImportAction()
{
    return std::make_unique<OpenAudioImportAction>();
}

}  // namespace MMM::UI
