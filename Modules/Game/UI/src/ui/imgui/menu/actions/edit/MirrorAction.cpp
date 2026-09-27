#include "config/AppConfig.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 镜像选中音符动作。
/// @details 菜单提示与快捷键判断共用用户配置，编辑结果交由逻辑命令生成。
class MirrorAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的镜像快捷键提示。
    /// @param context 单帧主菜单上下文，本查询无需读取。
    /// @param fallbackShortcut 配置无法格式化时使用的静态提示。
    /// @return 在下一次更新缓存前有效的字符串指针。
    /// @warning UI 热路径：仅格式化轻量快捷键配置，不得访问文件系统。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        // 缓存字符串以保证返回的 C 字符串在菜单绘制期间有效。
        m_shortcutBuffer = ShortcutUtils::formatShortcut(shortcutConfig.mirror);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 发布镜像选中音符命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变镜像语义。
    /// @warning 必须经命令分发保留选择变更的撤销记录。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 菜单层不读取选中集合，由逻辑层在命令执行时解析当前选择。
        MenuUtil::dispatchCommand(Logic::CmdMirrorSelected{});
    }

    /// @brief 消费用户配置的镜像快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    /// @note 禁止画布编辑快捷键时立即返回，不消费输入。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(shortcutConfig.mirror) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

private:
    /// @brief 当前帧快捷键显示缓存。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建镜像选中音符动作处理器。
/// @return 独占所有权的快捷键动作处理器。
/// @note 处理器只持有菜单提示字符串缓存。
std::unique_ptr<IMainMenuItemActionHandler> createMirrorAction()
{
    return std::make_unique<MirrorAction>();
}

}  // namespace MMM::UI
