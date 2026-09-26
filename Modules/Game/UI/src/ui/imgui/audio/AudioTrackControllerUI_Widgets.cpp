#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <utility>

#include <fmt/core.h>

namespace MMM::UI
{
namespace
{
/// @brief 使用音轨控制器内容字体测量单行文本宽度。
/// @param text 待测量的空字符结尾文本，可为空。
/// @return 当前内容字体下的像素宽度；空文本返回零。
///
/// 该重载从 SkinManager 获取实时字体，只用于同步布局路径。并行准备路径必须使用
/// 显式快照重载，不能在后台访问 ImGui 或皮肤管理器。
float measureTrackControllerText(const char* text)
{
    // 空指针没有可测量内容，也不能传入字体 API。
    if ( !text ) return 0.0f;

    // 内容字体缺失时回退当前 ImGui 字体，保持控件仍可布局。
    auto&   skinMgr = Config::SkinManager::instance();
    ImFont* font    = skinMgr.getFont("content");
    if ( !font ) {
        font = ImGui::GetFont();
    }
    // 宽度不换行、不裁剪，并使用当前 ImGui 字号。
    return font
        ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text, nullptr)
        .x;
}

/// @brief 使用 UI 快照中的字体测量单行文本宽度。
/// @param text 待测量文本，可为空。
/// @param font 主线程捕获的字体指针。
/// @param fontSize 捕获时的字体像素大小。
/// @return 快照字体下的单行宽度；输入无效时返回零。
///
/// 调用方保证字体对象在准备阶段保持存活；函数只调用字体的只读测量接口。
float measureTrackControllerText(const char* text, ImFont* font, float fontSize)
{
    // 快照缺失字体时不回退访问 ImGui 全局状态。
    if ( !text || !font ) return 0.0f;

    // FLT_MAX 禁止横向裁剪，wrapWidth 为零表示单行。
    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text, nullptr).x;
}

/// @brief 测量一组音轨控制器文本中的最大单行宽度。
/// @tparam N 编译期标签数量。
/// @param labels 待比较的文本指针数组。
/// @return 当前内容字体下的最大宽度。
template<size_t N>
float measureTrackControllerTextList(const std::array<const char*, N>& labels)
{
    // 从零开始，空数组自然返回零。
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        // 逐项复用同步字体 helper，并保留当前最大值。
        maxWidth = std::max(maxWidth, measureTrackControllerText(label));
    }
    return maxWidth;
}

/// @brief 使用 UI 快照字体测量一组文本中的最大单行宽度。
/// @tparam N 编译期标签数量。
/// @param labels 待比较的文本数组。
/// @param font 快照字体指针。
/// @param fontSize 快照字体大小。
/// @return 所有标签的最大单行宽度。
template<size_t N>
float measureTrackControllerTextList(const std::array<const char*, N>& labels,
                                     ImFont* font, float fontSize)
{
    // 只读取传入快照数据，适合布局准备阶段调用。
    float maxWidth = 0.0f;
    for ( const char* label : labels ) {
        maxWidth = std::max(maxWidth,
                            measureTrackControllerText(label, font, fontSize));
    }
    return maxWidth;
}

/// @brief 捕获音轨控制器同步测量所需的当前帧快照。
/// @param dpiScale 调用方观察到的窗口内容缩放。
/// @return 包含字体、翻译版本、主题间距和缩放设置的只读值快照。
///
/// 所有 ImGui、SkinManager 与 AppConfig
/// 访问集中在主线程捕获阶段。后续准备函数只
/// 使用快照字段计算布局，避免并行任务读取非线程安全全局 UI 状态。
UiFrameSnapshot captureAudioTrackControllerUiFrameSnapshot(float dpiScale)
{
    // 一次取得配置与皮肤引用，保证快照字段来自同一帧。
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& settings   = appConfig.getEditorSettings();
    const auto& aesthetics = settings.aesthetics;
    auto&       skinCfg    = Config::SkinManager::instance();
    const auto& style      = ImGui::GetStyle();

    // DPI 至少为一倍，防止布局控件缩小到不可交互尺寸。
    UiFrameSnapshot snapshot;
    snapshot.dpiScale     = std::max(1.0f, dpiScale);
    snapshot.framePadding = style.FramePadding;
    snapshot.frameHeight  = ImGui::GetFrameHeight();
    // 带 spacing 的帧高用于估算纵向区段和窗口边界。
    snapshot.frameHeightWithSpacing = ImGui::GetFrameHeightWithSpacing();
    // 内容和菜单字体分别捕获，fallback 保证皮肤未配置时仍可测量。
    snapshot.contentFont        = skinCfg.getFont("content");
    snapshot.menuFont           = skinCfg.getFont("menu");
    snapshot.fallbackFont       = ImGui::GetFont();
    snapshot.fontSize           = ImGui::GetFontSize();
    snapshot.translationVersion = skinCfg.getTranslator().getVersion();
    // 语言、字体偏好和倍率共同构成布局缓存失效条件。
    snapshot.language           = settings.language;
    snapshot.preferredAsciiFont = settings.preferredAsciiFont;
    snapshot.preferredCjkFont   = settings.preferredCjkFont;
    snapshot.fontSizeMultiplier = settings.fontSizeMultiplier;
    snapshot.uiScaleMultiplier  = settings.uiScaleMultiplier;
    snapshot.windowPadding      = aesthetics.windowPadding;
    // 美学间距变化会改变行宽高，需要进入快照比较。
    snapshot.itemSpacing = aesthetics.itemSpacing;
    return snapshot;
}
}  // namespace

/// @brief 获取并清空可复用的第 index 个 Clay 横向行。
/// @param index 行对象池索引。
/// @return 生命周期由控制器持有的稳定行引用。
///
/// deque 或稳定容器中的对象跨本帧 addLayout
/// 保持地址有效。只清空布局内容，不释放 对象池，避免每帧反复构造行对象。
CLayHBox& AudioTrackControllerUI::getRow(size_t index)
{
    // 池不足时只在高水位增长，已有索引保持复用。
    while ( m_rows.size() <= index ) m_rows.emplace_back();
    // 当前帧重建布局前移除上帧子元素和样式。
    auto& row = m_rows[index];
    row.clear();
    return row;
}

/// @brief 获取并清空可复用的第 index 个 Clay 纵向区段。
/// @param index 区段对象池索引。
/// @return 生命周期由控制器持有的稳定区段引用。
CLayVBox& AudioTrackControllerUI::getSection(size_t index)
{
    // 区段池与行池独立增长，索引由调用方本帧顺序分配。
    while ( m_sections.size() <= index ) m_sections.emplace_back();
    auto& sec = m_sections[index];
    sec.clear();
    return sec;
}

/// @brief 使用当前内容字体测量一个设置标签宽度。
/// @param label 标签文本。
/// @return 考虑字体 LegacySize 与 Scale 后的像素宽度。
///
/// 该方法保留给同步控件布局；窗口最小尺寸优先使用完整 LayoutMetricsCache。
float AudioTrackControllerUI::measureLabelWidth(const char* label)
{
    // 皮肤内容字体缺失时回退当前字体。
    auto&   skinMgr = Config::SkinManager::instance();
    ImFont* font    = skinMgr.getFont("content");
    if ( !font ) font = ImGui::GetFont();
    // LegacySize 乘字体 Scale 对应实际绘制字号。
    float  fontSize = font->LegacySize * font->Scale;
    ImVec2 sz       = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    return sz.x;
}

