#include "config/VisualConfig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace MMM::Config
{
// 本文件维护视觉配置的持久化契约、旧格式迁移和画布组件布局解析。

namespace
{
/// @brief 旧版拍号与分拍线时间相对局部区间高度的默认字号比例。
constexpr float LEGACY_REPEATED_TEXT_FONT_SIZE_RATIO = 0.18f;
// 该常量只用于读取缺少坐标系标记的旧布局，不写回当前格式。

/// @brief 将背景电平柱颜色限制为可渲染的 RGBA 范围。
/// @param color 待规整的配置颜色。
/// @param fallback 非法分量使用的默认颜色。
/// @return 所有分量均有限且位于零到一之间的颜色。
std::array<float, 4> sanitizeBackgroundLevelColor(
    std::array<float, 4> color, const std::array<float, 4>& fallback)
{
    // 逐分量处理可保留合法通道，仅对损坏通道使用对应后备值。
    for ( std::size_t index = 0U; index < color.size(); ++index ) {
        // NaN 和无穷值不能进入着色器常量，先替换再执行范围限制。
        if ( !std::isfinite(color[index]) ) color[index] = fallback[index];
        color[index] = std::clamp(color[index], 0.0f, 1.0f);
    }
    return color;
}
}  // namespace

/// @brief 将分拍线显示模式写为稳定英文标识。
/// @param j 接收模式字符串的 JSON 值。
/// @param mode 当前分拍线显示模式。
/// @note 三态标识替代旧版 drawBeatLines 布尔值。
void to_json(nlohmann::json& j, const BeatLineDisplayMode& mode)
{
    // switch 仅产生公开模式名称，不持久化界面翻译文本。
    switch ( mode ) {
    case BeatLineDisplayMode::Always: j = "Always"; break;
    case BeatLineDisplayMode::NearCursor: j = "NearCursor"; break;
    case BeatLineDisplayMode::Hidden: j = "Hidden"; break;
    }
}

/// @brief 从配置恢复分拍线显示模式。
/// @param j 待读取的模式标识。
/// @param mode 接收已知模式；非法值回退到 Always。
/// @note 旧布尔字段的迁移由 VisualConfig 根读取层处理。
void from_json(const nlohmann::json& j, BeatLineDisplayMode& mode)
{
    // 默认始终显示，避免损坏配置使编辑辅助线静默消失。
    mode = BeatLineDisplayMode::Always;
    if ( !j.is_string() ) return;

    const auto& value = j.get_ref<const std::string&>();
    if ( value == "NearCursor" ) {
        mode = BeatLineDisplayMode::NearCursor;
    } else if ( value == "Hidden" ) {
        mode = BeatLineDisplayMode::Hidden;
    }
}

/// @brief 将背景图填充方式写为稳定英文标识。
/// @param j 接收填充模式字符串。
/// @param mode 当前背景图布局方式。
/// @note 标识描述几何适配策略，不依赖图像实际尺寸。
void to_json(nlohmann::json& j, const BackgroundFillMode& mode)
{
    // 四个枚举分支与渲染器支持的背景适配算法一一对应。
    switch ( mode ) {
    case BackgroundFillMode::Stretch: j = "Stretch"; break;
    case BackgroundFillMode::AspectFit: j = "AspectFit"; break;
    case BackgroundFillMode::AspectFill: j = "AspectFill"; break;
    case BackgroundFillMode::Center: j = "Center"; break;
    }
}

/// @brief 从配置恢复背景图填充方式。
/// @param j 待读取的模式标识。
/// @param mode 接收已知模式；非法值回退到 Stretch。
/// @note 精确匹配防止未来模式被旧客户端错误解释。
void from_json(const nlohmann::json& j, BackgroundFillMode& mode)
{
    // 回退值在类型检查前建立，所有早退路径都产生确定状态。
    mode = BackgroundFillMode::Stretch;
    if ( !j.is_string() ) return;

    const auto& value = j.get_ref<const std::string&>();
    if ( value == "AspectFit" ) {
        mode = BackgroundFillMode::AspectFit;
    } else if ( value == "AspectFill" ) {
        mode = BackgroundFillMode::AspectFill;
    } else if ( value == "Center" ) {
        mode = BackgroundFillMode::Center;
    }
}

/// @brief 序列化背景频谱的尺寸、颜色和输入来源设置。
/// @param j 接收背景频谱配置对象。
/// @param config 待保存的频段、布局、透明度和声道颜色。
/// @note 数值净化在读取层完成，写出保留当前内存状态。
void to_json(nlohmann::json& j, const BackgroundSpectrumConfig& config)
{
    // 几何比例与颜色分别保存，允许主题之外独立定制背景电平柱。
    j = nlohmann::json{ { "enabled", config.enabled },
                        { "bandCount", config.bandCount },
                        { "widthRatio", config.widthRatio },
                        { "heightRatio", config.heightRatio },
                        { "baselineRatio", config.baselineRatio },
                        { "opacity", config.opacity },
                        { "leftBarColor", config.leftBarColor },
                        { "rightBarColor", config.rightBarColor },
                        { "includeHitEffects", config.includeHitEffects } };
}

/// @brief 从 JSON 恢复并限制背景频谱设置。
/// @param j 用户配置中的 spectrum 对象。
/// @param config 接收经过范围和有限值校验的配置。
/// @note 全部回退值来自默认结构，保持构造与反序列化一致。
void from_json(const nlohmann::json& j, BackgroundSpectrumConfig& config)
{
    // 单一默认快照避免字段默认值在此实现中重复硬编码。
    const BackgroundSpectrumConfig defaults;
    // 频段数量受分析器支持范围限制，防止异常配置放大计算量。
    config.enabled   = j.value("enabled", defaults.enabled);
    config.bandCount = std::clamp(j.value("bandCount", defaults.bandCount),
                                  BACKGROUND_SPECTRUM_MIN_BANDS,
                                  BACKGROUND_SPECTRUM_MAX_BANDS);
    // 宽高和基线比例限制在画布归一化空间的有效区间。
    config.widthRatio =
        std::clamp(j.value("widthRatio", defaults.widthRatio), 0.10f, 1.0f);
    config.heightRatio =
        std::clamp(j.value("heightRatio", defaults.heightRatio), 0.05f, 1.0f);
    config.baselineRatio = std::clamp(
        j.value("baselineRatio", defaults.baselineRatio), 0.05f, 1.0f);
    // 透明度允许完全隐藏，但不能超过标准混合范围。
    config.opacity =
        std::clamp(j.value("opacity", defaults.opacity), 0.0f, 1.0f);
    // 左右声道颜色逐分量净化，损坏一侧不会影响另一侧配置。
    config.leftBarColor = sanitizeBackgroundLevelColor(
        j.value("leftBarColor", defaults.leftBarColor), defaults.leftBarColor);
    config.rightBarColor = sanitizeBackgroundLevelColor(
        j.value("rightBarColor", defaults.rightBarColor),
        defaults.rightBarColor);
    // 打击特效输入开关缺失时沿用默认频谱仅分析背景音乐的策略。
    config.includeHitEffects =
        j.value("includeHitEffects", defaults.includeHitEffects);
}

/// @brief 序列化背景图布局、暗化、透明度和频谱子配置。
/// @param j 接收背景配置对象。
/// @param config 待保存的完整背景视觉参数。
/// @note 旧字段名 darken_ratio 和 opaque_ratio 为兼容契约继续保留。
void to_json(nlohmann::json& j, const BackgroundConfig& config)
{
    // 频谱作为子对象保存，背景图与音频可视化仍共享同一背景职责。
    j = nlohmann::json{ { "fillMode", config.fillMode },
                        { "darken_ratio", config.darken_ratio },
                        { "opaque_ratio", config.opaque_ratio },
                        { "spectrum", config.spectrum } };
}

/// @brief 从 JSON 恢复背景视觉配置。
/// @param j 用户配置中的 background 对象。
/// @param config 接收填充、混合和频谱设置。
/// @note 缺失填充模式采用 AspectFill，匹配当前背景显示默认。
void from_json(const nlohmann::json& j, BackgroundConfig& config)
{
    // 比例字段沿用历史键名，避免读取时丢失既有用户偏好。
    config.fillMode     = j.value("fillMode", BackgroundFillMode::AspectFill);
    config.darken_ratio = j.value("darken_ratio", 0.7f);
    config.opaque_ratio = j.value("opaque_ratio", 1.0f);
    config.spectrum     = j.value("spectrum", BackgroundSpectrumConfig());
}

/// @brief 序列化单个画布信息组件的位置和外观。
/// @param j 接收组件布局对象。
/// @param placement 待保存的显隐、锚点、字号比例和颜色。
/// @note 锚点及字号均使用归一化比例，避免绑定具体窗口分辨率。
void to_json(nlohmann::json& j, const CanvasComponentPlacement& placement)
{
    // 显隐和颜色与几何共同保存，重置函数只选择性恢复几何字段。
    j = nlohmann::json{ { "visible", placement.visible },
                        { "anchorX", placement.anchorX },
                        { "anchorY", placement.anchorY },
                        { "fontSizeRatio", placement.fontSizeRatio },
                        { "color", placement.color } };
}

/// @brief 从 JSON 恢复单个画布信息组件布局。
/// @param j 待读取的组件对象。
/// @param placement 接收字段值及通用后备布局。
/// @note 具体组件的产品默认值由外层布局读取时传入。
void from_json(const nlohmann::json& j, CanvasComponentPlacement& placement)
{
    // 通用回退适合未知组件，已知组件会由外层 json.value 提供专用默认。
    placement.visible       = j.value("visible", false);
    placement.anchorX       = j.value("anchorX", 0.5f);
    placement.anchorY       = j.value("anchorY", 0.12f);
    placement.fontSizeRatio = j.value("fontSizeRatio", 0.035f);
    placement.color =
        j.value("color", std::array<float, 4>{ 1.0f, 1.0f, 1.0f, 1.0f });
}

namespace
{

/// @brief 按当前轨道布局生成单轨 KPS 的默认位置。
/// @param trackIndex 从零开始的轨道序号。
/// @param trackCount 当前轨道总数。
/// @param trackLeft 轨道区域左边界比例。
/// @param trackRight 轨道区域右边界比例。
/// @param color KPS 整组颜色。
/// @return 位于对应轨道中心上方的默认布局。
/// @warning 渲染热路径：未自定义的逐轨 KPS 每帧调用；只做常数次数值计算。
CanvasComponentPlacement defaultKpsTrackPlacement(
    std::int32_t trackIndex, std::int32_t trackCount, float trackLeft,
    float trackRight, const std::array<float, 4>& color)
{
    // 轨道数量至少为一，避免除零并为异常输入提供稳定中心位置。
    const auto safeTrackCount = std::max(trackCount, 1);
    // 轨道索引限制到现有范围，配置层不生成画布外默认锚点。
    const auto safeTrackIndex = std::clamp(trackIndex, 0, safeTrackCount - 1);
    // 轨道区域也限制到归一化画布，右边界不得越过左边界。
    trackLeft  = std::clamp(trackLeft, 0.0f, 1.0f);
    trackRight = std::clamp(trackRight, trackLeft, 1.0f);

    // 逐轨 KPS 默认可见，并继承总 KPS 组件的统一颜色。
    CanvasComponentPlacement result;
    result.visible = true;
    result.anchorX = trackLeft + (static_cast<float>(safeTrackIndex) + 0.5f) /
                                     static_cast<float>(safeTrackCount) *
                                     (trackRight - trackLeft);
    result.anchorY = 0.15f;
    // 轨道越多字号按平方根缩小，同时保留可读下限和布局上限。
    result.fontSizeRatio =
        std::clamp(0.044f / std::sqrt(static_cast<float>(safeTrackCount)),
                   0.0125f,
                   0.035f);
    result.color = color;
    return result;
}

/// @brief 恢复组件的默认位置和尺寸，并保留显隐与颜色。
/// @param placement 需要复位的组件布局。
/// @param defaultPlacement 对应组件的默认布局。
void resetPlacementGeometry(CanvasComponentPlacement&       placement,
                            const CanvasComponentPlacement& defaultPlacement)
{
    // 可见性与颜色属于用户外观偏好，几何重置不得覆盖它们。
    placement.anchorX       = defaultPlacement.anchorX;
    placement.anchorY       = defaultPlacement.anchorY;
    placement.fontSizeRatio = defaultPlacement.fontSizeRatio;
}

}  // namespace

/// @brief 解析指定画布组件实例的最终只读布局。
/// @param type 组件类别。
/// @param instanceIndex KPS 轨道序号或总计实例标识。
/// @param trackCount 当前轨道数量。
/// @param trackLeft 轨道区域左边界比例。
/// @param trackRight 轨道区域右边界比例。
/// @return 合并自动布局、保存覆写和同步设置后的值。
/// @warning 渲染热路径：只做有序查找和常数计算，不得分配或排序容器。
CanvasComponentPlacement CanvasComponentLayoutConfig::resolvedPlacement(
    CanvasComponentType type, std::int64_t instanceIndex,
    std::int32_t trackCount, float trackLeft, float trackRight) const
{
    // 非逐轨 KPS 或总计实例直接复用对应单例组件布局。
    if ( type != CanvasComponentType::Kps ||
         instanceIndex == KPS_TOTAL_INSTANCE_INDEX ) {
        return placement(type);
    }

    // 无法表示为保存索引的异常实例回退总 KPS，不进行窄化截断。
    if ( instanceIndex > std::numeric_limits<std::int32_t>::max() ) {
        return kps;
    }
    // 自动位置始终可用，后续只在确有保存项时覆盖。
    const auto trackIndex         = static_cast<std::int32_t>(instanceIndex);
    const auto automaticPlacement = defaultKpsTrackPlacement(
        trackIndex, trackCount, trackLeft, trackRight, kps.color);
    // 同步字号只有有限正值才生效，零值表示尚未建立共享字号。
    const bool hasSynchronizedFontSize =
        std::isfinite(kpsTrackFontSizeRatio) && kpsTrackFontSizeRatio > 0.0f;
    const float synchronizedFontSize =
        hasSynchronizedFontSize
            ? std::clamp(kpsTrackFontSizeRatio, 0.0125f, 0.25f)
            : automaticPlacement.fontSizeRatio;
    // 先应用共享字号到默认值，使没有存储项的轨道也参与同步。
    CanvasComponentPlacement result = automaticPlacement;
    if ( hasSynchronizedFontSize ) {
        result.fontSizeRatio = synchronizedFontSize;
    }
    // kpsTracks 在读取和插入时保持按索引排序，可使用二分查找。
    const auto stored =
        std::lower_bound(kpsTracks.begin(),
                         kpsTracks.end(),
                         trackIndex,
                         [](const auto& entry, std::int32_t index) {
                             return entry.trackIndex < index;
                         });
    // 保存项覆盖自动锚点和字号，但最终公共外观仍由总 KPS 决定。
    if ( stored != kpsTracks.end() && stored->trackIndex == trackIndex ) {
        result = stored->placement;
    }
    // 尺寸同步作为最终规则覆盖单轨保存字号。
    if ( syncKpsTrackSizes ) {
        result.fontSizeRatio = synchronizedFontSize;
    }
    // 所有逐轨实例共享总开关和颜色，避免产生相互矛盾的外观状态。
    result.visible = kps.visible;
    result.color   = kps.color;
    return result;
}

/// @brief 获取或创建指定组件实例的可编辑布局。
/// @param type 组件类别。
/// @param instanceIndex KPS 轨道序号或总计实例标识。
/// @param trackCount 当前轨道数量。
/// @param trackLeft 轨道区域左边界比例。
/// @param trackRight 轨道区域右边界比例。
/// @return 现有布局或按自动位置插入的新持久布局引用。
/// @warning 交互编辑路径：新轨道首次访问可能向有序容器插入元素。
CanvasComponentPlacement& CanvasComponentLayoutConfig::editablePlacement(
    CanvasComponentType type, std::int64_t instanceIndex,
    std::int32_t trackCount, float trackLeft, float trackRight)
{
    // 不可细分组件、总计 KPS 和负实例都编辑单例布局。
    if ( type != CanvasComponentType::Kps ||
         instanceIndex == KPS_TOTAL_INSTANCE_INDEX || instanceIndex < 0 ) {
        return placement(type);
    }

    // 超出持久索引表示范围时返回总 KPS，避免写入截断后的错误轨道。
    if ( instanceIndex > std::numeric_limits<std::int32_t>::max() ) {
        return kps;
    }
    const auto trackIndex = static_cast<std::int32_t>(instanceIndex);
    // lower_bound 同时给出既有元素或维持排序的不命中插入位置。
    const auto stored =
        std::lower_bound(kpsTracks.begin(),
                         kpsTracks.end(),
                         trackIndex,
                         [](const auto& entry, std::int32_t index) {
                             return entry.trackIndex < index;
                         });
    if ( stored != kpsTracks.end() && stored->trackIndex == trackIndex ) {
        return stored->placement;
    }

    // 新轨道从当前轨道区域推导默认位置，并继承总 KPS 颜色。
    auto newPlacement = defaultKpsTrackPlacement(
        trackIndex, trackCount, trackLeft, trackRight, kps.color);
    // 已建立共享字号时，新创建的轨道应立即使用同一尺寸。
    if ( std::isfinite(kpsTrackFontSizeRatio) &&
         kpsTrackFontSizeRatio > 0.0f ) {
        newPlacement.fontSizeRatio =
            std::clamp(kpsTrackFontSizeRatio, 0.0125f, 0.25f);
    }
    // 在 lower_bound 位置插入保持后续热路径二分查找不变量。
    return kpsTracks.insert(stored, { trackIndex, newPlacement })->placement;
}

/// @brief 设置共享逐轨 KPS 字号并更新全部已保存轨道。
/// @param fontSizeRatio 相对画布高度的字号比例。
/// @warning 交互设置路径会遍历已保存轨道，不得在每帧渲染中调用。
void CanvasComponentLayoutConfig::synchronizeKpsTrackFontSize(
    float fontSizeRatio)
{
    // 非有限输入不改变现有共享值，避免污染全部轨道布局。
    if ( !std::isfinite(fontSizeRatio) ) return;
    kpsTrackFontSizeRatio = std::clamp(fontSizeRatio, 0.0125f, 0.25f);
    // 已保存项立即同步；未保存轨道在解析时读取共享值。
    for ( auto& track : kpsTracks ) {
        track.placement.fontSizeRatio = kpsTrackFontSizeRatio;
    }
}

/// @brief 设置逐轨 KPS 相对位置同步模式。
/// @param enabled 是否启用轨道间相对位置同步。
/// @note 与全部 KPS 绝对位置同步互斥，启用一方会关闭另一方。
void CanvasComponentLayoutConfig::setSyncKpsTrackRelativePositions(bool enabled)
{
    // 先保存请求状态，再维护两个位置同步模式的互斥不变量。
    syncKpsTrackRelativePositions = enabled;
    if ( enabled ) {
        syncAllKpsComponentPositions = false;
    }
}

/// @brief 设置全部 KPS 组件绝对位置同步模式。
/// @param enabled 是否启用总计和逐轨组件统一位置。
/// @note 与逐轨相对位置同步互斥，启用一方会关闭另一方。
void CanvasComponentLayoutConfig::setSyncAllKpsComponentPositions(bool enabled)
{
    // 禁用时不自动启用另一个模式，允许两种同步都关闭。
    syncAllKpsComponentPositions = enabled;
    if ( enabled ) {
        syncKpsTrackRelativePositions = false;
    }
}

/// @brief 恢复指定组件类型的默认几何布局。
/// @param type 要重置的画布组件类别。
/// @note 保留组件显隐和颜色；KPS 额外清除逐轨几何覆写及共享字号。
void CanvasComponentLayoutConfig::resetPlacementToDefault(
    CanvasComponentType type)
{
    // 每类组件使用自身产品默认布局，不能复用通用反序列化后备值。
    switch ( type ) {
    case CanvasComponentType::JudgmentLineTime:
        resetPlacementGeometry(judgmentLineTime,
                               DEFAULT_JUDGMENT_LINE_TIME_PLACEMENT);
        break;
    case CanvasComponentType::BeatNumber:
        resetPlacementGeometry(beatNumber, DEFAULT_BEAT_NUMBER_PLACEMENT);
        break;
    case CanvasComponentType::BeatLineTime:
        resetPlacementGeometry(beatLineTime, DEFAULT_BEAT_LINE_TIME_PLACEMENT);
        break;
    case CanvasComponentType::Kps:
        // 总 KPS 几何恢复后，逐轨覆写必须一起清除以重新启用自动布局。
        resetPlacementGeometry(kps, DEFAULT_KPS_TOTAL_PLACEMENT);
        kpsTracks.clear();
        kpsTrackFontSizeRatio = 0.0f;
        break;
    case CanvasComponentType::BackgroundSpectrum:
        // 背景频谱只恢复锚点和尺寸，继续保留用户显隐及颜色设置。
        resetPlacementGeometry(backgroundSpectrum,
                               DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT);
        break;
    // Count 是枚举哨兵，不对应可编辑组件或默认布局。
    case CanvasComponentType::Count: break;
    }
}

/// @brief 序列化单条玩家轨道的 KPS 布局覆写。
/// @param j 接收轨道索引与组件布局对象。
/// @param placement 待保存的轨道标识和布局。
/// @note 列表顺序不承载轨道语义，trackIndex 是稳定键。
void to_json(nlohmann::json& j, const CanvasKpsTrackPlacement& placement)
{
    // 索引和布局共同保存，避免数组排序变化后绑定到错误轨道。
    j = nlohmann::json{ { "trackIndex", placement.trackIndex },
                        { "placement", placement.placement } };
}

/// @brief 从 JSON 恢复单轨 KPS 布局覆写。
/// @param j 待读取的轨道布局对象。
/// @param placement 接收索引和通用组件布局。
/// @note 负索引和重复项由外层布局读取时统一清理。
void from_json(const nlohmann::json& j, CanvasKpsTrackPlacement& placement)
{
    // 此层保持值解析简单，集合不变量由拥有容器集中维护。
    placement.trackIndex = j.value("trackIndex", 0);
    placement.placement  = j.value("placement", CanvasComponentPlacement{});
}

/// @brief 序列化画布信息组件的全局及逐轨布局。
/// @param j 接收组件布局配置对象。
/// @param config 待保存的组件、同步模式和共享字号。
/// @note fontSizeUsesCanvasHeight 标记当前字号比例坐标系版本。
void to_json(nlohmann::json& j, const CanvasComponentLayoutConfig& config)
{
    // 三类文字组件与背景频谱均保存完整布局值。
    j = nlohmann::json{
        { "judgmentLineTime", config.judgmentLineTime },
        { "beatNumber", config.beatNumber },
        { "beatLineTime", config.beatLineTime },
        // 新格式字号相对画布高度计算，旧读取端可忽略该迁移标志。
        { "fontSizeUsesCanvasHeight", true },
        { "kps", config.kps },
        { "backgroundSpectrum", config.backgroundSpectrum },
        // 逐轨覆写和同步策略独立保存，支持关闭同步后恢复个别布局。
        { "kpsTracks", config.kpsTracks },
        { "syncKpsTrackSizes", config.syncKpsTrackSizes },
        { "syncKpsTrackRelativePositions",
          config.syncKpsTrackRelativePositions },
        { "syncAllKpsComponentPositions", config.syncAllKpsComponentPositions },
        { "kpsTrackFontSizeRatio", config.kpsTrackFontSizeRatio }
    };
}

/// @brief 从 JSON 恢复画布组件布局并迁移旧字号坐标系。
/// @param j 用户配置中的 canvasComponents 对象。
/// @param config 接收规范化后的组件和逐轨布局。
/// @note 读取结束时 kpsTracks 保证索引非负、升序且唯一。
void from_json(const nlohmann::json& j, CanvasComponentLayoutConfig& config)
{
    // 每个已知组件使用自身默认布局，避免通用 placement 默认覆盖产品位置。
    config.judgmentLineTime =
        j.value("judgmentLineTime", CanvasComponentPlacement());
    config.beatNumber = j.value("beatNumber", DEFAULT_BEAT_NUMBER_PLACEMENT);
    config.beatLineTime =
        j.value("beatLineTime", DEFAULT_BEAT_LINE_TIME_PLACEMENT);
    // 旧格式没有标志，其重复文字字号相对局部区间而非整个画布。
    const bool fontSizeUsesCanvasHeight =
        j.value("fontSizeUsesCanvasHeight", false);
    // 仅迁移配置中真实存在的旧字段，缺失项已经使用新默认值。
    if ( !fontSizeUsesCanvasHeight && j.contains("beatNumber") ) {
        config.beatNumber.fontSizeRatio =
            std::clamp(config.beatNumber.fontSizeRatio *
                           (DEFAULT_BEAT_NUMBER_PLACEMENT.fontSizeRatio /
                            LEGACY_REPEATED_TEXT_FONT_SIZE_RATIO),
                       0.0125f,
                       0.25f);
    }
    // 拍线时间与拍号使用同一旧字号基准，但分别保留各自新默认比例。
    if ( !fontSizeUsesCanvasHeight && j.contains("beatLineTime") ) {
        config.beatLineTime.fontSizeRatio =
            std::clamp(config.beatLineTime.fontSizeRatio *
                           (DEFAULT_BEAT_LINE_TIME_PLACEMENT.fontSizeRatio /
                            LEGACY_REPEATED_TEXT_FONT_SIZE_RATIO),
                       0.0125f,
                       0.25f);
    }
    // KPS 总计、背景频谱和逐轨覆写在字号迁移后独立恢复。
    config.kps = j.value("kps", DEFAULT_KPS_TOTAL_PLACEMENT);
    config.backgroundSpectrum =
        j.value("backgroundSpectrum", DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT);
    config.kpsTracks =
        j.value("kpsTracks", std::vector<CanvasKpsTrackPlacement>{});
    config.syncKpsTrackSizes = j.value("syncKpsTrackSizes", false);
    // 字号同步与位置同步互不排斥，分别恢复其持久化开关。
    config.syncKpsTrackRelativePositions =
        j.value("syncKpsTrackRelativePositions", false);
    // 通过 setter 恢复绝对同步以维护两个位置同步模式互斥。
    config.setSyncAllKpsComponentPositions(
        j.value("syncAllKpsComponentPositions", false));
    config.kpsTrackFontSizeRatio = j.value("kpsTrackFontSizeRatio", 0.0f);
    // 非有限或非正共享字号表示未建立同步基准，统一归零。
    if ( !std::isfinite(config.kpsTrackFontSizeRatio) ||
         config.kpsTrackFontSizeRatio <= 0.0f ) {
        config.kpsTrackFontSizeRatio = 0.0f;
    } else {
        config.kpsTrackFontSizeRatio =
            std::clamp(config.kpsTrackFontSizeRatio, 0.0125f, 0.25f);
    }
    // 负轨道索引没有运行时实例语义，先从持久化集合移除。
    std::erase_if(config.kpsTracks, [](const auto& placement) {
        return placement.trackIndex < 0;
    });
    // 稳定排序保留重复索引的文件先后顺序，随后明确保留首项。
    std::stable_sort(config.kpsTracks.begin(),
                     config.kpsTracks.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.trackIndex < rhs.trackIndex;
                     });
    // 去重建立二分查找和有序插入依赖的唯一索引不变量。
    config.kpsTracks.erase(std::unique(config.kpsTracks.begin(),
                                       config.kpsTracks.end(),
                                       [](const auto& lhs, const auto& rhs) {
                                           return lhs.trackIndex ==
                                                  rhs.trackIndex;
                                       }),
                           config.kpsTracks.end());
}

