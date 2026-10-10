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
/// @brief 横向 Body 使用较薄的贴图，检测误用头部高度计算偏移。
constexpr glm::vec4 HORIZONTAL_UV{ 0.65F, 0.2F, 0.05F, 0.01F };
/// @brief 箭头相对头部更窄，纵向高度独立于连接体。
constexpr glm::vec4 ARROW_UV{ 0.85F, 0.2F, 0.04F, 0.025F };
/// @brief 判定区比例与 Note 相同，但配置倍率不同，需按自身高度对齐。
constexpr glm::vec4 JUDGE_UV{ 0.3F, 0.3F, 0.1F, 0.025F };
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
/// @param expectedVertices 期望同类图元顶点数，单个部件为四，每轨判定区共十六。
/// @return 有限顶点数与预期一致时成功；空输出不能被当作通过。
/// @note 不检查 GPU 可见片元，离屏中心的贴片仍应保留完整 CPU 四点。
/// @note 本函数不按颜色筛选，缺省皮肤颜色不会导致漏计纹理。
/// @note 每个场景无选中高亮，同一 UV 不应由多个图元共同使用。
bool quadBounds(const MMM::Logic::RenderSnapshot& snapshot, const glm::vec4& uv,
                glm::vec4& bounds, std::size_t expectedVertices = 4)
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
    return count == expectedVertices;
}