/// @brief 判断布局测量缓存是否匹配当前帧状态。
/// @param cache 需要检查的布局缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param trackType 当前音轨类型。
/// @param trackName 当前窗口标题。
/// @return 完全匹配时返回 true。
///
/// 缓存键覆盖所有影响文本测量或控件间距的状态。字体对象本身由语言、字体偏好、
/// 翻译版本和倍率间接标识，避免比较不稳定的内部缓存地址。
/// @warning UI 热路径：只执行常量字段比较，不得测量文本或重新构建布局。
bool AudioTrackControllerUI::layoutMetricsMatch(const LayoutMetricsCache& cache,
                                                const UiFrameSnapshot& snapshot,
                                                TrackType          trackType,
                                                const std::string& trackName)
{
    // 浮点配置允许极小计算误差，但真实 UI 设置变化远大于此阈值。
    auto floatEqual = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 0.0001f;
    };

    // valid 是首个短路条件，默认构造缓存不会被误认为命中。
    return cache.valid && cache.trackType == trackType &&
           cache.trackName == trackName &&
           floatEqual(cache.dpiScale, snapshot.dpiScale) &&
           floatEqual(cache.fontSize, snapshot.fontSize) &&
           floatEqual(cache.framePadding.x, snapshot.framePadding.x) &&
           floatEqual(cache.framePadding.y, snapshot.framePadding.y) &&
           floatEqual(cache.frameHeight, snapshot.frameHeight) &&
           floatEqual(cache.frameHeightWithSpacing,
                      snapshot.frameHeightWithSpacing) &&
           cache.language == snapshot.language &&
           cache.translationVersion == snapshot.translationVersion &&
           cache.preferredAsciiFont == snapshot.preferredAsciiFont &&
           cache.preferredCjkFont == snapshot.preferredCjkFont &&
           floatEqual(cache.fontSizeMultiplier, snapshot.fontSizeMultiplier) &&
           floatEqual(cache.uiScaleMultiplier, snapshot.uiScaleMultiplier) &&
           floatEqual(cache.windowPadding, snapshot.windowPadding) &&
           floatEqual(cache.itemSpacing, snapshot.itemSpacing);
}

