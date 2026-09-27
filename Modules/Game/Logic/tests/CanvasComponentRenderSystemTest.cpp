#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/CanvasComponentLayout.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <vector>

namespace
{

/// @brief 为测试快照注入固定宽度 ASCII 字体度量和字形 UV。
/// @param snapshot 待初始化快照。
/// @note 固定内存字体消除系统字体、栅格器和图集加载状态的影响。
/// @note 字形步进一致，测试可用文本度量独立推导宽高。
/// @note 只有一个有效层级，因此布局用例不依赖其他字号档位。
/// @note 所有字形共用测试 UV，本助手不适用于按 UV 识别具体字符。
void configureAsciiFont(MMM::Logic::RenderSnapshot& snapshot)
{
    // 注入固定层级并同时打开图集与层级有效标志。
    // 缺少任一标志都会触发字体回退，掩盖布局测试真正的输入条件。
    constexpr std::size_t tierIndex = 5U;
    auto&                 atlas     = snapshot.asciiFontAtlasMetrics;
    atlas.valid                     = true;
    auto& font                      = atlas.tiers[tierIndex];
    font.valid                      = true;
    // 度量使用字号归一化值，位图高度和排版行高有意不同。
    // 后续布局检查基于 measureAsciiText，而不是把字形高度直接当作行高。
    font.ascender   = 0.8f;
    font.lineHeight = 1.0f;
    for ( std::uint32_t code = MMM::Common::ASCII_GLYPH_FIRST;
          code <= MMM::Common::ASCII_GLYPH_LAST;
          ++code ) {
        auto& glyph     = font.glyphs[code - MMM::Common::ASCII_GLYPH_FIRST];
        glyph.available = true;
        // 空格仅参与步进，不生成可见图元。
        // 这样 KPS 文本中的空白不会被误当成额外字形矩形。
        glyph.hasBitmap = code != static_cast<std::uint32_t>(' ');
        glyph.width     = 0.5f;
        glyph.height    = 0.75f;
        glyph.bearingX  = 0.0f;
        glyph.bearingY  = 0.75f;
        glyph.advanceX  = 0.6f;
        // 非空白字符都提供 UV，保证测试不会因缺字而静默跳过绘制。
        // UV 仅作有效纹理占位，具体字形内容通过格式化与度量断言验证。
        if ( glyph.hasBitmap ) {
            const auto textureId = MMM::Logic::asciiGlyphTextureId(
                tierIndex, static_cast<char>(code));
            snapshot.uvMap.emplace(static_cast<std::uint32_t>(textureId),
                                   glm::vec4(0.0f, 0.0f, 0.01f, 0.01f));
        }
    }
}

/// @brief 创建只含基础视口信息的画布组件渲染上下文。
/// @param currentTime 当前判定线时间。
/// @return 800x600 测试视口上下文。
/// @note 只初始化通用视口值，BPM、滚动缓存和 KPS 数据由各用例显式注入。
/// @note 返回值不拥有外部数据；调用方提供的缓存与跨度必须活到 render 返回。
/// @note 当前时间以秒表示，屏幕范围和判定线以像素表示。
MMM::Logic::System::CanvasComponentRenderContext makeRenderContext(
    double currentTime)
{
    // 统一默认视口与可见范围，个别裁剪案例随后只覆盖必要字段。
    // 判定线设在中部，为已通过与未到达的拍号同时留下空间。
    return {
        .currentTime    = currentTime,
        .viewportWidth  = 800.0f,
        .viewportHeight = 600.0f,
        .judgmentLineY  = 300.0f,
        .visibleTop     = 0.0f,
        .visibleBottom  = 600.0f,
        .renderScaleY   = 1.0f,
    };
}

/// @brief 验证高 DPI 画布按物理字号选择更高分辨率的字形档位。
/// @return 1.75 倍 DPI 下 14 逻辑像素选择 24 像素档位时返回 true。
/// @note 把所有档位设为有效，避免缺档回退掩盖 DPI 选择策略。
/// @note 逻辑字号乘栅格倍率后接近 24 像素，应选对应物理字号档位。
/// @note 只测试层级选择，不创建实际字形纹理或窗口。
/// @note 选择成功不代表真实字体栅格化完成，本例仅覆盖档位决策。
/// @note 有效档位数组和栅格倍率全部由用例控制，不读取显示器 DPI。
bool testDpiAwareAsciiFontTierSelection()
{
    MMM::Common::AsciiFontAtlasMetrics atlas;
    atlas.valid = true;
    // 1.75 倍对应物理字号约 24.5 像素，与逻辑 14 像素明显不同。
    // 若实现忽略 DPI，就会落在低分辨率档位而使断言失败。
    atlas.rasterScale = 1.75F;
    // 所有层级可用时，结果只能由字号策略决定。
    // 不在此用例中加入缺失档位的回退因素。
    for ( auto& tier : atlas.tiers ) {
        tier.valid = true;
    }

    // 先确认选择有效，再读取层级下标，避免失败结果被当作首档。
    // 断言物理栅格高度而非枚举序号，使结论直接对应 DPI 要求。
    const auto selection = MMM::Common::selectAsciiFont(atlas, 14.0F);
    return selection &&
           MMM::Common::ASCII_FONT_RASTER_HEIGHTS[selection.tierIndex] == 24U;
}

/// @brief 验证关闭组件时不会生成覆盖层几何。
/// @return 快照保持为空时返回 true。
/// @note 字体有效但所有组件沿用默认隐藏状态，隔离可见开关的作用。
/// @note 除可见几何，还检查编辑实例列表，隐藏组件不应留下可选中区域。
/// @note 不把缺少字形导致的空输出误当成隐藏逻辑正常。
bool testHiddenComponentDoesNotRender()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    // 先准备资源，再保持组件默认隐藏。
    // 检查的是配置门槛，不是无资源时无法绘制的退化路径。
    configureAsciiFont(snapshot);
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, makeRenderContext(12.345), config);
    // 顶点、索引、覆盖命令和布局实例都应为空。
    // 只检查命令可能漏掉无效几何或不可见但仍能编辑的实例。
    return snapshot.vertices.empty() && snapshot.indices.empty() &&
           snapshot.overlayCmds.empty() &&
           snapshot.canvasComponentInstances.empty();
}