/// @brief 验证指定实体的部件命中框与最终四点边界重合。
/// @note 本测试使用 Stretch 填充，布局矩形就是最终贴片矩形。
/// @param snapshot 同一轮 CPU 渲染输出，不能混用另一轮的命中数据。
/// @param entity 拾取身份对应的根音符实体。
/// @param part 被测端点或连接体部位。
/// @param bounds 根据最终纹理顶点独立提取的矩形。
/// @param subIndex 可选节点身份，负值表示不区分子索引。
/// @return 对应命中框存在且四个边界参数全部一致时成功。
/// @note 折线用例只有一段连接体，因此无需再用子索引消除歧义。
bool matchesHitbox(const MMM::Logic::RenderSnapshot& snapshot,
                   entt::entity entity, HoverPart part, const glm::vec4& bounds,
                   int subIndex = -1)
{
    const auto hit =
        std::find_if(snapshot.hitboxes.begin(),
                     snapshot.hitboxes.end(),
                     [entity, part, subIndex](const auto& value) {
                         return value.entity == entity && value.part == part &&
                                (subIndex < 0 || value.subIndex == subIndex);
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
        // Body、箭头、判定区各自独立 UV，完整快照测试可按身份提取边界。
        m_snapshot.uvMap.emplace(
            static_cast<uint32_t>(TextureID::HoldBodyHorizontal),
            HORIZONTAL_UV);
        m_snapshot.uvMap.emplace(
            static_cast<uint32_t>(TextureID::FlickArrowLeft), ARROW_UV);
        m_snapshot.uvMap.emplace(
            static_cast<uint32_t>(TextureID::FlickArrowRight), ARROW_UV);
        m_snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::JudgeArea),
                                 JUDGE_UV);
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
    /// @param time 本次画布显示时间，允许验证自动播放的驻留头部。
    /// @param preview 是否切换到正式预览区绘制路径。
    /// @param timeline 是否切换到时间线，优先于预览标识。
    /// @param width 视口宽度，用于覆盖辅助窗口缩放后的物件布局。
    void render(double time = 0.0, bool preview = false, bool timeline = false,
                float width = WIDTH)
    {
        // 索引指针只借用成员向量，生命周期覆盖本次同步快照生成。
        m_notes.ctx().emplace<const std::vector<entt::entity>*>(&m_sorted);
        MMM::Logic::System::NoteRenderSystem::generateSnapshot(
            m_notes,
            m_samples,
            {},
            {},
            m_timeline,
            {},
            &m_snapshot,
            timeline  ? "Timeline"
            : preview ? "Preview"
                      : "Basic2DCanvas",
            time,
            width,
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

/// @brief 验证长条 Body 按实际头尾中心连接，而不是沿未移动的时间位置伸出。
/// @param polyline 是否走折线单竖段入口，普通长条另有独立绘制与拾取流程。
/// @param bottom 是否启用底边定位；中心模式需保持旧画面。
/// @param fit 是否放大纵向布局并使用 Fit，覆盖点贴图的可见尺寸换算。
/// @param preview 是否在预览区验证独立的缩放和投影参数。
/// @param holding 是否推进到长条中途，验证驻留头部仍与 Body 相接。
/// @param reverse 是否使用负 Scroll，验证几何不依赖屏幕上的首尾顺序。
/// @return 实际四点接入两端中心，且可编辑状态下拾取包络同步时返回真。
/// @note 头尾倍率不同；统一减去某一个半高不能使两侧同时通过。
/// @note 期望中心从正式输出的点贴图读取，不复用被测偏移 helper。
/// @note 一个场景只有一段竖向 Body，四点断言也能排除重复或空输出。
/// @note 主画布和预览使用相同谱面数据，但独立布局参数决定最终贴图尺寸。
/// @note 用例不启动音频或 GPU，仅验证渲染器提交的 CPU 端点与包络。
/// @note 自动判定的完成消隐由其他回归保护，这里只取时长四分之一处。
bool testVerticalBodyPosition(bool polyline, bool bottom, bool fit,
                              bool preview, bool holding, bool reverse)
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        bottom ? MMM::Config::NoteTexturePosition::Bottom
               : MMM::Config::NoteTexturePosition::Center;
    if ( fit ) {
        // Fit 的纵向留白应被排除，不能直接使用布局框的一半高度。
        scene.m_config.visual.noteScaleY = 2;
        scene.m_config.visual.noteFillMode =
            MMM::Config::BackgroundFillMode::AspectFit;
    }
    if ( reverse ) {
        // 使用真实时间线和 ScrollCache 产生逆向投影，避免手工反转测试顶点。
        const auto scroll = scene.m_timeline.create();
        scene.m_timeline.emplace<MMM::Logic::TimelineComponent>(
            scroll,
            MMM::Logic::TimelineComponent{ .m_timestamp = 0,
                                           .m_effect =
                                               MMM::TimingEffect::SCROLL,
                                           .m_value = -1 });
        scene.m_timeline.ctx().get<MMM::Logic::System::ScrollCache>().rebuild(
            scene.m_timeline, scene.m_config, nullptr);
    }
    const double              duration = scene.timeAtY(reverse ? 480 : 120);
    MMM::Logic::NoteComponent note;
    note.m_type = polyline ? MMM::NoteType::POLYLINE : MMM::NoteType::HOLD;
    note.m_trackIndex = 1;
    note.m_duration   = duration;
    if ( polyline )
        note.m_subNotes.push_back({ .type       = MMM::NoteType::HOLD,
                                    .timestamp  = 0,
                                    .duration   = duration,
                                    .trackIndex = 1 });
    // 独立长条和单竖段折线保留不同身份，以覆盖两套拾取框生成路径。
    // 尾部的三倍皮肤倍率在主函数最后一次夹具加载后保持稳定。
    const auto entity = scene.add(note);
    // 自动播放阶段的头部应驻留在判定区；暂停时仍按原始节点时刻投影。
    scene.m_snapshot.isPlaying             = holding;
    scene.m_config.visual.simulateAutoplay = holding;
    scene.render(holding ? duration * 0.25 : 0, preview);
    glm::vec4 body{}, head{}, tail{};
    bool      ok = check(quadBounds(scene.m_snapshot, BODY_UV, body),
                         "竖向 Hold 缺少 Body");
    ok &= check(quadBounds(scene.m_snapshot, HEAD_UV, head),
                "竖向 Hold 缺少头部");
    ok &=
        check(quadBounds(scene.m_snapshot, END_UV, tail), "竖向 Hold 缺少尾部");
    // 不假定头部总在下方，负流速时应保留相反的几何顺序。
    // 点贴图中心是独立的可见连接目标，不从 Body 顶点反推期待位置。
    // 只平移整个主体会在头尾尺寸不同的场景失败，两个端点须分别成立。
    // 最小与最大中心保护正向和反向顺序，宽高比不决定连接方向。
    // 中心模式也运行同一断言，防止新补偿改变默认布局。
    // 播放中的驻留头使用同一资源，不能改成较小的中间 Node 高度。
    // 预览尺寸来自正式相机路径，未强制复用主画布宽高。
    const float headCenter = head.y + head.w / 2;
    const float tailCenter = tail.y + tail.w / 2;
    ok &= check(near(body.y, std::min(headCenter, tailCenter)) &&
                    near(body.y + body.w, std::max(headCenter, tailCenter)),
                "竖向 Body 未接入头尾视觉中心");
    // 预览不提供可编辑命中框；播放也不在此测试交互副作用。
    if ( !preview && !holding )
        ok &= check(matchesHitbox(scene.m_snapshot,
                                  entity,
                                  HoverPart::HoldBody,
                                  body,
                                  polyline ? 0 : -1),
                    "竖向 Body 拾取框没有同步补偿");
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
/// @param bottom 是否采用底边模式，连接两端的实际贴图高度不同。
/// @return 斜段 AABB 与最终四点一致且横向跨度不受倍率影响时返回真。
/// @note 玩家轨零与轨二中心分别是 160 和 480，均来自固定布局常量。
/// @note 只检测现有轴对齐拾取契约，不把测试扩展为多边形命中功能。
bool testPolylineCrossSection(bool bottom)
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        bottom ? MMM::Config::NoteTexturePosition::Bottom
               : MMM::Config::NoteTexturePosition::Center;
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
                    near(bounds.y, bottom ? 100.0F : 120.0F) &&
                    near(bounds.w, 180.0F),
                "折线倍率改变了轨差或时间跨度");
    ok &= check(
        matchesHitbox(scene.m_snapshot, entity, HoverPart::HoldBody, bounds),
        "折线连接体 AABB 不匹配");
    return ok;
}

/// @brief 验证底边模式在完整主画布路径中同步图元、拾取框和屏边剔除。
/// @param endScale 当前皮肤的独立长条尾倍率。
/// @return 所有点贴图底边落在对应时间位置，拾取框与实际几何一致时返回真。
/// @note 尾部底边仅露出十像素，覆盖移动后的视口交集判断。
/// @note 每个场景只生成一次快照，避免混用不同帧的顶点和拾取框。
/// @note 根实体身份保持不变，折线子节点以索引一区别首节点。
/// @note 场景采用 Stretch，让显示矩形与既有命中框契约完全重合。
/// @note 不启用音效、窗口或 GPU，失败可由 CPU 几何独立定位。
/// @note 单键、长条头尾与折线节点分别使用不同尺寸，覆盖最终尺寸偏移。
bool testBottomPosition(float endScale)
{
    Scene scene;
    // 只切换待测位置，其他布局参数继续沿用中心模式夹具。
    // 模拟暂停保证头部不会随自动播放贴到判定线上。
    scene.m_config.visual.noteTexturePosition =
        MMM::Config::NoteTexturePosition::Bottom;
    const auto tap  = scene.add({ .m_type       = MMM::NoteType::NOTE,
                                  .m_timestamp  = 0.0,
                                  .m_trackIndex = 0 });
    const auto hold = scene.add({ .m_type       = MMM::NoteType::HOLD,
                                  .m_timestamp  = 0.0,
                                  .m_duration   = scene.timeAtY(10.0F),
                                  .m_trackIndex = 2 });
    // 折线与其他物件不在同轨，避免重叠标识生成额外贴图片段。
    MMM::Logic::NoteComponent polyline;
    polyline.m_type = MMM::NoteType::POLYLINE;
    polyline.m_subNotes.push_back(
        { .type = MMM::NoteType::NOTE, .timestamp = 0.0, .trackIndex = 1 });
    polyline.m_subNotes.push_back({ .type       = MMM::NoteType::NOTE,
                                    .timestamp  = scene.timeAtY(120.0F),
                                    .trackIndex = 1 });
    // 节点位于第二条玩家轨的中心，和单键、长条没有视觉重叠。
    // 折线根时间仍是零，保持 Scene 要求的非降序观察索引。
    const auto node = scene.add(polyline);
    scene.render();
    // 三个 UV 区域各只出现一个图元，四点缺失或重复都会导致失败。
    // 长条头与折线头共享区域，在后面改用实体专属命中记录验证。
    glm::vec4 tapBounds{}, endBounds{}, nodeBounds{};
    bool      ok = check(quadBounds(scene.m_snapshot, NOTE_UV, tapBounds),
                         "底边单键缺少几何");
    ok &= check(quadBounds(scene.m_snapshot, END_UV, endBounds),
                "底边尾部被提前剔除");
    ok &= check(quadBounds(scene.m_snapshot, NODE_UV, nodeBounds),
                "底边节点缺少几何");
    // 期望边界由独立布局常量推导，不读取生产位置 helper。
    ok &= check(near(tapBounds.y + tapBounds.w, JUDGMENT_Y) &&
                    near(endBounds.y + endBounds.w, 10.0F) &&
                    near(endBounds.w, BASE_NOTE_HEIGHT * endScale) &&
                    near(nodeBounds.y + nodeBounds.w * 0.5F, 100.0F),
                "点贴图底边或节点参考中心未对齐");
    ok &=
        check(matchesHitbox(scene.m_snapshot, tap, HoverPart::Head, tapBounds),
              "底边单键拾取框错位");
    ok &= check(
        matchesHitbox(scene.m_snapshot, hold, HoverPart::HoldEnd, endBounds),
        "底边尾部拾取框错位");
    ok &= check(
        matchesHitbox(
            scene.m_snapshot, node, HoverPart::PolylineNode, nodeBounds, 1),
        "底边节点拾取框错位");
    // 长条头与折线头共用纹理，因此用固定轨中心检查实体专属命中框。
    ok &= check(matchesHitbox(scene.m_snapshot,
                              hold,
                              HoverPart::Head,
                              { 360.0F, 260.0F, 240.0F, 40.0F }),
                "底边长条头未按最终尺寸定位");
    // 同一测试在两种尾倍率下重复执行，偏移不能缓存第一轮尺寸。
    // 屏边可见性与拾取边界同时通过才算满足底边位置契约。
    // 构建目录夹具由调用方提供，测试不触碰源码资源或个人皮肤。
    return ok;
}

/// @brief 验证非等比缩放下 Fit 留白不参与底边位移。
/// @return 可见底边准确对齐，既有布局拾取框中心跟随贴图时返回真。
/// @note 拾取框保留原布局宽高，不要求缩到 Fit 图像的非留白区域。
/// @note 单键倍率为一半，最终可见高度为十五，而布局框高度为四十。
/// @note 明确放大纵向布局以触发宽度约束，默认夹具不会覆盖该分支。
/// @note 可见底边由实际顶点计算，命中框则按实体身份独立查找。
/// @note 两者分别断言，防止绘制和拾取同时使用错误布局高度而伪通过。
/// @note 不检查透明像素或纹理过滤，定位契约以提交的四边形为准。
bool testBottomAspectFit()
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        MMM::Config::NoteTexturePosition::Bottom;
    scene.m_config.visual.noteScaleY = 2.0F;
    scene.m_config.visual.noteFillMode =
        MMM::Config::BackgroundFillMode::AspectFit;
    const auto tap = scene.add({ .m_type       = MMM::NoteType::NOTE,
                                 .m_timestamp  = 0.0,
                                 .m_trackIndex = 0 });
    scene.render();
    glm::vec4 bounds{};
    // 原图宽高比为四；最终宽六十对应显示高十五，底边仍在判定时间。
    bool ok = check(quadBounds(scene.m_snapshot, NOTE_UV, bounds),
                    "Fit 单键未提交四点");
    ok &= check(near(bounds.w, 15.0F) && near(bounds.y + bounds.w, JUDGMENT_Y),
                "Fit 留白被计入底边位移");
    // 既有拾取布局宽六十高四十，只移动实际显示半高七点五。
    // 图像中心和布局框中心均应落到二百九十二点五，而非二百八十。
    ok &= check(matchesHitbox(scene.m_snapshot,
                              tap,
                              HoverPart::Head,
                              { 130.0F, 272.5F, 60.0F, 40.0F }),
                "Fit 拾取框未跟随显示中心");
    return ok;
}