/// @brief 构造音轨控制器布局测量缓存。
/// @param snapshot 当前帧 UI 快照。
/// @param trackType 当前音轨类型。
/// @param trackName 当前窗口标题。
/// @return 音轨控制器布局测量结果。
///
/// 缓存同时保存失效键和派生尺寸。主音轨包含声道控制、速度、音高、质量与分析入口，
/// 效果音轨只包含音量和预览，因此两类窗口分别计算行数及最宽控件组合。
///
/// 所有文本测量使用快照字体和字号；函数不查询 ImGui
/// 当前窗口，也不修改控件状态。
/// 返回结果可以先写入准备槽，再在主线程帧边界整体切换。
/// @details 缓存中的 minWindowSize
/// 是父窗口布局约束，不是每帧强制尺寸。用户仍可在
/// 允许范围内调整窗口，控件列会通过 Grow 和预设折行利用额外或不足空间。
///
/// 行对象本身不存入缓存；这里只计算所有行共享的像素度量。实际 Clay 树在每帧根据
/// 当前值和翻译文本重建，但复用行与区段对象池。
///
/// 主音轨 EQ 额外高度使用固定缩放基线，对应频谱预览和均衡器区域的现有布局契约。
/// 若这些区域结构变化，必须同步调整该高度以及调用方的内容构建逻辑。
AudioTrackControllerUI::LayoutMetricsCache
AudioTrackControllerUI::buildLayoutMetrics(const UiFrameSnapshot& snapshot,
                                           TrackType              trackType,
                                           const std::string&     trackName)
{
    // 先复制所有缓存键，派生尺寸完成后结果即可独立验证。
    LayoutMetricsCache cache;
    cache.valid                  = true;
    cache.trackType              = trackType;
    cache.trackName              = trackName;
    cache.dpiScale               = snapshot.dpiScale;
    cache.fontSize               = snapshot.fontSize;
    cache.framePadding           = snapshot.framePadding;
    cache.frameHeight            = snapshot.frameHeight;
    cache.frameHeightWithSpacing = snapshot.frameHeightWithSpacing;
    cache.language               = snapshot.language;
    cache.translationVersion     = snapshot.translationVersion;
    cache.preferredAsciiFont     = snapshot.preferredAsciiFont;
    cache.preferredCjkFont       = snapshot.preferredCjkFont;
    cache.fontSizeMultiplier     = snapshot.fontSizeMultiplier;
    cache.uiScaleMultiplier      = snapshot.uiScaleMultiplier;
    cache.windowPadding          = snapshot.windowPadding;
    cache.itemSpacing            = snapshot.itemSpacing;

    // 基础缩放至少为一倍，主题逻辑间距随后换算到像素并取整。
    const float scale       = std::max(1.0f, snapshot.dpiScale);
    const float itemSpacing = std::floor(snapshot.itemSpacing * scale);
    // 标签与控件之间的留白不小于八个缩放像素。
    const float labelPadding = std::ceil(std::max(8.0f * scale, itemSpacing));
    // 内容容器 padding 至少覆盖当前 ImGui 水平 FramePadding。
    const float contentPadding =
        std::ceil(std::max(4.0f * scale, snapshot.framePadding.x));
    // 行间距取主题间距的一半和四个缩放像素的较大者。
    const float contentSpacing =
        std::ceil(std::max(4.0f * scale, itemSpacing * 0.5f));
    // 每个设置行左右留白至少为八个缩放像素。
    const float rowPaddingX =
        std::ceil(std::max(8.0f * scale, snapshot.framePadding.x * 2.0f));
    // 行上下留白与 FramePadding 对齐。
    const float rowPaddingY =
        std::ceil(std::max(4.0f * scale, snapshot.framePadding.y));
    // 标签列与控件列之间使用不小于八像素的间距。
    const float rowSpacing = std::ceil(std::max(8.0f * scale, itemSpacing));
    // 预设按钮之间允许比主列间距更紧凑。
    const float presetSpacing = std::ceil(std::max(4.0f * scale, itemSpacing));
    // 行高同时容纳标准 Frame 和字体加垂直 padding。
    const float rowHeight =
        std::ceil(std::max(snapshot.frameHeight + rowPaddingY * 2.0f,
                           snapshot.fontSize + snapshot.framePadding.y * 2.0f));
    // 按钮高度不含设置行额外上下 padding。
    const float buttonHeight =
        std::ceil(std::max(snapshot.frameHeight,
                           snapshot.fontSize + snapshot.framePadding.y * 2.0f));
    // 静音图标按钮至少为方形，并保留 30 个缩放像素。
    const float muteButtonW = std::ceil(std::max(30.0f * scale, buttonHeight));
    // 内容字体缺失时使用捕获的 ImGui fallback，不在此读取全局状态。
    ImFont* font =
        snapshot.contentFont ? snapshot.contentFont : snapshot.fallbackFont;
    // 保存派生基础度量，后续构建函数统一读取同一缓存。
    cache.contentPadding  = contentPadding;
    cache.contentSpacing  = contentSpacing;
    cache.rowPaddingX     = rowPaddingX;
    cache.rowPaddingY     = rowPaddingY;
    cache.rowSpacing      = rowSpacing;
    cache.rowHeight       = rowHeight;
    cache.buttonHeight    = buttonHeight;
    cache.muteButtonWidth = muteButtonW;
    cache.presetSpacing   = presetSpacing;

    // 标签列按所有可能出现的设置标签最大宽度统一对齐。
    const std::array<const char*, 8> allLabels{
        TR_CACHE("ui.audio_manager.volume").data(),
        TR_CACHE("ui.audio_manager.speed_control").data(),
        TR_CACHE("ui.audio_manager.speed_presets").data(),
        TR_CACHE("ui.audio_manager.speed_value").data(),
        TR_CACHE("ui.audio_manager.stretch_quality").data(),
        TR_CACHE("ui.audio_manager.pitch_presets").data(),
        TR_CACHE("ui.audio_manager.pitch_value").data(),
        TR_CACHE("ui.audio_manager.play_preview").data()
    };
    cache.labelWidth =
        measureTrackControllerTextList(allLabels, font, snapshot.fontSize) +
        labelPadding;

    // 滑块最小宽度包含格式化数值、两侧 FramePadding 和额外拖动轨道。
    const float sliderMinW =
        measureTrackControllerText("0.0000", font, snapshot.fontSize) +
        snapshot.framePadding.x * 4.0f + std::floor(48.0f * scale);
    // L/R/LL/RR 声道按钮使用较小内边距，仍需容纳最长两个字符。
    const float lrPaddingX =
        std::ceil(std::max(3.0f * scale, snapshot.framePadding.x * 0.5f));
    const float lrButtonW =
        std::max({ measureTrackControllerText("L", font, snapshot.fontSize),
                   measureTrackControllerText("R", font, snapshot.fontSize),
                   measureTrackControllerText("LL", font, snapshot.fontSize),
                   measureTrackControllerText("RR", font, snapshot.fontSize),
                   24.0f * scale }) +
        lrPaddingX * 2.0f;
    cache.channelButtonWidth = std::ceil(lrButtonW);

    // 所有音轨至少需要静音按钮、列间距和一个可操作滑块。
    float widgetWidth = muteButtonW + rowSpacing + sliderMinW;
    if ( trackType == TrackType::Main ) {
        // 主音轨音量行还包含四个声道模式按钮及其间隙。
        widgetWidth += cache.channelButtonWidth * 4.0f + presetSpacing * 4.0f;

        // 速度预设按当前翻译中最长按钮测量。
        const std::array<const char*, 4> speedPresets{
            TR_CACHE("ui.audio_manager.speed_025x").data(),
            TR_CACHE("ui.audio_manager.speed_050x").data(),
            TR_CACHE("ui.audio_manager.speed_075x").data(),
            TR_CACHE("ui.audio_manager.speed_100x").data()
        };
        // 音高预设同样测量当前本地化标签。
        const std::array<const char*, 4> pitchPresets{
            TR_CACHE("ui.audio_manager.pitch_n24").data(),
            TR_CACHE("ui.audio_manager.pitch_n12").data(),
            TR_CACHE("ui.audio_manager.pitch_n5").data(),
            TR_CACHE("ui.audio_manager.pitch_0").data()
        };
        const float speedButtonsW = measureTrackControllerTextList(
                                        speedPresets, font, snapshot.fontSize) +
                                    snapshot.framePadding.x * 2.0f;
        // 单个预设按钮宽度由最长标签和左右 padding 决定。
        const float pitchButtonsW = measureTrackControllerTextList(
                                        pitchPresets, font, snapshot.fontSize) +
                                    snapshot.framePadding.x * 2.0f;
        // 两个分析按钮同排时需要两份文本、四份 padding 和一个列间距。
        const float analysisButtonsW =
            measureTrackControllerText(
                TR("ui.audio_manager.open_waveform").data(),
                font,
                snapshot.fontSize) +
            measureTrackControllerText(
                TR("ui.audio_manager.open_spectrum").data(),
                font,
                snapshot.fontSize) +
            snapshot.framePadding.x * 4.0f + rowSpacing;
        // 采用最宽控件组合，并设置 200 像素兜底工作区。
        widgetWidth = std::max({ widgetWidth,
                                 speedButtonsW,
                                 pitchButtonsW,
                                 analysisButtonsW,
                                 200.0f * scale });
    } else {
        // 效果音预览行包含播放、暂停和进度文字。
        const float playButtonW =
            std::max(80.0f * scale,
                     measureTrackControllerText(
                         TR("ui.audio_manager.resume_preview").data(),
                         font,
                         snapshot.fontSize) +
                         snapshot.framePadding.x * 2.0f);
        // 暂停按钮独立测量，翻译长度可能与播放不同。
        const float pauseButtonW =
            std::max(80.0f * scale,
                     measureTrackControllerText(
                         TR("ui.audio_manager.pause_preview").data(),
                         font,
                         snapshot.fontSize) +
                         snapshot.framePadding.x * 2.0f);
        // 进度区域按三位秒数的双时间格式预留宽度。
        const float progressW =
            measureTrackControllerText(
                "000.00s / 000.00s", font, snapshot.fontSize) +
            snapshot.framePadding.x * 2.0f;
        // 三个控件与两个间距共同决定效果音控件列宽度。
        widgetWidth = std::max(
            widgetWidth,
            playButtonW + pauseButtonW + progressW + rowSpacing * 2.0f);
    }

    // 行装饰包含左右 padding 和标签控件之间的一处间距。
    const float rowDecorations = rowPaddingX * 2.0f + rowSpacing;
    // 内容宽度再叠加外层容器左右 padding。
    const float contentWidth =
        cache.labelWidth + widgetWidth + rowDecorations + contentPadding * 2.0f;
    // 主音轨固定八行，效果音固定两行。
    const size_t rowCount = trackType == TrackType::Main ? 8U : 2U;
    // 内容高度包含顶部分隔线、容器 padding、行高和行间距。
    float contentH = 2.0f * scale + contentPadding * 2.0f +
                     rowCount * rowHeight +
                     (rowCount > 0 ? (rowCount - 1) * contentSpacing : 0.0f);

    if ( trackType == TrackType::Main ) {
        // 主音轨为速度说明、预设折行及分析区预留三个标准间距行。
        contentH += snapshot.frameHeightWithSpacing * 3.0f;
    }

    // 标题栏宽度需要容纳标题和两侧窗口控件空间。
    const float titleWidth =
        measureTrackControllerText(trackName.c_str(), font, snapshot.fontSize) +
        snapshot.frameHeight * 2.0f;
    // 整窗最小宽高包含窗口自身 padding 与一个额外帧间距。
    const float minWidth  = std::ceil(std::max(contentWidth, titleWidth) +
                                      snapshot.windowPadding * 2.0f);
    const float minHeight = std::ceil(contentH + snapshot.windowPadding * 2.0f +
                                      snapshot.frameHeightWithSpacing);
    cache.minWindowSize   = ImVec2(minWidth, minHeight);

    if ( trackType == TrackType::Main ) {
        // 展开 EQ 时额外容纳频谱概览和均衡器编辑区。
        cache.minWindowSizeWithEq =
            ImVec2(minWidth,
                   std::ceil(minHeight + 150.0f * scale + 220.0f * scale +
                             snapshot.frameHeightWithSpacing * 2.0f));
    } else {
        // 效果音控制器不提供主音轨 EQ，两个最小尺寸相同。
        cache.minWindowSizeWithEq = cache.minWindowSize;
    }
    // 返回完整值对象，调用方在帧边界一次性替换缓存。
    return cache;
}

/// @brief 获取音轨控制器布局测量缓存。
/// @param dpiScale 当前窗口内容缩放。
/// @return 与当前语言、字体、缩放和音轨类型匹配的布局测量结果。
///
/// 同步调用先捕获完整快照，缓存未命中时才重新测量所有标签。返回引用在下一次缓存
/// 替换前有效，不应跨帧保存。
///
/// 函数是 const，但缓存属于可变派生状态；重建不改变音轨业务值。调用方不得据此在
/// 非 UI 线程并发访问同一控制器实例。
/// @warning UI 热路径：窗口布局会重复查询；缓存命中时只比较固定字段。
const AudioTrackControllerUI::LayoutMetricsCache&
AudioTrackControllerUI::getLayoutMetrics(float dpiScale) const
{
    // 快照捕获集中读取本帧 UI 全局状态。
    UiFrameSnapshot snapshot =
        captureAudioTrackControllerUiFrameSnapshot(dpiScale);
    if ( !layoutMetricsMatch(
             m_layoutMetricsCache, snapshot, m_type, m_trackName) ) {
        // 语言、字体、缩放、主题或音轨类型变化时同步重建。
        m_layoutMetricsCache =
            buildLayoutMetrics(snapshot, m_type, m_trackName);
    }
    return m_layoutMetricsCache;
}

