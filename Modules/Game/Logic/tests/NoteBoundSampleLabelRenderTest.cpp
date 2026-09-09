#include "logic/ecs/system/NoteRenderSystem.h"

#include "common/AsciiFontData.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// 使用人工字形度量和 UV 标记辨认标签，不加载真实字体或启动 GPU。
// 本例检查快照几何与裁剪规则，不验证文字实际外观和音频播放。
/// @brief 测试画布宽度，配置中的归一化轨道边界以此换算。
constexpr float VIEWPORT_WIDTH = 800.0F;
/// @brief 测试画布高度，也是快照生成时的参考高度。
constexpr float VIEWPORT_HEIGHT = 600.0F;
/// @brief 玩家轨道区域左边界，对应归一化配置 0.1。
constexpr float PLAYER_LEFT = 80.0F;
/// @brief 四键布局中单条玩家轨宽，对应画布宽度的十分之一。
constexpr float LANE_WIDTH = 80.0F;
/// @brief 标签内容左边界，相对轨道留出两像素内边距。
constexpr float LABEL_LEFT = PLAYER_LEFT + 2.0F;
/// @brief 标签内容右边界，独立于音符本体的横向缩放。
constexpr float LABEL_RIGHT = PLAYER_LEFT + LANE_WIDTH - 2.0F;

