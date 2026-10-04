#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 判断工具栏可见性是否匹配软件默认精简布局。
/// @param visibility 待检查的工具栏可见性配置。
/// @return 与默认显示配置完全一致时返回 true。
/// @details 同时比较状态工具和独立按钮两个嵌套分组的全部字段。
bool matchesDefaultToolbarVisibility(
    const MMM::Config::ToolbarVisibilityConfig& visibility)
{
    // 分别引用状态工具和独立按钮，断言与配置层级保持一致。
    const auto& tools   = visibility.stateTools;
    const auto& buttons = visibility.independentButtons;
    // 默认保留基础编辑工具，颜色工具则在用户需要时显式开启。
    return tools.move && tools.marquee && tools.draw && !tools.colorBrush &&
           // 独立区显示常用映射、播放和音效操作，隐藏参数型次级按钮。
           !tools.colorEraser && tools.layout && !buttons.notePalette &&
           buttons.magnet && buttons.scrollTimingMapping &&
           buttons.beatLineDisplay && buttons.soundEffectTool &&
           buttons.playback && !buttons.playbackSpeed && !buttons.trackCount &&
           !buttons.beatDivisor;
}

/// @brief 验证两个工具栏按钮集合的全部隐藏状态可以完整往返。
/// @return 所有字段均写出并恢复为隐藏时返回 true。
/// @details 全 false 输入避免默认 true 值掩盖遗漏的序列化字段。
bool testToolbarVisibilityRoundTrip()
{
    // 两个分组都设置为全隐藏，确保每个 false 会被明确写入而非省略。
    MMM::Config::EditorSettings source;
    // 状态工具影响当前交互模式，六个字段必须保持各自键名。
    source.toolbarVisibility.stateTools = {
        // 连续 false 值仍需逐字段写出，不能依赖整体默认构造。
        .move = false,       .marquee = false,     .draw = false,
        .colorBrush = false, .colorEraser = false, .layout = false,
    };
    // 独立按钮不改变工具状态，使用另一嵌套对象持久化九个字段。
    source.toolbarVisibility.independentButtons = {
        // 九个按钮全关，覆盖这一分组的完整序列化键集合。
        .notePalette = false,         .magnet = false,
        .scrollTimingMapping = false, .beatLineDisplay = false,
        .soundEffectTool = false,     .playback = false,
        .playbackSpeed = false,       .trackCount = false,
        .beatDivisor = false,
    };

    // 编码对象同时用于结构检查，恢复对象用于逐字段值检查。
    const nlohmann::json encoded = source;
    // 恢复值检查布尔语义，encoded 检查两个分组确实写入嵌套对象。
    const auto  restored   = encoded.get<MMM::Config::EditorSettings>();
    const auto& stateTools = restored.toolbarVisibility.stateTools;
    // 引用来自恢复对象，断言不会误读 source 的原始值。
    const auto& buttons = restored.toolbarVisibility.independentButtons;
    // 所有字段均应保持隐藏，并且两个嵌套分组必须真实出现在 JSON 中。
    if ( stateTools.move || stateTools.marquee || stateTools.draw ||
         stateTools.colorBrush || stateTools.colorEraser || stateTools.layout ||
         buttons.notePalette || buttons.magnet || buttons.scrollTimingMapping ||
         buttons.beatLineDisplay || buttons.soundEffectTool ||
         buttons.playback || buttons.playbackSpeed || buttons.trackCount ||
         buttons.beatDivisor ||
         !encoded.at("toolbarVisibility").contains("stateTools") ||
         !encoded.at("toolbarVisibility").contains("independentButtons") ) {
        // 任一按钮意外恢复为默认显示都会使完整往返失败。
        XERROR("Toolbar visibility config did not round trip");
        return false;
    }
    // 全隐藏往返通过意味着 false 值没有被默认布局覆盖。
    return true;
}

/// @brief 验证缺少工具栏可见性字段时使用软件默认精简布局。
/// @return 显示抓取、框选、绘制、布局与常用独立按钮时返回 true。
/// @details 空对象模拟 toolbarVisibility 尚未加入持久化格式的历史配置。
bool testToolbarVisibilityDefaults()
{
    // 空对象模拟没有 toolbarVisibility 字段的历史配置。
    const auto restored =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 默认匹配辅助函数覆盖全部按钮，不只抽查少数字段。
    if ( !matchesDefaultToolbarVisibility(restored.toolbarVisibility) ) {
        XERROR("Toolbar visibility config did not use compact defaults");
        return false;
    }
    // 缺失整个分组时应得到公开精简布局，而不是全显示或全隐藏。
    return true;
}

