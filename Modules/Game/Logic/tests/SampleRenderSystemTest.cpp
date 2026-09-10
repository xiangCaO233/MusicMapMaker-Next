#include "logic/ecs/system/SampleRenderSystem.h"

#include "common/AsciiFontData.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// @brief 使用小容差比较纹理坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个坐标足够接近时返回 true。
/// @note 容差同时覆盖 UV 和像素位置的浮点运算误差。
/// @note 这里使用绝对误差，测试尺寸固定，不用于任意尺度的生产比较。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 测试用 Note 图集矩形，后两项为宽高而非右下角坐标。
/// @note 选择非正方形区域，使纵向尺寸公式遗漏纹理宽高比时能够被发现。
constexpr glm::vec4 NOTE_UV{ 0.25F, 0.35F, 0.2F, 0.1F };

/// @brief 为采样标签测试注入固定宽度 ASCII 字体度量和字形 UV。
/// @param snapshot 待初始化快照。
/// @note 所有字形使用确定的度量，不加载外部字体或依赖机器字体配置。
/// @note 每个可绘制字形拥有不同的 U 起点，测试据此在顶点中识别字符。
/// @note 空格只推进排版游标，不应产生字形四边形。
/// @note 注入的度量和 UV 必须使用同一字号层级。
void configureAsciiFont(MMM::Logic::RenderSnapshot& snapshot)
{
    // 选定固定字号层，避免字体层级选择受机器实际字体影响。
    // 该层必须同时写入度量和纹理映射，否则生成器会认为字形缺失。
    constexpr std::size_t tierIndex = 3U;
    auto&                 atlas     = snapshot.asciiFontAtlasMetrics;
    atlas.valid                     = true;
    auto& font                      = atlas.tiers[tierIndex];
    font.valid                      = true;
    // 度量按字号归一化，测试稍后由字号推导像素高度。
    // 上升部与位图高度分开设置，使基线定位仍经过正常排版逻辑。
    font.ascender   = 0.8F;
    font.lineHeight = 1.0F;
    for ( std::uint32_t code = MMM::Common::ASCII_GLYPH_FIRST;
          code <= MMM::Common::ASCII_GLYPH_LAST;
          ++code ) {
        auto& glyph     = font.glyphs[code - MMM::Common::ASCII_GLYPH_FIRST];
        glyph.available = true;
        // 空格保留 advance，但不提供位图。
        // 这样包含空格的长名称不会被额外可见四边形改变裁剪断言。
        glyph.hasBitmap = code != static_cast<std::uint32_t>(' ');
        glyph.width     = 0.5F;
        glyph.height    = 0.75F;
        glyph.bearingX  = 0.0F;
        glyph.bearingY  = 0.75F;
        glyph.advanceX  = 0.6F;
        if ( glyph.hasBitmap ) {
            const auto textureId = MMM::Logic::asciiGlyphTextureId(
                tierIndex, static_cast<char>(code));
            // 用字符编码构造互不相同的 U 起点。
            // 检查标题时可以识别目标字符，不必依赖其他字符的顶点数量。
            const float glyphU =
                0.6F +
                static_cast<float>(code - MMM::Common::ASCII_GLYPH_FIRST) *
                    0.001F;
            snapshot.uvMap.emplace(static_cast<std::uint32_t>(textureId),
                                   glm::vec4{ glyphU, 0.7F, 0.0008F, 0.01F });
        }
    }
}

/// @brief 为采样标签测试注入“初音”两个 CJK 字形及其 UV。
/// @param snapshot 待初始化快照。
/// @note 码点数组按升序提供，保持 Unicode 度量查询所需的顺序。
/// @note 只预装测试名称中的两个汉字，其他 ASCII 字符由另一个助手注入。
/// @note 字形使用不同于 ASCII 和 Note 的 UV 区域，避免误认本体顶点。
/// @note 仅提供内存字形，不请求字体服务或修改实际图集。
void configureUnicodeFont(MMM::Logic::RenderSnapshot& snapshot)
{
    // 测试直接提供 Unicode 标量值，避免预装阶段也使用待验证的 UTF-8 拆解。
    // 两个码点顺序与度量容器查询约定一致。
    constexpr std::array<std::uint32_t, 2> codepoints{ 0x521DU, 0x97F3U };
    auto& unicodeFont      = snapshot.unicodeFontMetrics;
    unicodeFont.valid      = true;
    unicodeFont.ascender   = 0.88F;
    unicodeFont.lineHeight = 1.0F;
    for ( std::size_t index = 0U; index < codepoints.size(); ++index ) {
        MMM::Common::UnicodeGlyphMetrics entry;
        entry.codepoint         = codepoints[index];
        entry.metrics.available = true;
        entry.metrics.hasBitmap = true;
        entry.metrics.width     = 0.9F;
        entry.metrics.height    = 0.9F;
        entry.metrics.bearingX  = 0.0F;
        entry.metrics.bearingY  = 0.85F;
        // 全角字形的步进与 ASCII 不同，可区分两种度量来源。
        // 度量都为正值，避免空位图回退隐藏 Unicode 绘制问题。
        entry.metrics.advanceX = 1.0F;
        const auto textureId =
            MMM::Logic::unicodeGlyphTextureId(entry.codepoint);
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(textureId),
            glm::vec4{ 0.82F + static_cast<float>(index) * 0.01F,
                       0.84F,
                       0.008F,
                       0.012F });
        // 度量与 UV 一同加入，模拟图集已经完成加载的状态。
        // 只注册其中之一会变成缺字案例，不能证明已有字形被复用。
        unicodeFont.glyphs.push_back(entry);
    }
}