/// @brief 验证启用时间组件后生成最终覆盖层且几何保持在配置边界内。
/// @return 覆盖命令、纹理类型和顶点范围均正确时返回 true。
/// @note 判定线时间组件只生成覆盖层，本例不运行完整音符渲染。
/// @note 通过同一字体度量计算允许边界，再检查实际顶点和颜色。
/// @note 纹理断言要求使用字形类型，但不根据测试 UV 反推出文本内容。
/// @note 锚点与字号均显式配置，不依赖用户磁盘设置。
bool testVisibleComponentRendersInOverlay()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    // 当前时间组件需要有谱面状态，显式满足这个前提。
    // 布局位置取非中心锚点，便于发现忽略锚点的实现。
    snapshot.hasBeatmap     = true;
    auto& placement         = config.judgmentLineTime;
    placement.visible       = true;
    placement.anchorX       = 0.25f;
    placement.anchorY       = 0.75f;
    placement.fontSizeRatio = 0.04f;
    placement.color         = { 0.2f, 0.4f, 0.6f, 0.8f };

    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, makeRenderContext(72.345), config);
    // 先确认实际产生几何，再逐项检查边界和颜色。
    // 空输出不能通过后面遍历零次的断言。
    if ( snapshot.vertices.empty() || snapshot.indices.empty() ||
         snapshot.overlayCmds.empty() ) {
        XERROR("Visible canvas component did not produce overlay geometry");
        return false;
    }
    // 覆盖层文本必须使用字形纹理，纯色占位不算有效文本输出。
    // 这里只检查纹理类型，不假设各字符如何合批。
    for ( const auto& command : snapshot.overlayCmds ) {
        if ( command.customTextureId ==
             static_cast<std::uint32_t>(MMM::Logic::TextureID::None) ) {
            XERROR("Canvas component did not use an ASCII glyph texture");
            return false;
        }
    }

    // 格式化与字体度量共同给出应占据的文本尺寸。
    // 由布局助手计算锚点边界，实际顶点仍来自独立渲染调用。
    const auto text =
        MMM::Logic::System::CanvasComponentRenderSystem::formatJudgmentLineTime(
            72.345);
    const float fontHeight    = placement.fontSizeRatio * 600.0f;
    const auto  fontSelection = MMM::Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontHeight);
    // 没有字体档位时无法建立期望边界，应直接失败。
    // 不能使用默认宽高继续比较而掩盖字体初始化问题。
    if ( !fontSelection ) {
        XERROR("Canvas component did not select an ASCII font tier");
        return false;
    }
    const auto textSize = MMM::Common::measureAsciiText(
        *fontSelection.metrics, text.data(), fontHeight);
    const auto bounds = MMM::Logic::canvasComponentBounds(
        placement, 800.0f, 600.0f, textSize.width, textSize.height);
    constexpr float epsilon = 1e-4f;
    // 每个字形角点都受配置范围约束，不能只检查整体起点。
    // 浮点容差允许度量到几何的微小舍入误差。
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.pos.x < bounds.left - epsilon ||
             vertex.pos.x > bounds.right + epsilon ||
             vertex.pos.y < bounds.top - epsilon ||
             vertex.pos.y > bounds.bottom + epsilon ) {
            XERROR("Canvas component vertex escaped its configured bounds");
            return false;
        }
        // RGBA 全通道应取组件配置，尤其透明度不能丢失。
        // 逐顶点检查可发现批次切换后部分字符沿用旧颜色。
        if ( std::abs(vertex.color.r - placement.color[0]) > epsilon ||
             std::abs(vertex.color.g - placement.color[1]) > epsilon ||
             std::abs(vertex.color.b - placement.color[2]) > epsilon ||
             std::abs(vertex.color.a - placement.color[3]) > epsilon ) {
            XERROR("Canvas component vertex did not use configured color");
            return false;
        }
    }
    return true;
}

/// @brief 验证未加载谱面时不会绘制当前判定线时间。
/// @return 时间组件已启用但快照保持无覆盖层几何时返回 true。
/// @note 只开启可见开关而不设置 hasBeatmap，验证空会话的显示门槛。
/// @note 负时间是合法格式化输入，不能成为本例空输出的唯一解释。
/// @note 字体已经准备好，组件消失应来自谱面状态而非资源缺失。
bool testJudgmentLineTimeDoesNotRenderWithoutBeatmap()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    // 可见开关与 hasBeatmap 是独立条件，只满足前者仍应隐藏。
    // 保留默认无谱面状态，不创建虚假的项目对象。
    config.judgmentLineTime.visible = true;

    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, makeRenderContext(-0.035), config);
    // 无谱面时同时禁止生成布局编辑实例。
    // 否则虽然文字不可见，仍可能留下可被拖动的空组件。
    return snapshot.vertices.empty() && snapshot.indices.empty() &&
           snapshot.overlayCmds.empty() &&
           snapshot.canvasComponentInstances.empty();
}

