#include "ui/imgui/ShortcutUtils.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/EditorShortcutActions.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui_internal.h>

namespace MMM::UI::ShortcutUtils
{

/// @brief 按稳定名称查找按键映射。
/// @param stableName 配置文件中的稳定按键名称。
/// @return 匹配到的按键映射；不存在时返回空。
/// @warning UI 热路径：线性扫描固定表，不分配字符串。
/// @note 稳定名称区分大小写，避免配置中出现多个等价写法。
const ShortcutKeyEntry* findKeyEntry(std::string_view stableName)
{
    // 函数内静态 constexpr 避免每次查询重新构造映射数组。
    static constexpr auto entries = shortcutKeyEntries();
    // 表规模固定且较小，线性扫描保持代码和序列化映射简单。
    for ( const auto& entry : entries ) {
        if ( entry.stableName == stableName ) {
            // 返回地址指向静态数组，生命周期覆盖整个进程。
            return &entry;
        }
    }
    // 未知名称可能来自较新配置，调用方决定回退显示或拒绝触发。
    return nullptr;
}

/// @brief 判断当前修饰键是否与绑定完全一致。
/// @param binding 待匹配的快捷键绑定。
/// @return 完全一致时返回 true。
/// @pre 必须存在有效 ImGui 上下文。
bool modifiersMatch(const Config::ShortcutBinding& binding)
{
    // ImGuiIO 聚合左右修饰键，本配置同样不区分左右侧。
    // 使用完全匹配，额外按住的修饰键也会阻止快捷键触发。
    return Interaction::modifier(Interaction::Modifier::Control) ==
               binding.ctrl &&
           Interaction::modifier(Interaction::Modifier::Shift) ==
               binding.shift &&
           Interaction::modifier(Interaction::Modifier::Alt) == binding.alt &&
           Interaction::modifier(Interaction::Modifier::Super) == binding.super;
}

/// @brief 将快捷键绑定格式化为菜单和设置页显示文本。
/// @param binding 待显示的快捷键绑定。
/// @return 已格式化文本；未启用时返回空字符串。
/// @note 格式化只用于展示，配置持久化仍保存各修饰键字段和稳定主键名。
std::string formatShortcut(const Config::ShortcutBinding& binding)
{
    // 禁用或缺少主键的绑定不在菜单右侧显示。
    if ( !binding.enabled || binding.key.empty() ) {
        return {};
    }

    // appendPart 只在已有内容后插入加号，避免前导或尾随分隔符。
    std::string text;
    auto        appendPart = [&text](std::string_view part) {
        if ( !text.empty() ) {
            // 修饰键与主键统一使用紧凑的加号连接格式。
            text += "+";
        }
        text += part;
    };

    // 修饰键按 Ctrl、Shift、Alt、Super 的稳定顺序显示。
    if ( binding.ctrl ) {
        appendPart("Ctrl");
    }
    if ( binding.shift ) {
        appendPart("Shift");
    }
    if ( binding.alt ) {
        appendPart("Alt");
    }
    if ( binding.super ) {
        appendPart("Super");
    }

    if ( const auto* entry = findKeyEntry(binding.key) ) {
        // 已知按键使用面向用户的 displayName，例如方向键显示 Left。
        appendPart(entry->displayName);
    } else {
        // 未知未来按键仍显示原配置文本，避免设置页呈现空值。
        appendPart(binding.key);
    }
    return text;
}

/// @brief 判断指定快捷键是否在当前帧按下。
/// @param binding 待检查的快捷键绑定。
/// @param repeat 是否允许按住重复触发。
/// @return 本帧触发时返回 true。
/// @warning UI 热路径：菜单和画布每帧调用；只做少量按键查询和字符串比较。
/// @pre 必须存在有效 ImGui 上下文。
bool isShortcutPressed(const Config::ShortcutBinding& binding, bool repeat)
{
    // 在查询主键前排除禁用、空键和修饰键不完全匹配的绑定。
    if ( !binding.enabled || binding.key.empty() || !modifiersMatch(binding) ) {
        return false;
    }

    // 配置中的稳定名称必须存在于当前版本映射表才能触发。
    const auto* entry = findKeyEntry(binding.key);
    if ( !entry ) {
        return false;
    }
    // repeat 由调用方决定按住时是否按 ImGui 重复率持续触发。
    return Interaction::keyPressed(entry->key, repeat);
}

/// @brief 通过稳定动作身份查询绑定，菜单、画布和新方案共用同一个入口。
/// @details
/// 仅在默认硬编码组合未改动时保留其宽松修饰键语义，用户重绑定严格匹配。
/// @warning 每帧只读配置和固定目录，不分配或保存配置。
/// @param action 固定动作字段名，目录查找失败即不触发任何编辑操作。
/// @param repeat 调用者决定是否重复，工具切换和菜单通常只消费首帧。
/// @param legacyModifiers
/// 当前表面对旧默认组合的额外修饰键要求；表格用零保持严格匹配。
/// @return 当前方案解析后的绑定在这一帧命中时返回 true。
/// @details 动作身份与主键不同，重映射复制不能顺带改掉所有字母 C 的操作。
/// 配置引用由 AppConfig 持有，方案返回的引用同样必须稳定。
/// 禁用或空键立即拒绝，不以其他动作的默认键补齐。
/// 只有完整等于旧默认组合时才启用历史修饰键通配规则。
/// 新绑定采用严格匹配，避免额外 Ctrl 或 Shift 同时触发多个编辑动作。
/// 主键查询也经过表面输入映射，按下、保持与释放使用同一键身份。
/// 设置页录制仍读取物理键，不把已经解释后的键再次保存为配置。
/// 调用者必须先检查弹窗、文本焦点和录制隔离，本 helper 不取得输入所有权。
/// BPM 和主画布继续各自决定动作归属，不能单凭绑定命中转交到后台谱面。
/// 目录是固定小表，不在热路径生成哈希字典或扫描文件系统。
/// 逻辑命令与保存流程在命中后单独执行，此函数本身没有编辑副作用。
bool isActionShortcutPressed(std::string_view action, bool repeat,
                             std::optional<unsigned> legacyModifiers)
{
    const auto* descriptor = Config::findEditorShortcut(action);
    if ( !descriptor ) return false;
    const auto& configured =
        Config::AppConfig::instance().getEditorSettings().shortcutConfig.*
        (descriptor->m_binding);
    const auto& binding =
        Interaction::activeProfile().shortcut(action, configured);
    const auto* key = findKeyEntry(binding.key);
    if ( !binding.enabled || !key ) return false;
    // 只有尚未改变的旧默认绑定保留历史通配修饰键；新配置不继承隐式歧义。
    static const Config::ShortcutConfig defaults;
    const bool legacy = Config::shortcutBindingsConflict(
        binding, defaults.*(descriptor->m_binding));
    const unsigned extras =
        legacy ? legacyModifiers.value_or(descriptor->m_legacyExtraModifiers)
               : 0;
    const auto matches =
        [&](Interaction::Modifier modifier, bool required, unsigned bit) {
            const bool pressed = Interaction::modifier(modifier);
            return required ? pressed : (!pressed || (extras & bit) != 0);
        };
    return matches(Interaction::Modifier::Control, binding.ctrl, 1) &&
           matches(Interaction::Modifier::Shift, binding.shift, 2) &&
           matches(Interaction::Modifier::Alt, binding.alt, 4) &&
           matches(Interaction::Modifier::Super, binding.super, 8) &&
           Interaction::keyPressed(key->key, repeat);
}

/// @brief 获取快捷键录制全局状态。
/// @return 当前进程内共享的录制状态。
/// @note 状态只由 UI 线程访问，不使用原子同步。
bool& shortcutRecordingActiveState()
{
    // 单一函数静态变量供设置页和全局快捷键派发共同访问。
    static bool active = false;
    return active;
}

/// @brief 设置当前是否正在录制快捷键。
/// @param active 是否处于录制状态。
/// @note 录制期间上层应暂停普通快捷键派发，避免新绑定同时执行命令。
void setShortcutRecordingActive(bool active)
{
    shortcutRecordingActiveState() = active;
}

/// @brief 判断当前是否正在录制快捷键。
/// @return 正在录制时返回 true。
/// @warning UI 热路径：只读取 UI 线程函数静态布尔值。
bool isShortcutRecordingActive()
{
    return shortcutRecordingActiveState();
}

/// @brief 判断稳定窗口 ID 是否为主谱面画布。
/// @param stableId ImGui 窗口稳定 ID。
/// @return 主谱面画布窗口返回 true。
/// @note Canvas_ 前缀覆盖多谱面标签，Basic2DCanvas 保留旧单画布名称。
bool isMainCanvasWindowId(std::string_view stableId)
{
    return stableId.starts_with("Canvas_") || stableId == "Basic2DCanvas";
}

/// @brief 判断当前键盘焦点是否允许触发谱面物件编辑快捷键。
/// @return 当前没有焦点窗口或焦点在主谱面画布时返回 true。
/// @warning UI 热路径：只读取当前 ImGui 导航窗口名称。
bool focusedWindowAllowsCanvasEditingShortcuts()
{
    // 启动、测试或未建立导航焦点时允许画布级快捷键继续判断。
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if ( !context || !context->NavWindow || !context->NavWindow->Name ) {
        return true;
    }

    // 动态窗口标题先提取最后一个 ### 后的稳定 ID。
    return isMainCanvasWindowId(stableWindowId(context->NavWindow->Name));
}

/// @brief 判断谱面物件编辑快捷键是否应让位给当前 UI 焦点。
/// @return 文本输入、弹窗、活跃控件或非主画布窗口聚焦时返回 true。
/// @warning UI 热路径：每帧快捷键派发前调用；只读取 ImGui 当前输入状态。
/// @note 此门禁用于物件编辑命令，不替代播放等具有独立路由规则的快捷键。
bool shouldBlockCanvasEditingShortcuts()
{
    // WantTextInput 覆盖文本控件，AnyItemActive 覆盖拖动和键盘编辑控件。
    const ImGuiIO& io = ImGui::GetIO();
    if ( io.WantTextInput || ImGui::IsAnyItemActive() ) {
        return true;
    }
    // 任意层级弹窗打开时保留键盘给弹窗，即使焦点仍报告主画布。
    if ( ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ) {
        return true;
    }
    // 最后以导航焦点窗口限制物件编辑命令只在主画布触发。
    return !focusedWindowAllowsCanvasEditingShortcuts();
}

/// @brief 录制当前帧刚按下的快捷键。
/// @return 捕获到的快捷键绑定；没有按键输入时为空。
/// @note Ctrl、Shift、Alt 与 Super 只作为修饰键记录，不会单独成为主键。
/// @warning UI 热路径：录制弹窗打开时每帧扫描固定映射表，不进行文件访问。
/// @pre 必须存在已开始输入帧的 ImGui 上下文。
std::optional<Config::ShortcutBinding> capturePressedShortcut()
{
    // 修饰键状态与捕获主键使用同一帧 ImGuiIO 快照。
    const ImGuiIO&        io      = ImGui::GetIO();
    static constexpr auto entries = shortcutKeyEntries();
    // 按表顺序选择本帧第一个刚按下且不重复触发的主键。
    for ( const auto& entry : entries ) {
        if ( ImGui::IsKeyPressed(entry.key, false) ) {
            // 新录制绑定始终启用，并使用稳定名称而非显示文本。
            Config::ShortcutBinding binding;
            binding.enabled = true;
            binding.key     = std::string(entry.stableName);
            // 四个聚合修饰键完整复制，保证后续完全匹配语义一致。
            binding.ctrl  = io.KeyCtrl;
            binding.shift = io.KeyShift;
            binding.alt   = io.KeyAlt;
            binding.super = io.KeySuper;
            // 捕获首个主键后立即返回，不组合多个非修饰键。
            return binding;
        }
    }
    // 当前帧没有支持的主键边沿。
    return std::nullopt;
}

/// @brief 获取指定编辑工具对应的快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
/// @note 返回引用与 settings 生命周期绑定。
/// @warning 热路径查询：只执行枚举 switch，不复制绑定字符串。
const Config::ShortcutBinding& getToolShortcut(
    const Config::EditorSettings& settings, Logic::EditTool tool)
{
    // 只读重载直接返回 EditorSettings 内部绑定，调用方不得延长其生命周期。
    switch ( tool ) {
    case Logic::EditTool::Move:
        return Interaction::activeProfile().shortcut(
            "toolMove", settings.shortcutConfig.toolMove);
    case Logic::EditTool::Marquee:
        return Interaction::activeProfile().shortcut(
            "toolMarquee", settings.shortcutConfig.toolMarquee);
    case Logic::EditTool::Draw:
        return Interaction::activeProfile().shortcut(
            "toolDraw", settings.shortcutConfig.toolDraw);
    case Logic::EditTool::ColorBrush:
        return Interaction::activeProfile().shortcut(
            "toolColorBrush", settings.shortcutConfig.toolColorBrush);
    case Logic::EditTool::ColorEraser:
        return Interaction::activeProfile().shortcut(
            "toolColorEraser", settings.shortcutConfig.toolColorEraser);
    case Logic::EditTool::Layout: break;
    }
    // Layout 当前没有独立绑定，回退 Move 保持返回引用始终有效。
    return Interaction::activeProfile().shortcut(
        "toolMove", settings.shortcutConfig.toolMove);
}

/// @brief 获取指定编辑工具对应的可写快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
/// @note 设置页通过该引用原地更新绑定，保存由外层统一执行。
/// @warning 返回可写引用，调用方必须在修改后触发配置同步和持久化。
Config::ShortcutBinding& getToolShortcut(Config::EditorSettings& settings,
                                         Logic::EditTool         tool)
{
    // 可写重载与只读重载保持完全相同的工具到字段映射。
    switch ( tool ) {
    case Logic::EditTool::Move: return settings.shortcutConfig.toolMove;
    case Logic::EditTool::Marquee: return settings.shortcutConfig.toolMarquee;
    case Logic::EditTool::Draw: return settings.shortcutConfig.toolDraw;
    case Logic::EditTool::ColorBrush:
        return settings.shortcutConfig.toolColorBrush;
    case Logic::EditTool::ColorEraser:
        return settings.shortcutConfig.toolColorEraser;
    case Logic::EditTool::Layout: break;
    }
    // 未单独配置的 Layout 返回可写 Move 绑定作为兼容回退。
    return settings.shortcutConfig.toolMove;
}

}  // namespace MMM::UI::ShortcutUtils