/// @brief 验证 BGM 轨道底色纹理与标题字号。
/// @return 底色未复用标签 UV 且标题按 16 像素字号绘制时返回 true。
/// @note 先通过屏幕边界定位底色四边形，再检查纹理，避免把标题当作底色。
/// @note 标题高度从字形度量推导，不依赖真实字体的栅格化结果。
/// @note 测试关注提交给渲染器的 CPU 几何，不需要 GPU 或窗口。
/// @note 每条 BGM 轨均须有可识别底色，不能只检查总顶点数。
bool testLaneLayoutVisuals()
{
    MMM::Logic::RenderSnapshot snapshot;
    constexpr glm::vec4        solidUv{ 0.1F, 0.2F, 0.04F, 0.06F };
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None), solidUv);
    configureAsciiFont(snapshot);

    // 布局保持在视口内部，使底色缺失不能归因于离屏裁剪。
    // 持久 BGM 轨数量决定需要逐条验证的底色范围。
    constexpr float        viewportWidth           = 800.0F;
    constexpr float        topY                    = 10.0F;
    constexpr float        bottomY                 = 590.0F;
    constexpr std::int32_t persistentBgmTrackCount = 3;
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        viewportWidth, 4, persistentBgmTrackCount, 0.1F, 0.5F, 0.0F);
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderLaneLayout(
        batcher,
        projection,
        0,
        persistentBgmTrackCount,
        viewportWidth,
        topY,
        bottomY);
    // 底色和标题可能使用不同纹理批次，检查前统一完成提交。
    // 纯色 UV 断言针对实际生成顶点，不依赖批处理器内部的当前纹理。
    batcher.flush();

    // 底色应取纯色图块中心，四角 UV 都相同。
    // 使用标签最后一次绑定的 UV 会在这里暴露，而非依赖肉眼观察乱码。
    const float expectedU = solidUv.x + solidUv.z * 0.5F;
    const float expectedV = solidUv.y + solidUv.w * 0.5F;
    for ( std::uint32_t laneIndex = 0U; laneIndex < projection.bgmLaneCount;
          ++laneIndex ) {
        const auto bounds =
            projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, laneIndex });
        if ( !bounds ) {
            XERROR("Unable to resolve BGM lane {} bounds", laneIndex);
            return false;
        }

        // 按每条轨道的完整矩形筛选候选，跳过标题字形与其他装饰。
        // 四边形打包是本用例的几何前提，所以逐四顶点检查。
        bool foundBackground = false;
        for ( std::size_t vertexIndex = 0U;
              vertexIndex + 3U < snapshot.vertices.size();
              vertexIndex += 4U ) {
            const auto* quad = snapshot.vertices.data() + vertexIndex;
            if ( !near(quad[0].pos.x, bounds->leftX) ||
                 !near(quad[0].pos.y, bottomY) ||
                 !near(quad[1].pos.x, bounds->rightX) ||
                 !near(quad[1].pos.y, bottomY) ||
                 !near(quad[2].pos.x, bounds->rightX) ||
                 !near(quad[2].pos.y, topY) ||
                 !near(quad[3].pos.x, bounds->leftX) ||
                 !near(quad[3].pos.y, topY) ) {
                continue;
            }

            // 只有几何与轨道边界匹配后，才检查该底色的纹理坐标。
            // 避免一个使用纯色 UV 的无关图元错误满足断言。
            foundBackground = true;
            // 纯色区域不应沿矩形插值到其他纹理，四个角都要采样同一中心点。
            // 只检查首角会遗漏其余角仍残留标签 UV 的批处理状态错误。
            for ( std::size_t corner = 0U; corner < 4U; ++corner ) {
                if ( !near(quad[corner].uv.u, expectedU) ||
                     !near(quad[corner].uv.v, expectedV) ) {
                    XERROR("BGM lane {} background reused a label glyph UV",
                           laneIndex);
                    return false;
                }
            }
            break;
        }
        // 没有匹配矩形也必须失败，不能让空顶点集合绕过内部 UV 检查。
        // 日志保留轨号，便于区分单条轨道缺失与整体渲染失败。
        if ( !foundBackground ) {
            XERROR("BGM lane {} background quad was not generated", laneIndex);
            return false;
        }
    }

    // ASCII 字形被安排在 U 大于等于 0.6 的区域。
    // 过滤掉底色后统计高度，避免视口高度掩盖标题字号回归。
    float maxLabelGlyphHeight = 0.0F;
    for ( std::size_t vertexIndex = 0U;
          vertexIndex + 3U < snapshot.vertices.size();
          vertexIndex += 4U ) {
        const auto* quad = snapshot.vertices.data() + vertexIndex;
        if ( quad[0].uv.u < 0.6F ) continue;
        maxLabelGlyphHeight = std::max(maxLabelGlyphHeight,
                                       std::abs(quad[0].pos.y - quad[3].pos.y));
    }
    // 测试字形高度为字号的 0.75，16 像素字号应生成 12 像素字形。
    if ( !near(maxLabelGlyphHeight, 12.0F) ) {
        XERROR("BGM lane label font size is too small: glyph height={}",
               maxLabelGlyphHeight);
        return false;
    }
    return true;
}

/// @brief 验证草稿轨道标题按追加轨及持久轨顺序绘制。
/// @return 可见标题依次为 DRAFT +、DRAFT 1 与 DRAFT 2 时返回 true。
/// @note 追加轨与持久轨共享布局，但追加轨标题使用加号而非编号。
/// @note 水平平移让草稿轨出现在测试视口中，避免不可见导致测试空通过。
/// @note 只选择能够区分三条轨道的字符，不要求标题的全部顶点顺序。
/// @note 每个标识在对应轨道中恰好出现一次，兼顾缺失和重复绘制。
bool testDraftLaneLabels()
{
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.1F, 0.2F, 0.04F, 0.06F });
    configureAsciiFont(snapshot);

    constexpr float viewportWidth = 800.0F;
    const auto      projection    = MMM::Logic::calculateCanvasLaneProjection(
        viewportWidth, 4, 0, 0.5F, 0.9F, 300.0F, true, false, true, 2, true);
    // 先验证测试确实展示三条草稿轨，再检查各轨标题。
    // 否则错误的平移参数可能让后续检查只看到追加轨。
    const auto visibleRange = projection.visibleDraftRange(0.0F, viewportWidth);
    if ( !visibleRange || visibleRange->first != 0U ||
         visibleRange->second != 3U ) {
        XERROR("Draft lanes were not visible for label rendering");
        return false;
    }

    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderLaneLayout(
        batcher, projection, 2, 0, viewportWidth, 10.0F, 590.0F);
    batcher.flush();

    // 字符 UV 与轨道横向范围同时匹配，避免在相邻标题中误认编号。
    // 计数而非存在性检查还能发现标题重复提交。
    /// @brief 判断指定草稿轨中是否恰好绘制一次目标字符。
    /// @param glyph 用于识别标题的 ASCII 字符。
    /// @param laneIndex 草稿轨在布局中的序号。
    /// @return 轨道有效且找到唯一匹配字形时返回 true。
    const auto hasGlyphAt = [&](char glyph, std::uint32_t laneIndex) {
        const auto bounds =
            projection.bounds({ MMM::Logic::CanvasLaneKind::Draft, laneIndex });
        // 轨道边界不存在时不能继续扫描其他轨道来满足字符断言。
        // 该失败将通过外层布尔链传递到 CTest 退出码。
        if ( !bounds ) return false;
        const float expectedU =
            0.6F +
            static_cast<float>(glyph - MMM::Common::ASCII_GLYPH_FIRST) * 0.001F;
        std::uint32_t count = 0U;
        for ( std::size_t vertexIndex = 0U;
              vertexIndex + 3U < snapshot.vertices.size();
              vertexIndex += 4U ) {
            const auto* quad = snapshot.vertices.data() + vertexIndex;
            if ( near(quad[0].uv.u, expectedU) &&
                 quad[0].pos.x >= bounds->leftX &&
                 quad[0].pos.x < bounds->rightX ) {
                ++count;
            }
        }
        // 同轨重复标题即使文字正确也算失败。
        // 追加轨与编号轨的字符分别检查，避免布局索引整体错位。
        return count == 1U;
    };

    // 追加轨位于持久草稿轨之前，编号从其后的第一条持久轨开始。
    // 只比对区分性字符，标题其他部分的排版变更不应影响这个顺序断言。
    return hasGlyphAt('+', 0U) && hasGlyphAt('1', 1U) && hasGlyphAt('2', 2U);
}

