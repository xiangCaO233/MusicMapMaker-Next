#include "logic/ecs/system/NoteRenderSystem.h"

#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
using MMM::Logic::HoverPart;
using MMM::Logic::TextureID;

/// @brief 固定画布尺寸，避免依赖实际窗口和显示器。
constexpr float WIDTH = 800.0F;
/// @brief 画布纵向裁剪范围是零至该高度。
constexpr float HEIGHT = 600.0F;
/// @brief 暂停时刻在画布中部，头尾均有明确可见余量。
constexpr float JUDGMENT_Y = 300.0F;
/// @brief 左右布局分别为 0.1 和 0.9，四轨下单轨宽度固定为 160。
constexpr float BASE_NOTE_WIDTH = 120.0F;
/// @brief 基础纹理宽高比为四，布局纵向倍率为 0.5。
constexpr float BASE_NOTE_HEIGHT = 20.0F;
/// @brief 人工 UV 区域互不相交，允许按最终 UV 反查部件几何。
constexpr glm::vec4 NOTE_UV{ 0.1F, 0.1F, 0.1F, 0.025F };
/// @brief 独立长条头与普通 Note 原始尺寸相同，只靠配置产生大小差异。
constexpr glm::vec4 HEAD_UV{ 0.3F, 0.1F, 0.1F, 0.025F };
/// @brief 长条尾部独立区域，用于验证离屏端点没有被提前丢弃。
constexpr glm::vec4 END_UV{ 0.5F, 0.1F, 0.1F, 0.025F };
/// @brief 连接体原始宽度为 Note 的四分之一，不沿时间轴应用倍率。
constexpr glm::vec4 BODY_UV{ 0.7F, 0.1F, 0.025F, 0.05F };
/// @brief 中间节点使用独立 UV，防止污染普通 Note 顶点统计。
constexpr glm::vec4 NODE_UV{ 0.8F, 0.1F, 0.05F, 0.025F };

/// @brief 使用日志记录失败，允许调用方继续收集同一场景的独立断言。
/// @param condition 被测条件。
/// @param message 失败时的测试说明。
/// @return 条件原值。
/// @note 测试使用返回值汇总，不依赖会在发布构建中被禁用的 assert。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) XERROR("NoteTextureScaleBoundsTest: {}", message);
    return condition;
}

/// @brief 比较 CPU 几何浮点结果，容纳图集与轨道换算的舍入误差。
/// @param actual 渲染或命中框输出的像素值。
/// @param expected 独立布局常量计算的期望值。
/// @return 相差不足千分之一像素时返回真。
/// @note 容差远小于任一被测纹理，不能掩盖遗漏或重复应用倍率。
bool near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.001F;
}

/// @brief 提取某个纹理提交的四点包围框，不依据绘制命令的 Scissor 猜测。
/// @param snapshot 正式渲染入口生成的 CPU 快照。
/// @param uv 只属于被测部件的非重叠区域。
/// @param bounds 输出左上角坐标与宽高。
/// @return 恰好四个有限顶点时成功；空输出不能被当作通过。
/// @note 不检查 GPU 可见片元，离屏中心的贴片仍应保留完整 CPU 四点。
/// @note 本函数不按颜色筛选，缺省皮肤颜色不会导致漏计纹理。
/// @note 每个场景无选中高亮，同一 UV 不应由多个图元共同使用。
bool quadBounds(const MMM::Logic::RenderSnapshot& snapshot, const glm::vec4& uv,
                glm::vec4& bounds)
{
    float       minX  = std::numeric_limits<float>::max();
    float       minY  = minX;
    float       maxX  = std::numeric_limits<float>::lowest();
    float       maxY  = maxX;
    std::size_t count = 0;
    // 先筛选纹理再检查坐标，轨道背景和纯色辅助线不属于本断言目标。
    for ( const auto& vertex : snapshot.vertices ) {
        // 图集内缩后的采样仍在所属区域，不需要硬编码内部图集分辨率。
        // CanvasTexCoord 使用 u/v，区域向量则分别保存起点与宽高。
        if ( vertex.uv.u < uv.x || vertex.uv.u > uv.x + uv.z ||
             vertex.uv.v < uv.y || vertex.uv.v > uv.y + uv.w )
            continue;
        if ( !std::isfinite(vertex.pos.x) || !std::isfinite(vertex.pos.y) )
            return false;
        // 四点必须有有限包络；单靠宽高比较无法可靠发现 NaN 顶点。
        minX = std::min(minX, vertex.pos.x);
        minY = std::min(minY, vertex.pos.y);
        maxX = std::max(maxX, vertex.pos.x);
        maxY = std::max(maxY, vertex.pos.y);
        ++count;
    }
    // 同一场景只放一个被测部件，重复高亮或错误贴图会破坏四点约束。
    bounds = { minX, minY, maxX - minX, maxY - minY };
    return count == 4;
}