/// @brief 验证横向连接体两端落在实际可见头部与终点的视觉中心。
/// @param snapshot 正式渲染入口输出，场景必须只包含一条横向 Body。
/// @param leftY 左端点期望中心，不依赖左右滑动方向。
/// @param rightY 右端点期望中心。
/// @param thickness 皮肤缩放后的横向连接体厚度。
/// @return 四点两侧的中点和厚度均一致时返回真。
/// @note 只检查包络会漏掉连接斜率反向，因此分别校验两侧顶点。
bool checkBodyConnections(const MMM::Logic::RenderSnapshot& snapshot,
                          float leftY, float rightY, float thickness)
{
    std::vector<glm::vec2> points;
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.uv.u >= HORIZONTAL_UV.x &&
             vertex.uv.u <= HORIZONTAL_UV.x + HORIZONTAL_UV.z &&
             vertex.uv.v >= HORIZONTAL_UV.y &&
             vertex.uv.v <= HORIZONTAL_UV.y + HORIZONTAL_UV.w )
            points.push_back({ vertex.pos.x, vertex.pos.y });
    }
    // Batcher 顺序为左下、右下、右上、左上；端点中线不随厚度缩放移动。
    if ( points.size() != 4 ) return false;
    return near((points[0].y + points[3].y) / 2, leftY) &&
           near((points[1].y + points[2].y) / 2, rightY) &&
           near(points[0].y - points[3].y, thickness) &&
           near(points[1].y - points[2].y, thickness);
}