/// @brief 验证窄 BGM 轨道标题会在轨道范围内自动滚动。
/// @return 同一字形随单调时钟左移且始终受轨道宽度裁剪时返回 true。
/// @note 两个确定时钟值覆盖暂停与滚动阶段，无需等待真实时间。
/// @note 以同一个字符的左边界作比较，避免不同字符宽度影响结论。
/// @note 返回空值同时表达字符缺失和裁剪越界，两者都应使测试失败。
/// @note 该测试不覆盖 GPU 裁剪，要求生成时几何已经被限制在轨道内。
bool testNarrowLaneLabelMarquee()
{
    constexpr float viewportWidth = 800.0F;
    const auto      projection    = MMM::Logic::calculateCanvasLaneProjection(
        viewportWidth, 4, 3, 0.1F, 0.3F, 0.0F);
    // 取第一条 BGM 轨作为窄标题视口，其他轨的相同字符不能参与测量。
    // 轨道存在性先检查，避免对失效的可选边界解引用。
    const auto firstLane =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 0U });
    if ( !firstLane ) {
        XERROR("Unable to resolve narrow BGM lane bounds");
        return false;
    }

    // 直接注入单调时间，测试执行耗时不参与滚动相位。
    // 每次使用独立快照，确保变化来自时钟而非累积的旧顶点。
    /// @brief 在指定时刻生成窄轨标题并读取 G 字形左边界。
    /// @param monotonicSeconds 注入快照的单调时钟，单位秒。
    /// @return 裁剪合法时返回位置，否则返回空值。
    const auto renderGlyphLeft =
        [&](double monotonicSeconds) -> std::optional<float> {
        MMM::Logic::RenderSnapshot snapshot;
        // 模拟两帧时钟即可验证滚动，不引入 sleep 或真实时间等待。
        // 同一投影与字体保持不变，字符位置才具有可比性。
        snapshot.snapshotSysTime = monotonicSeconds;
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
            glm::vec4{ 0.1F, 0.2F, 0.04F, 0.06F });
        configureAsciiFont(snapshot);

        MMM::Logic::System::Batcher batcher(&snapshot);
        MMM::Logic::System::SampleRenderSystem::renderLaneLayout(
            batcher, projection, 0, 3, firstLane->rightX, 10.0F, 590.0F);
        batcher.flush();

        // 选择标题内部的 G，避免首字符在裁剪边界附近完全消失。
        // U 起点由注入字体的同一编码映射计算，但位置来自实际渲染结果。
        constexpr float glyphU =
            0.6F +
            static_cast<float>('G' - MMM::Common::ASCII_GLYPH_FIRST) * 0.001F;
        for ( std::size_t vertexIndex = 0U;
              vertexIndex + 3U < snapshot.vertices.size();
              vertexIndex += 4U ) {
            const auto* quad = snapshot.vertices.data() + vertexIndex;
            if ( !near(quad[0].uv.u, glyphU) ) continue;
            const float glyphLeft  = quad[0].pos.x;
            const float glyphRight = quad[1].pos.x;
            // 标题两侧各留四像素内边距。
            // 检查整个字形宽度而非只有左端点，能发现右侧仍越界的部分裁剪。
            if ( glyphLeft < firstLane->leftX + 4.0F ||
                 glyphRight > firstLane->rightX - 4.0F ) {
                XERROR("Scrolling BGM lane label escaped its lane");
                return std::nullopt;
            }
            return glyphLeft;
        }
        // 未找到可测量字符同样使外层测试失败。
        // 不能用轨道左边界替代缺失字形，否则两时刻可能得到虚假位置。
        return std::nullopt;
    };

    // 两个时刻落在同一滚动周期的不同阶段，预期是向左推进。
    // 无需证明所有周期行为；这里只验证时钟驱动与基本裁剪契约。
    const auto pausedX   = renderGlyphLeft(1.0);
    const auto scrolledX = renderGlyphLeft(1.5);
    if ( !pausedX || !scrolledX || !(*scrolledX < *pausedX) ) {
        XERROR("Narrow BGM lane label did not scroll with the monotonic clock");
        return false;
    }
    return true;
}

/// @brief 返回自动采样测试沿用的兼容玩家轨道布局。
/// @note 保留旧式主轨比例，供没有指定独立 BGM 布局的用例复用。
/// @note 返回值归调用者所有，不共享会被其他测试修改的全局配置。
MMM::Config::TrackLayout defaultSampleTrackLayout()
{
    MMM::Config::TrackLayout layout;
    // 玩家区占视口的固定比例，BGM 区沿用兼容布局推导。
    // 独立 BGM 位置和宽度的用例会显式覆盖这一默认布局。
    layout.left  = 0.1F;
    layout.right = 0.5F;
    return layout;
}