/// @brief 验证指定实体的部件命中框与最终四点边界重合。
/// @note 本测试使用 Stretch 填充，布局矩形就是最终贴片矩形。
/// @param snapshot 同一轮 CPU 渲染输出，不能混用另一轮的命中数据。
/// @param entity 拾取身份对应的根音符实体。
/// @param part 被测端点或连接体部位。
/// @param bounds 根据最终纹理顶点独立提取的矩形。
/// @return 对应命中框存在且四个边界参数全部一致时成功。
/// @note 折线用例只有一段连接体，因此无需再用子索引消除歧义。
bool matchesHitbox(const MMM::Logic::RenderSnapshot& snapshot,
                   entt::entity entity, HoverPart part, const glm::vec4& bounds)
{
    const auto hit =
        std::find_if(snapshot.hitboxes.begin(),
                     snapshot.hitboxes.end(),
                     [entity, part](const auto& value) {
                         return value.entity == entity && value.part == part;
                     });
    // 先确认命中存在，避免通过读取缺失迭代器把错误变成崩溃。
    // Hitbox 保存左上角和宽高，不是两个对角坐标，比较前不做额外变换。
    return hit != snapshot.hitboxes.end() && near(hit->x, bounds.x) &&
           near(hit->y, bounds.y) && near(hit->w, bounds.z) &&
           near(hit->h, bounds.w);
}

/// @brief 最小主画布场景，所有数据归属与正式会话一致而不启动窗口或 GPU。
/// @note 各用例重新创建场景，避免候选索引或上一个快照的几何影响结果。
/// @note 不启动 BeatmapSession，防止测试触发音频、配置保存或目录监视。
/// @note 所有成员按值持有，观察索引只在同步渲染调用期间借用。
/// @note 时间线仅含一个 BPM，测试焦点是纹理尺寸而非变速积分算法。
struct Scene {
    /// @brief 固定布局及编辑门禁，不读取个人配置。
    MMM::Config::EditorConfig m_config;
    /// @brief 玩家音符注册表。
    entt::registry m_notes;
    /// @brief 空采样表，仅满足完整快照入口的领域边界。
    entt::registry m_samples;
    /// @brief 时间线注册表拥有滚动缓存的生命周期。
    entt::registry m_timeline;
    /// @brief 已按时间添加的实体观察索引，在渲染结束前保持存活。
    std::vector<entt::entity> m_sorted;
    /// @brief CPU 端顶点及拾取数据，不持有真实纹理对象。
    MMM::Logic::RenderSnapshot m_snapshot;

