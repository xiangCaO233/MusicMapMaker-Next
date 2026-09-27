#include "config/AppConfig.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 镜像粘贴动作。
/// @details 将系统剪贴板导入编辑器后，以镜像标志发布统一粘贴命令。
class MirrorPasteAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的镜像粘贴快捷键提示。
    /// @param context 单帧主菜单上下文，本查询无需读取。
    /// @param fallbackShortcut 配置为空时使用的静态提示。
    /// @return 在下一次刷新缓存前有效的字符串指针。
    /// @warning UI 热路径：只读取内存配置并格式化短字符串。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        // 成员缓存承接格式化结果，避免返回临时字符串地址。
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.mirrorPaste);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 从系统剪贴板同步并发布镜像粘贴命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变镜像粘贴语义。
    /// @note 新对象选择策略沿用普通粘贴的编辑器设置。
    /// @warning 系统剪贴板访问仅应由用户显式激活触发。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 在构造命令前导入外部内容，保证编辑器剪贴板状态最新。
        ClipboardBridge::importEditorClipboardFromSystem();
        // true 明确选择镜像模式，实际坐标变换由逻辑层负责。
        MenuUtil::dispatchCommand(Logic::CmdPaste{ true,
                                                   Config::AppConfig::instance()
                                                       .getEditorSettings()
                                                       .selectPastedObjects });
    }

    /// @brief 消费用户配置的镜像粘贴快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    /// @note 命中后返回 true，阻止普通 Ctrl+V 动作重复消费同一输入。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(shortcutConfig.mirrorPaste) ) {
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

/// @brief 创建镜像粘贴动作处理器。
/// @return 独占所有权的快捷键动作处理器。
/// @note 处理器只持有快捷键提示缓存，不持有剪贴板内容。
/// @warning 执行动作时必须位于可访问系统剪贴板的 UI 线程。
std::unique_ptr<IMainMenuItemActionHandler> createMirrorPasteAction()
{
    return std::make_unique<MirrorPasteAction>();
}

}  // namespace MMM::UI