/// @brief 序列化预览区四边内边距。
/// @param j 接收边距对象。
/// @param margin 待保存的左、上、右、下像素边距。
/// @note 分边保存允许未来对称或单边编辑而不改变文件格式。
void to_json(nlohmann::json& j, const PreviewAreaConfig::AreaMargin& margin)
{
    // 字段顺序按顺时针布局，便于人工检查配置。
    j = nlohmann::json{ { "left", margin.left },
                        { "top", margin.top },
                        { "right", margin.right },
                        { "bottom", margin.bottom } };
}

/// @brief 从 JSON 恢复预览区四边内边距。
/// @param j 待读取的边距对象。
/// @param margin 接收各边数值。
/// @note 缺失边统一采用四像素默认间距。
void from_json(const nlohmann::json& j, PreviewAreaConfig::AreaMargin& margin)
{
    // 各边独立读取，部分配置不会重置其他边的默认值。
    margin.left   = j.value("left", 4.0f);
    margin.top    = j.value("top", 4.0f);
    margin.right  = j.value("right", 4.0f);
    margin.bottom = j.value("bottom", 4.0f);
}

/// @brief 序列化预览区比例、边缘滚动和辅助线设置。
/// @param j 接收预览区配置对象。
/// @param config 待保存的预览视口偏好。
/// @note 分拍线与 Timing 线使用独立开关。
void to_json(nlohmann::json& j, const PreviewAreaConfig& config)
{
    // 几何、交互灵敏度和绘制开关共同构成预览区完整偏好。
    j = nlohmann::json{ { "areaRatio", config.areaRatio },
                        { "edgeScrollSensitivity",
                          config.edgeScrollSensitivity },
                        { "margin", config.margin },
                        { "drawBeatLines", config.drawBeatLines },
                        { "drawTimingLines", config.drawTimingLines } };
}