    /// @brief 初始化匀速时间线和互不重叠的人工图集。
    /// @note 设置实际布局缩放不为一，可同时发现纹理倍率覆盖布局倍率的问题。
    /// @note 快照必须接受交互且保持暂停，正式路径才会生成命中框。
    /// @note UV 尺寸按同一正方形图集定义，因此宽高比可直接用于布局计算。
    Scene()
    {
        // 明确设置会影响基准尺寸的每个布局值，不能依赖用户默认皮肤。
        m_config.visual.trackLayout.left   = 0.1F;
        m_config.visual.trackLayout.right  = 0.9F;
        m_config.visual.trackLayout.top    = 0.0F;
        m_config.visual.trackLayout.bottom = 1.0F;
        m_config.visual.noteScaleX         = 0.75F;
        m_config.visual.noteScaleY         = 0.5F;
        // 显式选择 Stretch，避免 Fit 留白使拾取布局与纹理像素区域不同。
        m_config.visual.noteFillMode = MMM::Config::BackgroundFillMode::Stretch;
        m_config.visual.beatLineDisplayMode =
            MMM::Config::BeatLineDisplayMode::Hidden;
        m_config.visual.simulateAutoplay        = false;
        m_config.settings.enablePolylineEditing = true;
        // 零时刻单个 BPM 提供非零基础速度，后续目标时间由缓存反算。
        const auto bpm = m_timeline.create();
        m_timeline.emplace<MMM::Logic::TimelineComponent>(
            bpm,
            MMM::Logic::TimelineComponent{ .m_timestamp = 0.0,
                                           .m_effect = MMM::TimingEffect::BPM,
                                           .m_value  = 120.0 });
        // 正式滚动缓存提供时间映射，测试不模拟 NoteRenderSystem 的内部算法。
        m_timeline.ctx().emplace<MMM::Logic::System::ScrollCache>().rebuild(
            m_timeline, m_config, nullptr);
        m_snapshot.hasBeatmap         = true;
        m_snapshot.acceptsInteraction = true;
        // None 的区域与所有 Note 部件隔离，不将背景矩形计入四点断言。
        m_snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::None),
                                 glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
        m_snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Note),
                                 NOTE_UV);
        m_snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::HoldHead),
                                 HEAD_UV);
        // 头尾原始尺寸相同，所以最终尺寸的差异只能来自各自的独立倍率。
        m_snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::HoldEnd),
                                 END_UV);
        m_snapshot.uvMap.emplace(
            static_cast<uint32_t>(TextureID::HoldBodyVertical), BODY_UV);
        m_snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Node),
                                 NODE_UV);
    }

    /// @brief 由目标屏幕中心反算匀速场景中的秒时间，只用于构造测试输入。
    /// @note 期望尺寸另以常量给出，不从待测顶点反推。
    /// @param centerY 目标音符中心在当前画布中的像素 Y，可为负值。
    /// @return 当前暂停时刻为零时，对应的非负谱面秒时间。
    /// @pre 场景时间线保持匀速且无 Jump，缓存的一秒位移必须非零。
    double timeAtY(float centerY) const
    {
        const auto& cache =
            m_timeline.ctx().get<MMM::Logic::System::ScrollCache>();
        const double speed =
            cache.getDisplayDelta(1.0, cache.getAbsY(0.0), 1.0);
        // 测试只反算中心，不能将半高混入 timestamp 或 duration。
        return (JUDGMENT_Y - centerY) / speed;
    }

    /// @brief 将实体与必要 Transform 一并登记，返回可定位拾取结果的身份。
    /// @param note 本次用例构造的组件值，登记后由 Registry 自己持有。
    /// @return 当前音符 Registry 中的稳定实体句柄。
    /// @pre 各次调用按时间非降序排列；测试不额外排序来掩盖输入错误。
    entt::entity add(const MMM::Logic::NoteComponent& note)
    {
        const auto entity = m_notes.create();
        m_notes.emplace<MMM::Logic::NoteComponent>(entity, note);
        m_notes.emplace<MMM::Logic::TransformComponent>(entity);
        // 存储实体号而非组件地址，后续插入实体不会使索引观察指针失效。
        m_sorted.push_back(entity);
        return entity;
    }

    /// @brief 走完整主画布路径，覆盖教程字段、候选剔除、实际顶点及命中框。
    /// @pre 每个场景只调用一次，快照和索引上下文无需模拟正式缓冲复用。
    /// @note 不启用草稿或 BGM 轨道，避免额外投影影响固定玩家几何。
    void render()
    {
        // 索引指针只借用成员向量，生命周期覆盖本次同步快照生成。
        m_notes.ctx().emplace<const std::vector<entt::entity>*>(&m_sorted);
        MMM::Logic::System::NoteRenderSystem::generateSnapshot(m_notes,
                                                               m_samples,
                                                               {},
                                                               {},
                                                               m_timeline,
                                                               {},
                                                               &m_snapshot,
                                                               "Basic2DCanvas",
                                                               0.0,
                                                               WIDTH,
                                                               HEIGHT,
                                                               JUDGMENT_Y,
                                                               4,
                                                               0,
                                                               0,
                                                               m_config,
                                                               HEIGHT);
    }
};

