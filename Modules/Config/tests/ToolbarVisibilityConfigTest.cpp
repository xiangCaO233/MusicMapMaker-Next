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
           !projectSettings.showManagerLabels &&
           projectSettings.toolbarVisibility.stateTools.colorBrush &&
           projectSettings.toolbarVisibility.independentButtons.trackCount;
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
                   testGlobalToolbarVisibilityPreservation()
               ? 0
               : 1;
}