/// @brief 验证拍号会按整拍复制并保持在各自拍内布局区域。
/// @return 实例编号、区域边界、默认暗橙色和字形几何均正确时返回 true。
/// @note 固定 120 BPM 和默认滚动映射，使可见整拍与屏幕区间可预测。
/// @note 拍号从一开始，实例索引用于区分重复布局的每一拍。
/// @note 逐实例验证区域，逐顶点验证颜色，二者不能互相替代。
/// @note 不测试文字内容解码，格式化契约由文件末尾的独立用例覆盖。
bool testBeatNumbersRenderInsideEachBeat()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.beatNumber.visible = true;

    // 用单个零秒 BPM 建立可预测的拍位相位。
    // Registry 在本次 render 完成前保持存活，组件地址仅作为观察引用。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    auto&          bpm =
        timelineRegistry.emplace<MMM::Logic::TimelineComponent>(bpmEntity);
    bpm.m_timestamp = 0.0;
    bpm.m_effect    = MMM::TimingEffect::BPM;
    bpm.m_value     = 120.0;

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    // 滚动缓存与 BPM 事件列表来自同一时间线。
    // 缓存负责时间到像素的映射，事件列表负责整拍或分拍的枚举。
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    // 在一秒位置生成快照，可见的编号应连续跨过当前拍。
    // 上下文借用事件列表和缓存，不为这次测试复制完整谱面。
    auto context        = makeRenderContext(1.0);
    context.bpmEvents   = bpmEvents;
    context.scrollCache = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    // 同时要求正确实例数量和非空绘制输出。
    // 只有元数据而未绘制字形，或重复生成实例，都不能通过。
    if ( snapshot.canvasComponentInstances.size() != 2U ||
         snapshot.vertices.empty() || snapshot.overlayCmds.empty() ) {
        XERROR("Beat number component did not render visible whole beats");
        return false;
    }
    // 实例编号是一基拍号，不是从可见区域重新开始的局部序号。
    // 当前视口改变后仍需保持全谱拍号的含义。
    if ( snapshot.canvasComponentInstances[0].instanceIndex != 2 ||
         snapshot.canvasComponentInstances[1].instanceIndex != 3 ) {
        XERROR("Beat number component did not preserve one-based beat order");
        return false;
    }

    constexpr float epsilon = 1e-4f;
    // 每一拍拥有自己的纵向布局区域，文字不能进入相邻拍的区域。
    // 水平区域覆盖全画布，不能误套用某一玩家轨宽。
    for ( const auto& instance : snapshot.canvasComponentInstances ) {
        if ( instance.type != MMM::Config::CanvasComponentType::BeatNumber ||
             instance.instanceIndex <= 0 || instance.regionLeft != 0.0f ||
             instance.regionRight != 800.0f ||
             instance.left < instance.regionLeft - epsilon ||
             instance.right > instance.regionRight + epsilon ||
             instance.top < instance.regionTop - epsilon ||
             instance.bottom > instance.regionBottom + epsilon ) {
            XERROR("Beat number escaped its per-beat layout region");
            return false;
        }
    }

    // 先验证默认配置常量，再验证输出使用该常量。
    // 只比较顶点与配置会在两者同时变成错误颜色时漏报。
    const auto& darkOrange = MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT.color;
    if ( std::abs(darkOrange[0] - 1.0f) > epsilon ||
         std::abs(darkOrange[1] - 140.0f / 255.0f) > epsilon ||
         std::abs(darkOrange[2]) > epsilon ||
         std::abs(darkOrange[3] - 1.0f) > epsilon ) {
        XERROR("Beat number default color is not #FF8C00");
        return false;
    }
    // 遍历全部字形，确认默认色传播到所有可见拍号。
    // 不依赖某个实例对应的顶点偏移，允许渲染端调整合批顺序。
    for ( const auto& vertex : snapshot.vertices ) {
        if ( std::abs(vertex.color.r - darkOrange[0]) > epsilon ||
             std::abs(vertex.color.g - darkOrange[1]) > epsilon ||
             std::abs(vertex.color.b - darkOrange[2]) > epsilon ||
             std::abs(vertex.color.a - darkOrange[3]) > epsilon ) {
            XERROR("Beat number did not use the default dark orange color");
            return false;
        }
    }
    return true;
}

/// @brief 验证不足一拍的相邻 BPM 红线仍各自推进一个拍号。
/// @return 三条密集红线对应的拍号依次为 1、2、3 时返回 true。
/// @note 相邻 BPM 起点仅间隔 0.1 秒，小于 120 BPM 的半秒拍长。
/// @note 即使每段不足一拍，新的 BPM 起点仍占用一个独立拍号。
/// @note 缓存和显式 BPM 列表使用相同数据，避免时序来源不一致。
/// @note 只要求三个目标实例存在，不限制视口中其他整拍实例数量。
bool testDenseBpmMarkersAdvanceBeatNumbers()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.beatNumber.visible = true;

    entt::registry                                timelineRegistry;
    std::array<MMM::Logic::TimelineComponent*, 3> bpmEvents{};
    // 三个段都短于半秒拍长，单纯截断拍数会错误合并编号。
    // 时间戳按升序构造，明确满足调用方提供有序列表的约定。
    constexpr std::array<double, 3> BPM_TIMES{ 0.0, 0.1, 0.2 };
    for ( std::size_t index = 0U; index < BPM_TIMES.size(); ++index ) {
        const auto entity = timelineRegistry.create();
        auto&      bpm =
            timelineRegistry.emplace<MMM::Logic::TimelineComponent>(entity);
        bpm.m_timestamp = BPM_TIMES[index];
        bpm.m_effect    = MMM::TimingEffect::BPM;
        bpm.m_value     = 120.0;
        // 保存组件观察地址供有序列表使用，渲染前不删除或替换这些实体。
        // 该列表不负责所有权，生命周期仍由本用例的 Registry 管理。
        bpmEvents[index] = &bpm;
    }

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    // 构造只读组件观察列表，不重新按 Registry 的遍历顺序排序。
    // 测试输入的顺序由前面的时间数组确定。
    const std::vector<const MMM::Logic::TimelineComponent*> orderedBpmEvents{
        bpmEvents.begin(), bpmEvents.end()
    };

    // 当前时间落在第三个 BPM 起点，使三个密集起点都靠近判定线。
    // 不使用播放时钟，避免测试速度影响哪些实例可见。
    auto context        = makeRenderContext(0.2);
    context.bpmEvents   = orderedBpmEvents;
    context.scrollCache = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    // 分别查找每个目标拍号，避免只靠总数判断。
    // 可见范围还可能包含后续整拍，因此不把总实例数限制为三。
    for ( std::int64_t expectedBeat = 1; expectedBeat <= 3; ++expectedBeat ) {
        const auto instance = std::find_if(
            snapshot.canvasComponentInstances.begin(),
            snapshot.canvasComponentInstances.end(),
            [expectedBeat](const auto& candidate) {
                return candidate.type ==
                           MMM::Config::CanvasComponentType::BeatNumber &&
                       candidate.instanceIndex == expectedBeat;
            });
        // 缺少任意密集起点的编号就报告具体拍号。
        // 这样可区分第一段计数错误与后续 BPM 段未推进。
        if ( instance == snapshot.canvasComponentInstances.end() ) {
            XERROR("Dense BPM marker did not advance to beat {}", expectedBeat);
            return false;
        }
    }
    return true;
}