/// @brief 验证单键缩小与长条头尾独立放大均反映到教程和拾取边界。
/// @param endScale 尾部独立倍率，不得继承单键或头部的值。
/// @return 三类部件尺寸、教程边界和三类拾取框都匹配时返回真。
/// @note 单键与长条不在同轨，重叠提示不应引入额外被测纹理。
bool testPartBounds(float endScale)
{
    Scene      scene;
    const auto tap  = scene.add({ .m_type       = MMM::NoteType::NOTE,
                                  .m_timestamp  = 0.0,
                                  .m_trackIndex = 0 });
    const auto hold = scene.add({ .m_type       = MMM::NoteType::HOLD,
                                  .m_timestamp  = 0.0,
                                  .m_duration   = scene.timeAtY(120.0F),
                                  .m_trackIndex = 2 });
    // 先生成一次快照，再比较所有输出层，避免使用不同帧状态。
    scene.render();
    glm::vec4 tapBounds{}, headBounds{}, endBounds{};
    // UV 身份、顶点数量和数值尺寸都要检查，防止空输出或误用同一纹理通过。
    bool ok = check(quadBounds(scene.m_snapshot, NOTE_UV, tapBounds),
                    "单键应提交四点");
    ok &= check(quadBounds(scene.m_snapshot, HEAD_UV, headBounds),
                "长条头应提交独立四点");
    ok &= check(quadBounds(scene.m_snapshot, END_UV, endBounds),
                "长条尾应提交独立四点");
    // 这些宽高直接来自固定轨宽和原始 UV 比例，不读取生产缩放 helper。
    // 当 Batcher 和命中框都犯相同倍率错误时，独立常量仍能识别回归。
    ok &= check(near(tapBounds.z, BASE_NOTE_WIDTH * 0.5F) &&
                    near(tapBounds.w, BASE_NOTE_HEIGHT * 0.5F),
                "单键倍率未保持 0.5");
    ok &= check(near(headBounds.z, BASE_NOTE_WIDTH * 2.0F) &&
                    near(headBounds.w, BASE_NOTE_HEIGHT * 2.0F),
                "长条头倍率未保持 2");
    ok &= check(near(endBounds.z, BASE_NOTE_WIDTH * endScale) &&
                    near(endBounds.w, BASE_NOTE_HEIGHT * endScale),
                "长条尾倍率不独立");
    // 独立头部应是单键四倍宽高，验证两个资源键没有按相同路径合并。
    // 教程使用普通 Note 的尺寸，不应随 HoldHead 或 HoldEnd 变大。
    ok &= check(near(scene.m_snapshot.playerNoteWidth, tapBounds.z) &&
                    near(scene.m_snapshot.playerNoteHeight, tapBounds.w),
                "教程框与单键纹理不一致");
    ok &=
        check(matchesHitbox(scene.m_snapshot, tap, HoverPart::Head, tapBounds),
              "单键拾取框不匹配");
    ok &= check(
        matchesHitbox(scene.m_snapshot, hold, HoverPart::Head, headBounds),
        "长条头拾取框不匹配");
    ok &= check(
        matchesHitbox(scene.m_snapshot, hold, HoverPart::HoldEnd, endBounds),
        "长条尾拾取框不匹配");
    return ok;
}

/// @brief 验证端点中心离屏但放大边缘仍在屏内时保留实际尾部几何。
/// @note 原始尾部半高为十，此处中心越界十五或二十五，旧剔除会删除它。
/// @param endScale 本轮 Lua 中的尾部倍率，为二或三。
/// @return 实际四点存在、中心未变且五像素边缘可见时返回真。
/// @note 头部仍位于画布内，保证测试焦点是端点提前剔除而非整物件消失。
bool testVisibleEnlargedTail(float endScale)
{
    Scene       scene;
    const float centerY = 5.0F - BASE_NOTE_HEIGHT * endScale * 0.5F;
    // 目标下边缘固定为五像素，使两个倍率都明确覆盖视口顶部。
    const auto hold = scene.add({ .m_type       = MMM::NoteType::HOLD,
                                  .m_timestamp  = 0.0,
                                  .m_duration   = scene.timeAtY(centerY),
                                  .m_trackIndex = 1 });
    scene.render();
    glm::vec4 bounds{};
    // 应提交完整贴片并交给裁剪处理，而非把图元位置强行钳回视口。
    bool ok = check(quadBounds(scene.m_snapshot, END_UV, bounds),
                    "放大尾部在屏边被提前剔除");
    // 先验证场景确实越过原半高，防止输入变化令回归条件意外失效。
    ok &= check(centerY < -BASE_NOTE_HEIGHT * 0.5F &&
                    near(bounds.y + bounds.w * 0.5F, centerY) &&
                    near(bounds.y + bounds.w, 5.0F),
                "尾部没有保留离屏中心与可见边缘");
    ok &=
        check(matchesHitbox(scene.m_snapshot, hold, HoverPart::HoldEnd, bounds),
              "屏边尾部命中框不匹配");
    return ok;
}