/// @brief 使用小容差比较测试几何数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note 非有限值不能通过此比较；本测试几何预期均为有限像素和 UV。
bool near(float lhs, float rhs)
{
    // 测试采用固定像素尺度，使用绝对容差吸收几何计算的浮点舍入差异。
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 为标签测试注入固定宽度 ASCII 字体度量和字形 UV。
/// @param snapshot 待初始化快照。
/// @pre 快照尚未配置同名字形 UV；emplace 不会替换已有键。
/// @note 度量与 UV 均为测试夹具，不代表真实字体的像素尺寸或图集内容。
/// @note 这里只发布 CPU 图集描述，不创建纹理对象或上传字形位图。
void configureAsciiFont(MMM::Logic::RenderSnapshot& snapshot)
{
    // 只开放一个有效字号档，让字体档位选择具有确定结果。
    constexpr std::size_t tierIndex = 3U;
    auto&                 atlas     = snapshot.asciiFontAtlasMetrics;
    atlas.valid                     = true;
    auto& font                      = atlas.tiers[tierIndex];
    font.valid                      = true;
    font.ascender                   = 0.8F;
    // 基线和字形承载偏移不是整数像素，允许检验实际顶点换算后的舍入结果。
    font.lineHeight = 1.0F;
    // 各档度量为归一化数值，最终字号缩放由被测渲染入口完成。
    // 补齐可打印 ASCII 区间，使资源名称、音量数字与标点都能参与排版。
    for ( std::uint32_t code = MMM::Common::ASCII_GLYPH_FIRST;
          code <= MMM::Common::ASCII_GLYPH_LAST;
          ++code ) {
        auto& glyph     = font.glyphs[code - MMM::Common::ASCII_GLYPH_FIRST];
        glyph.available = true;
        // 空格仍占水平步进，但没有可绘制位图，不能伪造空格四边形。
        glyph.hasBitmap = code != static_cast<std::uint32_t>(' ');
        glyph.width     = 0.5F;
        glyph.height    = 0.75F;
        glyph.bearingX  = 0.0F;
        glyph.bearingY  = 0.75F;
        glyph.advanceX  = 0.6F;
        // 字形宽度小于水平步进，保留字符间距以覆盖排版与几何宽度的差别。
        if ( glyph.hasBitmap ) {
            const auto textureId = MMM::Logic::asciiGlyphTextureId(
                tierIndex, static_cast<char>(code));
            const float glyphU =
                0.6F +
                static_cast<float>(code - MMM::Common::ASCII_GLYPH_FIRST) *
                    0.001F;
            // 每个字符分配不同 U 起点，滚动后即使位置相同也能辨认字符变化。
            // 字体 UV 全部位于专用范围，与下方音符和纯色纹理夹具隔离。
            // UV 宽度小于字符起点间隔，裁剪后的 U 仍处于该字符自己的区间。
            snapshot.uvMap.emplace(static_cast<std::uint32_t>(textureId),
                                   glm::vec4{ glyphU, 0.7F, 0.0008F, 0.01F });
        }
    }
}

/// @brief 记录一个标签字形顶点的屏幕位置与 UV。
struct GlyphVertex {
    /// @brief 字形顶点的画布横坐标。
    float x;
    /// @brief 字形顶点的画布纵坐标。
    float y;
    /// @brief 图集横向坐标，兼用于区分夹具中的不同字符。
    float u;
    /// @brief 图集纵向坐标，兼用于识别字体几何。
    float v;
};

/// @brief 收集首个玩家轨道范围内的 ASCII 字形顶点。
/// @param snapshot 待检查快照。
/// @return 绑定音效标签使用的字形顶点。
/// @note 依赖本测试的 UV 分区约定，不是通用的快照文字识别函数。
/// @note 只收集轨道内的顶点；不能用本函数单独证明不存在轨道外字形。
/// @note 返回值是独立副本，后续比较不受源快照容器重新分配影响。
std::vector<GlyphVertex> collectFirstLaneGlyphs(
    const MMM::Logic::RenderSnapshot& snapshot)
{
    std::vector<GlyphVertex> result;
    for ( const auto& vertex : snapshot.vertices ) {
        // 同时筛选纹理区域和首轨横向区域，避开音符本体以及侧栏标题。
        if ( vertex.uv.u < 0.6F || vertex.uv.v < 0.69F ||
             vertex.pos.x < LABEL_LEFT - 1e-4F ||
             vertex.pos.x > LABEL_RIGHT + 1e-4F ) {
            continue;
        }
        result.push_back(
            { vertex.pos.x, vertex.pos.y, vertex.uv.u, vertex.uv.v });
    }
    // 保留快照原始顶点顺序，后续按对应顶点比较，不能排序后掩盖排版变化。
    return result;
}

/// @brief 判断两组字形几何是否完全一致。
/// @param lhs 第一组几何。
/// @param rhs 第二组几何。
/// @return 位置与 UV 均一致时返回 true。
/// @note 两个空列表也返回 false，防止标签完全漏绘时等价性断言误通过。
/// @note 比较的是顶点序列，不比较绘制命令、颜色或最终屏幕像素。
bool glyphGeometryEqual(const std::vector<GlyphVertex>& lhs,
                        const std::vector<GlyphVertex>& rhs)
{
    if ( lhs.size() != rhs.size() || lhs.empty() ) return false;
    for ( std::size_t index = 0; index < lhs.size(); ++index ) {
        // UV 与位置都要一致：相同位置绘制不同字符不视为同一份标签几何。
        if ( !near(lhs[index].x, rhs[index].x) ||
             !near(lhs[index].y, rhs[index].y) ||
             !near(lhs[index].u, rhs[index].u) ||
             !near(lhs[index].v, rhs[index].v) ) {
            return false;
        }
    }
    return true;
}

/// @brief 为一个绑定音效的玩家 Tap 生成画布快照。
/// @param snapshot 输出快照。
/// @param enabled 是否启用绑定音效标签。
/// @param cameraId 目标画布 ID。
/// @param snapshotSysTime 标签滚动使用的单调时钟秒数。
/// @param noteScaleX 物件横向缩放。
/// @param useDraftLane 是否在自定义草稿轨道渲染绑定物件。
/// @pre snapshot 是本次检查独占的新快照，不复用前一次绘制的顶点与图集。
/// @note 每次重建注册表和配置，避免不同开关用例之间遗留渲染状态。
/// @note 资源名称只用于标签排版，测试不会创建或解码同名 WAV。
/// @note noteScaleX 仅改变本体宽度；测试用它检查标签是否错误继承本体缩放。
/// @note useDraftLane 为 true 时仍使用同一 Tap 类型，不混入长条或折线差异。
void renderBoundTap(MMM::Logic::RenderSnapshot& snapshot, bool enabled,
                    std::string_view cameraId, double snapshotSysTime,
                    float noteScaleX = 1.2F, bool useDraftLane = false)
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    // 固定 BPM 使纵向投影可重复；此例不把 SV 变化混入标签宽度测试。
    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    // 玩家区域宽 320 像素，四条轨道各 80 像素，与断言常量保持一致。
    if ( useDraftLane ) {
        // 使用不同于玩家轨宽的草稿区，能暴露错误复用玩家轨边界的实现。
        config.visual.trackLayout.draftLanes.left  = -0.21F;
        config.visual.trackLayout.draftLanes.width = 0.06F;
        config.settings.professionalMode           = true;
        // 草稿物件依赖专业模式可见性，不能把模式隐藏误判成标签漏绘。
    }
    config.visual.noteScaleX            = noteScaleX;
    config.visual.noteScaleY            = 1.0F;
    config.visual.showBoundSampleLabels = enabled;
    // 关闭拍线和时间线辅助元素，让本例聚焦绑定标签产生的字体几何。
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);
    // 缓存绑定到时间线注册表，生命周期覆盖整个同步快照生成调用。

    const auto                noteEntity = noteRegistry.create();
    MMM::Logic::NoteComponent note;
    note.m_timestamp  = 0.0;
    note.m_type       = MMM::NoteType::NOTE;
    note.m_trackIndex = useDraftLane ? -1 : 0;
    note.m_isDraft    = useDraftLane;
    // 负轨号与草稿标志同步切换，不构造只有一项改变的不一致物件。
    note.m_sampleBinding = MMM::AudioSampleBinding{
        // 长名称必须超过轨宽，才会进入跑马灯而不是静态短标签分支。
        .m_audioResourceId = "very_long_bound_effect_resource_name.wav",
        .m_volume          = 0.75F,
    };
    noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity,
                                                    std::move(note));
    noteRegistry.emplace<MMM::Logic::TransformComponent>(noteEntity);
    // 为实体提供渲染所需的变换组件，不借助会话加载流程隐式补齐组件。
    // 即便只有一个物件，也提供渲染器使用的排序索引，不依赖全实体扫描。
    const std::vector<entt::entity> sortedNotes{ noteEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);
    // 上下文借用局部索引；generateSnapshot 同步完成后才离开本函数。

    snapshot.hasBeatmap = true;
    // 明确进入已加载谱面的渲染分支，避免空谱面占位逻辑干扰结果。
    snapshot.snapshotSysTime = snapshotSysTime;
    // 显式注入标签动画时钟，不读取真实时间，也不通过 sleep 等待滚动。
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });
    configureAsciiFont(snapshot);
    // 音符纹理使用非方形比例，本体高度按纹理几何计算而不是缺图回退。

    // 相机 ID 决定主画布与预览策略，其余夹具保持一致以隔离测试变量。
    // 空采样注册表表示这里只测试音符绑定音效，不混入独立 BGM 采样标签。
    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        std::string(cameraId),
        0.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        300.0F,
        4,
        0,
        4,
        config,
        VIEWPORT_HEIGHT);
    // 快照拥有生成后的几何，局部注册表销毁不影响调用方继续检查顶点。
}

