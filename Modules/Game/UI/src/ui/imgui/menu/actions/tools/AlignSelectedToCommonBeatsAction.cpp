#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 选中音符节拍对齐动作。
/// @details 动作只负责统一菜单与快捷键入口，实际编辑由逻辑命令处理。
class AlignSelectedToCommonBeatsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    /// @brief 对齐动作的显示提示与触发使用同一目录身份。
    std::string_view shortcutActionId() const override
    {
        return "alignSelected";
    }
    std::string_view operationId() const override
    {
        return "AlignSelectedToCommonBeatsAction";
    }

    /// @brief 发布节拍对齐逻辑命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源；菜单与快捷键共享同一逻辑命令。
    /// @warning 命令可能修改当前选择，必须通过命令队列保留撤销语义。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 只投递值语义命令，避免菜单层直接依赖画布选择实现。
        MenuUtil::dispatchCommand(Logic::CmdAlignSelectedToCommonBeats{});
    }

    /// @brief 消费 Ctrl+F 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @details 输入映射只改变触发方式，对齐算法和撤销事务仍由逻辑命令处理。
    /// 物件编辑焦点门禁先于按键检查，工具窗口的字母输入不能穿透到谱面。
    /// 同一语义动作由菜单和键盘共用，方案无需复制对齐实现。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        // 禁用按键重复，确保一次按下只生成一条可撤销命令。
        if ( ShortcutUtils::isActionShortcutPressed("alignSelected") ) {
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

/// @brief 创建选中音符节拍对齐动作处理器。
/// @return 独占所有权的无状态动作处理器。
/// @note 处理器可同时服务菜单项与 Ctrl+F 快捷键。
std::unique_ptr<IMainMenuItemActionHandler>
createAlignSelectedToCommonBeatsAction()
{
    return std::make_unique<AlignSelectedToCommonBeatsAction>();
}

}  // namespace MMM::UI