/// @brief 验证部分配置只覆盖明确写出的按钮，其余按钮保持软件默认值。
/// @return 抓取工具被隐藏且其余按钮仍匹配默认精简布局时返回 true。
/// @details 只写 stateTools.move，覆盖最小嵌套对象的合并行为。
bool testPartialToolbarVisibilityDefaults()
{
    // 只覆盖 move，其他十四个字段必须继续采用各自默认值。
    const nlohmann::json partial{
        { "toolbarVisibility", { { "stateTools", { { "move", false } } } } },
    };
    // 首先证明显式 false 被解析，不能被默认 true 覆盖。
    auto restored = partial.get<MMM::Config::EditorSettings>();
    if ( restored.toolbarVisibility.stateTools.move ) {
        // 显式 false 必须优先于默认 true。
        XERROR("Partial toolbar visibility config did not hide move tool");
        return false;
    }
    // 手工恢复 move 后，完整对象应再次与默认布局完全一致。
    restored.toolbarVisibility.stateTools.move = true;
    // 其余字段没有被测试改写，任何偏差都来自省略字段解析。
    if ( !matchesDefaultToolbarVisibility(restored.toolbarVisibility) ) {
        // 复原 move 后仍不匹配说明某个省略字段没有采用默认值。
        XERROR("Partial toolbar visibility config changed omitted defaults");
        return false;
    }
    // 该比较间接验证所有省略字段均保持默认值。
    return true;
}

/// @brief 验证项目配置合并时保留全软件工具栏显示设置。
/// @return 标签、固定模式和全部按钮可见性均取自全局设置时返回 true。
/// @details 全局与项目值刻意相反，验证合并方向不会颠倒。
bool testGlobalToolbarVisibilityPreservation()
{
    // 全局设置刻意使用与项目设置相反的标签和固定窗口值。
    MMM::Config::EditorSettings globalSettings;
    globalSettings.showToolLabels                                  = true;
    globalSettings.fixedToolWindow                                 = false;
    globalSettings.toolbarHorizontal                               = true;
    globalSettings.toolbarDockEdge                                 = "bottom";
    globalSettings.showManagerLabels                               = false;
    globalSettings.toolbarVisibility.stateTools.colorBrush         = true;
    globalSettings.toolbarVisibility.independentButtons.trackCount = true;

    // 项目设置模拟谱面内持久化的旧显示状态，不应覆盖软件级偏好。
    MMM::Config::EditorSettings projectSettings;
    projectSettings.showToolLabels                                  = false;
    projectSettings.fixedToolWindow                                 = true;
    projectSettings.showManagerLabels                               = true;
    projectSettings.toolbarVisibility.stateTools.colorBrush         = false;
    projectSettings.toolbarVisibility.independentButtons.trackCount = false;

    // 合并函数只迁移全局显示字段，其他编辑设置不在本测试范围。
    MMM::Config::preserveGlobalToolbarDisplaySettings(projectSettings,
                                                      globalSettings);
    // 返回表达式抽查刻意设反的代表字段，确认覆盖方向。
    // 同时验证顶层标签、固定模式和两个嵌套按钮分组。
    return projectSettings.showToolLabels && !projectSettings.fixedToolWindow &&
           projectSettings.toolbarHorizontal &&
           projectSettings.toolbarDockEdge == "bottom" &&
           !projectSettings.showManagerLabels &&
           projectSettings.toolbarVisibility.stateTools.colorBrush &&
           projectSettings.toolbarVisibility.independentButtons.trackCount;
}

/// @brief 验证排布与停靠边缘的持久化、历史默认值和非法边缘回退。
/// @return 横排底部配置可往返，旧配置仍右侧竖排时返回 true。
bool testToolbarLayoutSettings()
{
    MMM::Config::EditorSettings settings;
    settings.toolbarHorizontal = true;
    settings.toolbarDockEdge   = "bottom";
    // 使用完整对象往返，避免只测试孤立字段而遗漏全局编辑设置序列化入口。
    const nlohmann::json serialized = settings;
    const auto restored = serialized.get<MMM::Config::EditorSettings>();
    // 旧配置缺失字段的情况必须单独检查，完整往返无法覆盖这个兼容入口。
    // 方向默认值与边缘默认值要成对匹配，避免首次布局和按钮排布不一致。
    const auto legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    // 非法边缘来自手工编辑的配置，不能直接交给 ImGui 拆分方向运算。
    const auto invalid = nlohmann::json{ { "toolbarDockEdge", "invalid" } }
                             .get<MMM::Config::EditorSettings>();
    return restored.toolbarHorizontal && restored.toolbarDockEdge == "bottom" &&
           !legacy.toolbarHorizontal && legacy.toolbarDockEdge == "right" &&
           invalid.toolbarDockEdge == "right";
}