/// @brief 验证拍起点越过判定线后，拍号会保留到文字离开轨道布局视口。
/// @return 当前拍文字仍被生成且绘制命令使用布局视口 scissor 时返回 true。
/// @note 判断保留条件应基于文字与布局视口相交，而非拍起点是否过判定线。
/// @note 用截短的布局视口区分整个画布高度与组件裁剪高度。
/// @note 实例可延伸到视口外，最终可见区域由绘制命令的 scissor 限制。
/// @note 测试不执行 GPU 裁剪，只检查提交前的命令数据。
bool testBeatNumberRendersUntilLayoutViewportExit()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.beatNumber.visible = true;

    // 用单个零秒 BPM 建立可预测的拍位相位。
    // Registry 在本次 render 完成前保持存活，组件地址仅作为观察引用。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    auto&          bpm =
        timelineRegistry.emplace<MMM::Logic::TimelineComponent>(bpmEntity);
    bpm.m_timestamp = 0.0;
    bpm.m_effect    = MMM::TimingEffect::BPM;
    bpm.m_value     = 120.0;

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    // 滚动缓存与 BPM 事件列表来自同一时间线。
    // 缓存负责时间到像素的映射，事件列表负责整拍或分拍的枚举。
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    auto context = makeRenderContext(1.4);
    // 整个视口高 650，但布局范围仅 25 至 600。
    // 判定线设为 500，使已经通过的拍号仍有一部分文字可见。
    context.viewportHeight = 650.0f;
    context.judgmentLineY  = 500.0f;
    context.visibleTop     = 25.0f;
    context.visibleBottom  = 600.0f;
    // 更改裁剪范围不改变 BPM 数据和滚动缓存。
    // 测试因此只检验离开视口的时机，不混入节拍相位变化。
    context.bpmEvents   = bpmEvents;
    context.scrollCache = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    // 查找第三拍而不是依赖其在可见实例数组中的位置。
    // 时间推进后其他拍的增删不能改变目标实例身份。
    const auto instance = std::find_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& candidate) { return candidate.instanceIndex == 3; });
    // 文字顶部仍在布局内，底部越过布局底边，应该保留并裁剪。
    // 若按拍起点过线立即删除，目标实例会在这里缺失。
    if ( instance == snapshot.canvasComponentInstances.end() ||
         instance->regionBottom <= context.visibleBottom ||
         instance->top <= context.judgmentLineY ||
         instance->top >= context.visibleBottom ||
         instance->bottom <= context.visibleBottom ) {
        XERROR("Beat number disappeared before leaving the layout viewport");
        return false;
    }

    // 裁剪高度取 visibleBottom 减 visibleTop，即 575 像素。
    // 不能继承全画布高度，也不能把 scissor 原点误设为判定线。
    for ( const auto& command : snapshot.overlayCmds ) {
        if ( command.scissor.x != 0 || command.scissor.y != 25 ||
             command.scissor.width != 800U || command.scissor.height != 575U ) {
            XERROR("Beat number did not use the layout viewport scissor");
            return false;
        }
    }
    // 要求至少提交一条覆盖命令，避免仅保留布局元数据。
    // 空命令集合不能因前面的遍历没有执行而视作裁剪正确。
    return !snapshot.overlayCmds.empty();
}

/// @brief 验证拍内可移动区域保留上边界并按当前文字半高向下扩展。
/// @return 上方空间、下方扩展量和拍头线文字中心均正确时返回 true。
/// @note 将纵向锚点设为一，验证文字中心与拍头对齐的布局约定。
/// @note 区域下端按文字半高扩展，上端保持原始拍区间边界。
/// @note 特意让文字跨越 visibleTop，避免过早按拍头位置剔除。
/// @note 字体高度取完整画布比例，不由当前拍的像素高度决定。
bool testBeatNumberLayoutRegionCentersOnBeatHead()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.beatNumber.visible = true;
    // 锚点一对应可移动区域的下端，正好检验半文字高度扩展。
    // 若只扩大内容框而不扩大区域，文字中心不会落在拍头。
    config.beatNumber.anchorY = 1.0f;

    // 用单个零秒 BPM 建立可预测的拍位相位。
    // Registry 在本次 render 完成前保持存活，组件地址仅作为观察引用。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    auto&          bpm =
        timelineRegistry.emplace<MMM::Logic::TimelineComponent>(bpmEntity);
    bpm.m_timestamp = 0.0;
    bpm.m_effect    = MMM::TimingEffect::BPM;
    bpm.m_value     = 120.0;

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    // 滚动缓存与 BPM 事件列表来自同一时间线。
    // 缓存负责时间到像素的映射，事件列表负责整拍或分拍的枚举。
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    auto context = makeRenderContext(1.08);
    // 把完整高度和可见范围设成不同值，区分字号基准与裁剪基准。
    // 字号按 750 高度计算，而不是按可见的 600 高度计算。
    context.viewportHeight = 750.0f;
    context.judgmentLineY  = 300.0f;
    context.visibleTop     = 100.0f;
    context.visibleBottom  = 700.0f;
    context.bpmEvents      = bpmEvents;
    context.scrollCache    = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    const auto instance = std::find_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& candidate) { return candidate.instanceIndex == 4; });
    // 第四拍必须先被枚举和保留，后续才有布局区域可以验证。
    // 实例缺失不能用空矩形代替，否则中心计算会给出无意义结果。
    if ( instance == snapshot.canvasComponentInstances.end() ) {
        XERROR("Beat number for beat-head alignment was not rendered");
        return false;
    }

    // 固定原始拍区间来自当前时间和默认滚动速度。
    // 这些像素期望不从实际实例反推，因此能发现区域整体平移错误。
    constexpr float rawRegionTop    = -160.0f;
    constexpr float rawRegionBottom = 90.0f;
    const float     fontHeight =
        config.beatNumber.fontSizeRatio * context.viewportHeight;
    const auto selection = MMM::Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontHeight);
    const auto text =
        MMM::Logic::System::CanvasComponentRenderSystem::formatBeatNumber(4);
    // 选择失败时不能调用度量接口解引用空指标。
    // 该失败表明字体前提未满足，不是布局锚点误差。
    if ( !selection ) {
        XERROR("Beat number did not select a font for layout alignment");
        return false;
    }
    const auto textSize = MMM::Common::measureAsciiText(
        *selection.metrics, text.data(), fontHeight);
    // 向下扩展量取文字排版高度的一半，而非字形位图高度。
    // 文字中心必须回到未扩展的拍头线，保留向上拖动空间。
    const float     expectedOffset = textSize.height * 0.5f;
    const float     contentCenterY = (instance->top + instance->bottom) * 0.5f;
    constexpr float epsilon        = 1e-4f;
    // 同时检查区域上端、下端、内容中心和跨裁剪边界状态。
    // 只检查中心可能漏掉整个可移动区域被错误缩小的问题。
    if ( std::abs(instance->regionTop - rawRegionTop) > epsilon ||
         std::abs(instance->regionBottom - (rawRegionBottom + expectedOffset)) >
             epsilon ||
         std::abs(contentCenterY - rawRegionBottom) > epsilon ||
         instance->top >= context.visibleTop ||
         instance->bottom <= context.visibleTop ) {
        XERROR("Beat number layout range did not center on the beat-head line");
        return false;
    }
    return true;
}