/// @brief 为单个零 offset 自动采样生成测试快照。
/// @param snapshot 输出快照。
/// @param resourceId 音频资源 ID。
/// @param snapshotSysTime 单调时钟秒数。
/// @param withAsciiFont 是否注入标签字体。
/// @param noteScaleX 物件横向缩放。
/// @param noteScaleY 物件纵向缩放。
/// @param erasing 是否启用待删除预览。
/// @param hovered 是否启用悬浮状态。
/// @param selected 是否启用选中状态。
/// @note 输出快照由调用方新建；允许提前注入 Unicode 度量。
/// @note 临时 Registry、索引和滚动缓存均活到 renderSamples 返回之后。
/// @note 资源 ID 只用作标签文本，不读取或解码同名音频文件。
/// @note 四条玩家轨后的统一轨号 4 对应第一条 BGM 轨。
/// @note 本助手固定零偏移，使标签测试不混入触发连线的几何。
/// @param layout 玩家区及 BGM 区的布局配置。
/// @note 判定线固定在六百像素视口中点，使零秒采样完整可见。
/// @note renderSamples 返回后快照保存值数据，局部 Registry 随助手退出销毁。
void renderSingleSample(
    MMM::Logic::RenderSnapshot& snapshot, std::string_view resourceId,
    double snapshotSysTime, bool withAsciiFont, float noteScaleX = 1.2F,
    float noteScaleY = 1.2F, bool erasing = false, bool hovered = false,
    bool                            selected = false,
    const MMM::Config::TrackLayout& layout   = defaultSampleTrackLayout())
{
    // 构造独立 BPM 时间线，让 ScrollCache 使用真实的时间映射入口。
    // 不传谱面指针，因为这一案例只需固定速度的零秒位置。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    // 把用例缩放显式写入配置，避免依赖磁盘上的编辑器设置。
    // 横纵两轴独立赋值，使后续测试能控制单一变量。
    config.visual.noteScaleX = noteScaleX;
    config.visual.noteScaleY = noteScaleY;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(timelineRegistry, config, nullptr);

    // 采样与 Timing 分开建 Registry，保持生产路径的对象域边界。
    // 资源名称复制进组件，渲染期间不借用 string_view 背后的字符缓冲。
    entt::registry sampleRegistry;
    const auto     sampleEntity = sampleRegistry.create();
    sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 0.0,
            .m_track           = 4,
            .m_audioResourceId = std::string(resourceId),
        });
    // 交互标志写入实际组件，以覆盖渲染器读取 ECS 的正常分支。
    // 不能直接给快照塞发光命令，否则会绕过被测反馈逻辑。
    sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity,
        MMM::Logic::InteractionComponent{
            .isHovered  = hovered,
            .isSelected = selected,
        });
    // 单实体索引天然按时间排序，零偏移的结束前缀与锚点相同。
    // 两份索引在本次调用内存活，渲染器只借用它们做候选裁剪。
    const std::vector<entt::entity> sortedEntities{ sampleEntity };
    const std::vector<double>       maxEndPrefix{ 0.0 };
    // 删除反馈通过快照目标集合传递，而不是提前删掉测试实体。
    // 对象种类也必须标为采样，以免同号音符实体的反馈被错误复用。
    if ( erasing ) {
        snapshot.erasingObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
        snapshot.erasingEntities.insert(sampleEntity);
    }

    snapshot.snapshotSysTime = snapshotSysTime;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note), NOTE_UV);
    // 关闭字体的用例只检查本体，避免把标签顶点误当额外装饰。
    // 已经预装的 Unicode 度量不会被 ASCII 助手覆盖。
    if ( withAsciiFont ) configureAsciiFont(snapshot);

    // 使用四玩家轨、一 BGM 轨，使组件统一轨号与布局相符。
    // 传入完整 TrackLayout，独立 BGM 宽度案例才能覆盖同一渲染入口。
    const auto projection =
        MMM::Logic::calculateCanvasLaneProjection(800.0F, 4, 1, layout, 0.0F);
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderSamples(sampleRegistry,
                                                          sortedEntities,
                                                          maxEndPrefix,
                                                          &snapshot,
                                                          batcher,
                                                          projection,
                                                          &cache,
                                                          config,
                                                          0.0,
                                                          300.0F,
                                                          800.0F,
                                                          0.0F,
                                                          600.0F,
                                                          1.0F);
    // 把尚未提交的本体或标签批次写入快照再返回。
    // 断言依赖最终几何与命令，不能在批处理器析构前漏掉末批。
    batcher.flush();
}

/// @brief 为 BGM 区画笔预览生成不含正式采样的快照。
/// @param snapshot 输出快照。
/// @note 保持正式采样 Registry 为空，以证明预览直接来自工具状态。
/// @note 画笔快照不应注册命中框，否则未提交对象会参与拾取。
/// @note 固定时间与判定线，让预览位于可见范围而非被剔除。
/// @note 输出快照须由调用方初始化，助手不负责清空已有几何。
void renderSampleBrushPreview(MMM::Logic::RenderSnapshot& snapshot)
{
    // 预览也走正常滚动映射，不用手工拼接屏幕顶点替代。
    // 固定 BPM 保证零秒画笔与传入的判定线位置一致。
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig       config;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(timelineRegistry, config, nullptr);

    entt::registry sampleRegistry;
    // 只设置画笔状态，不向采样 Registry 添加正式对象。
    // 因此出现的几何必须来自瞬态预览分支。
    snapshot.brush.isActive           = true;
    snapshot.brush.createsAudioSample = true;
    snapshot.brush.time               = 0.0;
    // 统一轨号跨过四条玩家轨，落在第一条 BGM 轨。
    // 资源名称只是预览标签，不要求对应文件存在。
    snapshot.brush.track           = 4;
    snapshot.brush.audioResourceId = "preview.wav";
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note), NOTE_UV);

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    // 空排序索引与空前缀表是有意设置的，正式对象候选应为空。
    // 即便没有候选，激活画笔仍应生成自己的可见反馈。
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderSamples(sampleRegistry,
                                                          {},
                                                          {},
                                                          &snapshot,
                                                          batcher,
                                                          projection,
                                                          &cache,
                                                          config,
                                                          0.0,
                                                          300.0F,
                                                          800.0F,
                                                          0.0F,
                                                          600.0F,
                                                          1.0F);
    // 预览分支与正式物件共用批处理器，也必须显式提交末批。
    // 不借助下一次绘制触发隐式切批，测试只检查这一轮输出。
    batcher.flush();
}