/// @brief 验证开关仅在主画布为绑定玩家物件生成标签。
/// @return 关闭和预览区均无标签，主画布开启时存在标签。
/// @note 使用固定相机标识验证策略分支，不模拟 GUI 标签页切换或焦点事件。
bool testBoundLabelToggleAndMainCanvasScope()
{
    // 先验证关闭分支，避免“从未画出任何标签”被误认为正确遵守开关。
    MMM::Logic::RenderSnapshot disabled;
    renderBoundTap(disabled, false, "Basic2DCanvas", 0.0);
    // 这里只拒绝字体几何，不要求关闭标签后整张快照为空，音符本体仍应存在。
    if ( !collectFirstLaneGlyphs(disabled).empty() ) {
        XERROR("Disabled bound sample label still rendered player glyphs");
        return false;
    }

    MMM::Logic::RenderSnapshot enabled;
    // 开启分支必须产生非空字形，形成与关闭分支对应的正向证据。
    renderBoundTap(enabled, true, "Basic2DCanvas", 0.0);
    if ( collectFirstLaneGlyphs(enabled).empty() ) {
        XERROR("Enabled bound sample label did not render player glyphs");
        return false;
    }

    MMM::Logic::RenderSnapshot preview;
    // 预览区即便开启配置也不展示主画布标签，不能只验证全局开关为 false。
    renderBoundTap(preview, true, "Preview", 0.0);
    for ( const auto& vertex : preview.vertices ) {
        // 预览比例不同，检查全部字体 UV，而不是复用主画布的首轨像素范围。
        if ( vertex.uv.u >= 0.6F && vertex.uv.v >= 0.69F ) {
            XERROR("Preview rendered a main-canvas bound sample label");
            return false;
        }
    }
    return true;
}