/// @brief 判断当前帧音轨控制器是否需要准备布局测量数据。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要刷新布局缓存时返回 true。
///
/// 关闭窗口不需要准备数据；重新打开时同步或并行准备路径会依据最新快照重建。
/// 该查询不消费准备结果，只供 UIManager 决定是否安排本帧测量；切换由 swap
/// 在安全 帧边界完成。
bool AudioTrackControllerUI::needsParallelUiPrepare(
    const UiFrameSnapshot& snapshot) const
{
    // 复用与同步查询完全相同的命中判据。
    return m_isOpen && !layoutMetricsMatch(
                           m_layoutMetricsCache, snapshot, m_type, m_trackName);
}

/// @brief 在 UI 主线程准备音轨控制器布局测量数据。
/// @param snapshot 当前帧 UI 快照。
///
/// 结果先写入准备槽，不直接替换当前渲染缓存；交换阶段保证同一帧只看到一套尺寸。
/// 连续准备请求会覆盖尚未交换的旧结果，只保留最新快照，避免积压过期布局。
void AudioTrackControllerUI::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    // buildLayoutMetrics 只读取值快照及稳定字体对象。
    m_preparedLayoutMetricsCache =
        buildLayoutMetrics(snapshot, m_type, m_trackName);
    // 有效标志最后写入，表示准备槽内容完整。
    m_hasPreparedLayoutMetrics = true;
}

/// @brief 将准备好的布局测量数据切换给主线程使用。
///
/// 无准备结果时是幂等空操作；存在结果时移动值对象并清除一次性有效标志。
/// 移动后准备槽内容未定义但有效标志已清除，下一次准备会完整赋值。
/// 交换不重新验证快照，调度方应保证准备结果属于当前待呈现的 UI 配置版本。
void AudioTrackControllerUI::swapPreparedUiFrameData()
{
    // 防止默认构造准备槽覆盖当前有效缓存。
    if ( !m_hasPreparedLayoutMetrics ) {
        return;
    }

    // 整体移动避免字段跨版本混合。
    m_layoutMetricsCache       = std::move(m_preparedLayoutMetricsCache);
    m_hasPreparedLayoutMetrics = false;
}

/// @brief 计算音轨控制器当前音轨类型所需的最小整窗尺寸。
/// @param dpiScale 当前窗口 DPI 缩放。
/// @return 当前内容状态对应的最小窗口尺寸。
///
/// 主音轨选择任意 EQ 预设时需要额外频谱和均衡器高度；效果音轨始终使用基础尺寸。
ImVec2 AudioTrackControllerUI::getMinWindowSize(float dpiScale) const
{
    // 先取得与本帧配置匹配的布局度量。
    const auto& cache = getLayoutMetrics(dpiScale);
    if ( m_type == TrackType::Main &&
         m_currentPreset != Audio::EQPreset::None ) {
        // 展开 EQ 内容时返回预留额外区域的尺寸。
        return cache.minWindowSizeWithEq;
    }
    return cache.minWindowSize;
}

/// @brief 向 Clay 设置列表追加一行“固定标签列 + 自适应控件列”。
/// @param parent 接收设置行的纵向容器。
/// @param rowIndex 本帧对象池游标，调用后递增。
/// @param label 左侧可见标签。
/// @param labelWidth 所有设置行共享的标签列宽。
/// @param widget 在右侧 Clay 矩形中提交 ImGui 控件的回调。
/// @param heightOverride 可选自定义行高，非正值使用缓存标准行高。
///
/// 左侧标签使用 Fit 测量并用 Grow 弹簧补足固定列宽，右侧控件占用剩余空间。行和
/// 左侧子行均来自复用池，回调只在本帧同步布局渲染期间调用。
///
/// rowIndex 同时分配主行和左侧子行，因此一次调用通常消耗两个池槽。生成 ID 使用
/// 消费后的序号和标签组合，在单个控制器窗口内保持确定且不与其他行冲突。
///
/// widget 以函数对象值形式进入 Clay 元素，捕获引用必须只指向本次外层 update 仍
/// 存活的局部变量；renderInCurrent 会在函数返回前同步执行所有回调。
void AudioTrackControllerUI::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                            const char* label, float labelWidth,
                                            CLayBox::DrawFunc widget,
                                            float             heightOverride)
{
    // 所有行从同一布局缓存读取 padding、spacing 与标准高度。
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    /// 把非负浮点尺寸向上取整到 Clay 的 uint16 像素。
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };

    // 主行消费一个池索引，并配置左右 padding、间距和垂直居中。
    auto& row = getRow(rowIndex++);
    row.setPadding(toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingY),
                   toLayoutPixels(layoutMetrics.rowPaddingY))
        .setSpacing(toLayoutPixels(layoutMetrics.rowSpacing))
        .setAlignment(Alignment::Center());

    // 行号与标签共同组成本帧稳定 Clay 元素 ID。
    std::string labelId = "AT_R" + std::to_string(rowIndex) + "_L_" + label;

    // 左侧子行由可见标签和弹性空白组成。
    auto& leftBox = getRow(rowIndex++);
    leftBox.clear();
    leftBox.setPadding(0, 0, 0, 0)
        .setSpacing(0)
        .setAlignment(Alignment::Center());

    // 标签元素按文本 Fit，绘制时在行高内垂直居中。
    leftBox.addElement(labelId + "_lbl",
                       Sizing::Fit(),
                       Sizing::Grow(),
                       [label](Clay_BoundingBox r, bool) {
                           // 当前字体高度用于计算本行局部 Y 偏移。
                           float textH  = ImGui::CalcTextSize(label).y;
                           float offset = (r.height - textH) * 0.5f;
                           ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                           ImGui::Text("%s", label);
                       });

    // 弹簧吸收标签实际宽度与统一标签列宽之间的差值。
    leftBox.addElement(
        labelId + "_lbl_spring", Sizing::Grow(), Sizing::Grow(), nullptr);

    // 左侧子行固定为缓存标签列宽，保证所有控件纵向对齐。
    row.addLayout((labelId + "_left").c_str(),
                  leftBox,
                  Sizing::Fixed(labelWidth),
                  Sizing::Grow());

    // 右侧元素增长占满剩余空间，并转发 Clay hover 状态。
    row.addElement(labelId + "_wgt",
                   Sizing::Grow(),
                   Sizing::Grow(),
                   [widget](Clay_BoundingBox r, bool h) { widget(r, h); });

    // 自定义高度不能小于标准行高，防止按钮被裁剪。
    float rowH = heightOverride > 0.0f
                     ? std::max(heightOverride, layoutMetrics.rowHeight)
                     : layoutMetrics.rowHeight;
    // 完整设置行横向增长，纵向使用最终固定高度。
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