/// @brief 验证自动采样本体与玩家 Tap 使用相同纹理和尺寸公式。
/// @return 采样本体的图集 UV、宽高与玩家 Tap 一致时返回 true。
/// @note 关闭字体以单独检查本体，零偏移也不应出现连接装饰。
/// @note UV 两个对角覆盖实际纹理区域，避免只检查纹理枚举而漏掉坐标错误。
/// @note 宽高由轨宽和纹理宽高比推导，不能直接复用被测函数的结果。
/// @note 颜色断言区分自动采样与玩家点击物件的样式。
/// @note 本例不覆盖非零偏移的锚点与触发点连线。
/// @note 尺寸验证使用 CPU 顶点包围盒，不依赖显示设备像素密度。
bool testSampleBodyMatchesTapTextureAndSize()
{
    MMM::Logic::RenderSnapshot snapshot;
    MMM::Config::EditorConfig  config;
    // 本用例验证当前默认尺寸，生成与期望值必须取自同一份默认配置。
    // 辅助函数的固定缩放仅服务于其他显式尺寸案例，不能混入这里。
    renderSingleSample(snapshot,
                       "sample.wav",
                       0.0,
                       false,
                       config.visual.noteScaleX,
                       config.visual.noteScaleY);
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);

    // 零偏移且无字体时，本体应恰好是一个四边形、两个三角形。
    // 先检查数量再访问 front，空结果应记录失败而非越界。
    if ( snapshot.vertices.size() != 4U || snapshot.indices.size() != 6U ) {
        XERROR("Zero-offset sample rendered decorations over its Note texture");
        return false;
    }

    // 同时验证 UV 区域与屏幕包围盒，分别约束纹理选择和尺寸。
    // 不假定纵向顶点顺序，允许渲染约定中的上下翻转。
    bool  foundNoteTopLeft     = false;
    bool  foundNoteBottomRight = false;
    float minX                 = snapshot.vertices.front().pos.x;
    float maxX                 = minX;
    float minY                 = snapshot.vertices.front().pos.y;
    float maxY                 = minY;
    for ( const auto& vertex : snapshot.vertices ) {
        foundNoteTopLeft = foundNoteTopLeft || (near(vertex.uv.u, NOTE_UV.x) &&
                                                near(vertex.uv.v, NOTE_UV.y));
        foundNoteBottomRight =
            foundNoteBottomRight || (near(vertex.uv.u, NOTE_UV.x + NOTE_UV.z) &&
                                     near(vertex.uv.v, NOTE_UV.y + NOTE_UV.w));
        minX = std::min(minX, vertex.pos.x);
        maxX = std::max(maxX, vertex.pos.x);
        minY = std::min(minY, vertex.pos.y);
        maxY = std::max(maxY, vertex.pos.y);
    }
    if ( !foundNoteTopLeft || !foundNoteBottomRight ) {
        XERROR("Sample body did not use the configured Note texture UV");
        return false;
    }

    // 兼容布局下采样尺寸应沿用玩家单轨宽度。
    // 纹理宽高比只影响高度换算，横向缩放不能重复进入纵向公式。
    const float laneWidth     = projection.player.singleTrackWidth;
    const float expectedWidth = laneWidth * config.visual.noteScaleX;
    const float expectedHeight =
        laneWidth / (NOTE_UV.z / NOTE_UV.w) * config.visual.noteScaleY;
    if ( !near(maxX - minX, expectedWidth) ||
         !near(maxY - minY, expectedHeight) ) {
        XERROR("Sample body size diverged from the player Tap size");
        return false;
    }
    // 本体是统一着色，检查一个顶点即可覆盖其样式入口。
    // BGM 蓝色与玩家物件区分，透明度也属于该反馈契约。
    const auto& sampleVertex = snapshot.vertices.front();
    if ( !near(sampleVertex.color.r, 0.36F) ||
         !near(sampleVertex.color.g, 0.72F) ||
         !near(sampleVertex.color.b, 0.92F) ||
         !near(sampleVertex.color.a, 0.96F) ) {
        XERROR("Sample body lost its distinct BGM object color");
        return false;
    }
    return true;
}

/// @brief 验证 BGM 采样本体跟随独立轨道 X 与宽度。
/// @return 本体在自定义 BGM 轨道内居中且尺寸按其宽度缩放时返回 true。
/// @note BGM 区与玩家区采用显著不同的宽度，以暴露错误沿用玩家轨宽。
/// @note 位置和宽度都必须符合 BGM 布局，仅尺寸正确不足以证明轨道选择正确。
/// @note 固定缩放只服务于该布局案例，不依赖用户配置文件。
bool testSampleUsesIndependentBgmLaneWidth()
{
    MMM::Config::TrackLayout layout;
    layout.left  = 0.1F;
    layout.right = 0.5F;
    // 显式把 BGM 区移到玩家区之外，避免错误布局恰好重合。
    // 五个百分点视口宽度可推导出独立的四十像素单轨宽。
    layout.bgmLanes.left  = 0.7F;
    layout.bgmLanes.width = 0.05F;
    MMM::Logic::RenderSnapshot snapshot;
    renderSingleSample(snapshot,
                       "independent.wav",
                       0.0,
                       false,
                       0.75F,
                       0.75F,
                       false,
                       false,
                       false,
                       layout);
    // 先保证本体存在，再读取前两个角点验证左边界和宽度。
    // 本例不启用字体，几何顺序不会被标题插入影响。
    if ( snapshot.vertices.size() < 4U ) return false;
    const float left  = snapshot.vertices[0].pos.x;
    const float right = snapshot.vertices[1].pos.x;
    // 800×0.05=40 像素单轨，0.75 横向缩放后本体宽 30 像素。
    return near(left, 565.0F) && near(right - left, 30.0F);
}

/// @brief 验证 BGM 画笔按下后立即绘制半透明自动采样并跟随笔刷位置。
/// @return 预览使用采样颜色、Note 纹理且不生成可拾取正式实体时返回 true。
/// @note 通过几何、索引、命中框三者联合约束瞬态预览。
/// @note 半透明度应在采样本色基础上降低，不换成普通玩家物件颜色。
/// @note 接受纵向 UV 的两种端点顺序，避免把翻转约定当成样式错误。
bool testSampleBrushPreview()
{
    MMM::Logic::RenderSnapshot snapshot;
    // 预览助手使用空 Registry，测试无需查找或销毁临时 ECS 实体。
    // 命中框必须为空，确保绘制反馈不会变成可编辑正式对象。
    renderSampleBrushPreview(snapshot);
    if ( snapshot.vertices.size() != 4U || snapshot.indices.size() != 6U ||
         !snapshot.hitboxes.empty() ) {
        XERROR("Sample brush preview did not render as one transient object");
        return false;
    }
    // 前面的数量校验保证此处有一个完整预览四边形。
    // 采样本色的 alpha 减半，以明确区分尚未提交与正式对象。
    const auto& vertex = snapshot.vertices.front();
    if ( !near(vertex.color.r, 0.36F) || !near(vertex.color.g, 0.72F) ||
         !near(vertex.color.b, 0.92F) || !near(vertex.color.a, 0.48F) ||
         !near(vertex.uv.u, NOTE_UV.x) ||
         (!near(vertex.uv.v, NOTE_UV.y) &&
          !near(vertex.uv.v, NOTE_UV.y + NOTE_UV.w)) ) {
        XERROR(
            "Sample brush preview lost styling: color=({:.3f},{:.3f},{:.3f},"
            "{:.3f}) uv=({:.3f},{:.3f})",
            vertex.color.r,
            vertex.color.g,
            vertex.color.b,
            vertex.color.a,
            vertex.uv.u,
            vertex.uv.v);
        return false;
    }
    return true;
}