/// @brief 验证独立滑键及折线内滑键的 Body 按可见贴图中心连接。
/// @param polyline 是否通过折线根与子节点路径绘制。
/// @param direction 正负一分别覆盖向右和向左的真实轨道跨度。
/// @param bottom 是否启用底边；中心路径用于保护原有几何契约。
/// @param fit 是否使用收缩后的实际图像高度，不能按布局留白计算连接点。
/// @return 连接点、横向跨度及主体命中框与预期一致时返回真。
/// @note 头部高度与箭头高度不同，箭头仍跟随起点中心，横段始终水平。
/// @param preview 是否检查缩小后的预览画布。
/// @param interior 是否在折线中间放置 Flick，起点采用 Node 而非根头贴图。
/// @note 所有期望来自最终顶点或独立布局常量，未调用生产位移 helper。
bool testFlickBodyPosition(bool polyline, int direction, bool bottom, bool fit,
                           bool preview, bool interior)
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        bottom ? MMM::Config::NoteTexturePosition::Bottom
               : MMM::Config::NoteTexturePosition::Center;
    if ( fit ) {
        // 放大纵向布局使 Fit 留白明显，防止 Stretch 恰好掩盖错误。
        scene.m_config.visual.noteScaleY = 2;
        scene.m_config.visual.noteFillMode =
            MMM::Config::BackgroundFillMode::AspectFit;
    }
    MMM::Logic::NoteComponent note;
    note.m_type = polyline ? MMM::NoteType::POLYLINE : MMM::NoteType::FLICK;
    note.m_trackIndex = direction > 0 ? 0 : 2;
    note.m_dtrack     = direction;
    // 单节点折线只包含自身横向主体，不引入第二段连接纹理或重复箭头。
    if ( polyline && interior ) {
        // 先放较早节点，最后的横滑以独立 Node 作为起点参考。
        // 中间节点与箭头都小于根头部，能发现只修复单节点折线的遗漏。
        note.m_subNotes.push_back({ .type       = MMM::NoteType::NOTE,
                                    .timestamp  = -0.1,
                                    .trackIndex = note.m_trackIndex });
    }
    if ( polyline )
        note.m_subNotes.push_back({ .type       = MMM::NoteType::FLICK,
                                    .timestamp  = 0.0,
                                    .trackIndex = note.m_trackIndex,
                                    .dtrack     = direction });
    const auto entity = scene.add(note);
    scene.render(0.0, preview);
    // 左右箭头共用合成 UV，但单次场景只绘制一个方向，不混用顶点。
    // 四点约束还可检测误把主体绘制了两遍或切换状态导致的重复图元。
    glm::vec4 body{}, arrow{}, head{};
    bool      ok = check(quadBounds(scene.m_snapshot, HORIZONTAL_UV, body),
                         "滑键 Body 缺少四点");
    ok &= check(quadBounds(scene.m_snapshot, ARROW_UV, arrow),
                "滑键箭头缺少四点");
    ok &=
        check(quadBounds(scene.m_snapshot, interior ? NODE_UV : HEAD_UV, head),
              "滑键头部缺少四点");
    // 预览尺寸按自己的轨宽变化，水平性由两端实际中心的独立断言保证。
    // 原始厚度取 UV 高比 0.4，独立皮肤倍率二，只改变截面不改端点。
    const float thickness  = preview ? body.w : fit ? 64.0F : 16.0F;
    const float headCenter = head.y + head.w / 2;
    const float endCenter  = arrow.y + arrow.w / 2;
    ok &= check(checkBodyConnections(scene.m_snapshot,
                                     direction > 0 ? headCenter : endCenter,
                                     direction > 0 ? endCenter : headCenter,
                                     thickness),
                "横向 Body 未连接头部和箭头中心");
    // 实际头部和箭头中心必须共线，不能用包络变高掩盖错误斜率。
    // 端点中心由实际图元反查，不重复生产代码的位移计算公式。
    // Fit 时头部与箭头各自收缩，不能把 Stretch 的预期平移直接套用。
    // Body 仍保持原厚度规则，不随点贴图的 Fit 高度一起变细。
    ok &= check(near(headCenter, endCenter), "箭头和起点中心不共线");
    ok &= check(near(body.w, thickness), "横向 Body 包络高度错误");
    // 横向偏移只由轨号决定，底边设置不能缩短或延长轨间跨度。
    // 两侧轨道中心相距一百六十像素，不受头部独立倍率二影响。
    // 在向左场景中左端属于箭头，可检测将起终点顺序固定写死的实现。
    if ( !preview )
        ok &= check(near(body.x, direction > 0 ? 160.0F : 320.0F) &&
                        near(body.z, 160.0F),
                    "Body 横向跨度改变");
    if ( bottom && !interior ) {
        // 起点保持底边定位，箭头的纵向位置则以起点的视觉中心为准。
        // 箭头仍有自己的尺寸和倍率，不需要将它拉伸到头部相同高度。
        ok &= check(near(head.y + head.w, JUDGMENT_Y),
                    "滑键头部底边没有保持时间锚点");
    }
    // 折线主体携带子索引零；独立滑键使用根实体的普通部件身份。
    // 拾取不要求裁掉 Fit 留白，但横向 Body 的自由四角必须完整被命中框包住。
    // 每个场景重新创建索引，不能靠上一模式残留的快照使断言通过。
    if ( !preview )
        ok &= check(matchesHitbox(scene.m_snapshot,
                                  entity,
                                  HoverPart::HoldBody,
                                  body,
                                  polyline ? (interior ? 1 : 0) : -1),
                    "滑键 Body 拾取框未同步位置");
    // Fit 的拾取保留布局留白，只在 Stretch 时要求箭头图元和命中完全重合。
    // 预览不生成编辑命中框，正式主画布路径仍需覆盖箭头身份和子索引。
    if ( !preview && !fit )
        ok &= check(matchesHitbox(scene.m_snapshot,
                                  entity,
                                  HoverPart::FlickArrow,
                                  arrow,
                                  polyline ? (interior ? 1 : 0) : -1),
                    "箭头拾取没有同步起点中心");
    return ok;
}