/// @brief 验证分拍线时间逐分拍绘制并限制在单个分拍扩展区域内。
/// @return 时间、实例序号、独立颜色及向下半高扩展均正确时返回 true。
/// @note 分拍数为四，在 120 BPM 下相邻分拍相差 0.125 秒。
/// @note 选定实例对应一秒整点，便于独立核对时间、区域与索引。
/// @note 增加视觉纵向倍率只应拉伸网格间距，不应放大时间文字。
/// @note 独立组件颜色不应被同类拍号默认色覆盖。
bool testBeatLineTimesRenderInsideEachSubdivision()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    // 分拍时间与当前时间一样需要谱面状态，显式打开以进入绘制分支。
    // 自定义颜色和末端锚点用于区分该组件的独立配置。
    snapshot.hasBeatmap         = true;
    config.beatLineTime.visible = true;
    config.beatLineTime.anchorY = 1.0f;
    config.beatLineTime.color   = { 0.2f, 0.7f, 0.9f, 0.8f };

    // 用单个零秒 BPM 建立可预测的拍位相位。
    // Registry 在本次 render 完成前保持存活，组件地址仅作为观察引用。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    auto&          bpm =
        timelineRegistry.emplace<MMM::Logic::TimelineComponent>(bpmEntity);
    bpm.m_timestamp = 0.0;
    bpm.m_effect    = MMM::TimingEffect::BPM;
    bpm.m_value     = 120.0;

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    // 滚动缓存与 BPM 事件列表来自同一时间线。
    // 缓存负责时间到像素的映射，事件列表负责整拍或分拍的枚举。
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    auto context = makeRenderContext(1.0);
    // 四分拍间隔为 0.125 秒，一秒整点对应一基索引九。
    // 时间标签仍显示绝对秒，不把分拍索引当作时间值。
    context.beatDivisor = 4;
    context.bpmEvents   = bpmEvents;
    context.scrollCache = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    // 类型与索引同时匹配，避免误取其他组件的同号实例。
    // 该位置落在判定线，便于直接核对布局下边界。
    const auto instance = std::find_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& candidate) {
            return candidate.type ==
                       MMM::Config::CanvasComponentType::BeatLineTime &&
                   candidate.instanceIndex == 9;
        });
    // 指定索引九应对应一秒时间，不依赖实例数组中的存储位置。
    // 即使可见分拍数量变化，这个稳定索引仍用于查找同一目标。
    if ( instance == snapshot.canvasComponentInstances.end() ) {
        XERROR("Beat line time at 1.000 seconds was not rendered");
        return false;
    }

    // 单个分拍占 62.5 像素，原始范围从 237.5 到 300。
    // 布局只额外向下扩展半文字高，上方仍以该分拍范围为限。
    constexpr float rawRegionTop    = 237.5f;
    constexpr float rawRegionBottom = 300.0f;
    const float     fontHeight =
        config.beatLineTime.fontSizeRatio * context.viewportHeight;
    const auto selection = MMM::Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontHeight);
    const auto text =
        MMM::Logic::System::CanvasComponentRenderSystem::formatJudgmentLineTime(
            1.0);
    // 文字尺寸由有效字体度量定义，缺少档位时应直接报告失败。
    // 不使用零宽度作为默认期望，避免标签完全不绘制却通过。
    if ( !selection ) {
        XERROR("Beat line time did not select an ASCII font");
        return false;
    }
    const auto textSize = MMM::Common::measureAsciiText(
        *selection.metrics, text.data(), fontHeight);
    // 与拍号同样使用文字半高对齐，但时间字符串拥有独立宽度。
    // 因此还需检查输出内容宽度与格式化后的文字度量一致。
    const float     expectedOffset = textSize.height * 0.5f;
    const float     contentCenterY = (instance->top + instance->bottom) * 0.5f;
    constexpr float epsilon        = 1e-4f;
    if ( std::abs(instance->regionTop - rawRegionTop) > epsilon ||
         std::abs(instance->regionBottom - (rawRegionBottom + expectedOffset)) >
             epsilon ||
         std::abs(contentCenterY - rawRegionBottom) > epsilon ||
         std::abs((instance->right - instance->left) - textSize.width) >
             epsilon ) {
        XERROR("Beat line time escaped its subdivision layout region");
        return false;
    }

    // 第二份快照只改变纵向网格倍率，其他配置和当前时间保持一致。
    // 独立输出防止前一次的实例或顶点参与后一次查找。
    MMM::Logic::RenderSnapshot stretchedSnapshot;
    configureAsciiFont(stretchedSnapshot);
    stretchedSnapshot.hasBeatmap = true;
    // 复制上下文保留相同时间、BPM 和视口高度，只覆盖 renderScaleY。
    // 标签字号比以视口高度为基准，不应受这个倍率影响。
    auto stretchedContext         = context;
    stretchedContext.renderScaleY = 5.0f;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &stretchedSnapshot, stretchedContext, config);
    // 用同一组件类型与稳定索引匹配两份快照。
    // 放大后可见实例总数可能减少，不能按数组下标配对。
    const auto stretchedInstance = std::find_if(
        stretchedSnapshot.canvasComponentInstances.begin(),
        stretchedSnapshot.canvasComponentInstances.end(),
        [](const auto& candidate) {
            return candidate.type ==
                       MMM::Config::CanvasComponentType::BeatLineTime &&
                   candidate.instanceIndex == 9;
        });
    // 缩放后目标实例仍必须存在，宽高应与未拉伸时相同。
    // 这里只放大拍间距，不允许把标签当作音符几何一起缩放。
    if ( stretchedInstance ==
             stretchedSnapshot.canvasComponentInstances.end() ||
         std::abs((stretchedInstance->right - stretchedInstance->left) -
                  (instance->right - instance->left)) > epsilon ||
         std::abs((stretchedInstance->bottom - stretchedInstance->top) -
                  (instance->bottom - instance->top)) > epsilon ) {
        XERROR("Beat line time size changed with the visual grid height");
        return false;
    }

    // 所有时间字形均使用该组件的独立颜色。
    // 不能因为共用拍线枚举逻辑，就继承拍号组件的暗橙色。
    for ( const auto& vertex : snapshot.vertices ) {
        if ( std::abs(vertex.color.r - config.beatLineTime.color[0]) >
                 epsilon ||
             std::abs(vertex.color.g - config.beatLineTime.color[1]) >
                 epsilon ||
             std::abs(vertex.color.b - config.beatLineTime.color[2]) >
                 epsilon ||
             std::abs(vertex.color.a - config.beatLineTime.color[3]) >
                 epsilon ) {
            XERROR("Beat line time did not use its independent color");
            return false;
        }
    }
    // 颜色循环之外再要求非空几何和命令，防止空输出绕过颜色断言。
    // 实例元数据正确也不能替代实际文字提交。
    return !snapshot.vertices.empty() && !snapshot.overlayCmds.empty();
}