/// @brief 验证普通模式过滤旧配置中的高级入口，专业模式恢复原显示偏好。
/// @return 模式过滤、用户隐藏偏好和持久化往返均保持一致时返回 true。
/// @details 使用历史全显示配置，覆盖从专业模式切回时高级按钮仍为 true 的情况。
/// 能力掩码不写回源对象；用户主动隐藏的基础工具也不能被总开关重新打开。
/// @par 回归边界
/// - 全显示偏好与有效显示分别检查，避免默认 false 掩盖能力漏项。
/// - 磁吸、映射、分拍线、音效与底部播放入口在普通模式仍可显示。
/// - 倍速、主轨数和分拍数量属于基础参数入口，普通模式不能强制隐藏。
/// - 软件配置序列化保存用户偏好，不保存本帧能力掩码。
/// - 模式往返不改基础工具的手动隐藏状态。
/// 本用例不创建 UI 窗口，浮层关闭由 ToolbarView 使用同一有效配置执行。
/// 配色工具的活动策略退出另由会话测试覆盖，不从按钮不可见推断已禁用输入。
bool testProfessionalToolbarVisibility()
{
    MMM::Config::EditorSettings settings;
    // 把默认隐藏的高级按钮全部打开，模拟用户已定制过的专业工具栏。
    auto& tools      = settings.toolbarVisibility.stateTools;
    tools.colorBrush = tools.colorEraser = true;
    auto& buttons       = settings.toolbarVisibility.independentButtons;
    buttons.notePalette = buttons.playbackSpeed = buttons.trackCount =
        buttons.beatDivisor                     = true;
    settings.professionalMode                   = false;
    const auto basic = settings.effectiveToolbarVisibility();
    // 普通模式固定能力集合，但仍允许用户在集合内自行隐藏基础按钮。
    auto compact                             = basic;
    compact.independentButtons.playbackSpeed = false;
    compact.independentButtons.trackCount    = false;
    compact.independentButtons.beatDivisor   = false;
    // 基础参数开关应保留 true，其他按钮仍遵循默认布局和配色过滤。
    // 比较前只在测试副本恢复默认隐藏，不得把这三个偏好写回生产配置。
    if ( !matchesDefaultToolbarVisibility(compact) ||
         !basic.independentButtons.playbackSpeed ||
         !basic.independentButtons.trackCount ||
         !basic.independentButtons.beatDivisor ) {
        XERROR("Ordinary mode did not preserve basic toolbar controls");
        return false;
    }
    // 过滤后序列化仍保存原偏好，不能把临时能力状态持久化为用户选择。
    const nlohmann::json serialized = settings;
    auto restored             = serialized.get<MMM::Config::EditorSettings>();
    restored.professionalMode = true;
    const auto advanced       = restored.effectiveToolbarVisibility();
    if ( !advanced.stateTools.colorBrush || !advanced.stateTools.colorEraser ||
         !advanced.independentButtons.notePalette ||
         !advanced.independentButtons.playbackSpeed ||
         !advanced.independentButtons.trackCount ||
         !advanced.independentButtons.beatDivisor ) {
        XERROR("Professional mode lost saved toolbar visibility");
        return false;
    }
    // 原偏好保持完整；总开关往返不应强制恢复用户手动隐藏的基础项。
    settings.toolbarVisibility.stateTools.draw           = false;
    settings.toolbarVisibility.independentButtons.magnet = false;
    // 基础参数允许用户关闭，模式切换不能把手动隐藏的按钮重新打开。
    buttons.playbackSpeed = buttons.trackCount = buttons.beatDivisor = false;
    const auto hidden = settings.effectiveToolbarVisibility();
    return !hidden.stateTools.draw && !hidden.independentButtons.magnet &&
           !hidden.independentButtons.playbackSpeed &&
           !hidden.independentButtons.trackCount &&
           !hidden.independentButtons.beatDivisor &&
           settings.toolbarVisibility.stateTools.colorBrush &&
           settings.toolbarVisibility.independentButtons.notePalette;
}
}  // namespace

/// @brief 运行工具栏按钮可见性持久化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    // 顺序覆盖完整往返、旧版默认、部分对象和项目合并四条入口。
    // 短路失败由对应子测试日志定位，main 不重复输出。
    return testToolbarVisibilityRoundTrip() &&
                   testToolbarVisibilityDefaults() &&
                   testPartialToolbarVisibilityDefaults() &&
                   testGlobalToolbarVisibilityPreservation() &&
                   testToolbarLayoutSettings() &&
                   testProfessionalToolbarVisibility()
               ? 0
               : 1;
}