/// @brief 验证右键按住自动采样时显示红色半透明待删除效果。
/// @return 采样本体切换为红色半透明且不影响纹理时返回 true。
/// @note 待删除只改变显示反馈，不要求实际删除 Registry 中的实体。
/// @note 关闭字体后四顶点约束可排除额外覆盖装饰。
/// @note 本案例校验删除色和透明度，纹理一致性由本体案例覆盖。
bool testSampleErasePreview()
{
    MMM::Logic::RenderSnapshot snapshot;
    // 显式开启 erasing，其他交互状态保持关闭。
    // 隔离删除反馈，防止悬浮发光改变预期顶点数量。
    renderSingleSample(snapshot, "erase.wav", 0.0, false, 1.2F, 1.2F, true);
    if ( snapshot.vertices.size() != 4U ) {
        XERROR("Sample erase preview rendered unexpected decorations");
        return false;
    }
    // 删除色采用红色半透明，与采样原本的蓝色反馈可明确区分。
    // 只检查状态色，不把资源名作为删除选择条件。
    const auto& vertex = snapshot.vertices.front();
    if ( !near(vertex.color.r, 1.0F) || !near(vertex.color.g, 0.2F) ||
         !near(vertex.color.b, 0.2F) || !near(vertex.color.a, 0.5F) ) {
        XERROR("Sample erase preview was not red and translucent");
        return false;
    }
    return true;
}

/// @brief 验证自动采样悬浮和选中状态沿用本体颜色并写入发光层。
/// @return 两种交互状态均只增加同色发光命令时返回 true。
/// @note 悬浮与选中分别构造，避免两标记同时打开掩盖其中一个失效。
/// @note 发光层命令与普通几何分开检查，不能只看到亮色顶点就视为成功。
/// @note 两份快照在循环检查期间均存活，循环只借用观察指针。
/// @note 本体与发光各保留一个四边形，因此共享 Note UV 的顶点应有八个。
/// @note 未同时启用删除反馈，本例不规定多种状态同时存在时的优先级。
/// @note 测试关注层与颜色，不验证 GPU 后处理产生的最终光晕。
bool testSampleInteractionGlow()
{
    MMM::Logic::RenderSnapshot hoveredSnapshot;
    MMM::Logic::RenderSnapshot selectedSnapshot;
    // 两次调用只分别开启悬浮或选中，使用相同本体尺寸和无字体条件。
    // 便于共用后面的发光断言，又能独立暴露任一状态的缺失。
    renderSingleSample(hoveredSnapshot,
                       "hovered.wav",
                       0.0,
                       false,
                       1.2F,
                       1.2F,
                       false,
                       true,
                       false);
    // 选中案例单独关闭悬浮，确认发光并非只由悬浮路径生成。
    // 相同尺寸确保后面的八顶点检查适用于两个独立状态。
    renderSingleSample(selectedSnapshot,
                       "selected.wav",
                       0.0,
                       false,
                       1.2F,
                       1.2F,
                       false,
                       false,
                       true);

    for ( const auto* snapshot : { &hoveredSnapshot, &selectedSnapshot } ) {
        // 发光应是一条六索引命令，独立于普通本体的提交。
        // 若只改变本体颜色而未生成发光层，这一检查会直接失败。
        if ( snapshot->glowCmds.size() != 1U ||
             snapshot->glowCmds.front().indexCount != 6U ) {
            XERROR("Interactive sample did not generate one body glow command");
            return false;
        }

        std::size_t noteVertexCount = 0U;
        for ( const auto& vertex : snapshot->vertices ) {
            // 按 Note 图块范围筛选本体与发光顶点，兼容其角点顺序。
            // 边界容差容纳浮点运算，避免图块边缘被误判为其他纹理。
            const bool usesNoteUv =
                vertex.uv.u >= NOTE_UV.x - 1e-4F &&
                vertex.uv.u <= NOTE_UV.x + NOTE_UV.z + 1e-4F &&
                vertex.uv.v >= NOTE_UV.y - 1e-4F &&
                vertex.uv.v <= NOTE_UV.y + NOTE_UV.w + 1e-4F;
            if ( !usesNoteUv ) continue;
            ++noteVertexCount;
            if ( !near(vertex.color.r, 0.36F) || !near(vertex.color.g, 0.72F) ||
                 !near(vertex.color.b, 0.92F) ||
                 !near(vertex.color.a, 0.96F) ) {
                XERROR(
                    "Interactive sample replaced its base color instead of "
                    "reusing it for glow");
                return false;
            }
        }
        // 四个本体顶点加四个发光顶点，颜色应保持同源。
        // 过多顶点可能代表重复提交，过少则说明某一层缺失。
        if ( noteVertexCount != 8U ) {
            XERROR(
                "Interactive sample base and glow geometry did not both use "
                "the Note texture");
            return false;
        }
    }
    return true;
}