/// @brief 验证逐轨及总 KPS 实例，并覆盖单轨 KPS 位于轨道外的场景。
/// @return 实例数量、索引、全画布布局与裁剪以及颜色均正确时返回 true。
/// @note 同时启用拍号，验证后绘制的 KPS 不会继承前一组件的局部裁剪。
/// @note 一条轨道使用实例覆盖，把文本移到玩家区以外但仍在全画布内。
/// @note 轨道实例索引与总计保留索引必须稳定，不能由绘制顺序重新编号。
/// @note KPS 数值直接由上下文注入；事件统计在另一个用例中验证。
bool testKpsRendersPerTrackAndTotal()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    snapshot.hasBeatmap = true;
    // 显式模拟播放态，使 KPS 的显示条件与事件统计场景一致。
    // 本例不启动播放器，计数由下面的固定数组提供。
    snapshot.isPlaying        = true;
    config.beatNumber.visible = true;
    config.kps.visible        = true;
    config.kps.anchorY        = 0.05f;
    config.kps.color          = { 0.3f, 0.8f, 0.9f, 0.75f };
    // 只为第二条轨创建实例布局覆盖，保留组级颜色。
    // 将其放到玩家轨左侧和布局上方，检验 KPS 的全画布定位。
    auto& secondTrack = config.editablePlacement(
        MMM::Config::CanvasComponentType::Kps, 1, 3, 0.2f, 0.8f);
    secondTrack.anchorX       = 0.1f;
    secondTrack.anchorY       = 0.05f;
    secondTrack.fontSizeRatio = 0.04f;

    // 用单个零秒 BPM 建立可预测的拍位相位。
    // Registry 在本次 render 完成前保持存活，组件地址仅作为观察引用。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    auto&          bpm =
        timelineRegistry.emplace<MMM::Logic::TimelineComponent>(bpmEntity);
    bpm.m_timestamp = 0.0;
    bpm.m_effect    = MMM::TimingEffect::BPM;
    bpm.m_value     = 120.0;

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    // 滚动缓存与 BPM 事件列表来自同一时间线。
    // 缓存负责时间到像素的映射，事件列表负责整拍或分拍的枚举。
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    // 三个轨计数不同，总和为十二，能区分轨内索引与总计。
    // 上下文只借用该数组，渲染期间保持其生命周期。
    const std::array<std::uint32_t, 3> kps{ 2U, 4U, 6U };
    auto                               context = makeRenderContext(1.0);
    context.trackCount                         = 3;
    context.trackLeft                          = 0.2f;
    context.trackRight                         = 0.8f;
    context.trackKps                           = kps;
    context.bpmEvents                          = bpmEvents;
    context.scrollCache                        = &cache;
    // 拍号只在中间布局范围内绘制，KPS 则被放在顶部外侧。
    // 两种裁剪范围同时出现，才能检测批处理 scissor 状态遗留。
    context.visibleTop    = 100.0f;
    context.visibleBottom = 500.0f;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    // 拍号也启用，因此先按组件类型过滤再计数。
    // 总数四代表三条轨与一个总计，不是所有画布实例的总数。
    const auto kpsInstanceCount = std::count_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& instance) {
            return instance.type == MMM::Config::CanvasComponentType::Kps;
        });
    // 数量必须恰好等于轨数加总计，重复或漏绘都应失败。
    // 接下来的索引检查进一步避免数量相同但身份错误。
    if ( kpsInstanceCount != 4 ) {
        XERROR("KPS component did not produce three tracks and one total");
        return false;
    }
    // 按稳定实例索引找回布局，不依赖覆盖层中的提交顺序。
    // 总计使用保留索引，不能与任何一条轨道混用。
    /// @brief 在已生成快照中查找指定 KPS 实例。
    /// @param instanceIndex 轨道索引或总计保留索引。
    /// @return 匹配实例的迭代器；缺失时返回 end。
    const auto findInstance = [&snapshot](std::int64_t instanceIndex) {
        return std::find_if(
            snapshot.canvasComponentInstances.begin(),
            snapshot.canvasComponentInstances.end(),
            [instanceIndex](const auto& instance) {
                return instance.type == MMM::Config::CanvasComponentType::Kps &&
                       instance.instanceIndex == instanceIndex;
            });
    };
    // 总计使用专用索引，逐轨实例仍按零基编号查询。
    // 显示字符串中的一基编号在末尾格式化断言中单独验证。
    const auto total  = findInstance(MMM::Config::KPS_TOTAL_INSTANCE_INDEX);
    const auto first  = findInstance(0);
    const auto second = findInstance(1);
    const auto third  = findInstance(2);
    // 先确认每个目标都存在，再访问它们的矩形。
    // 数量正确仍可能存在重复索引而缺少某一条轨。
    if ( total == snapshot.canvasComponentInstances.end() ||
         first == snapshot.canvasComponentInstances.end() ||
         second == snapshot.canvasComponentInstances.end() ||
         third == snapshot.canvasComponentInstances.end() ) {
        XERROR("KPS component instance indices were not stable");
        return false;
    }

    constexpr float epsilon = 1e-4f;
    // 第二轨锚点 0.1、0.05 应映射到全画布的 80、30。
    // 若误以玩家区为基准，位置会被平移或限制到区域内部。
    const float secondCenterX = (second->left + second->right) * 0.5f;
    const float secondCenterY = (second->top + second->bottom) * 0.5f;
    // 不只比较锚点中心，还要求实例区域是整个 800×600 画布。
    // 这样可发现内容偶然位于预期位置、但编辑可移动范围仍被轨道限制的情况。
    if ( std::abs(secondCenterX - 80.0f) > epsilon ||
         std::abs(secondCenterY - 30.0f) > epsilon ||
         second->right >= context.trackLeft * context.viewportWidth ||
         second->bottom >= context.visibleTop ||
         total->bottom >= context.visibleTop || second->regionLeft != 0.0f ||
         second->regionTop != 0.0f || second->regionRight != 800.0f ||
         second->regionBottom != 600.0f ) {
        XERROR("Per-track KPS override was constrained to the track region");
        return false;
    }
    // 后面读取末条命令前，必须确认实际生成了覆盖层。
    // 布局实例存在本身不代表文字已提交。
    if ( snapshot.overlayCmds.empty() ) {
        XERROR("KPS component did not produce an overlay command");
        return false;
    }
    // KPS 在拍号之后生成，末条命令应恢复全画布裁剪。
    // 若沿用拍号的局部 scissor，位于顶部的 KPS 会被裁掉。
    const auto& kpsCommand = snapshot.overlayCmds.back();
    if ( kpsCommand.scissor.x != 0 || kpsCommand.scissor.y != 0 ||
         kpsCommand.scissor.width != 800U ||
         kpsCommand.scissor.height != 600U ) {
        XERROR("KPS inherited the preceding beat component scissor");
        return false;
    }
    // 只检查 KPS 末批索引引用的颜色，避免混入前面的拍号字形。
    // 这验证组色传递，不把每种组件都要求成同一颜色。
    const auto commandIndexEnd = kpsCommand.indexOffset + kpsCommand.indexCount;
    // 该夹具检查末条 KPS 命令直接引用的顶点索引。
    // 断言范围只覆盖这份命令，不据此证明任意顶点基址的通用批处理行为。
    for ( std::uint32_t index = kpsCommand.indexOffset; index < commandIndexEnd;
          ++index ) {
        const auto& vertex = snapshot.vertices[snapshot.indices[index]];
        if ( std::abs(vertex.color.r - config.kps.color[0]) > epsilon ||
             std::abs(vertex.color.g - config.kps.color[1]) > epsilon ||
             std::abs(vertex.color.b - config.kps.color[2]) > epsilon ||
             std::abs(vertex.color.a - config.kps.color[3]) > epsilon ) {
            XERROR("KPS instances did not share the configured group color");
            return false;
        }
    }
    // 格式化另以固定字符串校验：轨索引一显示为 K2。
    // 总计文本带 TOTAL 前缀，与逐轨文本有不同的身份标记。
    using System = MMM::Logic::System::CanvasComponentRenderSystem;
    return std::string(System::formatTrackKps(1, 4U).data()) == "K2 4 KPS" &&
           std::string(System::formatTotalKps(12U).data()) == "TOTAL 12 KPS";
}