/// @brief 从 JSON 恢复预览区配置。
/// @param j 用户配置中的 previewConfig 对象。
/// @param config 接收比例、边距和辅助线开关。
/// @note 旧配置默认隐藏分拍线但保留 Timing 线。
void from_json(const nlohmann::json& j, PreviewAreaConfig& config)
{
    // 默认值与 PreviewAreaConfig 构造语义保持一致。
    config.areaRatio             = j.value("areaRatio", 5.0f);
    config.edgeScrollSensitivity = j.value("edgeScrollSensitivity", 1.0f);
    config.margin = j.value("margin", PreviewAreaConfig::AreaMargin());
    // 两类辅助线缺失时采用当前预览区的低干扰显示组合。
    config.drawBeatLines   = j.value("drawBeatLines", false);
    config.drawTimingLines = j.value("drawTimingLines", true);
}

/// @brief 将频谱细节等级写为稳定英文标识。
/// @param j 接收性能等级字符串。
/// @param level 当前频谱细节等级。
/// @note 标识描述相对质量，不固化具体采样参数。
void to_json(nlohmann::json& j, const SpectrumDetailLevel& level)
{
    // 六档模式对应分析器预设，配置文件保持人类可读。
    switch ( level ) {
    case SpectrumDetailLevel::Performance: j = "Performance"; break;
    case SpectrumDetailLevel::Balanced: j = "Balanced"; break;
    case SpectrumDetailLevel::Fine: j = "Fine"; break;
    case SpectrumDetailLevel::Ultra: j = "Ultra"; break;
    case SpectrumDetailLevel::Extreme: j = "Extreme"; break;
    case SpectrumDetailLevel::Experimental: j = "Experimental"; break;
    }
}