/// @brief 验证同刻横滑接入长条时，Body 的终点使用 Node 的视觉中心。
/// @param bottom 是否以底边定位头部，中心模式必须保持既有拓扑。
/// @param preview 是否通过预览画布生成正式渲染几何。
/// @param fit 是否验证有宽高约束的填充模式。
/// @return 横向连接、节点参考中心及主画布子段命中框符合预期时返回真。
/// @note 该拓扑的横段没有末端箭头，不能用不存在的箭头尺寸补偿。
/// @note 竖向长条与横向连接使用不同 UV，互不干扰四点统计。
bool testFlickHoldConnection(bool bottom, bool preview, bool fit)
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        bottom ? MMM::Config::NoteTexturePosition::Bottom
               : MMM::Config::NoteTexturePosition::Center;
    if ( fit )
        scene.m_config.visual.noteFillMode =
            MMM::Config::BackgroundFillMode::AspectFit;
    MMM::Logic::NoteComponent note;
    note.m_type = MMM::NoteType::POLYLINE;
    // 横滑从第一轨抵达第二轨，长条从同一时间和同一终点继续。
    // 首贴图的皮肤倍率为二，Node 没有额外倍率，仍需共用视觉中心。
    note.m_subNotes.push_back({ .type       = MMM::NoteType::FLICK,
                                .timestamp  = 0,
                                .trackIndex = 0,
                                .dtrack     = 1 });
    note.m_subNotes.push_back({ .type       = MMM::NoteType::HOLD,
                                .timestamp  = 0,
                                .duration   = 0.1,
                                .trackIndex = 1 });
    const auto entity = scene.add(note);
    scene.render(0.0, preview);
    glm::vec4 body{}, head{}, node{};
    bool      ok = check(quadBounds(scene.m_snapshot, HORIZONTAL_UV, body),
                         "接长条横段缺少 Body");
    ok &= check(quadBounds(scene.m_snapshot, HEAD_UV, head),
                "接长条横段缺少头部");
    ok &= check(quadBounds(scene.m_snapshot, NODE_UV, node),
                "接长条横段缺少终点 Node");
    // 用实际节点顶点确定连接高度，不能复用末端 HoldEnd 或箭头的尺寸。
    // 横段子索引零保持编辑身份，节点仍属于索引一。
    // Node 的 UV 高度与头部不同，能够区分资源身份选错和整体位移错误。
    // 长条尾部稍后结束，测试时节点仍可见，不依赖播放中的消隐分支。
    // 主体厚度为独立倍率后的十六像素，不能取 Node 自身的高度替代。
    // 同刻节点使用前一横段投影，二者应共用时间位置和头部视觉中心。
    // 最终包络来自 Body 顶点，拾取比较不会把同一个错误公式当作期望。
    // 预览按自己的画布比例缩小截面，只要求四角与两个中心连通。
    // 主画布的固定厚度另行断言，防止错误节点位置被主体增厚掩盖。
    if ( !preview ) ok &= check(near(body.w, 16.0F), "同刻横段厚度错误");
    ok &= check(
        checkBodyConnections(
            scene.m_snapshot, head.y + head.w / 2, node.y + node.w / 2, body.w),
        "横段未连接实际 Node 中心");
    ok &= check(near(node.y + node.w * 0.5F, head.y + head.w * 0.5F),
                "同刻横段被节点的独立高度位移画斜");
    // 预览只有导航能力，不生成 Note 编辑命中框。
    // 主画布仍验证节点及连接体各自的交互身份与实际位置。
    if ( !preview ) {
        ok &= check(matchesHitbox(
                        scene.m_snapshot, entity, HoverPart::HoldBody, body, 0),
                    "接长条横段命中框错位");
        if ( !fit )
            ok &= check(
                matchesHitbox(
                    scene.m_snapshot, entity, HoverPart::PolylineNode, node, 1),
                "参考头部偏移后节点命中框错位");
    }
    return ok;
}