/// @brief 验证轨道宽度不足时依次隐藏轨道编号与 KPS 后缀。
/// @return 三条轨道分别使用完整、无编号和纯数值文本时返回 true。
/// @note 轨宽相同而字号递增，分别触发完整文本、去轨号和纯数值级别。
/// @note 每条轨使用不同数值，期望字符串显式指定，避免只按长度猜测级别。
/// @note 宽度比较建立在固定测试字体上，不依赖真实字符的位图形状。
/// @note 总 KPS 可能同时生成，查找时必须同时匹配类型与轨道索引。
/// @note 期望文本通过宽度间接验证，不检查每个字形的实际图像。
/// @note 相同内容宽度的任意其他字符串不在本例辨识范围内。
bool testKpsTextAdaptsToTrackWidth()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    snapshot.hasBeatmap = true;
    snapshot.isPlaying  = true;
    config.kps.visible  = true;

    constexpr std::int32_t trackCount = 3;
    constexpr float        trackLeft  = 0.2f;
    constexpr float        trackRight = 0.8f;
    // 保持同一轨宽，按三档字号增加文本占用宽度。
    // 这样紧凑级别变化来自真实可容纳宽度，而非不同组件开关。
    const std::array<float, trackCount> fontSizeRatios{ 0.035f, 0.06f, 0.1f };
    // 每条轨单独创建字号覆盖，防止最后一次赋值覆盖整组配置。
    // 其他布局参数沿用同一玩家区域。
    for ( std::int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex ) {
        auto& placement =
            config.editablePlacement(MMM::Config::CanvasComponentType::Kps,
                                     trackIndex,
                                     trackCount,
                                     trackLeft,
                                     trackRight);
        placement.fontSizeRatio =
            fontSizeRatios[static_cast<std::size_t>(trackIndex)];
    }

    const std::array<std::uint32_t, trackCount> kps{ 1U, 2U, 3U };
    auto context       = makeRenderContext(1.0);
    context.trackCount = trackCount;
    context.trackLeft  = trackLeft;
    context.trackRight = trackRight;
    // 计数数组与轨数严格对应，避免缺少某轨数据触发回退文本。
    // 数组在同步渲染调用之后才离开作用域。
    context.trackKps = kps;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    // 期望依次去掉轨号与后缀，但必须保留实际 KPS 数值。
    // 固定字符串与被测选择策略分开，避免用同一分支生成期望。
    const std::array<const char*, trackCount> expectedTexts{ "K1 1 KPS",
                                                             "2 KPS",
                                                             "3" };
    constexpr float                           epsilon = 1e-4f;
    for ( std::int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex ) {
        const auto instance =
            std::find_if(snapshot.canvasComponentInstances.begin(),
                         snapshot.canvasComponentInstances.end(),
                         [trackIndex](const auto& candidate) {
                             return candidate.type ==
                                        MMM::Config::CanvasComponentType::Kps &&
                                    candidate.instanceIndex == trackIndex;
                         });
        // 每一条轨都必须生成自己的实例，不能以总 KPS 替代。
        // 失败日志输出轨号，便于区分哪个紧凑级别没有产生文本。
        if ( instance == snapshot.canvasComponentInstances.end() ) {
            XERROR("Adaptive KPS text did not render track {}", trackIndex);
            return false;
        }

        // 每轨按其覆盖字号选择字体并测量期望字符串。
        // 不能用统一字号比较，否则可能把字号变化误判成紧凑文本变化。
        const float fontPixelHeight =
            fontSizeRatios[static_cast<std::size_t>(trackIndex)] *
            context.viewportHeight;
        const auto selection = MMM::Common::selectAsciiFont(
            snapshot.asciiFontAtlasMetrics, fontPixelHeight);
        // 不同字号都借用同一已注入层级的归一化度量。
        // 字号缩放仍由选择结果和像素高度共同决定，不能跳过度量。
        if ( !selection ) {
            XERROR("Adaptive KPS text did not select an ASCII font");
            return false;
        }
        // 固定宽度字体让三个紧凑级别具有可区分的内容宽度。
        // 读取实例包围盒验证实际选中的文本级别。
        const float expectedWidth =
            MMM::Common::measureAsciiText(
                *selection.metrics,
                expectedTexts[static_cast<std::size_t>(trackIndex)],
                fontPixelHeight)
                .width;
        // 比较内容宽度而非布局区域宽度，后者对三个紧凑级别都相同。
        // 容差只吸收浮点舍入，不允许误选另一个字符串长度。
        if ( std::abs((instance->right - instance->left) - expectedWidth) >
             epsilon ) {
            XERROR("Adaptive KPS text selected the wrong compact level");
            return false;
        }
    }
    return true;
}