/// @brief 从配置恢复频谱细节等级。
/// @param j 待读取的等级标识。
/// @param level 接收已知等级；非法值回退到 Performance。
/// @note 根 VisualConfig 可为字段缺失指定 Balanced 默认值。
void from_json(const nlohmann::json& j, SpectrumDetailLevel& level)
{
    // 类型级回退选择最低负载，损坏配置不会放大运行时计算成本。
    level = SpectrumDetailLevel::Performance;
    if ( !j.is_string() ) return;

    const auto& value = j.get_ref<const std::string&>();
    if ( value == "Balanced" ) {
        level = SpectrumDetailLevel::Balanced;
    } else if ( value == "Fine" ) {
        level = SpectrumDetailLevel::Fine;
    } else if ( value == "Ultra" ) {
        level = SpectrumDetailLevel::Ultra;
    } else if ( value == "Extreme" ) {
        level = SpectrumDetailLevel::Extreme;
    } else if ( value == "Experimental" ) {
        level = SpectrumDetailLevel::Experimental;
    }
}

/// @brief 将辅助横向区域覆盖值序列化为配置对象。
/// @param j 输出 JSON 对象。
/// @param layout 待保存的横向区域布局。
/// @note 未设置或非有限可选值不会写入配置对象。
void to_json(nlohmann::json& j, const HorizontalRegionLayout& layout)
{
    // 空字段保留“按旧布局动态推导”的迁移语义，不写入 JSON。
    j = nlohmann::json::object();
    if ( layout.left && std::isfinite(*layout.left) ) {
        // left 和 right 可独立覆盖旧轨道区域边界。
        j["left"] = *layout.left;
    }
    if ( layout.right && std::isfinite(*layout.right) ) {
        j["right"] = *layout.right;
    }
    if ( layout.width && std::isfinite(*layout.width) ) {
        // 宽度只在内存值有限时持久化，正值约束由读取端执行。
        j["width"] = *layout.width;
    }
}