/// @brief 验证草稿绑定音效标签使用独立草稿轨道范围。
/// @return 标签字形存在且全部限制在自定义草稿轨内时返回 true。
/// @note 目标草稿轨位于玩家区域左侧，不能把草稿区方向与 BGM 区混淆。
/// @note 本例断言内边距后的文本区域，而不是物件放大后的视觉外缘。
bool testDraftBoundLabelUsesIndependentLane()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderBoundTap(snapshot, true, "Basic2DCanvas", 0.0, 1.2F, true);
    // 固定布局将目标草稿轨投影到 24～72 像素，明显不同于玩家首轨 80～160。
    constexpr float draftLaneLeft  = 24.0F;
    constexpr float draftLaneRight = 72.0F;
    // 左右各两像素内边距与玩家标签相同，但基础宽度取自独立草稿轨。
    bool foundGlyph = false;
    for ( const auto& vertex : snapshot.vertices ) {
        // 纵向窗口围住判定线附近的物件，排除画布顶部的草稿轨标题。
        if ( vertex.uv.u < 0.6F || vertex.uv.v < 0.69F ||
             vertex.pos.y < 200.0F || vertex.pos.y > 350.0F ) {
            continue;
        }
        foundGlyph = true;
        // 直接检查窗口内全部字体顶点，不能先按正确横向范围过滤再验证范围。
        if ( vertex.pos.x < draftLaneLeft + 2.0F - 1e-4F ||
             vertex.pos.x > draftLaneRight - 2.0F + 1e-4F ) {
            XERROR("Draft bound label escaped lane: x={}", vertex.pos.x);
            return false;
        }
    }
    // 边界条件全部通过仍需有实际字形，否则“没有绘制”会形成空集合假通过。
    if ( !foundGlyph ) XERROR("Draft bound label did not render glyphs");
    return foundGlyph;
}

/// @brief 验证玩家物件标签复用采样标签的滚动与固定轨道裁剪语义。
/// @return 单调时钟推动文本且横向物件缩放不改变标签范围时返回 true。
/// @note 只比较两个离散时刻，不检查跑马灯完整周期、端点停顿或逐帧连续性。
/// @note 等价检查包含 UV，因此部分字形裁剪位置改变也会被识别。
bool testBoundLabelMarqueeAndFixedLaneWidth()
{
    // 同一布局只改变快照时钟，判定变化来自标签滚动而非音符位置变化。
    MMM::Logic::RenderSnapshot start;
    MMM::Logic::RenderSnapshot later;
    renderBoundTap(start, true, "Basic2DCanvas", 0.0, 1.2F);
    renderBoundTap(later, true, "Basic2DCanvas", 2.25, 1.2F);
    // 选择非零时刻覆盖滚动阶段，不用实际等待来驱动动画进度。
    const auto startGlyphs = collectFirstLaneGlyphs(start);
    // 复制几何而不是保存容器引用，两帧快照的比较输入彼此独立。
    const auto laterGlyphs = collectFirstLaneGlyphs(later);
    // 两帧都必须存在文字，消失的标签不能作为“发生滚动”的成功条件。
    if ( startGlyphs.empty() || laterGlyphs.empty() ||
         glyphGeometryEqual(startGlyphs, laterGlyphs) ) {
        XERROR("Bound sample label did not scroll with the shared marquee");
        return false;
    }

    MMM::Logic::RenderSnapshot narrow;
    MMM::Logic::RenderSnapshot wide;
    // 固定相同时钟，只放大横向本体；标签应保持轨宽和两侧内边距不变。
    renderBoundTap(narrow, true, "Basic2DCanvas", 2.25, 0.5F);
    renderBoundTap(wide, true, "Basic2DCanvas", 2.25, 3.0F);
    // 本体从半轨宽变化到三倍轨宽，标签几何仍应逐顶点相等。
    if ( !glyphGeometryEqual(collectFirstLaneGlyphs(narrow),
                             collectFirstLaneGlyphs(wide)) ) {
        XERROR("Bound sample label width followed horizontal object scale");
        return false;
    }

    // 此检查针对已收集的首轨顶点；草稿测试另行检查其自身窗口中的全部字体。
    // 它不构成对玩家轨外全部字形的独立扫描，避免夸大本断言的覆盖范围。
    for ( const auto* glyphs : { &startGlyphs, &laterGlyphs } ) {
        for ( const auto& vertex : *glyphs ) {
            if ( vertex.x < LABEL_LEFT - 1e-4F ||
                 vertex.x > LABEL_RIGHT + 1e-4F ) {
                XERROR("Bound sample label escaped its player lane");
                return false;
            }
        }
    }
    return true;
}

}  // namespace

/// @brief 运行玩家物件绑定音效标签渲染回归测试。
/// @return 全部测试通过时返回 0。
/// @note 不创建外部资源文件，全部输入由各场景的局部夹具构造。
/// @note 失败采用短路返回，不继续执行后面的场景；成功返回才表示三项均完成。
int main()
{
    // 任一场景失败立即返回非零，日志由具体场景提供失败原因。
    return testBoundLabelToggleAndMainCanvasScope() &&
                   testDraftBoundLabelUsesIndependentLane() &&
                   testBoundLabelMarqueeAndFixedLaneWidth()
               ? 0
               : 1;
}