/// @brief 构建音量、静音和主音轨声道模式设置行。
/// @param parent 接收设置行的 Clay 纵向容器。
/// @param rowIndex 本帧行对象池游标。
/// @param labelWidth 统一标签列宽度。
/// @param volume 当前线性音量，控件可原地修改。
/// @param muted 当前静音状态，控件可原地修改。
/// @param changed 任一音量或静音值变化时置 true。
///
/// 所有音轨显示静音按钮与音量滑块；主音轨额外提供静音左、静音右、左复制到右、右
/// 复制到左四个互斥声道模式。再次点击活动模式会恢复 Stereo。
/// @warning UI 热路径：窗口可见时每帧构建控件；不得执行音频解码或阻塞操作。
/// @details
/// 静音状态和音量值通过引用交给外层，因此普通按钮与滑块不会直接调用音轨 音量
/// API。主声道模式是独立 AudioManager 状态，按钮点击时立即写入。
///
/// 四个模式按钮视觉上互斥：静音模式使用危险色，复制模式使用绿色。Stereo
/// 没有独立 按钮，用户再次点击当前活动模式即可恢复。
void AudioTrackControllerUI::buildVolumeSection(CLayVBox& parent,
                                                size_t&   rowIndex,
                                                float labelWidth, float& volume,
                                                bool& muted, bool& changed)
{
    // 从统一缓存取得按钮尺寸和行内间距，避免本函数重复测量文本。
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    const float btnWidth  = layoutMetrics.muteButtonWidth;
    const float btnHeight = layoutMetrics.buttonHeight;
    // 声道按钮使用较紧凑水平 padding，同时尊重主题最小值。
    const float lrPaddingX = std::ceil(std::max(
        3.0f * layoutMetrics.dpiScale, layoutMetrics.framePadding.x * 0.5f));
    const float lrGap      = layoutMetrics.presetSpacing;
    const float lrButtonW  = layoutMetrics.channelButtonWidth;
    const float rowSpacing = layoutMetrics.rowSpacing;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.volume").data(),
        labelWidth,
        [this,
         &volume,
         &muted,
         &changed,
         btnWidth,
         btnHeight,
         lrPaddingX,
         lrGap,
         lrButtonW,
         rowSpacing](Clay_BoundingBox r, bool) {
            // AudioManager 是主音轨声道模式的运行态真值。
            auto& audio = Audio::AudioManager::instance();

            // 静音按钮在 Clay 分配行内垂直居中。
            float offset = (r.height - btnHeight) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });

            // 图标先按静音状态选择，未静音时再按音量三档显示。
            const char* icon = ICON_MMM_VOLUME_MUTE;
            if ( !muted ) {
                // 低音量使用关闭扬声器图标，但不会隐式设置 muted。
                if ( volume <= 0.33f )
                    icon = ICON_MMM_VOLUME_OFF;
                else if ( volume <= 0.66f )
                    // 中等音量使用低音量扬声器图标。
                    icon = ICON_MMM_VOLUME_LOW;
                else
                    // 高音量使用完整扬声器图标。
                    icon = ICON_MMM_VOLUME_HIGH;
            }

            // 静音状态用危险色强调；标志确保样式栈精确配对。
            bool pushedTextColor = false;
            if ( muted ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
                pushedTextColor = true;
            }

            // 图标按钮透明底色，尺寸与缓存度量保持一致。
            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            Utils::pushFixedButtonStyleVars();
            if ( ::MMM::UI::FeedbackButton(icon,
                                           ImVec2(btnWidth, btnHeight)) ) {
                // 点击只切换本地值，外层在 changed 后统一提交音频参数。
                muted   = !muted;
                changed = true;
            }
            Utils::popFixedButtonStyleVars();
            ImGui::PopStyleColor();

            if ( pushedTextColor ) {
                // 仅静音分支压入了额外文本色。
                ImGui::PopStyleColor();
            }

            if ( ImGui::IsItemHovered() ) {
                // Tooltip 描述点击后的动作，而不是当前状态。
                ImGui::SetTooltip("%s",
                                  muted ? TR("ui.audio_manager.unmute").data()
                                        : TR("ui.audio_manager.mute").data());
            }

            // 音量滑块紧跟静音按钮，使用缓存主列间距。
            ImGui::SameLine(0, rowSpacing);

            // 主音轨为四个声道按钮预留宽度，效果音不保留该区域。
            float lrWidth = (m_type == TrackType::Main)
                                ? (lrButtonW * 4.0f + lrGap * 4.0f)
                                : 0.0f;
            // 滑块吸收剩余宽度，并保留 40 像素最低可操作轨道。
            float sliderWidth = r.width - btnWidth - rowSpacing - lrWidth;
            sliderWidth       = std::max(sliderWidth, 40.0f);
            ImGui::SetNextItemWidth(sliderWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##Volume", &volume, 0.0f, 1.0f, "%.4f") ) {
                // 滑块变化由外层统一应用到对应音轨。
                changed = true;
                if ( muted && volume > 0.0f ) {
                    // 从静音状态拖到正音量时自动恢复发声，符合常见混音器行为。
                    muted = false;
                }
            }

            if ( m_type == TrackType::Main ) {
                // 声道模式只属于主混音器，效果音行到此结束。
                auto channelMode = audio.getMainMixerChannelMode();
                // 复制模式用绿色，静音模式由调用方传入危险色。
                const ImVec4 copyModeColor{ 0.45f, 1.0f, 0.48f, 1.0f };
                /// 绘制一个互斥声道模式按钮并直接更新 AudioManager。
                auto drawChannelButton = [&](const char*             id,
                                             Audio::MixerChannelMode mode,
                                             const char*             tooltip,
                                             const ImVec4& activeColor) {
                    // 当前模式决定活动色和再次点击的恢复语义。
                    const bool active = channelMode == mode;
                    ImGui::SameLine(0, lrGap);
                    if ( active ) {
                        // 只有活动按钮压入强调文本色。
                        ImGui::PushStyleColor(ImGuiCol_Text, activeColor);
                    }
                    ImGui::PushStyleVar(
                        ImGuiStyleVar_FramePadding,
                        ImVec2(lrPaddingX, style.FramePadding.y));
                    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                        ImVec2(0.5f, 0.5f));
                    if ( ::MMM::UI::FeedbackButton(
                             id, ImVec2(lrButtonW, btnHeight)) ) {
                        // 点击活动模式恢复 Stereo，点击非活动模式切换到目标。
                        channelMode =
                            active ? Audio::MixerChannelMode::Stereo : mode;
                        audio.setMainMixerChannelMode(channelMode);
                    }
                    ImGui::PopStyleVar(2);
                    if ( active ) {
                        // 与条件 PushStyleColor 配对。
                        ImGui::PopStyleColor();
                    }
                    if ( ImGui::IsItemHovered() ) {
                        // 每个缩写按钮通过 Tooltip 解释完整声道操作。
                        ImGui::SetTooltip("%s", tooltip);
                    }
                };

                // L/R 使用危险色表示静音对应声道。
                drawChannelButton("L##MainMixerMuteL",
                                  Audio::MixerChannelMode::MuteLeft,
                                  TR("ui.audio_manager.mute_l").data(),
                                  Utils::UIThemeUtils::getDangerColor());
                drawChannelButton("R##MainMixerMuteR",
                                  Audio::MixerChannelMode::MuteRight,
                                  TR("ui.audio_manager.mute_r").data(),
                                  Utils::UIThemeUtils::getDangerColor());
                // LL/RR 使用绿色表示把一侧内容复制到另一侧。
                drawChannelButton("LL##MainMixerCopyL",
                                  Audio::MixerChannelMode::CopyLeftToRight,
                                  "LL",
                                  copyModeColor);
                drawChannelButton("RR##MainMixerCopyR",
                                  Audio::MixerChannelMode::CopyRightToLeft,
                                  "RR",
                                  copyModeColor);
            }
        });
}