/// @brief 从配置对象读取辅助横向区域覆盖值。
/// @param j 输入 JSON 对象。
/// @param layout 接收合法覆盖值的横向区域布局。
/// @note 非对象输入清空全部覆盖，使上层继续使用动态默认布局。
void from_json(const nlohmann::json& j, HorizontalRegionLayout& layout)
{
    // 非对象或非法数值按字段回退为空，避免一项损坏拖累另一个覆盖值。
    layout = {};
    if ( !j.is_object() ) return;
    if ( const auto it = j.find("left"); it != j.end() && it->is_number() ) {
        // 数字类型仍可能表示非有限浮点，转换后必须再次验证。
        const float value = it->get<float>();
        if ( std::isfinite(value) ) layout.left = value;
    }
    if ( const auto it = j.find("right"); it != j.end() && it->is_number() ) {
        const float value = it->get<float>();
        if ( std::isfinite(value) ) layout.right = value;
    }
    if ( const auto it = j.find("width"); it != j.end() && it->is_number() ) {
        // 宽度额外要求为正，避免生成反向或退化区域。
        const float value = it->get<float>();
        if ( std::isfinite(value) && value > 0.0F ) layout.width = value;
    }
}

/// @brief 序列化主轨道边界及辅助横向区域覆盖。
/// @param j 接收轨道布局对象。
/// @param layout 待保存的主区域和草稿、批注、BGM 区域。
/// @note 辅助区空覆盖保持旧版按主轨道动态推导的语义。
void to_json(nlohmann::json& j, const TrackLayout& layout)
{
    // 辅助区作为嵌套对象保存，旧版本缺少这些键时仍可无损加载。
    j = nlohmann::json{ { "left", layout.left },
                        { "top", layout.top },
                        { "right", layout.right },
                        { "bottom", layout.bottom },
                        { "draftLanes", layout.draftLanes },
                        { "annotation", layout.annotation },
                        { "bgmLanes", layout.bgmLanes } };
}

