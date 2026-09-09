#include "logic/ecs/system/SampleRenderSystem.h"

#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/AudioObjectLabelRenderer.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/context/SessionContext.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace MMM::Logic::System
{

// 本文件负责独立音频轨及其采样标记，不负责音频解码、混音或播放调度。
// 布局、基础物件和交互发光共用同一投影，避免独立轨宽变化后反馈层错位。
namespace
{

/// @brief 草稿与 BGM 轨道标题使用的画布字体像素高度。
constexpr float LANE_LABEL_FONT_PIXEL_HEIGHT = 16.0F;

/// @brief 批注栏底色键，避免主画布热路径构造临时字符串。
const std::string ANNOTATION_GUTTER_BACKGROUND_COLOR_KEY =
    "annotations.gutter_background";
/// @brief 批注栏边框色键，避免主画布热路径构造临时字符串。
const std::string ANNOTATION_GUTTER_BORDER_COLOR_KEY =
    "annotations.gutter_border";
/// @brief 草稿轨道标题色键，避免主画布热路径构造临时字符串。
const std::string DRAFT_TRACK_LABEL_COLOR_KEY = "draft_tracks.label";
/// @brief BGM 轨道底色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_BACKGROUND_COLOR_KEY = "bgm_tracks.background";
/// @brief BGM 轨道交替底色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_ALTERNATE_COLOR_KEY = "bgm_tracks.alternate";
/// @brief BGM 轨道边框色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_BORDER_COLOR_KEY = "bgm_tracks.border";
/// @brief BGM 区分隔线色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_SEPARATOR_COLOR_KEY = "bgm_tracks.separator";
/// @brief BGM 轨道标题色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_LABEL_COLOR_KEY = "bgm_tracks.label";
/// @brief 自动采样主体色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_SAMPLE_COLOR_KEY = "bgm_tracks.sample";
/// @brief 自动采样偏移色键，避免主画布热路径构造临时字符串。
const std::string BGM_TRACK_OFFSET_COLOR_KEY = "bgm_tracks.offset";

/// @brief 判断皮肤颜色是否为缺省的洋红哨兵。
/// @param color 皮肤颜色。
/// @return 颜色为缺省哨兵时返回 true。
/// @note 使用完整 RGBA 精确比较，透明洋红不作为缺省色处理。
/// @note 哨兵与显式配置成同一 RGBA 的颜色无法区分，回退遵循皮肤查询约定。
/// @warning 轨道渲染热路径仅比较数值，不查询皮肤资源。
bool isMissingSkinColor(const Config::Color& color)
{
    return color.r == 1.0F && color.g == 0.0F && color.b == 1.0F &&
           color.a == 1.0F;
}

/// @brief 获取可回退的轨道布局皮肤颜色。
/// @param key 皮肤颜色键。
/// @param fallback 缺失时使用的颜色。
/// @return 转换后的 GLM 颜色。
/// @note 只在缺省哨兵时回退，合法的全透明配置仍应保持不可见。
/// @warning 主画布热路径：只接收静态键并执行内存内皮肤颜色查询。
glm::vec4 laneColor(const std::string& key, glm::vec4 fallback)
{
    const auto color = Config::SkinManager::instance().getColor(key);
    if ( isMissingSkinColor(color) ) return fallback;
    return { color.r, color.g, color.b, color.a };
}

/// @brief 获取自动采样锚点与实际触发点覆盖的时间区间。
/// @param sample 自动采样组件。
/// @return `[min,max]` 时间区间，单位秒。
/// @note 区间表示锚点与偏移触发点之间的连接，不是音频资源的播放持续时间。
/// @pre 组件锚点和偏移换算出的时间有限，排序索引使用同一换算规则。
/// @warning 可见性热路径纯数值查询，不探测音频文件时长。
std::pair<double, double> sampleTimeRange(const SampleComponent& sample)
{
    // 返回值复制两个端点，不让调用方借用 effectiveTime() 临时结果的引用。
    // 偏移允许为负，必须先排序两个端点，不能假定触发点总在锚点之后。
    return std::minmax(sample.m_timestamp, sample.effectiveTime());
}

/// @brief 从排序索引收集当前可见时间范围内的自动采样候选。
/// @param registry 自动采样注册表。
/// @param sortedEntities 已按区间起点排序的实体。
/// @param maxEndPrefix 区间终点前缀最大值。
/// @param visibleRanges 当前滚动窗口映射出的可见时间范围。
/// @param result 输出实体列表。
/// @param seen 输出去重集合。
/// @pre 排序表与前缀表来自同一代样本数据，排序键为 sampleTimeRange 的起点。
/// @pre 查询期间注册表和索引保持稳定；失效实体检查不能修复已经失序的索引。
/// @note 清空并复用输出容器；没有一致的索引时返回空结果，不退回全表扫描。
/// @note 返回的是保守时间候选，横向轨道和最终像素范围仍需由绘制阶段过滤。
/// @note 索引有效性由维护端保证；这里对实体句柄的检查只避免访问失效组件。
/// @warning 主画布热路径：只遍历索引命中的候选区间。
void collectVisibleSamples(
    entt::registry& registry, const std::vector<entt::entity>& sortedEntities,
    const std::vector<double>&                 maxEndPrefix,
    std::span<const std::pair<double, double>> visibleRanges,
    std::vector<entt::entity>& result, std::unordered_set<entt::entity>& seen)
{
    result.clear();
    // 返回列表存值类型句柄，后续绘制必须重新从同一 Registry 取得组件。
    // 保留容量供下一帧复用；查询结果仅借用实体标识，不持有组件引用。
    // 多段窗口共享一次查询的去重集合，不把同一采样绘制多份。
    seen.clear();
    if ( sortedEntities.empty() ||
         maxEndPrefix.size() != sortedEntities.size() ) {
        return;
    }

    // 可见时间可能不是单段：反向 SV 使相隔很远的时间同时投影到窗口内。
    // 每段独立二分后按实体身份合并，不能只查询最早到最晚的一个大包围区间。
    for ( const auto& rawRange : visibleRanges ) {
        // 不把窗口起点夹到零，负时间锚点与跨零偏移连接也可能出现在视口中。
        // 滚动反转可能给出反序端点，扩展四分之一秒为边缘图元保留候选余量。
        const double rangeStart =
            std::min(rawRange.first, rawRange.second) - 0.25;
        const double rangeEnd =
            std::max(rawRange.first, rawRange.second) + 0.25;
        // 起点晚于窗口右端的样本不可能相交，先二分确定候选上界。
        const auto high = std::upper_bound(
            sortedEntities.begin(),
            sortedEntities.end(),
            rangeEnd,
            [&registry](double value, entt::entity entity) {
                // 比较器的参数顺序是“窗口右端、实体起点”，返回右端是否早于起点。
                // 相等起点必须留在候选内，故不能改为非严格比较。
                if ( !registry.valid(entity) ||
                     !registry.all_of<SampleComponent>(entity) ) {
                    return false;
                }
                const auto range = sampleTimeRange(
                    registry.get<const SampleComponent>(entity));
                return value < range.first;
            });
        const auto highIndex =
            static_cast<std::size_t>(high - sortedEntities.begin());
        // highIndex 可以为零或等于总数，两种边界均可形成合法的半开前缀区间。
        // 不减一构造“最后候选”，避免空查询时产生无符号下溢。
        // 前缀最大值单调不减，二分只需检查起点已满足条件的这一段。
        // 使用 lower_bound 保留终点恰好落在窗口起点上的接触区间。
        // 结束时间前缀允许保留很早开始但仍跨过视窗的偏移连接。
        // 单靠样本起点 lower_bound 会遗漏这种跨窗口对象。
        const auto low = std::lower_bound(
            maxEndPrefix.begin(), maxEndPrefix.begin() + highIndex, rangeStart);
        const auto lowIndex =
            static_cast<std::size_t>(low - maxEndPrefix.begin());

        // 前缀范围可能因早期超长连接而包含不少不相交样本，
        // 下方的真实区间判断不可因已经做过两次二分就省略。
        for ( std::size_t index = lowIndex; index < highIndex; ++index ) {
            // 前缀只给出保守范围，逐候选仍需检查实体有效性和真实区间交集。
            const auto entity = sortedEntities[index];
            if ( !registry.valid(entity) ||
                 !registry.all_of<SampleComponent>(entity) ) {
                continue;
            }
            const auto range =
                sampleTimeRange(registry.get<const SampleComponent>(entity));
            if ( range.second < rangeStart || range.first > rangeEnd ) continue;
            // 零偏移是退化成单点的合法区间，端点相等也应保留为可见候选。
            // 反向 SV 映射的多个可见时间段可能重叠，实体身份用于跨段去重。
            if ( seen.insert(entity).second ) {
                // 保持首次命中的遍历顺序，不为去重结果额外排序。
                result.push_back(entity);
            }
        }
    }
}

}  // namespace

/// @brief 绘制批注侧栏以及可见草稿和 BGM 轨道的布局元素。
/// @param batcher 提供画布快照并接收底板、边界和标题命令。
/// @param projection 已计算的独立区域投影，包含临时追加轨。
/// @param persistentDraftTrackCount 持久化草稿轨数，用于区分追加入口。
/// @param persistentBgmTrackCount 持久化 BGM 轨数，不含运行时追加入口。
/// @param viewportWidth 横向可见画布宽度。
/// @param topY 轨道绘制区域上边界。
/// @param bottomY 轨道绘制区域下边界，必须高于 topY 才有可绘区域。
/// @note 本函数会改变批处理裁剪状态，后续绘制须设置自己的裁剪区域。
/// @pre batcher 关联有效快照，投影坐标与传入视口使用相同像素单位。
/// @note 标题时间来自同一份快照，不能在每条轨道中分别读取系统时间。
/// @note 轨道编号是投影区域内的编号，不用玩家轨数给显示标题增加全局偏移。
/// @note 本入口绘制标题与侧栏，不负责玩家音符背景或批注文字内容。
/// @note 布局底板不生成拾取区域，轨道追加入口的交互由画布层单独处理。
/// @warning 主画布快照热路径按可见轨道绘制，不枚举音符或加载字体资源。
void SampleRenderSystem::renderLaneLayout(
    Batcher& batcher, const CanvasLaneProjection& projection,
    std::int32_t persistentDraftTrackCount,
    std::int32_t persistentBgmTrackCount, float viewportWidth, float topY,
    float bottomY)
{
    if ( !projection.valid || bottomY <= topY ) {
        // 无有效区域时不改变批处理状态，避免产生负高度裁剪矩形。
        return;
    }
    // 横向只处理与视口相交的侧栏，允许独立区域随画布平移部分离屏。
    const float gutterLeft = std::max(0.0F, projection.annotationLeftX);
    const float gutterRight =
        std::min(viewportWidth, projection.annotationRightX);
    if ( gutterRight > gutterLeft ) {
        // 背景与边框分别回退，皮肤可以只覆盖其中一种颜色。
        const auto gutterBackground =
            laneColor(ANNOTATION_GUTTER_BACKGROUND_COLOR_KEY,
                      { 0.025F, 0.035F, 0.05F, 0.94F });
        const auto gutterBorder = laneColor(ANNOTATION_GUTTER_BORDER_COLOR_KEY,
                                            { 0.28F, 0.78F, 0.94F, 0.75F });
        batcher.setTexture(TextureID::None);
        // 底板是纯色几何，不能沿用之前文字或音符图集的纹理状态。
        batcher.setScissor(
            gutterLeft, topY, gutterRight - gutterLeft, bottomY - topY);
        batcher.pushQuad(gutterLeft,
                         bottomY,
                         gutterRight - gutterLeft,
                         bottomY - topY,
                         gutterBackground);
        // pushQuad 的纵坐标以底边为基准，绘制 topY 到 bottomY 时传 bottomY。
        // 裁剪矩形则使用左上角，两种入口的 Y 语义不能混用。
        // 边框仍使用实际布局边界，不能把被裁切的视窗边缘画成侧栏边框。
        batcher.pushQuad(projection.annotationLeftX,
                         bottomY,
                         1.5F,
                         bottomY - topY,
                         gutterBorder);
        batcher.pushQuad(projection.annotationRightX - 1.5F,
                         bottomY,
                         1.5F,
                         bottomY - topY,
                         gutterBorder);
        // 两条边线都保留完整布局坐标，由 scissor 剪掉离屏部分，而不是移动边线。
    }

    // 投影直接给出可见轨索引区间，避免遍历全部持久化草稿轨。
    const auto visibleDraftRange =
        projection.visibleDraftRange(0.0F, viewportWidth);
    if ( visibleDraftRange ) {
        const auto  draftLabel  = laneColor(DRAFT_TRACK_LABEL_COLOR_KEY,
                                            { 0.96F, 0.69F, 0.52F, 0.92F });
        const float visibleLeft = std::max(0.0F, projection.draftLeftX);
        const float visibleRight =
            std::min(viewportWidth, projection.draftRightX);
        // 侧栏区域独立裁剪，草稿标题不可延伸到玩家轨或批注栏。
        batcher.setScissor(
            visibleLeft, topY, visibleRight - visibleLeft, bottomY - topY);

        // 运行时轨数超过持久化轨数时，左侧额外轨作为追加入口显示。
        // 持久化计数先钳到非负，避免转成无符号后误判为极大轨数。
        const bool hasAppendLane =
            projection.draftLaneCount >
            static_cast<std::uint32_t>(std::max(0, persistentDraftTrackCount));
        const auto [begin, end] = *visibleDraftRange;
        // 轨道地址由区域种类和局部索引组成，不能把 Draft 索引当作 BGM 索引。
        // 可见索引采用半开区间；投影返回的末尾不是可绘制轨道本身。
        for ( std::uint32_t index = begin; index < end; ++index ) {
            const auto bounds =
                projection.bounds({ CanvasLaneKind::Draft, index });
            if ( !bounds ) continue;

            // 追加轨的文字固定，不为它分配真实轨号，避免给用户造成已持久化的含义。
            std::array<char, 32> labelBuffer{};
            // 标题保存在栈缓冲内，格式化结果只在这次文本提交期间借用。
            std::string_view labelText{ "DRAFT +" };
            if ( !hasAppendLane || index != 0U ) {
                // 追加入口占据索引零，真实草稿轨编号因而不能再无条件加一。
                const auto number = hasAppendLane ? index : index + 1U;
                const auto result = fmt::format_to_n(labelBuffer.data(),
                                                     labelBuffer.size() - 1,
                                                     "DRAFT {}",
                                                     number);
                *result.out       = '\0';
                // 为结尾零留一字节，同时用实际写入范围构造视图，不依赖 strlen。
                labelText = std::string_view(
                    labelBuffer.data(),
                    static_cast<std::size_t>(result.out - labelBuffer.data()));
            }
            // labelText 要在栈缓冲被下一轮覆盖前提交给文字几何生成器；
            // 快照中保留的是生成结果，不得改成跨帧保存这个 string_view。
            // 标题宽度扣除左右内边距，超长内容交给现有跑马灯文本入口处理。
            renderMarqueeCanvasAsciiText(batcher,
                                         labelText,
                                         bounds->leftX + 4.0F,
                                         topY + 4.0F,
                                         LANE_LABEL_FONT_PIXEL_HEIGHT,
                                         projection.draftLaneWidth - 8.0F,
                                         draftLabel,
                                         batcher.snapshot->snapshotSysTime);
        }
    }

    // 草稿与批注侧栏已独立处理，BGM 为空不应提前跳过前面的区域。
    if ( projection.bgmLaneCount == 0 ) return;
    // 返回前可能已写入草稿标题；这里不清空已有命令，也不重置调用方的批处理器。
    const auto visibleRange = projection.visibleBgmRange(0.0F, viewportWidth);
    if ( !visibleRange ) return;

    // BGM 完全不可见时连皮肤色也不查询；颜色不随轨号变化的部分只取一次。
    const auto background = laneColor(BGM_TRACK_BACKGROUND_COLOR_KEY,
                                      { 0.035F, 0.055F, 0.075F, 0.92F });
    const auto alternate  = laneColor(BGM_TRACK_ALTERNATE_COLOR_KEY,
                                      { 0.055F, 0.08F, 0.105F, 0.92F });
    const auto border =
        laneColor(BGM_TRACK_BORDER_COLOR_KEY, { 0.32F, 0.48F, 0.62F, 0.55F });
    const auto separator = laneColor(BGM_TRACK_SEPARATOR_COLOR_KEY,
                                     { 0.28F, 0.78F, 0.94F, 0.95F });
    const auto label =
        laneColor(BGM_TRACK_LABEL_COLOR_KEY, { 0.72F, 0.88F, 0.96F, 0.92F });

    const float visibleLeft  = std::max(0.0F, projection.bgmLeftX);
    const float visibleRight = std::min(viewportWidth, projection.bgmRightX);
    // 整个 BGM 区共用一份裁剪框，单轨底色可按完整轨宽提交再由它截断。
    // 不把左右半截轨道重新均分，否则横向滚动时标题和本体会变宽或变窄。
    batcher.setScissor(
        visibleLeft, topY, visibleRight - visibleLeft, bottomY - topY);

    const auto [begin, end] = *visibleRange;
    for ( std::uint32_t index = begin; index < end; ++index ) {
        const auto bounds = projection.bounds({ CanvasLaneKind::Bgm, index });
        if ( !bounds ) continue;
        // 每轮标题可能切换到字体纹理，画下一轨底板前必须恢复无纹理状态。
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(bounds->leftX,
                         bottomY,
                         projection.bgmLaneWidth,
                         bottomY - topY,
                         index % 2 == 0 ? background : alternate);
        // 交替底色按绝对轨索引选取，横向滚动不能让同一轨道改变颜色。
        batcher.pushQuad(bounds->leftX, bottomY, 1.5F, bottomY - topY, border);
        // 单轨左边线与整个 BGM 区起点分隔线职责不同，后者稍后以强调色单独绘制。

        std::array<char, 32> labelBuffer{};
        const bool appendLane = index == static_cast<std::uint32_t>(std::max(
                                             0, persistentBgmTrackCount));
        // 这里只把现有投影中的指定索引标成入口，不负责实际增加持久化轨数。
        // 点击或拖入追加轨后的数据变更属于交互逻辑，不在绘制中完成。
        // BGM 追加入口在已持久化轨道之后，与草稿区的左侧追加入口位置不同。
        std::string_view labelText{ "BGM +" };
        if ( appendLane ) {
            labelText = "BGM +";
        } else {
            const auto result = fmt::format_to_n(labelBuffer.data(),
                                                 labelBuffer.size() - 1,
                                                 "BGM {}",
                                                 index + 1);
            *result.out       = '\0';
            labelText         = std::string_view(
                labelBuffer.data(),
                static_cast<std::size_t>(result.out - labelBuffer.data()));
        }
        renderMarqueeCanvasAsciiText(batcher,
                                     labelText,
                                     bounds->leftX + 4.0F,
                                     topY + 4.0F,
                                     LANE_LABEL_FONT_PIXEL_HEIGHT,
                                     projection.bgmLaneWidth - 8.0F,
                                     label,
                                     batcher.snapshot->snapshotSysTime);
    }

    if ( projection.bgmLeftX >= 0.0F && projection.bgmLeftX <= viewportWidth ) {
        // 分隔线只标识 BGM 区真实起点，不在横向裁剪后的视口边缘补画。
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(projection.bgmLeftX - 1.5F,
                         bottomY,
                         3.0F,
                         bottomY - topY,
                         separator);
        // 仍受当前 BGM 区 scissor 约束，不为强调线临时扩张裁剪到邻接玩家区域。
    }
}

/// @brief 根据可见时间区间绘制采样锚点、偏移标记、交互反馈与画笔预览。
/// @param registry 当前采样实体及交互状态所在的注册表。
/// @param sortedEntities 按采样时间范围起点排序的实体索引。
/// @param maxEndPrefix 与排序索引同代的范围终点前缀最大值。
/// @param snapshot 本帧查询暂存、命中框与发光命令的写入目标。
/// @param batcher 基础层批处理器；发光层使用单独的命令容器。
/// @param projection 玩家、草稿与 BGM 轨道的横向投影。
/// @param cache 提供视觉锚点及 SV 时间区间反查的滚动缓存。
/// @param config 控制采样尺寸、纹理填充及标签缩放的编辑器配置。
/// @param currentTime 当前视觉时间，单位为秒。
/// @param judgmentLineY 判定线在画布中的纵坐标。
/// @param viewportWidth 横向裁剪范围的右边界，左边界为零。
/// @param topY 可绘制区域的上边界。
/// @param bottomY 可绘制区域的下边界。
/// @param renderScaleY 滚动距离到像素的有符号比例，不允许接近零。
/// @note 命中框在已有列表后追加，调用方负责本帧列表的初始化。
/// @pre topY 小于 bottomY，视口尺寸和几何缩放已由上层校验。
/// @pre batcher 与 snapshot 对应同一帧；采样注册表在绘制期间不被并发修改。
/// @note 排序索引仅用于收集候选，拖动固定实体仍要参与后续空间裁剪。
/// @note 本函数只追加快照图元和命中区域，不修改采样时间、轨号或音量。
/// @note 返回后基础批处理器仍可能保留待提交几何，由调用方完成最终 flush。
/// @pre 配置本体缩放有限且为有效几何尺寸，标签字号回退不负责修复本体配置。
/// @warning
/// 每帧热路径：复用快照暂存与时间索引，禁止新增文件访问、完整实体扫描或等待。
void SampleRenderSystem::renderSamples(
    entt::registry& registry, const std::vector<entt::entity>& sortedEntities,
    const std::vector<double>& maxEndPrefix, RenderSnapshot* snapshot,
    Batcher& batcher, const CanvasLaneProjection& projection,
    const ScrollCache* cache, const Config::EditorConfig& config,
    double currentTime, float judgmentLineY, float viewportWidth, float topY,
    float bottomY, float renderScaleY)
{
    if ( !snapshot || !cache || !projection.valid ||
         std::abs(renderScaleY) < 1e-6F ) {
        // 缺少投影依据时不生成命中框；近零比例也不能用于反解可见时间。
        // 这里只退出当前层，不清除调用方在同一快照中已生成的其他物件命中框。
        return;
    }
    const auto visibleLaneRange =
        projection.visibleBgmRange(0.0F, viewportWidth);

    // 像素窗口先反解到滚动绝对坐标，再由缓存求所有可见时间段；
    // 有符号缩放允许视觉方向翻转，不能直接把 topY 当作最早时间。
    // 使用同一视觉锚点反查窗口与投影物件，避免两套滚动基准造成边缘漏绘。
    // 反向滚动可能交换上下界；反查结果还可能包含多个不连续时间段。
    const double currentAbsY = cache->getVisualAnchorAbsY(currentTime);
    // 有符号比例只改变纵向投影，轨道地址和水平宽度始终由 projection 决定。
    const double topAbsY = currentAbsY + (judgmentLineY - topY) /
                                             static_cast<double>(renderScaleY);
    const double bottomAbsY =
        currentAbsY +
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    const auto visibleTimeRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));
    // 时间余量在候选查询内添加，最终视觉裁剪仍按像素窗口处理，不扩展快照视口。
    snapshot->sampleQueryScratch.clear();
    snapshot->sampleQuerySeenScratch.clear();
    // 即使 BGM 不可见也清掉上次结果，随后只加入本帧仍需保留的拖动物件。
    // 不能因为跳过普通查询而继续复用上一视口的候选集合。
    // BGM 区离屏时跳过普通索引查询，但不能跳过随后跨区拖动的固定实体。
    if ( visibleLaneRange ) {
        collectVisibleSamples(registry,
                              sortedEntities,
                              maxEndPrefix,
                              visibleTimeRanges,
                              snapshot->sampleQueryScratch,
                              snapshot->sampleQuerySeenScratch);
    }
    // 拖动实体可能尚未反映在排序索引中，额外加入候选后仍接受轨道和像素裁剪。
    // 与普通候选共享去重集合，防止同一实体重复生成命中框。
    if ( const auto* pinned = registry.ctx().find<DragRenderPinnedEntities>();
         pinned && pinned->entities ) {
        // pinned 只提供本帧额外查询入口，不授予实体新的所有权或有效性。
        // 仍检查当前 Registry，避免已结束拖动或已删除对象留下过期句柄。
        for ( const auto entity : *pinned->entities ) {
            if ( !registry.valid(entity) ||
                 !registry.all_of<SampleComponent>(entity) ||
                 !snapshot->sampleQuerySeenScratch.insert(entity).second ) {
                continue;
            }
            snapshot->sampleQueryScratch.push_back(entity);
        }
    }
    const auto sampleColor =
        laneColor(BGM_TRACK_SAMPLE_COLOR_KEY, { 0.36F, 0.72F, 0.92F, 0.96F });
    const auto offsetColor =
        laneColor(BGM_TRACK_OFFSET_COLOR_KEY, { 0.96F, 0.56F, 0.28F, 0.92F });
    const auto textColor = audioObjectLabelColor();
    const auto noteTextureIt =
        snapshot->uvMap.find(static_cast<std::uint32_t>(TextureID::Note));
    // 只借用当前快照已经发布的 UV，不在绘制过程中请求加载缺失纹理。
    // 缺少或退化的 UV 不参与比例计算；圆角纯色本体仍能提供采样的可见反馈。
    const bool hasNoteTexture = noteTextureIt != snapshot->uvMap.end() &&
                                std::isfinite(noteTextureIt->second.z) &&
                                std::isfinite(noteTextureIt->second.w) &&
                                noteTextureIt->second.z > 1e-6F &&
                                noteTextureIt->second.w > 1e-6F;
    // UV 范围宽高用于计算轮廓比例，零高会导致本体尺寸除零，不能仅检查键存在。
    const float noteTextureAspect =
        hasNoteTexture ? noteTextureIt->second.z / noteTextureIt->second.w
                       : 1.0F;
    // 无纹理时用正方形比例计算本体高度，与圆角回退形状保持一致。
    // 不取上一个采样或上一份快照的纹理比例，避免皮肤切换后的尺寸残留。
    // 这里的回退仅用于偏移标签字号，不改变配置或本体几何的缩放语义。
    const float sampleTextScale = std::isfinite(config.visual.noteScaleY) &&
                                          config.visual.noteScaleY > 0.0F
                                      ? config.visual.noteScaleY
                                      : 1.0F;
    // 本体宽高与文字字号并非同一参数链，水平缩放不能改变偏移标签的字号。
    /// @brief 使用与基础层一致的几何绘制采样本体，供基础层和发光层复用。
    /// @param targetBatcher 目标批处理器。
    /// @param bodyX 采样本体左边界。
    /// @param anchorY 采样本体纵向中心。
    /// @param bodyWidth 采样本体宽度。
    /// @param bodyHeight 采样本体高度。
    /// @param color 采样本体颜色。
    /// @note 输入 anchorY 是中心，批处理四边形入口则使用底边纵坐标。
    /// @pre 调用前已设置目标层裁剪，本函数只切换纹理并提交形状。
    /// @note 不提交文字、连线或命中框，发光层因而不会复制这些基础层附件。
    /// @warning 主画布热路径：每个可见或发光采样调用，禁止引入资源查询与分配。
    const auto renderSampleBody = [&](Batcher&  targetBatcher,
                                      float     bodyX,
                                      float     anchorY,
                                      float     bodyWidth,
                                      float     bodyHeight,
                                      glm::vec4 color) {
        // 所有形状入口都接收调用方颜色，橡皮擦预览无需修改皮肤缓存。
        // 纹理轮廓与无纹理回退仅在此选择，基础层和发光层不能各自猜测尺寸。
        if ( hasNoteTexture ) {
            // 填充方式沿用音符配置；基础层与发光层必须使用相同纹理轮廓。
            targetBatcher.setTexture(TextureID::Note);
            targetBatcher.pushFilledQuad(bodyX,
                                         anchorY + bodyHeight * 0.5F,
                                         bodyWidth,
                                         bodyHeight,
                                         { noteTextureAspect, 1.0F },
                                         config.visual.noteFillMode,
                                         color);
        } else {
            // 无纹理时保留固定像素圆角，本体仍可用于选择和拖动反馈。
            targetBatcher.setTexture(TextureID::None);
            targetBatcher.pushRoundedQuad(bodyX,
                                          anchorY + bodyHeight * 0.5F,
                                          bodyWidth,
                                          bodyHeight,
                                          4.0F,
                                          color);
        }
    };

    batcher.setScissor(0.0F, topY, viewportWidth, bottomY - topY);
    // 命中框另行记录完整几何；这里的裁剪只约束最终可见图元。
    bool hasSampleGlow = false;

    // 查询集合可以同时含普通命中和拖动固定实体；后面统一按当前组件投影。
    // 不从旧排序键恢复坐标，否则未提交的拖动会仍显示在原位置。
    for ( const auto entity : snapshot->sampleQueryScratch ) {
        if ( !registry.valid(entity) ||
             !registry.all_of<SampleComponent>(entity) ) {
            continue;
        }
        const auto& sample  = registry.get<const SampleComponent>(entity);
        const auto  address = CanvasLaneAddress::fromAbsoluteTrack(
            sample.m_track, projection.playerLaneCount);
        // 模型存绝对轨号，投影接收区域内索引；两种编号不可直接混用。
        const auto* interaction =
            registry.try_get<const InteractionComponent>(entity);
        // 非交互采样仍可正常显示；缺少交互组件只表示没有悬浮、选中和拖动状态。
        const bool isDragging = interaction && interaction->isDragging;
        // 玩家轨道中的常驻采样不在此层展示；跨区拖动期间保留移动中的本体。
        if ( address.kind == CanvasLaneKind::Player && !isDragging ) {
            continue;
        }
        if ( address.kind == CanvasLaneKind::Bgm &&
             (!visibleLaneRange || address.index < visibleLaneRange->first ||
              address.index >= visibleLaneRange->second) ) {
            continue;
        }
        // 跨区拖动使用目标轨道的真实边界，不将样本强制夹回原 BGM 区。
        // 拖动目标没有有效投影时不伪造一个与命中区域不一致的可见本体。
        const auto bounds = projection.bounds(address);
        if ( !bounds || bounds->rightX <= 0.0F ||
             bounds->leftX >= viewportWidth ) {
            continue;
        }

        // 横向边界接触但没有正宽交集时跳过；纵向则给标签和手柄保留额外候选余量。
        // 锚点和实际播放时间分别投影，偏移可能跨越 SV
        // 变化，不能按固定像素平移。
        const float anchorY =
            judgmentLineY -
            static_cast<float>(cache->getDisplayDelta(
                sample.m_timestamp, currentAbsY, sample.m_timestamp)) *
                renderScaleY;
        const double effectiveTime = sample.effectiveTime();
        const float  effectiveY =
            judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                effectiveTime, currentAbsY, effectiveTime)) *
                                renderScaleY;

        // m_timestamp 是可移动的锚点，effectiveTime 已包含资源偏移。
        // 显示触发点不回写锚点时间，否则反复绘制会把偏移累加到模型中。
        // 跨区拖动时直接使用当前地址边界，视觉和命中宽度保持一致。
        const float laneWidth = bounds->rightX - bounds->leftX;
        const float bodyWidth = laneWidth * config.visual.noteScaleX;
        const float bodyHeight =
            (laneWidth / noteTextureAspect) * config.visual.noteScaleY;
        // 横向缩放只影响宽度，高度仍以原始轨宽和纹理宽高比计算。
        // 因而调窄本体不会间接改变纵向高度或偏移手柄的触发时间。
        // 连线跨入窗口时，即使两端本体不在窗口内，也不能仅按锚点剔除。
        const float verticalPadding =
            std::max(32.0F, bodyHeight * 0.5F + 24.0F);
        // 余量包含本体半高及附属标记，不能只用音符中心判断是否已经离开窗口。
        if ( std::max(anchorY, effectiveY) < topY - verticalPadding ||
             std::min(anchorY, effectiveY) > bottomY + verticalPadding ) {
            continue;
        }

        const float bodyX   = bounds->leftX + (laneWidth - bodyWidth) * 0.5F;
        const float centerX = bounds->leftX + laneWidth * 0.5F;
        // 本体以轨道中心对齐，noteScaleX 改变两侧留白，不改变锚点所属轨道。
        const float offsetHandleSize =
            std::clamp(laneWidth * 0.12F, 8.0F, 14.0F);
        // 手柄不使用 noteScaleX 作为缩放基准，缩窄音符时仍保持可拾取面积。
        // 手柄保持可操作的像素大小范围，不随宽轨无限增大或窄轨缩成一点。
        const float offsetHandleCenterX =
            bodyX + bodyWidth - offsetHandleSize * 0.5F;
        const float offsetHandleX =
            offsetHandleCenterX - offsetHandleSize * 0.5F;
        // 手柄右边缘与本体右边缘对齐；连线仍放在整条轨道的水平中心。
        // 零偏移默认隐藏手柄，悬浮或选中后仍可从重合位置开始调整偏移。
        const bool showOffsetHandle =
            sample.m_offsetMs != 0 ||
            (interaction &&
             (interaction->isSelected || interaction->isHovered));

        // 非零偏移始终提供手柄；零偏移只借交互状态显露，不额外绘制偏移连线。
        // 不能把 showOffsetHandle 同时当成是否存在真实时间偏移的判断。
        glm::vec4 bodyColor = sampleColor;
        // 实体编号并不跨对象类别唯一，橡皮擦反馈必须同时匹配对象种类。
        const bool isErasing =
            snapshot->erasingObjectKind == ChartObjectKind::AudioSample &&
            snapshot->erasingEntities.contains(entity);
        // 擦除集合来自当前快照的交互意图，不反向写入组件选中状态。
        glm::vec4 sampleOffsetColor = offsetColor;
        glm::vec4 sampleTextColor   = textColor;
        if ( isErasing ) {
            // 橡皮擦预览统一覆盖本体、偏移和文本颜色，但不在渲染阶段删除实体。
            bodyColor         = { 1.0F, 0.2F, 0.2F, 0.5F };
            sampleOffsetColor = { 1.0F, 0.2F, 0.2F, 0.7F };
            sampleTextColor   = { 1.0F, 0.35F, 0.35F, 0.75F };
        }
        hasSampleGlow = hasSampleGlow ||
                        (interaction &&
                         (interaction->isSelected || interaction->isHovered));
        // 只有基础层通过裁剪的交互对象才需要启动后面的独立发光遍历。

        // 偏移连线只提交窗口内的纵向片段；标签保留带符号毫秒值。
        if ( sample.m_offsetMs != 0 ) {
            batcher.setTexture(TextureID::None);
            const float connectorTop =
                std::max(topY, std::min(anchorY, effectiveY));
            const float connectorBottom =
                std::min(bottomY, std::max(anchorY, effectiveY));
            // 两端可能因反向 SV 倒置，连线长度取排序后的屏幕区间而非时间差。
            if ( connectorBottom >= connectorTop ) {
                // 零高度连接可自然退化；负高度则说明交集为空，不能交给图元入口。
                batcher.pushQuad(centerX - 1.0F,
                                 connectorBottom,
                                 2.0F,
                                 connectorBottom - connectorTop,
                                 sampleOffsetColor);
            }
            std::array<char, 32> offsetBuffer{};
            // 明确保留正号，区分提前触发与延后触发，不把偏移绝对值当时刻。
            const auto offsetResult = fmt::format_to_n(offsetBuffer.data(),
                                                       offsetBuffer.size() - 1,
                                                       "{:+} ms",
                                                       sample.m_offsetMs);
            // 文本单位仍为毫秒，与组件偏移存储一致；纵坐标则由秒单位触发点投影。
            *offsetResult.out = '\0';
            renderCanvasAsciiText(
                batcher,
                std::string_view(offsetBuffer.data(),
                                 static_cast<std::size_t>(offsetResult.out -
                                                          offsetBuffer.data())),
                bounds->leftX + 4.0F,
                effectiveY + 4.0F,
                11.0F * sampleTextScale,
                laneWidth - 8.0F,
                sampleOffsetColor);
            // 触发时间横标记使用轨宽比例，不跟随本体水平缩放改变其标识范围。
            batcher.pushQuad(bounds->leftX + laneWidth * 0.18F,
                             effectiveY + 1.5F,
                             laneWidth * 0.64F,
                             3.0F,
                             sampleOffsetColor);
            // 横标记对应 effectiveY，锚点本体稍后仍绘制在未偏移的 anchorY。
        }
        if ( showOffsetHandle ) {
            // 手柄围绕触发点而非锚点居中，图元和下方命中框必须使用同一中心。
            batcher.setTexture(TextureID::None);
            batcher.pushRoundedQuad(offsetHandleX,
                                    effectiveY + offsetHandleSize * 0.5F,
                                    offsetHandleSize,
                                    offsetHandleSize,
                                    offsetHandleSize * 0.25F,
                                    sampleOffsetColor);
        }

        renderSampleBody(
            batcher, bodyX, anchorY, bodyWidth, bodyHeight, bodyColor);
        // 本体允许部分离屏，沿用全尺寸几何与 scissor，避免边缘处纹理被压缩。

        // 标签依赖资源标识和事件音量，不在渲染线程解析项目路径或打开音频文件。
        // 跑马灯使用快照时间，多个样本不会因逐项读时钟产生不同动画基准。
        renderAudioObjectLabel(batcher,
                               sample.m_audioResourceId,
                               sample.m_volume,
                               bounds->leftX,
                               anchorY - bodyHeight * 0.5F,
                               laneWidth,
                               config.visual.noteScaleY,
                               sampleTextColor,
                               snapshot->snapshotSysTime);
        // 标签锚在本体上边缘，不能使用
        // effectiveY，否则调整资源偏移会拖走资源名。
        // 标签使用完整轨宽布局，不随本体 noteScaleX 变窄而减少可用文本宽度。

        // 播放或只展示的视口仍绘制反馈，但不能向拾取系统追加可操作区域。
        // 锚点与偏移手柄采用不同部位标识，后续拖动据此区分移动和调偏移。
        if ( !snapshot->isPlaying && snapshot->acceptsInteraction ) {
            // subIndex 为 -1
            // 表示采样没有折线子节点；对象种类与部位共同确定拖动语义。
            // 本体命中框采用左上角坐标，不能照搬 pushQuad 的底边 Y。
            snapshot->hitboxes.push_back({
                entity,
                HoverPart::SampleAnchor,
                -1,
                bodyX,
                anchorY - bodyHeight * 0.5F,
                bodyWidth,
                bodyHeight,
                ChartObjectKind::AudioSample,
            });
            if ( showOffsetHandle ) {
                // 手柄隐藏时不能留下透明拾取区，否则零偏移本体附近会误入偏移拖动。
                snapshot->hitboxes.push_back({
                    entity,
                    HoverPart::SampleOffset,
                    -1,
                    offsetHandleX,
                    effectiveY - offsetHandleSize * 0.5F,
                    offsetHandleSize,
                    offsetHandleSize,
                    ChartObjectKind::AudioSample,
                });
                // 锚点与手柄即使重合也保留不同部位，拾取优先级由消费端统一处理。
            }
        }
    }

    // 先提交基础层待处理几何，再向独立发光列表写入；不混用两个批处理器的状态。
    if ( hasSampleGlow ) {
        // 没有选中或悬浮的可见候选时，不创建额外的发光批处理器。
        batcher.flush();
        Batcher glowBatcher(snapshot, &snapshot->glowCmds);
        // 独立命令列表仍向同一快照提交几何，必须显式设置其裁剪状态。
        glowBatcher.setScissor(0.0F, topY, viewportWidth, bottomY - topY);
        for ( const auto entity : snapshot->sampleQueryScratch ) {
            // 复用本帧候选，不为发光再次执行可见时间反查或 Registry 全量扫描。
            if ( !registry.valid(entity) ||
                 !registry.all_of<SampleComponent>(entity) ) {
                continue;
            }
            const auto* interaction =
                registry.try_get<const InteractionComponent>(entity);
            // 使用与基础层同一帧的组件状态，不从快照命中框反推物件是否选中。
            if ( !interaction ||
                 (!interaction->isSelected && !interaction->isHovered) ) {
                // 拖动标志本身不等于高亮资格，发光仍由选中或悬浮状态决定。
                continue;
            }
            const auto& sample  = registry.get<const SampleComponent>(entity);
            const auto  address = CanvasLaneAddress::fromAbsoluteTrack(
                sample.m_track, projection.playerLaneCount);
            if ( address.kind == CanvasLaneKind::Player &&
                 !interaction->isDragging ) {
                continue;
            }
            if ( address.kind == CanvasLaneKind::Bgm &&
                 (!visibleLaneRange ||
                  address.index < visibleLaneRange->first ||
                  address.index >= visibleLaneRange->second) ) {
                continue;
            }
            const auto bounds = projection.bounds(address);
            if ( !bounds || bounds->rightX <= 0.0F ||
                 bounds->leftX >= viewportWidth ) {
                continue;
            }

            // 发光是独立命令层，不能假定基础层前面设置的纹理或裁剪仍有效。
            // 本体 helper 为该层设置轮廓，图元最终写入同一快照的几何存储。
            const float anchorY =
                judgmentLineY -
                static_cast<float>(cache->getDisplayDelta(
                    sample.m_timestamp, currentAbsY, sample.m_timestamp)) *
                    renderScaleY;
            // 发光层沿用实际目标轨道宽度，避免拖动预览错位。
            const float laneWidth = bounds->rightX - bounds->leftX;
            const float bodyWidth = laneWidth * config.visual.noteScaleX;
            const float bodyHeight =
                (laneWidth / noteTextureAspect) * config.visual.noteScaleY;
            // 发光仅覆盖本体，不覆盖偏移连线，因此这里直接按本体范围裁剪。
            if ( anchorY + bodyHeight * 0.5F < topY ||
                 anchorY - bodyHeight * 0.5F > bottomY ) {
                continue;
            }
            // 连线可见但本体离屏时只保留基础层连接，不发出远处本体的发光图元。
            const float bodyX = bounds->leftX + (laneWidth - bodyWidth) * 0.5F;
            renderSampleBody(glowBatcher,
                             bodyX,
                             anchorY,
                             bodyWidth,
                             bodyHeight,
                             sampleColor);
            // 发光继续采用皮肤采样色，橡皮擦红色预览只影响基础层的局部配色。
        }
        glowBatcher.flush();
        // 后续画笔预览仍回到基础 batcher，不把半透明预览写进 glowCmds。
        // 发光批次必须在局部批处理器销毁前提交，不依赖基础层的最终 flush。
    }

    // 新建预览没有实体身份，不写命中框，也不参与上面的选中发光遍历。
    const auto& brush = snapshot->brush;
    // 只借用快照画笔值，不消费或结束手势；逻辑线程负责发布下一帧预览状态。
    // 画笔预览独立于可见实体数量，没有现存采样时也必须能显示新建落点。
    if ( !brush.isActive || !brush.createsAudioSample || brush.track < 0 ) {
        // 先排除负轨号，再转换为无符号绝对轨索引，避免无效预览绕回大数。
        return;
    }
    // 普通音符画笔不进入此入口，避免一份落点同时产生音符预览和采样预览。
    const auto brushAddress = CanvasLaneAddress::fromAbsoluteTrack(
        static_cast<std::uint32_t>(brush.track), projection.playerLaneCount);
    // 新建入口不能借用跨区拖动例外：拖进玩家区的已有采样和新采样预览是两种流程。
    if ( brushAddress.kind != CanvasLaneKind::Bgm || !visibleLaneRange ||
         brushAddress.index < visibleLaneRange->first ||
         brushAddress.index >= visibleLaneRange->second ) {
        // 新建采样仅预览 BGM 区；已有采样跨区拖动由上面的实体分支负责。
        return;
    }
    const auto brushBounds = projection.bounds(brushAddress);
    if ( !brushBounds || brushBounds->rightX <= 0.0F ||
         brushBounds->leftX >= viewportWidth ) {
        return;
    }
    // 预览的尺寸来源与已存在样本相同，正式创建后不会因使用另一轨宽而突然跳变。

    // 画笔预览使用目标 BGM 轨道的独立宽度。
    const float laneWidth = brushBounds->rightX - brushBounds->leftX;
    const float bodyWidth = laneWidth * config.visual.noteScaleX;
    const float bodyHeight =
        (laneWidth / noteTextureAspect) * config.visual.noteScaleY;
    const float anchorY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            brush.time, currentAbsY, brush.time)) *
                            renderScaleY;
    // 预览没有实体可供时间索引查询，因此直接对最终本体矩形执行纵向裁剪。
    if ( anchorY + bodyHeight * 0.5F < topY ||
         anchorY - bodyHeight * 0.5F > bottomY ) {
        return;
    }
    const float bodyX = brushBounds->leftX + (laneWidth - bodyWidth) * 0.5F;
    glm::vec4   previewColor = sampleColor;
    previewColor.a *= 0.5F;
    // 在皮肤原透明度上减半，而非赋固定透明度，保持主题原有的可见性选择。
    if ( hasNoteTexture ) {
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(bodyX,
                               anchorY + bodyHeight * 0.5F,
                               bodyWidth,
                               bodyHeight,
                               { noteTextureAspect, 1.0F },
                               config.visual.noteFillMode,
                               previewColor);
    } else {
        batcher.setTexture(TextureID::None);
        batcher.pushRoundedQuad(bodyX,
                                anchorY + bodyHeight * 0.5F,
                                bodyWidth,
                                bodyHeight,
                                4.0F,
                                previewColor);
    }
    // 半透明预览只影响这次提交的颜色，不修改全局 sampleColor 或皮肤原始值。
    glm::vec4 previewTextColor = textColor;
    previewTextColor.a *= 0.5F;
    // 文字与本体分别在各自皮肤透明度上减半，不能复用本体 alpha
    // 覆盖文字主题设置。
    // 新建预览尚无独立事件音量，标签采用单位增益，不读取已有实体的状态。
    renderAudioObjectLabel(batcher,
                           brush.audioResourceId,
                           1.0F,
                           brushBounds->leftX,
                           anchorY - bodyHeight * 0.5F,
                           laneWidth,
                           config.visual.noteScaleY,
                           previewTextColor,
                           snapshot->snapshotSysTime);
}

}  // namespace MMM::Logic::System