/// @brief 构建主音轨速度、拉伸质量和音高控制区。
/// @param parent 接收设置行的 Clay 容器。
/// @param rowIndex 本帧行对象池游标。
/// @param labelWidth 统一标签列宽。
/// @param availWidgetW 控件列当前可用宽度，用于预计算按钮折行。
/// @param speed 期望播放速度，可由预设或滑块修改。
/// @param pitch 音高半音偏移，可由预设或滑块修改。
/// @param changed 任一需要外层应用的值变化时置 true。
///
/// 预设按钮先用当前字体测量并计算行数，随后以相同规则在 Clay
/// 回调中实际折行，保证
/// 行高与绘制一致。速度说明同时显示期望值和音频引擎实际速度，窄列时拆为两行。
/// @warning UI
/// 热路径：只测量固定数量文本和构建控件，不得访问文件系统或解码音频。
/// @details 速度范围为 0.25x 到 2.0x，音高范围为正负 24 半音。
/// 预设只提供常用值，滑块允许在完整范围内连续调整；两条路径都修改相同引用并设置
/// changed，外层随后统一应用数值。
///
/// 拉伸质量直接交给 AudioManager，因为它不是外层统一提交的简单数值。changed
/// 仍会 置位，使调用方按既有流程刷新相关显示或持久化状态。
///
/// 翻译中的速度说明可以用一处 `|`
/// 划分期望和实际格式。宽度足够时两段合并在一行，
/// 不足时保持各自格式拆为上下两行，避免自动换行切断数值或单位。
///
/// 折行预计算使用 availWidgetW，实际绘制使用 Clay 回调矩形宽度。二者应来自同一
/// 布局度量；若未来改变父布局，必须同步保持判断公式一致。
void AudioTrackControllerUI::buildSpeedAndPitchSection(
    CLayVBox& parent, size_t& rowIndex, float labelWidth, float availWidgetW,
    float& speed, float& pitch, bool& changed)
{
    // AudioManager 提供实际播放速度和拉伸质量状态。
    auto&       audio = Audio::AudioManager::instance();
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    // 行高、按钮高和预设间距统一来自缓存。
    const float rowPadY     = layoutMetrics.rowPaddingY;
    const float widgetH     = layoutMetrics.buttonHeight;
    const float spacing     = layoutMetrics.presetSpacing;
    const float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
    // 控件列至少容纳四个按钮高度，避免极窄尺寸下计算负宽度。
    availWidgetW = std::max(availWidgetW, widgetH * 4.0f);

    /// 去掉翻译片段首尾 ASCII 空白，避免 `|` 分割后出现额外间距。
    auto trim = [](const std::string& str) {
        // 全为空白时返回空字符串。
        size_t first = str.find_first_not_of(" \t\r\n");
        if ( first == std::string::npos ) return std::string();
        // substr 长度覆盖最后一个非空白字符。
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    };

    // 实际速度可能受音高保持与引擎拉伸实现影响，需与期望值分开显示。
    float       actualSpeed = (float)audio.getActualPlaybackSpeed();
    std::string rawStr      = TR("ui.audio_manager.speed_info").data();
    // 翻译以 `|` 可选分隔期望与实际两段格式。
    size_t pipePos = rawStr.find('|');
    bool   hasPipe = (pipePos != std::string::npos);
    // 没有分隔符时整个翻译作为单一速度格式。
    std::string leftFmt  = hasPipe ? trim(rawStr.substr(0, pipePos)) : rawStr;
    std::string rightFmt = hasPipe ? trim(rawStr.substr(pipePos + 1)) : "";

    // 固定栈缓冲避免为 printf 格式化引入动态缓冲管理。
    char leftBuf[128]  = { 0 };
    char rightBuf[128] = { 0 };
    char fullBuf[256]  = { 0 };

    // 翻译格式负责数值标签，调用方确保只包含匹配的浮点占位符。
    // 拖动值只用于预览标签；项目值在释放前保持原样，避免触发资源重建。
    snprintf(leftBuf,
             sizeof(leftBuf),
             leftFmt.c_str(),
             m_speedSliderEditing ? m_speedSliderDraft : speed);
    if ( hasPipe ) {
        // 双段模式分别格式化期望和实际值，再组合为单行候选。
        snprintf(rightBuf, sizeof(rightBuf), rightFmt.c_str(), actualSpeed);
        snprintf(fullBuf, sizeof(fullBuf), "%s | %s", leftBuf, rightBuf);
    } else {
        // 单段模式直接复制左侧结果。
        snprintf(fullBuf, sizeof(fullBuf), "%s", leftBuf);
    }

    // 字符串按值捕获到 Clay 回调，生命周期覆盖本帧布局渲染。
    std::string leftStr  = leftBuf;
    std::string rightStr = rightBuf;
    std::string fullStr  = fullBuf;

    // 组合文本超出控件列时预留两行及中间间距。
    float textW      = ImGui::CalcTextSize(fullStr.c_str()).x;
    bool  labelWraps = (textW > availWidgetW);
    float labelH = labelWraps ? (2.0f * widgetH + lineSpacing + rowPadY * 2.0f)
                              : layoutMetrics.rowHeight;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_control").data(),
        labelWidth,
        [leftStr, rightStr, fullStr, labelWraps, rowPadY, widgetH, lineSpacing](
            Clay_BoundingBox r, bool) {
            // 文本垂直对齐与标准 Frame 标签一致。
            ImGui::AlignTextToFramePadding();

            if ( labelWraps ) {
                // 第一行显示用户期望速度。
                ImGui::SetCursorScreenPos({ r.x, r.y + rowPadY });
                ImGui::TextUnformatted(leftStr.c_str());

                // 第二行显示音频引擎实际速度。
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + rowPadY + widgetH + lineSpacing });
                ImGui::TextUnformatted(rightStr.c_str());
            } else {
                // 单行模式在自定义 Clay 行高内垂直居中。
                float offset = (r.height - widgetH) * 0.5f;
                ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                ImGui::TextUnformatted(fullStr.c_str());
            }
        },
        labelH);

    // 速度预设文本在当前语言下可能宽度不同，必须逐项测量。
    std::string speed025 = TR("ui.audio_manager.speed_025x").data();
    std::string speed050 = TR("ui.audio_manager.speed_050x").data();
    std::string speed075 = TR("ui.audio_manager.speed_075x").data();
    std::string speed100 = TR("ui.audio_manager.speed_100x").data();

    // 显示标签与目标速度数组按索引严格对应。
    std::vector<std::string> speedPresets = {
        speed025, speed050, speed075, speed100
    };
    std::vector<float> targetSpeeds = { 0.25f, 0.5f, 0.75f, 1.0f };

    // 先模拟与绘制回调相同的横向排布，计算实际行数。
    float currentX   = 0.0f;
    int   speedLines = 1;
    for ( size_t i = 0; i < speedPresets.size(); ++i ) {
        float btnW = ImGui::CalcTextSize(speedPresets[i].c_str()).x +
                     ImGui::GetStyle().FramePadding.x * 2.0f;
        if ( i > 0 ) {
            if ( currentX + spacing + btnW < availWidgetW ) {
                // 当前行仍可容纳时累计间距和按钮宽度。
                currentX += spacing + btnW;
            } else {
                // 超宽时换行并以当前按钮作为新行起点。
                speedLines++;
                currentX = btnW;
            }
        } else {
            // 第一个按钮建立首行已用宽度。
            currentX = btnW;
        }
    }
    // 总高度包含每行按钮、行间距和设置行上下 padding。
    float speedPresetsH =
        speedLines * widgetH + (speedLines - 1) * lineSpacing + rowPadY * 2.0f;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_presets").data(),
        labelWidth,
        [this,
         speedPresets,
         targetSpeeds,
         &speed,
         &changed,
         rowPadY,
         widgetH,
         spacing,
         lineSpacing](Clay_BoundingBox r, bool) {
            // 实际绘制从控件矩形顶部 padding 后开始。
            ImGui::SetCursorScreenPos({ r.x, r.y + rowPadY });

            // 与预计算阶段使用相同宽度和换行规则。
            float currentX = 0.0f;
            float currentY = 0.0f;
            for ( size_t i = 0; i < speedPresets.size(); ++i ) {
                float btnW = ImGui::CalcTextSize(speedPresets[i].c_str()).x +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
                if ( i > 0 ) {
                    if ( currentX + spacing + btnW < r.width ) {
                        // 同行按钮交由 ImGui SameLine 排布。
                        ImGui::SameLine();
                        currentX += spacing + btnW;
                    } else {
                        // 换行时显式设置下一行绝对屏幕坐标。
                        currentY += widgetH + lineSpacing;
                        ImGui::SetCursorScreenPos(
                            { r.x, r.y + rowPadY + currentY });
                        currentX = btnW;
                    }
                } else {
                    currentX = btnW;
                }

                // PushID 使用预设索引区分可能相同的本地化按钮文本。
                ImGui::PushID(static_cast<int>(i));
                if ( ::MMM::UI::FeedbackButton(speedPresets[i].c_str()) ) {
                    // 预设是离散操作，直接提交并覆盖任何先前的滑块草稿。
                    speed                = targetSpeeds[i];
                    m_speedSliderDraft   = speed;
                    m_speedSliderEditing = false;
                    // 外层在全部控件完成后统一提交速度与音高。
                    changed = true;
                }
                ImGui::PopID();
            }
        },
        speedPresetsH);

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_value").data(),
        labelWidth,
        [this, &speed, &changed](Clay_BoundingBox r, bool) {
            // 速度滑块填满控件列，范围覆盖四分之一到两倍速度。
            ImGui::SetNextItemWidth(r.width);
            // 空闲时采纳项目模型的新值；活动时沿用本控件跨帧草稿。
            if ( !m_speedSliderEditing ) m_speedSliderDraft = speed;
            ::MMM::UI::FeedbackSliderFloat(
                "##SpeedSlider", &m_speedSliderDraft, 0.25f, 2.0f, "%.4fx");
            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                // 离线拉伸整首音频很昂贵，松开时只提交最终倍率一次。
                // 与当前项目值相同的编辑无需再生成配置命令。
                changed              = changed || speed != m_speedSliderDraft;
                speed                = m_speedSliderDraft;
                m_speedSliderEditing = false;
            } else {
                // 点击但未改值也保持活动状态，下一帧不可重置其拖动起点。
                m_speedSliderEditing = ImGui::IsItemActive();
            }
            if ( ImGui::IsItemHovered() ) {
                // Tooltip 在右侧展开，避免遮挡左侧设置标签。
                Utils::renderTooltip(
                    TR("ui.audio_manager.speed_tooltip").data(),
                    Utils::TooltipDir::Right);
            }
        });

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.stretch_quality").data(),
        labelWidth,
        [&changed, &audio](Clay_BoundingBox r, bool) {
            // 显示顺序必须与 StretchQuality 枚举整数值一致。
            const char* qualityNames[] = {
                TR("ui.audio_manager.quality_fast").data(),
                TR("ui.audio_manager.quality_balanced").data(),
                TR("ui.audio_manager.quality_finer").data(),
                TR("ui.audio_manager.quality_best").data()
            };

            // 组合框以 int 适配 ImGui API，选择后再转换回强类型枚举。
            int currentQuality = static_cast<int>(audio.getPlaybackQuality());
            ImGui::SetNextItemWidth(r.width);
            if ( ::MMM::UI::FeedbackCombo("##StretchQuality",
                                          &currentQuality,
                                          qualityNames,
                                          IM_ARRAYSIZE(qualityNames)) ) {
                audio.setPlaybackQuality(
                    static_cast<Audio::AudioManager::StretchQuality>(
                        currentQuality));
                // 质量直接写入 AudioManager，同时通知外层状态变化。
                changed = true;
            }
        });

    // 音高预设复用速度预设相同的测量与折行策略。
    std::string pitchN24 = TR("ui.audio_manager.pitch_n24").data();
    std::string pitchN12 = TR("ui.audio_manager.pitch_n12").data();
    std::string pitchN5  = TR("ui.audio_manager.pitch_n5").data();
    std::string pitch0   = TR("ui.audio_manager.pitch_0").data();

    // 显示标签与半音目标数组按索引对应。
    std::vector<std::string> pitchPresets = {
        pitchN24, pitchN12, pitchN5, pitch0
    };
    std::vector<float> targetPitches = { -24.0f, -12.0f, -5.0f, 0.0f };

    // 从新的一行重新模拟可用宽度。
    currentX       = 0.0f;
    int pitchLines = 1;
    for ( size_t i = 0; i < pitchPresets.size(); ++i ) {
        float btnW = ImGui::CalcTextSize(pitchPresets[i].c_str()).x +
                     ImGui::GetStyle().FramePadding.x * 2.0f;
        if ( i > 0 ) {
            if ( currentX + spacing + btnW < availWidgetW ) {
                // 当前行可容纳时继续累计。
                currentX += spacing + btnW;
            } else {
                // 否则增加一行并从当前按钮宽度重新计数。
                pitchLines++;
                currentX = btnW;
            }
        } else {
            currentX = btnW;
        }
    }
    // 预计算高度与实际回调行距完全一致。
    float pitchPresetsH =
        pitchLines * widgetH + (pitchLines - 1) * lineSpacing + rowPadY * 2.0f;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.pitch_presets").data(),
        labelWidth,
        [this,
         pitchPresets,
         targetPitches,
         &pitch,
         &changed,
         rowPadY,
         widgetH,
         spacing,
         lineSpacing](Clay_BoundingBox r, bool) {
            // 从控件区域顶部 padding 位置开始绘制音高按钮。
            ImGui::SetCursorScreenPos({ r.x, r.y + rowPadY });

            float currentX = 0.0f;
            float currentY = 0.0f;
            for ( size_t i = 0; i < pitchPresets.size(); ++i ) {
                float btnW = ImGui::CalcTextSize(pitchPresets[i].c_str()).x +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
                if ( i > 0 ) {
                    if ( currentX + spacing + btnW < r.width ) {
                        ImGui::SameLine();
                        currentX += spacing + btnW;
                    } else {
                        currentY += widgetH + lineSpacing;
                        ImGui::SetCursorScreenPos(
                            { r.x, r.y + rowPadY + currentY });
                        currentX = btnW;
                    }
                } else {
                    currentX = btnW;
                }

                ImGui::PushID(static_cast<int>(i + 100));
                // 偏移 ID 区间，避免与速度预设在同一窗口发生冲突。
                if ( ::MMM::UI::FeedbackButton(pitchPresets[i].c_str()) ) {
                    // 音高预设同样构成一次完整编辑，覆盖滑块草稿。
                    pitch                = targetPitches[i];
                    m_pitchSliderDraft   = pitch;
                    m_pitchSliderEditing = false;
                    changed              = true;
                }
                ImGui::PopID();
            }
        },
        pitchPresetsH);

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.pitch_value").data(),
        labelWidth,
        [this, &pitch, &changed](Clay_BoundingBox r, bool) {
            // 连续滑块允许 -24 到 +24 半音的非预设值。
            ImGui::SetNextItemWidth(r.width);
            // 音高和倍速都进入同一份离线 PCM 缓存键，采用相同提交边界。
            if ( !m_pitchSliderEditing ) m_pitchSliderDraft = pitch;
            ::MMM::UI::FeedbackSliderFloat(
                "##PitchSlider", &m_pitchSliderDraft, -24.0f, 24.0f, "%.4f st");
            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                // 音高与倍率共用资源级离线 DSP，编辑结束才触发重建。
                // 未改变最终项目值时跳过重复加载。
                changed              = changed || pitch != m_pitchSliderDraft;
                pitch                = m_pitchSliderDraft;
                m_pitchSliderEditing = false;
            } else {
                m_pitchSliderEditing = ImGui::IsItemActive();
            }
        });
}