/// @brief 从 JSON 恢复主轨道与辅助区域布局。
/// @param j 待读取的轨道布局对象。
/// @param layout 接收边界和可选辅助区覆盖。
/// @note 缺失辅助区字段构造空覆盖，不固定旧版动态布局结果。
void from_json(const nlohmann::json& j, TrackLayout& layout)
{
    // 主轨道默认占画布中间六成宽度和九成高度。
    layout.left       = j.value("left", 0.2f);
    layout.top        = j.value("top", 0.05f);
    layout.right      = j.value("right", 0.8f);
    layout.bottom     = j.value("bottom", 0.95f);
    layout.draftLanes = j.value("draftLanes", HorizontalRegionLayout{});
    layout.annotation = j.value("annotation", HorizontalRegionLayout{});
    layout.bgmLanes   = j.value("bgmLanes", HorizontalRegionLayout{});
    // 空辅助覆盖由画布根据主轨道和项目内容在运行期解析。
}

/// @brief 序列化特定 Key 数对应的独立画布布局。
/// @param j 接收 Key 数布局对象。
/// @param config 待保存的轨道、判定线和组件布局快照。
/// @note judgeline_pos 沿用历史键名以保持文件兼容。
void to_json(nlohmann::json& j, const KeyCountLayoutConfig& config)
{
    // keyCount 作为集合稳定键，其余字段构成该键数的完整覆写。
    j = nlohmann::json{
        { "keyCount", config.keyCount },
        { "trackLayout", config.trackLayout },
        { "judgeline_pos", config.judgmentLinePosition },
        { "canvasComponents", config.canvasComponents },
    };
}

/// @brief 从 JSON 恢复单个 Key 数布局快照。
/// @param j 待读取的布局对象。
/// @param config 接收键数和各视觉子布局。
/// @note 无效键数与重复项由 VisualConfig 集合读取时清理。
void from_json(const nlohmann::json& j, KeyCountLayoutConfig& config)
{
    // 此层只解析单项，保持集合验证集中在拥有者中。
    config.keyCount             = j.value("keyCount", 0);
    config.trackLayout          = j.value("trackLayout", TrackLayout());
    config.judgmentLinePosition = j.value("judgeline_pos", 0.85f);
    config.canvasComponents =
        j.value("canvasComponents", CanvasComponentLayoutConfig());
    // 单项读取不主动应用布局，调用方按当前 Key 数选择何时生效。
}

/// @brief 在有序集合中查找指定 Key 数的布局覆写。
/// @param keyCount 玩家轨道数量。
/// @return 命中时返回容器内稳定观察指针，否则返回 nullptr。
/// @warning 渲染和交互查询路径只执行二分查找，不得在此排序或分配。
const KeyCountLayoutConfig* VisualConfig::findKeyCountLayout(
    std::int32_t keyCount) const
{
    // 非正键数代表未指定布局，直接使用旧版根级配置。
    if ( keyCount <= 0 ) return nullptr;
    // keyCountLayouts 在读取和编辑插入时维持升序不变量。
    const auto stored =
        std::lower_bound(keyCountLayouts.begin(),
                         keyCountLayouts.end(),
                         keyCount,
                         [](const auto& entry, std::int32_t count) {
                             return entry.keyCount < count;
                         });
    // lower_bound 可能返回更大的下一项，必须再次比较精确键值。
    if ( stored == keyCountLayouts.end() || stored->keyCount != keyCount ) {
        return nullptr;
    }
    return &*stored;
}

/// @brief 获取指定 Key 数的有效轨道布局。
/// @param keyCount 玩家轨道数量。
/// @return 专用布局存在时返回其引用，否则返回根级兼容布局。
/// @warning 渲染热路径只进行有序查找，不复制布局对象。
const TrackLayout& VisualConfig::trackLayoutForKeyCount(
    std::int32_t keyCount) const
{
    // 根级 trackLayout 是旧配置和未定制键数的共同后备。
    const auto* stored = findKeyCountLayout(keyCount);
    return stored ? stored->trackLayout : trackLayout;
}

/// @brief 获取指定 Key 数的有效判定线位置。
/// @param keyCount 玩家轨道数量。
/// @return 专用位置存在时返回该值，否则返回根级 judgeline_pos。
/// @warning 渲染热路径只执行有序查找和标量返回。
float VisualConfig::judgmentLinePositionForKeyCount(std::int32_t keyCount) const
{
    const auto* stored = findKeyCountLayout(keyCount);
    return stored ? stored->judgmentLinePosition : judgeline_pos;
}

/// @brief 获取指定 Key 数的有效画布组件布局。
/// @param keyCount 玩家轨道数量。
/// @return 专用组件布局存在时返回其引用，否则返回根级布局。
/// @warning 渲染热路径不复制可能包含逐轨覆写的组件容器。
const CanvasComponentLayoutConfig& VisualConfig::canvasComponentsForKeyCount(
    std::int32_t keyCount) const
{
    const auto* stored = findKeyCountLayout(keyCount);
    return stored ? stored->canvasComponents : canvasComponents;
}

namespace
{
/// @brief 取得或建立指定 Key 数布局，新增项继承旧版布局模板。
/// @param layouts 按 Key 数升序保存的布局集合。
/// @param keyCount 玩家轨道数量。
/// @param trackLayout 轨道布局模板。
/// @param judgmentLinePosition 判定线位置模板。
/// @param canvasComponents 画布组件布局模板。
/// @return 对应 Key 数的可写布局。
/// @warning 设置交互路径可能向有序容器插入并移动后续元素。
KeyCountLayoutConfig& editableKeyCountLayout(
    std::vector<KeyCountLayoutConfig>& layouts, std::int32_t keyCount,
    const TrackLayout& trackLayout, float judgmentLinePosition,
    const CanvasComponentLayoutConfig& canvasComponents)
{
    // lower_bound 同时定位现有项和保持升序所需的插入位置。
    const auto stored =
        std::lower_bound(layouts.begin(),
                         layouts.end(),
                         keyCount,
                         [](const auto& entry, std::int32_t count) {
                             return entry.keyCount < count;
                         });
    // 已存在覆写直接返回，编辑不会重置其其他布局字段。
    if ( stored != layouts.end() && stored->keyCount == keyCount ) {
        return *stored;
    }
    // 首次创建完整继承根级模板，使修改一个子项不丢失其他视觉设置。
    return *layouts.insert(stored,
                           KeyCountLayoutConfig{
                               .keyCount             = keyCount,
                               .trackLayout          = trackLayout,
                               .judgmentLinePosition = judgmentLinePosition,
                               .canvasComponents     = canvasComponents,
                           });
}
}  // namespace