/// @brief 验证斜向折线仅加宽两端横截面，连接轨差和时间跨度保持不变。
/// @note 两个普通节点只有一段竖向纹理连接，确保四点统计不存在歧义。
/// @return 斜段 AABB 与最终四点一致且时轨跨度不受倍率影响时返回真。
/// @note 玩家轨零与轨二中心分别是 160 和 480，均来自固定布局常量。
/// @note 只检测现有轴对齐拾取契约，不把测试扩展为多边形命中功能。
bool testPolylineCrossSection()
{
    Scene                     scene;
    MMM::Logic::NoteComponent note;
    note.m_type = MMM::NoteType::POLYLINE;
    // 两个节点分属不同时轨位置；同轨直线无法发现误缩放横向跨度。
    note.m_subNotes.push_back(
        { .type = MMM::NoteType::NOTE, .timestamp = 0.0, .trackIndex = 0 });
    note.m_subNotes.push_back({ .type       = MMM::NoteType::NOTE,
                                .timestamp  = scene.timeAtY(120.0F),
                                .trackIndex = 2 });
    const auto entity = scene.add(note);
    scene.render();
    glm::vec4 bounds{};
    // 两端中心相隔 320 像素，原始截面 30 像素乘二后应为 60。
    // 若把整个斜段 AABB 等比缩放，宽高都会错误变成双倍。
    bool ok = check(quadBounds(scene.m_snapshot, BODY_UV, bounds),
                    "折线连接体应提交四点");
    ok &= check(near(bounds.x, 130.0F) && near(bounds.z, 380.0F) &&
                    near(bounds.y, 120.0F) && near(bounds.w, 180.0F),
                "折线倍率改变了轨差或时间跨度");
    ok &= check(
        matchesHitbox(scene.m_snapshot, entity, HoverPart::HoldBody, bounds),
        "折线连接体 AABB 不匹配");
    return ok;
}

/// @brief 写入仅含本测试倍率的皮肤，再通过正式加载器应用。
/// @note 资源使用人工 UV，因此不需要真实图片或纹理解码器。
/// @warning 仅写入 CTest 指定的构建输出目录，不修改源码资源。
/// @param path 当前用例专用 Lua 夹具路径。
/// @param translations 应用翻译目录，生产加载器按只读方式使用。
/// @param endScale 当前尾部倍率；其余纹理键在各轮之间保持一致。
/// @return 文件完整写入并经生产配置加载器读取成功时返回真。
/// @note 重用同一路径也验证重新加载后没有保留上一轮尾部倍率。
bool loadTestSkin(const std::filesystem::path& path,
                  const std::filesystem::path& translations, float endScale)
{
    std::ofstream file(path, std::ios::binary);
    if ( !file ) return false;
    file << "return { meta = { name = 'Bounds Test' }, assets = {}, "
            "texture_scales = { ['note.note'] = 0.5, ['note.holdhead'] = 2, "
            "['note.holdbodyvertical'] = 2, ['note.holdend'] = "
         << endScale << " } }\n";
    // 先关闭文件再交给 Lua 读取，确保验证的是本轮完整写入的脚本。
    file.close();
    return file.good() && MMM::Config::SkinManager::instance().loadSkin(
                              MMM::Config::pathToUtf8(path), translations);
}
}  // namespace

/// @brief 执行无 GPU 的纹理缩放几何、拾取与屏边可见性回归。
/// @param argc 必须提供专用输出目录与应用翻译目录。
/// @param argv 第一参数只写测试夹具，第二参数只读生产翻译资源。
/// @return 全部场景通过返回零，输入或断言失败返回一。
int main(int argc, char** argv)
{
    if ( !check(argc == 3, "需要输出目录与翻译目录") ) return 1;
    const auto      output       = MMM::Config::utf8ToPath(argv[1]);
    const auto      translations = MMM::Config::utf8ToPath(argv[2]);
    std::error_code error;
    // 使用错误码分支，不依赖异常，也不清理或覆盖其他测试产物。
    std::filesystem::create_directories(output, error);
    if ( !check(!error, "无法创建测试输出目录") ) return 1;
    // 失败仍执行其他独立场景，最终统一返回非零以便 CTest 展示全部差异。
    bool ok = true;
    // 每轮重新加载皮肤但不更改布局，倍率缓存串用将反映为明确的尺寸失败。
    for ( const float endScale : { 2.0F, 3.0F } ) {
        if ( !check(
                 loadTestSkin(
                     output / "note-bounds-skin.lua", translations, endScale),
                 "无法加载纹理倍率夹具") )
            return 1;
        // 两个尾倍率共用相同头倍率，能发现资源键串用或重复乘倍率。
        ok &= testPartBounds(endScale);
        ok &= testVisibleEnlargedTail(endScale);
    }
    // 折线沿用最后一轮皮肤，其中连接体始终明确配置二倍厚度。
    ok &= testPolylineCrossSection();
    if ( ok ) XINFO("Note texture scale bounds tests passed");
    return ok ? 0 : 1;
}