/// @brief 验证判定区与 Note 共用对齐方式，但使用自身最终高度。
/// @param bottom 是否选择底边位置，中心模式必须保留旧判定区几何。
/// @return 四轨判定区最终边界与固定时间锚点一致时返回真。
/// @note 判定区皮肤倍率为 1.5，不能直接照搬倍率为 0.5 的 Note 偏移。
/// @note 不改变判定时间或滚动原点，只检查四个轨道图元的可见位置。
bool testJudgmentAreaPosition(bool bottom)
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        bottom ? MMM::Config::NoteTexturePosition::Bottom
               : MMM::Config::NoteTexturePosition::Center;
    scene.render();
    glm::vec4 area{};
    // 无 Note 的场景仍应显示判定区域，排除从某个可见物件借用偏移。
    // 同一快照没有判定区高亮或拾取标记，十六点计数具有唯一来源。
    // 场景四轨，每轨四个顶点；聚合包络仍有相同的上、下边缘。
    bool ok = check(quadBounds(scene.m_snapshot, JUDGE_UV, area, 16),
                    "判定区未覆盖四轨");
    // 中心模式保留上下各十五像素，底边模式完全移到时间锚点上方。
    // 独立比较高度可防止通过缩短判定区来伪造正确的下边缘。
    const float expectedBottom = bottom ? JUDGMENT_Y : JUDGMENT_Y + 15.0F;
    // 原始高二十乘独立倍率 1.5，位置必须在此缩放之后再计算。
    ok &= check(near(area.w, 30.0F) && near(area.y + area.w, expectedBottom),
                "判定区未同步 Note 的位置规则");
    return ok;
}