/// @brief 获取指定 Key 数的可编辑轨道布局。
/// @param keyCount 玩家轨道数量。
/// @return 正键数对应的专用布局；非正值返回根级布局。
/// @warning 首次编辑某键数会创建并插入完整布局快照。
TrackLayout& VisualConfig::editableTrackLayoutForKeyCount(std::int32_t keyCount)
{
    // 非正键数沿用旧版根级编辑入口，不创建无效集合项。
    if ( keyCount <= 0 ) return trackLayout;
    return editableKeyCountLayout(keyCountLayouts,
                                  keyCount,
                                  trackLayout,
                                  judgeline_pos,
                                  canvasComponents)
        .trackLayout;
}

/// @brief 获取指定 Key 数的可编辑判定线位置。
/// @param keyCount 玩家轨道数量。
/// @return 正键数对应的位置引用；非正值返回根级位置。
/// @warning 首次编辑某键数会创建并插入完整布局快照。
float& VisualConfig::editableJudgmentLinePositionForKeyCount(
    std::int32_t keyCount)
{
    if ( keyCount <= 0 ) return judgeline_pos;
    return editableKeyCountLayout(keyCountLayouts,
                                  keyCount,
                                  trackLayout,
                                  judgeline_pos,
                                  canvasComponents)
        .judgmentLinePosition;
}

/// @brief 获取指定 Key 数的可编辑画布组件布局。
/// @param keyCount 玩家轨道数量。
/// @return 正键数对应的组件布局；非正值返回根级布局。
/// @warning 首次编辑某键数会创建并插入完整布局快照。
CanvasComponentLayoutConfig& VisualConfig::editableCanvasComponentsForKeyCount(
    std::int32_t keyCount)
{
    if ( keyCount <= 0 ) return canvasComponents;
    return editableKeyCountLayout(keyCountLayouts,
                                  keyCount,
                                  trackLayout,
                                  judgeline_pos,
                                  canvasComponents)
        .canvasComponents;
}

/// @brief 把指定 Key 数的专用布局复制到根级兼容字段。
/// @param keyCount 要应用的玩家轨道数量。
/// @note 未找到专用项时保持根级布局不变。
/// @warning 设置切换路径会复制包含逐轨覆写的组件布局，不可每帧调用。
void VisualConfig::applyKeyCountLayout(std::int32_t keyCount)
{
    // 先以观察指针查找，缺失时不创建新布局。
    const auto* stored = findKeyCountLayout(keyCount);
    if ( !stored ) return;
    // 命中项由有序集合拥有，复制期间不会改变集合本身。
    // 三个字段作为同一布局快照应用，避免出现键数间混合状态。
    trackLayout      = stored->trackLayout;
    judgeline_pos    = stored->judgmentLinePosition;
    canvasComponents = stored->canvasComponents;
}

/// @brief 序列化完整视觉配置及各 Key 数布局覆写。
/// @param j 接收视觉配置根对象。
/// @param config 待保存的画布、背景、预览和交互视觉参数。
/// @note 继续写出旧 drawBeatLines 布尔字段供旧版本兼容读取。
void to_json(nlohmann::json& j, const VisualConfig& config)
{
    // 背景频谱 enabled 的真实来源已迁移到画布组件 visible 字段。
    auto background = config.background;
    background.spectrum.enabled =
        config.canvasComponents.backgroundSpectrum.visible;
    // 布局根、组件和 Key 数覆写共同保存，支持按轨道数独立定制。
    j = nlohmann::json{
        { "trackLayout", config.trackLayout },
        { "canvasComponents", config.canvasComponents },
        { "keyCountLayouts", config.keyCountLayouts },
        // background 副本携带同步后的旧 enabled 值，保持向后兼容。
        { "background", background },
        { "previewConfig", config.previewConfig },
        { "trackBoxLineWidth", config.trackBoxLineWidth },
        { "judgeline_pos", config.judgeline_pos },
        // 音符缩放、标签和填充模式描述物件的基础呈现。
        { "noteScaleX", config.noteScaleX },
        { "noteScaleY", config.noteScaleY },
        { "showBoundSampleLabels", config.showBoundSampleLabels },
        { "noteFillMode", config.noteFillMode },
        // 三类视觉时间偏移分别保存，避免波形或频谱校准相互覆盖。
        { "visualOffset", config.visualOffset },
        { "waveformVisualOffset", config.waveformVisualOffset },
        { "spectrumVisualOffset", config.spectrumVisualOffset },
        // 时间线缩放、滚动动画和吸附阈值属于画布交互视觉参数。
        { "timelineZoom", config.timelineZoom },
        { "scrollAnimationDuration", config.scrollAnimationDuration },
        { "enableLinearScrollMapping", config.enableLinearScrollMapping },
        { "snapThreshold", config.snapThreshold },
        { "beatLineAlpha", config.beatLineAlpha },
        { "hoverSubdivisionLineExtensionRatio",
          config.hoverSubdivisionLineExtensionRatio },
        // 新三态模式和光标邻域比例共同控制分拍线动态显示。
        { "beatLineDisplayMode", config.beatLineDisplayMode },
        { "beatLineCursorVisibleRatio", config.beatLineCursorVisibleRatio },
        { "beatLineCursorFadeRatio", config.beatLineCursorFadeRatio },
        { "drawBeatLinesBeforeFirstTiming",
          config.drawBeatLinesBeforeFirstTiming },
        // 旧布尔值只表达 Hidden 与非 Hidden，供旧客户端读取。
        { "drawBeatLines",
          config.beatLineDisplayMode != BeatLineDisplayMode::Hidden },
        { "spectrumDetailLevel", config.spectrumDetailLevel },
        // 打击特效和交互包围盒调试参数位于列表末端便于扩展。
        { "enableHitEffects", config.enableHitEffects },
        { "nonHoldHitEffectDuration", config.nonHoldHitEffectDuration },
        { "debugDrawHitboxes", config.debugDrawHitboxes },
        { "interactionHitboxScaleX", config.interactionHitboxScaleX },
        { "interactionHitboxScaleY", config.interactionHitboxScaleY }
    };
}