/// @brief 验证 HitEffect 消费事件形成逐轨一秒滚动 KPS。
/// @return 新事件、过期事件和清空路径的计数均正确时返回 true。
/// @note 显式关闭视觉打击特效，但保留 KPS，验证统计不依赖特效显示。
/// @note 时间由参数逐次推进，不等待真实一秒或启动播放线程。
/// @note 带轨差的 Flick 计入终点轨，其余点击保留起始轨。
/// @note 清空应保留已配置轨数并把计数归零，不能留下过期事件。
bool testHitEffectKpsRollingWindow()
{
    using HitSystem = MMM::Logic::System::HitFXSystem;
    // 事件按显式时间与轨号构造，默认不携带轨差。
    // 后面只对一个 Flick 改轨差，隔离终点轨统计行为。
    /// @brief 创建用于 KPS 窗口验证的普通点击事件。
    /// @param timestamp 事件发生时间，单位秒。
    /// @param trackIndex 零基玩家轨号。
    /// @return 不带额外轨差的值事件。
    const auto makeEvent = [](double timestamp, int trackIndex) {
        return HitSystem::HitEvent{ timestamp,
                                    MMM::NoteType::NOTE,
                                    HitSystem::HitEvent::Role::None,
                                    1,
                                    trackIndex,
                                    0,
                                    0.0,
                                    false };
    };

    HitSystem                 system;
    MMM::Config::EditorConfig config;
    // 关闭特效但开启 KPS，避免视觉效果开关意外禁用统计。
    // 整个测试仅调用 update，不等待图形或音频线程。
    config.visual.enableHitEffects             = false;
    config.visual.canvasComponents.kps.visible = true;
    auto offsetEvent                           = makeEvent(0.8, 0);
    offsetEvent.type                           = MMM::NoteType::FLICK;
    // Flick 从轨零跨到轨二，计数应跟随实际打击终点。
    // 如果只按根轨累计，首轮会错误得到轨零三个事件。
    offsetEvent.trackOffset = 2;
    // 初次窗口包含两个起轨点击和一个落到轨二的 Flick。
    // 不同时间事件仍按当前一秒窗口统计，不要求时间恰好等于 update。
    system.update(
        1.0, { makeEvent(0.4, 0), makeEvent(1.0, 0), offsetEvent }, 3, config);
    // 每次更新后重新取得统计视图，不跨修改长期保留旧访问结果。
    // 首轮先检查长度再读三条轨，防止空统计容器造成越界。
    auto kps = system.trackKps();
    if ( kps.size() != 3U || kps[0] != 2U || kps[1] != 0U || kps[2] != 1U ) {
        XERROR("Initial per-track KPS counts were incorrect");
        return false;
    }

    // 时间推进后 0.4 秒事件过期，0.8 与 1.0 秒事件仍在窗口内。
    // 新增轨一事件验证过期清理与新事件累计能在同次更新中完成。
    system.update(1.5, { makeEvent(1.5, 1) }, 3, config);
    kps = system.trackKps();
    // 此时三个轨各剩一个事件，能同时检查旧事件保留和新轨累计。
    // 轨零若仍为二，说明过期删除没有推进；轨二缺失则可能丢了轨差事件。
    if ( kps[0] != 1U || kps[1] != 1U || kps[2] != 1U ) {
        XERROR("KPS rolling window did not expire the oldest event");
        return false;
    }

    // 空输入也必须推进过期清理，不能仅在有新事件时更新计数。
    // 选择越过边界的时间，避免把精确一秒的包含规则混入本例。
    system.update(2.01, {}, 3, config);
    kps = system.trackKps();
    // 2.01 秒时只有 1.5 秒的轨一事件仍在最近一秒内。
    // 该轮无新输入，证明统计窗口不是仅由新事件驱动。
    if ( kps[0] != 0U || kps[1] != 1U || kps[2] != 0U ) {
        XERROR("KPS rolling window did not retain only the last second");
        return false;
    }

    // 清空同时重置统计历史，常用于播放状态切换或跳转后的复位。
    // 保留三条轨道的输出形状，方便消费端继续按原索引访问。
    system.clearActiveEffects();
    kps = system.trackKps();
    // 清空后继续暴露三条零值轨道，而不是缩成空数组。
    // 这种形状稳定性让 UI 无需因复位重新构造逐轨组件。
    return kps.size() == 3U && kps[0] == 0U && kps[1] == 0U && kps[2] == 0U;
}

/// @brief 验证判定线时间的固定精度格式与负值处理。
/// @return 常规、负值和非有限输入均符合约定时返回 true。
/// @note 固定字符串比较覆盖小时、毫秒补零和负号，不受区域设置影响。
/// @note 非有限时间回退零；负拍号也以零表示。
/// @note 不把时间单位转换与格式化混在一起，输入始终为秒。
/// @note 通过立即复制返回缓冲构造字符串，不保留临时缓冲的 data 指针。
/// @note 拍号前缀与时间格式分别比较，不把两种显示协议混用。
bool testJudgmentLineTimeFormatting()
{
    using System = MMM::Logic::System::CanvasComponentRenderSystem;
    // 正值覆盖跨小时进位，负值覆盖符号与毫秒尾零。
    // NaN 不得流入时间整数换算，约定回退为零时间文本。
    return std::string(System::formatJudgmentLineTime(3723.456).data()) ==
               "01:02:03.456" &&
           std::string(System::formatJudgmentLineTime(-1.25).data()) ==
               "-00:00:01.250" &&
           std::string(System::formatJudgmentLineTime(
                           std::numeric_limits<double>::quiet_NaN())
                           .data()) == "00:00:00.000" &&
           // 多位拍号不能截断，负数则按显示协议钳制为零。
           // 这与负时间保留负号的规则不同，不能共用同一格式化分支。
           std::string(System::formatBeatNumber(12345).data()) == "#12345" &&
           std::string(System::formatBeatNumber(-5).data()) == "#0";
}

}  // namespace

/// @brief 运行画布组件最终覆盖层回归测试。
/// @return 全部测试通过时返回 0。
/// @note 按布尔短路报告首个失败，CTest 依据退出码判定本组结果。
/// @note 所有数据在内存中构造，不写入测试资源目录。
int main()
{
    // 先验证字体基础与可见开关，再验证拍位布局和 KPS。
    // 布尔链保留首次失败，详细原因由各用例日志提供。
    return testDpiAwareAsciiFontTierSelection() &&
                   testHiddenComponentDoesNotRender() &&
                   testVisibleComponentRendersInOverlay() &&
                   testJudgmentLineTimeDoesNotRenderWithoutBeatmap() &&
                   testBeatNumbersRenderInsideEachBeat() &&
                   testDenseBpmMarkersAdvanceBeatNumbers() &&
                   testBeatNumberRendersUntilLayoutViewportExit() &&
                   testBeatNumberLayoutRegionCentersOnBeatHead() &&
                   testBeatLineTimesRenderInsideEachSubdivision() &&
                   testKpsRendersPerTrackAndTotal() &&
                   testKpsTextAdaptsToTrackWidth() &&
                   testHitEffectKpsRollingWindow() &&
                   testJudgmentLineTimeFormatting()
               ? 0
               : 1;
}