/// @brief 检查辅助判定框匹配当前视图物件的可见高度及轨道范围。
/// @param bottom 是否选择底边，否则验证中心模式。
/// @param timeline 是否绘制时间线，否则绘制预览区。
/// @param professional 时间线是否启用独立类型轨道。
/// @param fit 是否放大纵向布局并使用 Fit，检测留白与缩放处理。
/// @param width 当前辅助窗口宽度，不借用主画布轨宽。
/// @return 判定框尺寸、定位、逻辑时间和几何区间均正确时返回真。
/// @note 判定框使用独立填充色，不依赖顶点顺序或描边拓扑。
/// @note 将实际物件四点作为高度参考，避免用同一尺寸公式掩盖绘制分歧。
/// @note 普通、专业和预览布局各有轨宽策略，不能共享固定像素高度。
/// @note 测试从正式快照入口生成几何，不自行调用内部尺寸助手。
bool testAuxiliaryPosition(bool bottom, bool timeline, bool professional,
                           bool fit, float width)
{
    Scene scene;
    scene.m_config.visual.noteTexturePosition =
        bottom ? MMM::Config::NoteTexturePosition::Bottom
               : MMM::Config::NoteTexturePosition::Center;
    scene.m_config.settings.professionalMode = professional;
    // 时间线标记采用专用泳道尺寸，不强行套用玩家 Note 的横纵布局倍率。
    // 下方切换填充模式同时验证两条尺寸路径，实际四点始终作为独立参考。
    // 切换位置不能改变主画布布局、事件时间或任何轨道的身份。
    // 不对称边距可区分轨道宽度、视口宽度和主画布的布局宽度。
    scene.m_config.visual.previewConfig.margin.left  = 24.0F;
    scene.m_config.visual.previewConfig.margin.right = 40.0F;
    if ( fit ) {
        // 纵向放大使预览 Note 出现 Fit 留白，判定框不能按原布局高计算。
        scene.m_config.visual.noteScaleY = 2.0F;
        scene.m_config.visual.noteFillMode =
            MMM::Config::BackgroundFillMode::AspectFit;
    }
    // 预览只放一个当前时刻单键，其实际图元是该视图的判定高度参考。
    // 时间线仍只有零时刻 BPM，普通与专业标记均可唯一提取。
    if ( !timeline ) scene.add({ .m_type = MMM::NoteType::NOTE });
    scene.render(0.0, !timeline, timeline, width);
    float       minX  = std::numeric_limits<float>::max();
    float       minY  = minX;
    float       maxX  = std::numeric_limits<float>::lowest();
    float       maxY  = maxX;
    std::size_t count = 0;
    // 填充与边框使用不同颜色，边框宽度不能改变填充尺寸断言。
    // 颜色通过生产皮肤加载器进入快照，不把背景或视野包围盒混进来。
    for ( const auto& vertex : scene.m_snapshot.vertices ) {
        const auto& color = vertex.color;
        if ( !near(color.r, 0.13F) || !near(color.g, 0.27F) ||
             !near(color.b, 0.41F) )
            continue;
        minX = std::min(minX, vertex.pos.x);
        minY = std::min(minY, vertex.pos.y);
        maxX = std::max(maxX, vertex.pos.x);
        maxY = std::max(maxY, vertex.pos.y);
        ++count;
    }
    glm::vec4 marker{};
    bool      ok = check(quadBounds(scene.m_snapshot, NOTE_UV, marker),
                         "辅助视图缺少尺寸参考物件的实际四点");
    // 判定框和物件必须具有相同高度与锚点，而非只共享中心或下边缘。
    // 当前单键与 BPM 都在零时刻，所以可直接比较上下边界。
    ok &= check(count == 4 && near(maxY - minY, marker.w) &&
                    near(minY, marker.y) && near(maxY, marker.y + marker.w),
                "辅助判定框高度未匹配当前视图的物件尺寸");
    const float expectedLeft = timeline ? (professional ? 0.0F : 30.0F) : 24.0F;
    const float expectedRight = timeline ? width - expectedLeft : width - 40.0F;
    // 专业模式的判定框覆盖全部泳道；预览排除左右边距与密度图区域。
    // 横向覆盖与物件自身皮肤缩放独立，不能缩成单个物件的宽度。
    ok &= check(near(minX, expectedLeft) && near(maxX, expectedRight),
                "辅助判定框宽度未覆盖对应轨道范围");
    ok &= check(near(marker.y + marker.w * (bottom ? 1.0F : 0.5F), JUDGMENT_Y),
                "辅助判定框与物件锚点不一致");
    if ( !timeline ) return ok;
    // 贴图位移只作用于图元，零时刻及屏幕吸附锚点必须保持不变。
    // 拖动和高亮通过真实几何区间访问物件，不能回退到固定尺寸标记。
    const auto& elements = scene.m_snapshot.timelineElements;
    ok &= check(elements.size() == 1, "时间线事件数量被位置选项改变");
    if ( elements.size() == 1 ) {
        const auto& element = elements.front();
        ok &= check(element.time == 0.0 && near(element.y, JUDGMENT_Y),
                    "判定框尺寸错误写入了时间线吸附锚点");
        ok &= check(element.bpmMarker.hasMarkerGeometry &&
                        element.bpmMarker.markerVertexCount == 4,
                    "时间线拾取没有发布实际标记几何");
    }
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
            "colors = { preview = { judgment_guide = { "
            "fill = {0.13, 0.27, 0.41, 1}, border = {0.17, 0.31, 0.47, 1} } } "
            "}, "
            "texture_scales = { ['note.note'] = 0.5, ['note.holdhead'] = 2, "
            "['note.holdbodyvertical'] = 2, ['note.holdbodyhorizontal'] = 2, "
            "['note.arrowleft'] = 0.75, ['note.arrowright'] = 1.6, "
            "['panel.track.judgearea'] = 1.5, ['note.holdend'] = "
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
        ok &= testBottomPosition(endScale);
    }
    // 折线沿用最后一轮皮肤，其中连接体始终明确配置二倍厚度。
    ok &= testPolylineCrossSection(false);
    ok &= testPolylineCrossSection(true);
    // 独立 Hold 与折线竖段分别覆盖主画布、预览和播放中的驻留头部。
    // 负流速仅验证暂停投影，自动判定的回流钳制由既有极端 SV 测试负责。
    for ( const bool polyline : { false, true } )
        for ( const bool bottom : { false, true } )
            for ( const bool fit : { false, true } )
                for ( const bool preview : { false, true } ) {
                    ok &= testVerticalBodyPosition(
                        polyline, bottom, fit, preview, false, false);
                    ok &= testVerticalBodyPosition(
                        polyline, bottom, fit, preview, true, false);
                    ok &= testVerticalBodyPosition(
                        polyline, bottom, fit, preview, false, true);
                }
    ok &= testBottomAspectFit();
    // 主画布和缩小预览共用节点接点规则，Fit 也不能引入同刻斜线。
    for ( const bool bottom : { false, true } )
        for ( const bool preview : { false, true } )
            for ( const bool fit : { false, true } )
                ok &= testFlickHoldConnection(bottom, preview, fit);
    // 两个渲染入口、两个方向及两种位置共同保护横向 Body 的接线。
    for ( const bool bottom : { false, true } ) {
        ok &= testJudgmentAreaPosition(bottom);
        // 窗口变窄、物件缩放与 Fit 留白都必须同步到辅助判定框。
        // 专业泳道与普通中央标记区域分别验证，不共用宽度期望。
        for ( const float width : { WIDTH, 320.0F } )
            for ( const bool fit : { false, true } ) {
                ok &= testAuxiliaryPosition(bottom, false, false, fit, width);
                ok &= testAuxiliaryPosition(bottom, true, false, fit, width);
                ok &= testAuxiliaryPosition(bottom, true, true, fit, width);
            }
        for ( const bool polyline : { false, true } ) {
            for ( const int direction : { -1, 1 } ) {
                // 两侧箭头采用不同皮肤倍率；主画布和预览都必须保持横向连接。
                for ( const bool fit : { false, true } )
                    for ( const bool preview : { false, true } ) {
                        ok &= testFlickBodyPosition(
                            polyline, direction, bottom, fit, preview, false);
                        if ( polyline )
                            ok &= testFlickBodyPosition(polyline,
                                                        direction,
                                                        bottom,
                                                        fit,
                                                        preview,
                                                        true);
                    }
            }
        }
    }
    if ( ok ) XINFO("Note texture scale bounds tests passed");
    return ok ? 0 : 1;
}