/// @brief 从 JSON 恢复完整视觉配置并执行版本迁移与范围校正。
/// @param j 用户配置中的 visual 对象。
/// @param config 接收规范化后的全部视觉设置。
/// @note 读取结束时 Key 数布局有序唯一，关键比例均处于支持范围。
void from_json(const nlohmann::json& j, VisualConfig& config)
{
    // 先检测新背景频谱组件是否存在，用于决定是否迁移旧 enabled 字段。
    const bool hasBackgroundSpectrumComponent =
        j.contains("canvasComponents") &&
        j.at("canvasComponents").is_object() &&
        j.at("canvasComponents").contains("backgroundSpectrum");
    // 根级布局仍是旧配置及未定制键数的兼容模板。
    config.trackLayout = j.value("trackLayout", TrackLayout());
    config.canvasComponents =
        j.value("canvasComponents", CanvasComponentLayoutConfig());
    // Key 数集合读取后先移除无效键，再建立升序唯一不变量。
    config.keyCountLayouts =
        j.value("keyCountLayouts", std::vector<KeyCountLayoutConfig>{});
    std::erase_if(config.keyCountLayouts,
                  [](const auto& layout) { return layout.keyCount <= 0; });
    // 稳定排序让重复键数按文件中首次出现的配置获胜。
    std::stable_sort(config.keyCountLayouts.begin(),
                     config.keyCountLayouts.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.keyCount < rhs.keyCount;
                     });
    // 去重后运行时查询可以安全使用 lower_bound。
    config.keyCountLayouts.erase(
        std::unique(config.keyCountLayouts.begin(),
                    config.keyCountLayouts.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return lhs.keyCount == rhs.keyCount;
                    }),
        config.keyCountLayouts.end());
    // 背景先恢复旧频谱字段，再按新组件存在性决定迁移方向。
    config.background = j.value("background", BackgroundConfig());
    if ( !hasBackgroundSpectrumComponent ) {
        // 旧 enabled 迁移为组件显隐，旧基线和高度推导组件中心锚点。
        config.canvasComponents.backgroundSpectrum.visible =
            config.background.spectrum.enabled;
        config.canvasComponents.backgroundSpectrum.anchorY =
            std::clamp(config.background.spectrum.baselineRatio -
                           config.background.spectrum.heightRatio * 0.5f,
                       0.0f,
                       1.0f);
    }
    // 迁移完成后统一以组件 visible 为权威值，防止两份状态分叉。
    config.background.spectrum.enabled =
        config.canvasComponents.backgroundSpectrum.visible;
    // 预览、轨道线和判定线采用当前构造默认，保持空配置可用。
    config.previewConfig     = j.value("previewConfig", PreviewAreaConfig());
    config.trackBoxLineWidth = j.value("trackBoxLineWidth", 1.5f);
    config.judgeline_pos     = j.value("judgeline_pos", 0.85f);
    // 音符缩放和标签默认从类型实例读取，避免复制头文件默认常量。
    config.noteScaleX = j.value("noteScaleX", VisualConfig{}.noteScaleX);
    config.noteScaleY = j.value("noteScaleY", VisualConfig{}.noteScaleY);
    config.showBoundSampleLabels =
        j.value("showBoundSampleLabels", VisualConfig{}.showBoundSampleLabels);
    config.noteFillMode = j.value("noteFillMode", BackgroundFillMode::Stretch);
    // 三类时间偏移独立恢复，缺失值均表示无校准偏移。
    config.visualOffset            = j.value("visualOffset", 0.0f);
    config.waveformVisualOffset    = j.value("waveformVisualOffset", 0.0f);
    config.spectrumVisualOffset    = j.value("spectrumVisualOffset", 0.0f);
    config.timelineZoom            = j.value("timelineZoom", 1.0f);
    config.scrollAnimationDuration = j.value("scrollAnimationDuration", 0.12f);
    config.enableLinearScrollMapping =
        j.value("enableLinearScrollMapping", false);
    // 悬浮分拍线延伸比例直接限制到归一化零至一范围。
    config.snapThreshold                      = j.value("snapThreshold", 16.0f);
    config.beatLineAlpha                      = j.value("beatLineAlpha", 0.75f);
    config.hoverSubdivisionLineExtensionRatio = std::clamp(
        j.value("hoverSubdivisionLineExtensionRatio", 0.5f), 0.0f, 1.0f);
    // 新三态字段优先；仅在缺失时把旧 drawBeatLines 布尔迁移为两端状态。
    if ( j.contains("beatLineDisplayMode") ) {
        config.beatLineDisplayMode =
            j.value("beatLineDisplayMode", BeatLineDisplayMode::Always);
    } else {
        config.beatLineDisplayMode = j.value("drawBeatLines", true)
                                         ? BeatLineDisplayMode::Always
                                         : BeatLineDisplayMode::Hidden;
    }
    // 可见核心和淡出区分别限制到设置界面支持范围。
    config.beatLineCursorVisibleRatio =
        std::clamp(j.value("beatLineCursorVisibleRatio", 0.16f), 0.05f, 0.50f);
    config.beatLineCursorFadeRatio =
        std::clamp(j.value("beatLineCursorFadeRatio", 0.20f), 0.02f, 0.40f);
    // 调色板覆盖属于运行期组合结果，不从视觉配置文件恢复。
    config.overrideBeatLineColors = false;
    config.beatLineColors         = {};
    config.drawBeatLinesBeforeFirstTiming =
        j.value("drawBeatLinesBeforeFirstTiming", true);
    // 频谱细节缺失时选择质量与负载折中的 Balanced。
    config.spectrumDetailLevel =
        j.value("spectrumDetailLevel", SpectrumDetailLevel::Balanced);
    config.enableHitEffects = j.value("enableHitEffects", true);
    config.nonHoldHitEffectDuration =
        j.value("nonHoldHitEffectDuration",
                VisualConfig::DEFAULT_NON_HOLD_HIT_EFFECT_DURATION);
    // 非 Hold 特效时长先处理非有限值，再执行产品范围夹取。
    if ( !std::isfinite(config.nonHoldHitEffectDuration) ) {
        config.nonHoldHitEffectDuration =
            VisualConfig::DEFAULT_NON_HOLD_HIT_EFFECT_DURATION;
    }
    config.nonHoldHitEffectDuration =
        std::clamp(config.nonHoldHitEffectDuration,
                   VisualConfig::MIN_NON_HOLD_HIT_EFFECT_DURATION,
                   VisualConfig::MAX_NON_HOLD_HIT_EFFECT_DURATION);
    // 调试绘制默认关闭，横纵拾取缩放允许独立校准。
    config.debugDrawHitboxes = j.value("debugDrawHitboxes", false);
    config.interactionHitboxScaleX =
        j.value("interactionHitboxScaleX",
                VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE);
    config.interactionHitboxScaleY =
        j.value("interactionHitboxScaleY",
                VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE);
    // 非有限缩放回退标准值，避免比较和几何计算传播 NaN。
    if ( !std::isfinite(config.interactionHitboxScaleX) ) {
        config.interactionHitboxScaleX =
            VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE;
    }
    if ( !std::isfinite(config.interactionHitboxScaleY) ) {
        config.interactionHitboxScaleY =
            VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE;
    }
    // 有限缩放最终夹取到调试界面同一范围，保证拾取盒不会退化或过大。
    config.interactionHitboxScaleX =
        std::clamp(config.interactionHitboxScaleX,
                   VisualConfig::MIN_INTERACTION_HITBOX_SCALE,
                   VisualConfig::MAX_INTERACTION_HITBOX_SCALE);
    config.interactionHitboxScaleY =
        std::clamp(config.interactionHitboxScaleY,
                   VisualConfig::MIN_INTERACTION_HITBOX_SCALE,
                   VisualConfig::MAX_INTERACTION_HITBOX_SCALE);
}

}  // namespace MMM::Config