/// @brief 构建效果音的播放、暂停和进度预览行。
/// @param parent 接收设置行的 Clay 容器。
/// @param rowIndex 本帧行对象池游标。
/// @param labelWidth 统一标签列宽。
///
/// 播放按钮在已暂停时显示继续并调用 resume，否则从头触发
/// play；暂停按钮始终只暂停 当前效果音实例。进度条读取 AudioManager
/// 快照，不控制播放位置。
/// @warning UI 热路径：效果音控制器可见时每帧读取播放时间，不得阻塞音频线程。
/// @details
/// 播放时间和总时长仅用于显示，不参与控制器布局缓存，因为它们每帧变化但
/// 不改变固定格式所需的最小宽度。
///
/// ProgressBar 不是可交互 Seek 控件；用户无法从本行修改效果音播放头，避免把预览
/// 操作与主音轨时间轴语义混合。
void AudioTrackControllerUI::buildEffectPreviewSection(CLayVBox& parent,
                                                       size_t&   rowIndex,
                                                       float     labelWidth)
{
    // 尺寸和间距统一读取布局缓存。
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    const float buttonHeight = layoutMetrics.buttonHeight;
    const float dpiScale     = layoutMetrics.dpiScale;
    const float gap          = layoutMetrics.rowSpacing;

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.play_preview").data(),
        labelWidth,
        [this, buttonHeight, dpiScale, gap](Clay_BoundingBox r, bool) {
            // 回调执行时读取最新效果音运行状态。
            auto& audio = Audio::AudioManager::instance();

            // 控件从 Clay 分配矩形左上角开始横向排列。
            ImGui::SetCursorScreenPos({ r.x, r.y });

            // 暂停状态决定主按钮的文本和点击行为。
            bool        isPaused = audio.isSFXPaused(m_trackId);
            const char* playText =
                isPaused ? TR("ui.audio_manager.resume_preview").data()
                         : TR("ui.audio_manager.play_preview").data();
            const char* pauseText = TR("ui.audio_manager.pause_preview").data();
            const auto& style     = ImGui::GetStyle();
            /// 计算满足 DPI 下限和本地化文本的按钮宽度。
            auto buttonWidth = [&](const char* text) {
                return std::max(
                    80.0f * dpiScale,
                    ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f);
            };
            const float playButtonW  = buttonWidth(playText);
            const float pauseButtonW = buttonWidth(pauseText);

            if ( ::MMM::UI::FeedbackButton(
                     playText, ImVec2(playButtonW, buttonHeight)) ) {
                if ( isPaused ) {
                    // 已暂停实例从当前播放头继续。
                    audio.resumeSoundEffect(m_trackId);
                } else {
                    // 未暂停状态触发一次效果音播放。
                    audio.playSoundEffect(m_trackId);
                }
            }

            // 暂停按钮与主按钮保持缓存行间距。
            ImGui::SameLine(0, gap);

            if ( ::MMM::UI::FeedbackButton(
                     pauseText, ImVec2(pauseButtonW, buttonHeight)) ) {
                audio.pauseSoundEffect(m_trackId);
            }

            // 进度条占用两个按钮之后的剩余宽度。
            ImGui::SameLine(0, gap);

            // 时长为零时进度回退零，避免除零。
            float duration     = (float)audio.getSFXDuration(m_trackId);
            float playbackTime = (float)audio.getSFXPlaybackTime(m_trackId);
            float progress =
                (duration > 0.0f) ? (playbackTime / duration) : 0.0f;
            std::string progressText =
                fmt::format("{:.2f}s / {:.2f}s", playbackTime, duration);

            // 极窄窗口下剩余宽度钳制为零，不产生负尺寸。
            float remaining = r.width - playButtonW - pauseButtonW - gap * 2.0f;
            remaining       = std::max(0.0f, remaining);
            ImGui::ProgressBar(progress,
                               ImVec2(remaining, buttonHeight),
                               progressText.c_str());
        });
}