/// @brief 判断两份快照的标签几何是否完全一致。
/// @param lhs 第一份快照。
/// @param rhs 第二份快照。
/// @return 忽略前四个物件本体顶点后，标签位置与 UV 均一致时返回 true。
/// @note 前四个顶点必须是本体，调用方不能传入带额外前置装饰的快照。
/// @note 至少需要一个标签顶点，两个空标签不能被视作有效相等。
/// @note 只比较位置和 UV，不用于证明颜色或绘制命令一致。
/// @note 顶点顺序是这两个快照的共同约定，不进行几何集合排序。
/// @note 调用期间仅借用快照，不修改其批次与图集缓存。
bool labelGeometryEqual(const MMM::Logic::RenderSnapshot& lhs,
                        const MMM::Logic::RenderSnapshot& rhs)
{
    // 顶点数量不一致即可证明标签几何不同，无需逐点比较。
    // 两份快照都没有标签时返回 false，防止静止测试空通过。
    if ( lhs.vertices.size() != rhs.vertices.size() ||
         lhs.vertices.size() <= 4U ) {
        return false;
    }
    // 跳过固定四顶点本体，让横向本体缩放不影响标签比较。
    // UV 也需一致，滚动裁剪可能保持边界位置却改变所见字形片段。
    for ( std::size_t index = 4U; index < lhs.vertices.size(); ++index ) {
        const auto& left  = lhs.vertices[index];
        const auto& right = rhs.vertices[index];
        if ( !near(left.pos.x, right.pos.x) || !near(left.pos.y, right.pos.y) ||
             !near(left.uv.u, right.uv.u) || !near(left.uv.v, right.uv.v) ) {
            return false;
        }
    }
    return true;
}

/// @brief 计算忽略物件本体后的标签字形几何高度。
/// @param snapshot 待检查快照。
/// @return 标签字形覆盖高度；没有标签几何时返回零。
/// @note 调用方保证前四个顶点属于本体，其后只包含标签几何。
/// @note 用所有标签顶点的上下包围范围计算高度，不依赖顶点方向。
/// @note 返回像素高度，比例测试可消除固定字体度量的影响。
/// @note 不计算字形基线或排版行高，只测量实际位图几何。
/// @note 空格没有位图，不会独立增大这里的垂直范围。
float labelGlyphHeight(const MMM::Logic::RenderSnapshot& snapshot)
{
    // 没有任何标签顶点时返回零，调用方据此报告字体或标签缺失。
    // 不使用本体高度填充默认值，以免纵向缩放测试得到假阳性。
    if ( snapshot.vertices.size() <= 4U ) return 0.0F;
    float minY = snapshot.vertices[4U].pos.y;
    float maxY = minY;
    for ( std::size_t index = 5U; index < snapshot.vertices.size(); ++index ) {
        minY = std::min(minY, snapshot.vertices[index].pos.y);
        maxY = std::max(maxY, snapshot.vertices[index].pos.y);
    }
    return maxY - minY;
}