/// @brief 构建打开波形与频谱分析视图的按钮行。
/// @param parent 接收按钮行的 Clay 容器。
/// @param rowIndex 本帧行对象池游标。
/// @param sourceManager 非拥有 UI 管理器，用于按名称查找分析视图。
///
/// 本行只属于主音轨控制器。按钮打开既有视图实例，不创建新窗口，也不直接执行波形
/// 或频谱计算；分析视图自行管理其缓存和渲染生命周期。
/// @details UIManager
/// 注册表以固定英文名称作为唯一键，可见标题来自翻译。点击已经
/// 注册的视图不会重复构造或播放打开反馈，保持窗口原有停靠与内部状态。
///
/// 两个按钮平分扣除一个 gap 后的可用宽度。极窄窗口下宽度钳制到零，最小窗口尺寸
/// 正常会阻止这种退化，但这里仍保持几何计算安全。
/// @warning UI 热路径：只在按钮点击且视图不存在时分配新视图。
/// @details 分析视图构造函数只接收本地化标题，固定注册键仍用于后续去重与
/// UIManager 查找。语言变化后的标题刷新由视图整体生命周期规则处理。
///
/// sourceManager
/// 由控制器更新入口保证非空；回调只在本帧同步执行，不跨帧保存指针。
void AudioTrackControllerUI::buildAnalysisButtons(CLayVBox&  parent,
                                                  size_t&    rowIndex,
                                                  UIManager* sourceManager)
{
    // 行 padding、gap 与按钮高度来自同一布局缓存。
    const auto& layoutMetrics =
        getLayoutMetrics(Config::AppConfig::instance().getWindowContentScale());
    /// 把非负浮点尺寸向上取整到 Clay 像素。
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    const float gap          = layoutMetrics.rowSpacing;
    const float buttonHeight = layoutMetrics.buttonHeight;

    // 分析按钮使用独立整行，不需要左侧设置标签列。
    auto& row = getRow(rowIndex++);
    row.setPadding(toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingX),
                   toLayoutPixels(layoutMetrics.rowPaddingY),
                   toLayoutPixels(layoutMetrics.rowPaddingY))
        .setSpacing(toLayoutPixels(gap))
        .setAlignment(Alignment::Center());

    // 行号参与稳定元素 ID，避免与其他控制器按钮组冲突。
    std::string rowId = "AT_Analysis_R" + std::to_string(rowIndex);
    row.addElement(
        rowId + "_btns",
        Sizing::Grow(),
        Sizing::Grow(),
        [this, sourceManager, gap, buttonHeight](Clay_BoundingBox r, bool) {
            // 两个按钮从 Clay 分配区域左上角开始同行排列。
            ImGui::SetCursorScreenPos({ r.x, r.y });
            // 扣除一个间距后平均分配宽度，负结果防御性钳制为零。
            float btnW = std::max(0.0f, (r.width - gap) * 0.5f);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.audio_manager.open_waveform").data(),
                     ImVec2(btnW, buttonHeight)) ) {
                // 注册键与动态本地化标题分离。
                std::string viewName = "AudioWaveform";
                if ( !sourceManager->getView<AudioWaveformView>(viewName) ) {
                    // 新建分析窗口时显式播放打开反馈。
                    ::MMM::UI::PlayPopupOpenFeedback();
                    // unique_ptr 所有权转交 UIManager 统一驱动和销毁。
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioWaveformView>(
                            TR("ui.audio_manager.waveform_title").data()));
                }
            }
            // 频谱按钮与波形按钮保持同一行和缓存间距。
            ImGui::SameLine(0, gap);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.audio_manager.open_spectrum").data(),
                     ImVec2(btnW, buttonHeight)) ) {
                // 频谱使用独立稳定注册键。
                std::string viewName = "AudioSpectrum";
                if ( !sourceManager->getView<AudioSpectrumView>(viewName) ) {
                    // 已存在视图时不重复注册，保留用户窗口状态。
                    ::MMM::UI::PlayPopupOpenFeedback();
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioSpectrumView>(
                            TR("ui.audio_manager.spectrum_title").data()));
                }
            }
        });

    // 分析行使用标准设置行高度，与其余区段纵向节奏一致。
    float rowH = layoutMetrics.rowHeight;
    // 行在父容器中横向增长，纵向固定。
    parent.addLayout(
        (rowId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

}  // namespace MMM::UI