/// @brief 验证短标签保持居中静止，长标签在轨道宽度内循环滚动。
/// @return 标签静止、滚动和 CPU 侧水平裁剪均符合预期时返回 true。
/// @note 短名称作为静止对照，长名称作为溢出滚动案例。
/// @note 使用相同布局和字体，只改变快照时钟，避免外部变量影响位置。
/// @note 长标签即使运动也必须保持 CPU 裁剪，不能只验证两帧不同。
/// @note 两时刻都要求存在字形，防止整段标签消失被误判为滚动。
bool testSampleLabelMarquee()
{
    MMM::Logic::RenderSnapshot shortStart;
    MMM::Logic::RenderSnapshot shortLater;
    // 短名称应完全容纳于轨道内，时钟推进不能引起位置变化。
    // 两份独立快照还避免残留标签顶点导致错误相等。
    renderSingleSample(shortStart, "fx.wav", 0.0, true);
    renderSingleSample(shortLater, "fx.wav", 2.25, true);
    if ( !labelGeometryEqual(shortStart, shortLater) ) {
        XERROR("Short sample label moved despite fitting inside the object");
        return false;
    }

    // 同样时钟差用于长名称，越界文本才应进入滚动分支。
    // 要求两帧都有字形，不能把整段丢弃误判为滚动。
    MMM::Logic::RenderSnapshot longStart;
    MMM::Logic::RenderSnapshot longLater;
    renderSingleSample(
        longStart, "very_long_sample_resource_name.wav", 0.0, true);
    renderSingleSample(
        longLater, "very_long_sample_resource_name.wav", 2.25, true);
    if ( longStart.vertices.size() <= 4U || longLater.vertices.size() <= 4U ||
         labelGeometryEqual(longStart, longLater) ) {
        XERROR("Long sample label did not advance with the monotonic clock");
        return false;
    }

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    const auto laneBounds =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 0U });
    // 轨道边界是标签裁剪断言的依据，缺失时无法定义合法范围。
    // 直接失败而不跳过裁剪检查，避免测试配置错误造成空通过。
    if ( !laneBounds ) {
        XERROR("Unable to resolve the first BGM lane bounds");
        return false;
    }
    // 采样标签使用两像素水平内边距，与轨道标题的内边距不同。
    // 依据轨道边界裁剪，不能根据缩放后的采样本体宽度扩大范围。
    const float labelLeft  = laneBounds->leftX + 2.0F;
    const float labelRight = laneBounds->rightX - 2.0F;
    // 同时检查滚动前后所有字形顶点，覆盖左右边缘的部分字形。
    // 仅忽略本体；它本身可以因缩放超出标签裁剪范围。
    for ( const auto* snapshot : { &longStart, &longLater } ) {
        for ( std::size_t index = 4U; index < snapshot->vertices.size();
              ++index ) {
            const float x = snapshot->vertices[index].pos.x;
            if ( x < labelLeft - 1e-4F || x > labelRight + 1e-4F ) {
                XERROR("Scrolling sample label escaped its lane width");
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证标签字号跟随纵向物件缩放，裁剪范围不跟随横向缩放。
/// @return 字号比例和固定轨道宽度裁剪均符合预期时返回 true。
/// @note 横向与纵向缩放分两组测试，每组只改变一个变量。
/// @note 横向用长文本触发裁剪，纵向用短文本避免裁剪干扰高度。
/// @note 高度必须非零后才能取比值，缺失字体不能通过比例断言。
bool testSampleLabelScaleAndFixedLaneWidth()
{
    // 先保持纵向缩放相同，只拉开横向尺寸。
    // 若标签错误跟随本体宽度，其裁剪 UV 或顶点位置会变化。
    MMM::Logic::RenderSnapshot narrowBody;
    MMM::Logic::RenderSnapshot wideBody;
    renderSingleSample(narrowBody,
                       "very_long_sample_resource_name.wav",
                       2.25,
                       true,
                       0.5F,
                       1.0F);
    renderSingleSample(
        wideBody, "very_long_sample_resource_name.wav", 2.25, true, 3.0F, 1.0F);
    // 两份标签必须具有相同裁剪片段，而不只是相同宽度。
    // 逐顶点 UV 比较能识别横向缩放错误改变滚动窗口的情况。
    if ( !labelGeometryEqual(narrowBody, wideBody) ) {
        XERROR("Sample label bounds changed with horizontal object scale");
        return false;
    }

    // 第二组固定横向缩放，只改变纵向尺寸。
    // 短名称没有水平滚动，测得高度差可归因于字号缩放。
    MMM::Logic::RenderSnapshot shortBody;
    MMM::Logic::RenderSnapshot tallBody;
    renderSingleSample(shortBody, "fx.wav", 0.0, true, 1.0F, 0.5F);
    renderSingleSample(tallBody, "fx.wav", 0.0, true, 1.0F, 2.0F);
    // 纵向缩放从半倍到两倍，期望字形高度比为四。
    // 先确认较小字形存在，再做除法并检查比例。
    const float shortGlyphHeight = labelGlyphHeight(shortBody);
    const float tallGlyphHeight  = labelGlyphHeight(tallBody);
    if ( shortGlyphHeight <= 0.0F ||
         !near(tallGlyphHeight / shortGlyphHeight, 4.0F) ) {
        XERROR("Sample label font size did not follow vertical object scale");
        return false;
    }
    return true;
}

/// @brief 验证采样标签按 UTF-8 码点使用按需加载的 CJK 字形。
/// @return “初音”均使用 Unicode 图集纹理且没有被替换成问号时返回 true。
/// @note 预装字形的名称不应重新发起缺字请求。
/// @note 按码点对应的独立 UV 检查字形，不能用总顶点数代替 Unicode 解析验证。
/// @note 扩展名属于 ASCII，仍通过普通字形助手提供。
bool testSampleLabelCjkGlyphs()
{
    MMM::Logic::RenderSnapshot snapshot;
    // 先注入两枚汉字，随后 ASCII 助手补充扩展名字形。
    // 这模拟名称出现时图集已经包含所需字符的状态。
    configureUnicodeFont(snapshot);
    renderSingleSample(snapshot, "初音.wav", 0.0, true);

    // 已有字形不能继续请求刷新，否则播放时会反复触发低频图集工作。
    // 这里只验证请求计数，不执行实际字体加载。
    if ( snapshot.requestedUnicodeGlyphCount != 0U ) {
        XERROR("Loaded CJK sample label requested a redundant atlas refresh");
        return false;
    }

    // 跳过四个本体顶点后按 U 起点识别两个汉字。
    // 找到其中一个不足以通过，避免 UTF-8 解码只处理首码点。
    bool foundFirst  = false;
    bool foundSecond = false;
    for ( std::size_t index = 4U; index < snapshot.vertices.size(); ++index ) {
        const float u = snapshot.vertices[index].uv.u;
        foundFirst    = foundFirst || near(u, 0.82F);
        foundSecond   = foundSecond || near(u, 0.83F);
    }
    // 两个预装码点都应出现，不能退化成 ASCII 问号占位。
    // UV 判断只针对具体测试字形，其他标签字符不能替代它们。
    if ( !foundFirst || !foundSecond ) {
        XERROR("CJK sample label did not use the Unicode glyph atlas");
        return false;
    }
    return true;
}

/// @brief 验证可见 CJK 标签会回报当前图集缺失的码点。
/// @return 截图项目的日文资源名各码点只回报一次时返回 true。
/// @note 同一名称中的重复码点只请求一次，保持首次出现顺序。
/// @note ASCII 标点与扩展名已有度量，不应进入 Unicode 缺字列表。
/// @note 请求检查使用 Unicode 码点而非 UTF-8 字节，避免多字节拆分回归。
/// @note 这里只检验请求生成，不启动异步图集加载或等待刷新。
/// @note 期望码点数量小于快照请求容量，本例不涉及容量溢出策略。
/// @note 被裁剪文本中的缺字仍按当前生成器的标签请求行为检查。
bool testMissingCjkGlyphRequestsAtlasRefresh()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderSingleSample(
        snapshot, "ぴょん (feat. 初音ミク & 重音テト).mp3", 0.0, true);
    // 期望列表手写为码点值，独立于被测 UTF-8 解析路径。
    // 重复的音字只保留第一次，顺序按资源名中的首次出现排列。
    constexpr std::array<std::uint32_t, 10> expectedCodepoints{
        0x3074U, 0x3087U, 0x3093U, 0x521DU, 0x97F3U,
        0x30DFU, 0x30AFU, 0x91CDU, 0x30C6U, 0x30C8U
    };
    // 请求数量同时检查缺失和重复，先于逐项比较避免读越界。
    // ASCII 标点和括号已有字体，不应增加 Unicode 请求计数。
    if ( snapshot.requestedUnicodeGlyphCount != expectedCodepoints.size() ) {
        XERROR("Missing CJK label glyphs were not reported for atlas refresh");
        return false;
    }
    // 逐项核对而非只比较集合，确保请求顺序在重复刷新时稳定。
    // 不需要等待异步加载；快照应立即带回本帧发现的缺字。
    for ( std::size_t index = 0U; index < expectedCodepoints.size(); ++index ) {
        if ( snapshot.requestedUnicodeGlyphs[index] !=
             expectedCodepoints[index] ) {
            XERROR("CJK atlas refresh request lost the screenshot resource ID");
            return false;
        }
    }
    return true;
}

}  // namespace

/// @brief 运行自动采样渲染系统回归测试。
/// @return 全部测试通过时返回 0。
/// @note 用布尔短路保留首个失败用例的日志，退出码供 CTest 判断。
/// @note 测试数据全部在内存中构造，不生成资源文件或音频输出。
int main()
{
    // 先验证公共布局，再验证采样本体和标签，便于首条日志定位失败职责。
    // 每个用例自建状态，短路执行不会改变后续用例所需的全局初始化。
    return testLaneLayoutVisuals() && testDraftLaneLabels() &&
                   testNarrowLaneLabelMarquee() &&
                   testSampleBodyMatchesTapTextureAndSize() &&
                   testSampleUsesIndependentBgmLaneWidth() &&
                   testSampleBrushPreview() && testSampleErasePreview() &&
                   testSampleInteractionGlow() && testSampleLabelMarquee() &&
                   testSampleLabelScaleAndFixedLaneWidth() &&
                   testSampleLabelCjkGlyphs() &&
                   testMissingCjkGlyphRequestsAtlasRefresh()
               ? 0
               : 1;
}
