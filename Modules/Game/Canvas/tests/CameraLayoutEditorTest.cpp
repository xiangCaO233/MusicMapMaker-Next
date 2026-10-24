#include "canvas/CameraLayoutEditor.h"
#include "canvas/CameraLayoutMath.h"
#include "canvas/PlanarObservationInput.h"
#include "common/render/CanvasThicknessCompensation.h"
#include "common/render/TextureID.h"
#include "config/EditorConfig.h"
#include "config/VisualConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <vector>

namespace MMM::Canvas
{
/// @brief 测试通过模式边沿验证预览生命周期，不初始化 Vulkan。
// 这里只开放测试所需的模式选择。
// 生产位置计算、窗口绘制和生命周期判定均不替换为模拟。
// 测试不访问应用主窗口，也不需要图形设备。
struct CameraLayoutEditorTestAccess {
    /// @brief 仅查询工具选择，不模拟实际快捷键事件处理。
    static bool rotating(const CameraLayoutEditor& editor)
    {
        return editor.m_rotate;
    }
    /// @brief 模拟工具栏模式选择，避免依赖中文字体的控件宽度。
    static void mode(CameraLayoutEditor& editor, bool camera)
    {
        editor.m_cameraMode = camera;
    }
};
}  // namespace MMM::Canvas
namespace
{
/// @brief 延长真实可见范围必须增加远端音符，而不是拉伸现有物件。
/// @return 一倍与三倍画布的物件、裁剪、尺寸及配置往返均正确时返回 true。
/// @note 使用独占 UV 识别音符，不把辅助图元数量误认作物件数量。
/// @note 两轮生成复用同一索引和滚动映射，唯一变量是画布长度。
bool checkCanvasLength()
{
    using namespace MMM;
    // 独立注册表不连接会话或 GPU，直接检查生产快照几何。
    // 每次调用构建全新夹具，不读取用户正在编辑的谱面。
    // 默认速度缓存使用正式 rebuild 路径，不能以简化线性函数替代。
    // 自动游玩关闭，避免物件因判定状态消失而干扰可见范围断言。
    // 拍线单独隐藏，便于把几何差异归因到真实音符。
    entt::registry       notes, samples, timeline;
    Config::EditorConfig config;
    config.visual.canvasCameraPoseEnabled = true;
    config.visual.simulateAutoplay        = false;
    config.visual.beatLineDisplayMode     = Config::BeatLineDisplayMode::Hidden;
    // 真实 BPM 同时供滚动缓存与自动拍线使用，不能用空列表绕过网格生成。
    const auto  bpmEntity = timeline.create();
    const auto& bpm       = timeline.emplace<Logic::TimelineComponent>(
        bpmEntity,
        Logic::TimelineComponent{
            .m_timestamp = 0, .m_effect = TimingEffect::BPM, .m_value = 120 });
    const std::vector<const Logic::TimelineComponent*> bpms{ &bpm };
    auto& cache = timeline.ctx().emplace<Logic::System::ScrollCache>();
    cache.rebuild(timeline, config, nullptr);
    // 缓存逆映射让物件分别落在判定线上方 100 和 1000 像素。
    // 不假定默认 BPM 或滚速，后一个物件必须位于原视口之外。
    // 判定线固定在 550，因此两个头部分别落在 Y=450 和 Y=-450。
    // 三倍高度向远端多出 1200 像素，足以包含完整远端音符。
    // 物件位于同一轨，避免横向轨宽差异掩盖纵向缩放错误。
    // 时间按递增滚动距离插入，与常规正速缓存的排序要求一致。
    std::vector<entt::entity> sortedNotes;
    for ( double distance : { 100.0, 1000.0 } ) {
        const auto           entity = notes.create();
        Logic::NoteComponent note;
        note.m_type       = NoteType::NOTE;
        note.m_timestamp  = cache.getTime(cache.getAbsY(0) + distance);
        note.m_trackIndex = 0;
        notes.emplace<Logic::NoteComponent>(entity, note);
        notes.emplace<Logic::TransformComponent>(entity);
        sortedNotes.push_back(entity);
    }
    // 渲染器借用排序索引，局部向量必须存活到两轮生成结束。
    // TransformComponent 与生产实体保持一致，让候选走正常渲染路径。
    // 不设置 hovered 或 selected，避免描边重复顶点影响四角数量。
    // 两轮都从空快照开始，防止上一轮几何残留假装新增远端内容。
    notes.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);
    // 复用完整生成入口，普通物件、活动画笔和光标拍线必须经历相同布局边界。
    const auto generate = [&](Logic::RenderSnapshot& snapshot) {
        Logic::System::NoteRenderSystem::generateSnapshot(notes,
                                                          samples,
                                                          {},
                                                          {},
                                                          timeline,
                                                          bpms,
                                                          &snapshot,
                                                          "Basic2DCanvas",
                                                          0,
                                                          800,
                                                          600,
                                                          550,
                                                          4,
                                                          0,
                                                          0,
                                                          config,
                                                          600);
    };
    std::vector<glm::vec2> nearVertices;
    // 第三轮启用动态厚度，正面单位距离不应出现旧实现的统一翻倍。
    for ( const auto settings :
          { glm::vec2(1, 0), glm::vec2(3, 0), glm::vec2(3, 100) } ) {
        const float length                        = settings.x;
        config.visual.canvasThicknessCompensation = settings.y;
        config.visual.canvasCameraLength          = length;
        Logic::RenderSnapshot snapshot;
        snapshot.hasBeatmap = true;
        // 音符独占此 UV 区域，底板和纯色线不会被计入。
        // 非正方形 UV 保留真实纹理比例，可发现高度被独立放大的错误。
        // 不依赖磁盘皮肤图片，测试结果不受当前主题或纹理资源影响。
        // 直接调用完整生成入口，同时覆盖布局上界向物件筛选的传递。
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(Logic::TextureID::Note),
            glm::vec4(0.2F, 0.3F, 0.1F, 0.05F));
        generate(snapshot);
        std::vector<glm::vec2> currentNear;
        int                    farCount = 0;
        for ( const auto& vertex : snapshot.vertices ) {
            if ( vertex.uv.u < 0.19F || vertex.uv.u > 0.31F ||
                 vertex.uv.v < 0.29F || vertex.uv.v > 0.36F )
                continue;
            // Tap 为四顶点；负 Y 物件只允许在扩展范围后出现。
            // 只比较 UV 命中的顶点，不能把底板变长误当作物件变多。
            // 近端四角按原始顺序记录，逐点对照同时检查宽高与锚点。
            // 测试不把相机透视后的远小近大误认作源几何缩放。
            if ( vertex.pos.y < 0 )
                ++farCount;
            else
                currentNear.emplace_back(vertex.pos.x, vertex.pos.y);
        }
        if ( currentNear.size() != 4 || farCount != (length > 1 ? 4 : 0) )
            return false;
        if ( length == 1 ) nearVertices = currentNear;
        // 逐顶点相等同时覆盖尺寸、位置和原有时间到像素比例。
        else if ( settings.y == 0 && currentNear != nearVertices )
            return false;
        if ( settings.y > 0 ) {
            // 只提高局部高度，横向位置、宽度及中心时间不能随补偿改变。
            // 不以顶点总数替代尺寸检查，底板变长不能满足此断言。
            for ( std::size_t i = 0; i < currentNear.size(); ++i ) {
                if ( currentNear[i].x != nearVertices[i].x ) return false;
            }
            // 纹理四角按批处理器固定顺序提交，零与二分别位于两条高度边。
            // 正面单位距离的投影已经等厚，满补偿不得再重复加高。
            const float oldHeight =
                std::abs(nearVertices[0].y - nearVertices[2].y);
            const float newHeight =
                std::abs(currentNear[0].y - currentNear[2].y);
            if ( std::abs(newHeight - oldHeight) > 0.01F ) return false;
        }
        if ( length > 1 ) {
            // 只有几何没有有效裁剪仍会不可见，必须检查提交命令。
            // 命令保存逻辑负坐标，渲染录制阶段才转换成非负屏幕矩形。
            // 此断言能捕获批处理器仍把上界截到零的旧行为。
            // 不要求所有批次扩展，背景与界面组件仍可使用屏幕空间。
            if ( !std::any_of(snapshot.cmds.begin(),
                              snapshot.cmds.end(),
                              [](const auto& command) {
                                  return command.scissor.y < 0;
                              }) )
                return false;
        }
    }
    // 在真实斜视相机下生成交互快照，检查实际贴图与拾取使用同一补偿。
    // 复用已有普通音符，可发现完整生产入口漏传投影参数的情况。
    config.visual.canvasCameraPosition = { 0, -0.8F, 0.9F };
    config.visual.canvasCameraRotation = { 45, 0, 0 };
    Logic::RenderSnapshot spatial;
    spatial.hasBeatmap         = true;
    spatial.acceptsInteraction = true;
    // 独立图块使用非正方形比例，高度与轨宽不能被误当作同一个量。
    // 所有音符都使用这块纹理，未配置的高亮图元不会干扰四角匹配。
    spatial.uvMap.emplace(static_cast<std::uint32_t>(Logic::TextureID::Note),
                          glm::vec4(.2F, .3F, .1F, .05F));
    generate(spatial);
    // 两个物件均位于三倍场景内，不能通过丢弃远端物件满足尺寸断言。
    // 拾取开启后主画布必须发布有效的强度与矩阵。
    if ( spatial.hitboxes.size() != 2 ||
         spatial.thicknessCompensation.m_amount != 1 )
        return false;
    for ( const auto& box : spatial.hitboxes ) {
        // 源平面外包框用于现有逆投影拾取，不应拿屏幕坐标直接比较。
        // 逐框计数可发现近端框错误包住远端音符，也能发现只补偿绘制未补偿命中的回归。
        int contained = 0;
        for ( const auto& vertex : spatial.vertices ) {
            // UV 区域只属于 Note，辅助线和底板不能代替真实音符匹配拾取框。
            if ( vertex.uv.u < .19F || vertex.uv.u > .31F ||
                 vertex.uv.v < .29F || vertex.uv.v > .36F )
                continue;
            // 远近音符沿 Y 分离，每个拾取框应恰好覆盖对应贴图的四角。
            // 容差只覆盖浮点误差，不借助用户额外配置的交互倍率扩大命中范围。
            if ( vertex.pos.x >= box.x - .01F &&
                 vertex.pos.x <= box.x + box.w + .01F &&
                 vertex.pos.y >= box.y - .01F &&
                 vertex.pos.y <= box.y + box.h + .01F )
                ++contained;
        }
        if ( contained != 4 ) return false;
    }
    // 后续预览范围检查沿用原来的正面相机，避免厚度设置掩盖可见性问题。
    // 这些配置只属于局部夹具，不写入实际用户设置。
    config.visual.canvasCameraPosition = { 0, 0, 1 };
    config.visual.canvasCameraRotation = { 0, 0, 0 };
    // 操作预览的终点落在二维视口之外，仍应保留扩展范围内的真实半透明几何。
    // 不直接调用内部画笔函数，防止测试绕过范围向预览入口的传递。
    config.visual.canvasThicknessCompensation = 0;
    // 两个时间沿正向滚动递增，长条持续时间必须保持非负。
    // 近端处于原画布内，远端处于扩展区，能识别只画出一半的错误预览。
    // 所有坐标仍用当前滚动缓存，不通过改变速度或平移判定线制造扩展效果。
    const double nearTime = cache.getTime(cache.getAbsY(0) + 100);
    const double farTime  = cache.getTime(cache.getAbsY(0) + 1000);
    for ( auto type : { NoteType::HOLD, NoteType::POLYLINE } ) {
        Logic::RenderSnapshot preview;
        preview.hasBeatmap = true;
        // 独立图块用于分辨正式 Note 与画笔的尾部、节点；不依赖当前磁盘皮肤。
        // 三种图块均为合法正尺寸，缺图不能成为远端几何缺失的替代解释。
        preview.uvMap.emplace(static_cast<uint32_t>(Logic::TextureID::Note),
                              glm::vec4(.2F, .3F, .1F, .05F));
        preview.uvMap.emplace(static_cast<uint32_t>(Logic::TextureID::HoldEnd),
                              glm::vec4(.5F, .3F, .1F, .05F));
        preview.uvMap.emplace(static_cast<uint32_t>(Logic::TextureID::Node),
                              glm::vec4(.6F, .3F, .1F, .05F));
        // 画笔保存在快照而非注册表，避免正式音符误满足预览端点的断言。
        // 每个类型使用新快照，上一轮几何和命令不能残留到下一轮。
        preview.brush.isActive = true;
        preview.brush.type     = type;
        preview.brush.time     = nearTime;
        preview.brush.duration = farTime - nearTime;
        // 折线使用一个竖段再横移，涵盖离屏末端节点与连接段的裁剪。
        // 原始时间通过滚动缓存反解，不将秒数直接当成画布像素。
        preview.brush.polylineSegments = {
            { .type       = NoteType::HOLD,
              .timestamp  = nearTime,
              .duration   = farTime - nearTime,
              .trackIndex = 0 },
            { .type = NoteType::NOTE, .timestamp = farTime, .trackIndex = 1 }
        };
        generate(preview);
        const float targetU     = type == NoteType::HOLD ? .5F : .6F;
        const bool  farGeometry = std::any_of(
            preview.vertices.begin(),
            preview.vertices.end(),
            [&](const auto& vertex) {
                return vertex.pos.y < -100 && vertex.uv.u >= targetU - .001F &&
                       vertex.uv.u <= targetU + .101F;
            });
        if ( !farGeometry ) return false;
    }
    // 自动拍线在负 Y 光标附近也按扩展轨道高度计算渐隐范围。
    // 高分拍保证窗口内有候选，不把某条特定 BPM 网格位置当作固定结果。
    config.visual.beatLineDisplayMode = Config::BeatLineDisplayMode::NearCursor;
    config.visual.beatLineCursorVisibleRatio = .2F;
    config.visual.beatLineCursorFadeRatio    = .1F;
    config.settings.beatDivisor              = 128;
    // 独立快照只开启悬浮状态，不能要求画笔激活后自动拍线才可显示。
    // 正式物件可以继续存在，拍线断言使用专用收集结果排除音符几何干扰。
    // 该模式必须经过正常玩家域收集，不直接调用渐隐数学函数自证正确。
    Logic::RenderSnapshot reveal;
    reveal.hasBeatmap       = true;
    reveal.isHoveringCanvas = true;
    reveal.hoveredTime      = farTime;
    reveal.uvMap.emplace(static_cast<uint32_t>(Logic::TextureID::Note),
                         glm::vec4(.2F, .3F, .1F, .05F));
    // 原始画布高度仍为六百，不增大 viewport 参数来掩盖长度倍率未传递。
    // 当前三倍长度来自前面的配置，测试与真实三维布局采用同一扩展入口。
    generate(reveal);
    // 光标在负四百五十像素附近，离它一百至一百五十像素的网格仍在扩展显示带内。
    // 原二维渐隐总半径只有九十像素，旧实现无法输出此区间的可见拍线。
    // 只要求此连续区间存在候选，不绑定具体 BPM 步号或浮点步进顺序。
    // 测试范围远离扩展轨道边界，因此失败可归因于渐隐半径而非最外侧裁剪。
    // 这里检查逻辑可见线；实际 GPU 透视与鼠标逆投影仍由相机套件覆盖。
    if ( !std::any_of(
             reveal.playerBeatLines.begin(),
             reveal.playerBeatLines.end(),
             [](const auto& line) { return line.y < -550 && line.y > -600; }) )
        return false;
    // 常显模式排除光标渐隐，直接验证远处分拍线的最终屏幕厚度。
    // 此处使用原有皮肤线宽，不在测试中人为加粗来满足屏幕像素下限。
    // 绘制快照与验证矩阵共用当前四键配置，避免只验证另一套相机参数。
    // 下限检查针对生成后的四边形，不能仅验证 helper 返回值大于一。
    // 使用内置斜轨姿态，让源平面上的细线明显受到透视压缩。
    // 单独的纯色标识筛选网格，底板、音符和轨道边框不能使断言误通过。
    config.visual.beatLineDisplayMode    = Config::BeatLineDisplayMode::Always;
    config.visual.canvasCameraPosition   = { 0, -.8F, .9F };
    config.visual.canvasCameraRotation   = { 45, 0, 0 };
    config.visual.overrideBeatLineColors = true;
    for ( auto& color : config.visual.beatLineColors )
        color = { .123F, .456F, .789F, 1 };
    Logic::RenderSnapshot always;
    always.hasBeatmap = true;
    generate(always);
    const auto matrix =
        MMM::Canvas::canvasCameraPoseProjection(800, 600, 0, config.visual, 4);
    int distantLines = 0;
    // 批处理器每个网格矩形输出四顶点，零和三分别是同侧的下、上边。
    // 只检查原二维视口上方的图元，近端线足够粗不能证明远端没有丢失。
    for ( std::size_t i = 0; i + 3 < always.vertices.size(); i += 4 ) {
        const auto& first = always.vertices[i];
        if ( std::abs(first.color.r - .123F) > .001F || first.pos.y > -100 )
            continue;
        const auto& last   = always.vertices[i + 3];
        const auto  bottom = matrix * glm::vec4(first.pos.x, first.pos.y, 0, 1);
        const auto  top    = matrix * glm::vec4(last.pos.x, last.pos.y, 0, 1);
        // 相机前方才能参与除法；厚度必须在透视除法和视口缩放后度量。
        // 此测试不以提高颜色 alpha 代替几何覆盖，也不依赖 GPU 抗锯齿设置。
        if ( bottom.w <= 0 || top.w <= 0 ) return false;
        const float pixels =
            std::abs(bottom.y / bottom.w - top.y / top.w) * 300;
        if ( pixels < 1.0F ) return false;
        ++distantLines;
    }
    // 防止整个远端范围都没生成网格时，空集合比较反而被视作成功。
    if ( distantLines == 0 ) return false;
    // 真实序列化保留长度，旧配置继续使用一倍高度。
    // 每次从 JSON 创建新对象，不能由内存中的旧值补上遗漏字段。
    // 删除键模拟旧版配置，而不是给该键显式填写默认值。
    nlohmann::json saved = config.visual;
    if ( saved.get<Config::VisualConfig>().canvasCameraLength != 3 )
        return false;
    saved.erase("canvasCameraLength");
    if ( saved.get<Config::VisualConfig>().canvasCameraLength != 1 )
        return false;
    // 越界数值夹取，错误类型回退，不依赖异常捕获。
    // 上限同时保护每帧候选数量与底板平铺开销。
    // 字符串不能自动解析为长度，配置语义须与其它浮点字段一致。
    // 这些读取不能修改原始配置对象，否则前面的三倍夹具可能污染默认值。
    // 旧键缺失、数字越界和类型错误分别检查，覆盖三种不同恢复路径。
    saved["canvasCameraLength"] = 1000;
    if ( saved.get<Config::VisualConfig>().canvasCameraLength != 32 )
        return false;
    saved["canvasCameraLength"] = "invalid";
    if ( saved.get<Config::VisualConfig>().canvasCameraLength != 1 )
        return false;
    return true;
}

/// @brief 实测透视后两条厚度边的垂直像素距离，覆盖距离、旋转及运动补偿组合。
/// @note 使用真实批处理入口与最终相机矩阵，不以局部放大倍数代替视觉厚度。
bool checkDynamicThickness()
{
    using namespace MMM;
    Config::VisualConfig visual;
    visual.canvasCameraPoseEnabled = true;
    // 厚度以逻辑像素定义，窗口宽高不同可暴露遗漏视口比例的计算。
    // 近远端和左右轨道分别测量，避免只在画布中央侥幸等厚。
    // 相机朝向包含俯仰和画面旋转，单纯除以俯仰余弦不能通过本组测试。
    // 连续切换正视、小倾角、大倾角与旋转方向，再返回初始视角，防止缓存固定补偿。
    // 正负偏航改变透视压缩的方向，不能只覆盖绕水平轴的一种预览姿态。
    for ( const auto camera : { glm::vec4(0, 0, 0, .65F),
                                glm::vec4(15, 0, -18, 1),
                                glm::vec4(15, -35, 18, 1.3F),
                                glm::vec4(45, 0, 18, 2),
                                glm::vec4(45, 35, -25, 1.2F),
                                glm::vec4(65, 8, -25, 1.2F),
                                glm::vec4(0, 0, 0, .65F) } ) {
        const float distance        = camera.w;
        visual.canvasCameraRotation = { camera.x, camera.y, camera.z };
        visual.canvasCameraPosition = { .01F * camera.y,
                                        -0.8F * distance,
                                        0.9F * distance };
        // 自然透视、部分匀速与完全匀速均使用各自最终矩阵。
        // 这样可捕获厚度补偿错误读取未经运动补偿的原始相机参数。
        for ( float motion : { 0.0F, 50.0F, 100.0F } ) {
            visual.canvasMotionCompensation = motion;
            // 播放偏移改变同一源点的深度，测试不能永远只测暂停时的投影。
            // 此处显式传入偏移，验证算法不额外将偏移重复加到音符坐标。
            for ( float offset : { 0.0F, 35.0F } ) {
                const auto matrix = Canvas::canvasCameraPoseProjection(
                    800, 600, offset, visual, 4);
                /// @brief 投影源几何到逻辑像素，不经过待测的补偿函数。
                const auto project = [&](glm::vec2 p) {
                    const auto clip = matrix * glm::vec4(p, 0, 1);
                    return (glm::vec2(clip) / clip.w + 1.0F) *
                           glm::vec2(400, 300);
                };
                // 远端超出原二维画布，仍须以实际深度获得同一屏幕目标。
                // 每个纵向位置分别测左右轨，覆盖旋转后横向深度不一致的情况。
                for ( float y : { -1200.0F, -200.0F, 200.0F, 480.0F } ) {
                    for ( float x : { 180.0F, 520.0F } ) {
                        // 投影后的同一条轨道横线仍是一条直线，沿此线的位移不计入厚度。
                        // 使用独立法线测量可区分真正变厚与斜视引起的横向剪切。
                        const auto      start = project({ x, y });
                        const auto      end   = project({ x + 80, y });
                        const auto      row   = glm::normalize(end - start);
                        const glm::vec2 normal(-row.y, row.x);
                        // 满补偿保持源高宽比
                        // 24/80，实际像素高度随该位置投影宽度变化。
                        // 宽度不参与补偿，可在绘制前独立计算参考尺度，避免循环验证。
                        const float fullHeight =
                            24 * glm::length(end - start) / 80;
                        // 原始四角来自固定宽高的正式
                        // pushQuad，不手写待测函数的中间结果。
                        // 三个强度都验证实际最终顶点，关闭分支也不能偷偷改变几何。
                        for ( float amount : { 0.0F, 0.5F, 1.0F } ) {
                            Logic::RenderSnapshot snapshot;
                            // 新快照不继承上一次的顶点，避免某个强度读到其他强度的测试结果。
                            // 矩阵和视口必须成套提供，不能混用窗口物理尺寸与画布逻辑尺寸。
                            snapshot.thicknessCompensation = { matrix,
                                                               { 800, 600 },
                                                               amount };
                            Logic::System::Batcher batcher(&snapshot);
                            batcher.setTexture(Logic::TextureID::Note);
                            batcher.pushQuad(x, y + 12, 80, 24, glm::vec4(1));
                            if ( snapshot.vertices.size() != 4 ) return false;
                            // 两条边独立比较，不能用平均高度掩盖一边厚一边薄。
                            // 断言还检查源横坐标，禁止通过整体放大音符宽度凑厚度。
                            for ( int edge : { 0, 1 } ) {
                                const auto& bottom =
                                    snapshot.vertices[edge].pos;
                                const auto& top =
                                    snapshot.vertices[3 - edge].pos;
                                // 基准距离直接投影原始上下边，不从算法内部导数或放大比例推导期望值。
                                // 这样即使补偿公式与局部比例一起算错，测试仍能识别实际像素误差。
                                const float natural = std::abs(glm::dot(
                                    project({ x + edge * 80, y + 12 }) -
                                        project({ x + edge * 80, y - 12 }),
                                    normal));
                                // 测量最终提交的四角，因此同时覆盖 pushQuad
                                // 到自由四边形的调用链。
                                // 纹理倍率为默认一，当前断言专门检验透视缩放，不混入皮肤资产变化。
                                const float actual = std::abs(
                                    glm::dot(project({ bottom.x, bottom.y }) -
                                                 project({ top.x, top.y }),
                                             normal));
                                // 半强度分别测量上下两侧位移的几何中点；
                                // 满强度使用当前位置宽度对应的目标厚度。
                                // 透视令两侧自然位移不对称，不能先合并总高度再取中点。
                                const auto anchor =
                                    project({ x + edge * 80, y });
                                const float lower = std::abs(glm::dot(
                                    project({ x + edge * 80, y + 12 }) - anchor,
                                    normal));
                                const float upper = std::abs(glm::dot(
                                    project({ x + edge * 80, y - 12 }) - anchor,
                                    normal));
                                // 期望只使用独立屏幕投影，不读取算法的倍率或齐次反解中间值。
                                const float expected =
                                    amount == 0 ? natural
                                    : amount == 1
                                        ? fullHeight
                                        : std::sqrt(lower * fullHeight * .5F) +
                                              std::sqrt(upper * fullHeight *
                                                        .5F);
                                if ( std::abs(actual - expected) > 0.015F ||
                                     bottom.x != x + edge * 80 ||
                                     top.x != bottom.x )
                                    return false;
                            }
                            // 持续连接体、纯色和屏幕层不继承定高贴图的补偿。
                            // 每次清空顶点但保留同一快照参数，以覆盖批处理器纹理筛选。
                            for ( auto texture :
                                  { Logic::TextureID::HoldBodyVertical,
                                    Logic::TextureID::Background,
                                    Logic::TextureID::None } ) {
                                snapshot.vertices.clear();
                                batcher.setTexture(texture);
                                batcher.pushQuad(
                                    x, y + 12, 80, 24, glm::vec4(1));
                                if ( snapshot.vertices[0].pos.y != y + 12 ||
                                     snapshot.vertices[2].pos.y != y - 12 )
                                    return false;
                            }
                        }
                    }
                }
            }
        }
    }
    // 底边模式必须固定原来的底边，不能因增加厚度而穿过判定线。
    // 正面等比退距不应额外增厚；此处再压缩纵向一半以检验底边锚点。
    Logic::RenderSnapshot aligned;
    aligned.thicknessCompensation = {
        Common::Render::planarObservationProjection(800, 600, 0, 0, 0, 2),
        { 800, 600 },
        1
    };
    // 当前横向缩放为二分之一、纵向为四分之一，补偿需要将局部高度翻倍。
    // 源底边仍停在 Y=300，不能随恢复高宽比而穿过判定线。
    aligned.thicknessCompensation.m_projection[1][1] *= .5F;
    Logic::System::Batcher alignedBatcher(&aligned);
    // 先开启底边作用域再切换纹理，验证对齐缓存的刷新顺序。
    // 输入半高与定位偏移应抵消为原来的时间锚点。
    alignedBatcher.setNoteTexturePosition(Config::NoteTexturePosition::Bottom);
    alignedBatcher.setTexture(Logic::TextureID::Note);
    alignedBatcher.pushQuad(200, 312, 80, 24, glm::vec4(1));
    if ( aligned.vertices[0].pos.y != 300 ||
         std::abs(aligned.vertices[2].pos.y - 252) > .01F )
        return false;
    // 同一纹理切到固定屏幕层后不应重复补偿，纹理筛选不能代替空间判断。
    // 新四角追加在原四角之后，分别测量两次提交避免空结果侥幸通过。
    alignedBatcher.setScreenSpace(true);
    alignedBatcher.pushQuad(200, 312, 80, 24, glm::vec4(1));
    if ( aligned.vertices[4].pos.y != 300 || aligned.vertices[6].pos.y != 276 )
        return false;
    // 奇异投影没有有限解时必须完整保留四角，防止半枚贴图被翻到相机后方。
    // 上面只比较有效视角，以下单独验证退化投影的错误契约。
    // 零矩阵使全部点的 W 为零，不能依赖某个偶然可见端点触发回退。
    Common::Render::CanvasThicknessCompensation invalid{ glm::mat4(0),
                                                         { 800, 600 },
                                                         1 };
    std::array<glm::vec2, 4>                    points{
        glm::vec2(0, 20), glm::vec2(80, 20), glm::vec2(80, 0), glm::vec2(0, 0)
    };
    const auto original = points;
    if ( invalid.compensate(points) || points != original ) return false;
    // 关闭时不要求矩阵有效，二维兼容路径必须逐点恒等。
    // 同一退化输入先开启后关闭，还可发现错误保留上次失败状态的实现。
    invalid.m_amount = 0;
    if ( !invalid.compensate(points) || points != original ) return false;
    return true;
}

/// @brief 验证部分厚度补偿不会让远端贴图提前进入等高平台或相对增厚。
/// @return 随距离增加，像素高度与相对宽度的厚度均持续减小时为真。
/// @note 使用用户反馈对应的内置斜轨相机，宽度保留真实透视。
/// @note 此处检查视觉趋势，不复制待测补偿公式作为期望结果。
/// @note 高宽比约束覆盖相对变厚的观感，不能仅比较远近绝对像素高度。
/// @note 远端样本仍在有效裁剪范围内，不能依赖失败回退获得预期趋势。
bool checkPartialThickness()
{
    using namespace MMM;
    Config::VisualConfig visual;
    visual.canvasCameraPoseEnabled = true;
    visual.canvasCameraPosition    = { 0, -0.8F, 0.9F };
    visual.canvasCameraRotation    = { 45, 0, 0 };
    const auto matrix =
        Canvas::canvasCameraPoseProjection(800, 600, 0, visual, 4);
    /// @brief 独立投影实际提交的顶点，输出逻辑屏幕像素。
    const auto project = [&](glm::vec2 point) {
        const auto clip = matrix * glm::vec4(point, 0, 1);
        return glm::vec2(clip) / clip.w * glm::vec2(400, 300);
    };
    // 第一处样本建立近端基准，后续只比较相邻位置，不预设某个固定高度常数。
    float previousHeight = 1.0e6F, previousRatio = 1.0e6F;
    // 前两处覆盖近端与红线附近，后续位置继续推进到远端延长区。
    // 原先线性混合的固定高度下限会令后几个音符的高宽比反向增大。
    // 全部样本使用同一轨道与源尺寸，排除分区轨宽差异造成的高度变化。
    // 延长区继续采样可识别渐近下限，不能只检查一对恰好相似的近远位置。
    for ( float y : { 450.0F, 0.0F, -600.0F, -1800.0F, -4800.0F } ) {
        Logic::RenderSnapshot snapshot;
        // 每个位置独立生成几何，避免在上一轮已补偿顶点上重复叠乘。
        snapshot.thicknessCompensation = { matrix, { 800, 600 }, 0.35F };
        Logic::System::Batcher batcher(&snapshot);
        batcher.setTexture(Logic::TextureID::Note);
        batcher.pushQuad(400, y + 12, 80, 24, glm::vec4(1));
        // 宽度沿未改动的中心线测量，避免把倾斜厚度边的横移计入轨宽。
        // 使用实际四角高度，防止只修改配置或候选范围便通过测试。
        const auto& bottom = snapshot.vertices[0].pos;
        const auto& top    = snapshot.vertices[3].pos;
        const float height = std::abs(project({ bottom.x, bottom.y }).y -
                                      project({ top.x, top.y }).y);
        const float width =
            glm::length(project({ 480, y }) - project({ 400, y }));
        // 统一源宽高后，归一化高度只反映补偿与相机变化，不混入不同皮肤资产。
        const float ratio = height / width;
        // 未达满补偿时保留纵向透视；越远不能比前一个位置更高或相对更厚。
        // 很小的容差只覆盖投影舍入，不能掩盖固定下限造成的明显反转。
        if ( height >= previousHeight || ratio > previousRatio + 0.0001F )
            return false;
        previousHeight = height;
        previousRatio  = ratio;
    }
    return true;
}

/// @brief 验证运动补偿在判定线固定的条件下消除纵向透视加速。
/// @return 匀速、分母边界和配置兼容均符合预期时为真。
/// @note 直接测量投影屏幕位置，而不是把配置数值当作渲染结果。
bool checkCompensation()
{
    using namespace MMM;
    Config::VisualConfig visual;
    visual.canvasCameraPoseEnabled                    = true;
    visual.canvasCameraPosition                       = { 0, -0.8F, 0.9F };
    visual.canvasCameraRotation                       = { 45, 0, 0 };
    visual.editableJudgmentLinePositionForKeyCount(4) = 0.8F;
    const auto original =
        Canvas::canvasCameraPoseProjection(800, 600, 0, visual, 4);
    // 多种强度共享原始判定线位置，不能在增大补偿时整体漂移。
    // 使用独立四键判定线覆盖非默认位置，避免只在公共回退值下保持锚点。
    // 零强度仍构造完整矩阵，验证兼容分支不是测试绕过了生产投影。
    // 相机采用内置斜轨方案的角度和位置，保证测试覆盖实际推荐用法。
    for ( float amount : { 0.0F, 50.0F, 100.0F } ) {
        visual.canvasMotionCompensation = amount;
        const auto matrix =
            Canvas::canvasCameraPoseProjection(800, 600, 0, visual, 4);
        // 使用生产输入映射验证补偿后的矩阵，不复制内部逆投影公式。
        // 相同源坐标必须可从屏幕位置恢复，避免只有画面变化而鼠标仍用旧投影。
        // 取画布内点保证位于相机前方；离屏外推另由观察相机测试覆盖。
        // 该检查分别经过零、中间与完全补偿，不仅检查最终极值。
        const auto input = Canvas::makePlanarObservationInputProjection(
            matrix, 800, 600, true);
        const glm::vec2 source(360, 400);
        const auto screen = Canvas::planarObservationProject(input, source);
        if ( !screen ) return false;
        const auto recovered =
            Canvas::planarObservationUnproject(input, *screen);
        if ( !recovered || glm::length(*recovered - source) > 0.01F )
            return false;
        // 下一组比较固定判定线，输入可逆不能代替视觉锚点保持的验证。
        // 投影返回的深度仍须合法，否则可逆但不可见的结果不能用于实际编辑。
        // 拾取容差只覆盖浮点运算，不允许接近一个逻辑像素的系统性偏移。
        // 三维参数不写入谱面模型，因此 source 始终是原始时间/轨道平面坐标。
        // 左、中、右三点均应保持，不能只固定判定线与中轴的交点。
        // 比较透视除法后的 XY，齐次矩阵整体倍数不属于视觉漂移。
        for ( float x : { 100.0F, 400.0F, 700.0F } ) {
            const auto before = original * glm::vec4(x, 480, 0, 1);
            const auto after  = matrix * glm::vec4(x, 480, 0, 1);
            if ( glm::length(glm::vec2(before) / before.w -
                             glm::vec2(after) / after.w) > 0.0001F )
                return false;
        }
        if ( amount == 100 ) {
            // 相同世界 Y 间隔应产生相同屏幕间隔，涵盖远端原视口之外的部分。
            // 同时检查深度，修改透视 W 后不能误将整段物件裁掉。
            // 首个差分作为基准，后续三个差分逐个比较，不只比较首尾平均速度。
            // 五个采样点包含远端负像素坐标，覆盖画布长度扩展后的区域。
            // 只检查数学均匀性，不将该测试宣称为 GPU 或实机播放帧率测试。
            float previous = 0, spacing = 0;
            for ( int i = 0; i < 5; ++i ) {
                const auto point =
                    matrix * glm::vec4(400, -600 + i * 250, 0, 1);
                if ( point.w <= 0 || point.z < 0 || point.z > point.w )
                    return false;
                const float y = point.y / point.w;
                if ( i == 1 ) spacing = y - previous;
                if ( i > 1 && std::abs(y - previous - spacing) > 0.0001F )
                    return false;
                previous = y;
            }
        }
    }
    // 持久化前保留非零的两项设置，缺失字段不能凭默认零值侥幸通过。
    // JSON 使用真实 VisualConfig 的 ADL 定义，不自建测试专用字段映射。
    // 原始配置仍留在栈上，反序列化结果必须来自独立对象。
    visual.canvasThicknessCompensation = 80;
    nlohmann::json saved               = visual;
    const auto     restored            = saved.get<Config::VisualConfig>();
    // 导入使用正式 ADL，旧文件缺少字段时必须恢复无补偿的旧视觉。
    // 无效类型和超范围各覆盖一次，避免外部编辑的配置污染矩阵。
    if ( restored.canvasMotionCompensation != 100 ||
         restored.canvasThicknessCompensation != 80 )
        return false;
    // 删除键模拟旧版文件；不能仅填写零，否则无法发现错误的缺省策略。
    // 两个字段互相独立，缺失任意一个都不应从另一个推导数值。
    saved.erase("canvasMotionCompensation");
    saved.erase("canvasThicknessCompensation");
    const auto legacy = saved.get<Config::VisualConfig>();
    if ( legacy.canvasMotionCompensation != 0 ||
         legacy.canvasThicknessCompensation != 0 )
        return false;
    saved["canvasMotionCompensation"]    = 200;
    saved["canvasThicknessCompensation"] = "invalid";
    const auto invalid                   = saved.get<Config::VisualConfig>();
    if ( invalid.canvasMotionCompensation != 100 ||
         invalid.canvasThicknessCompensation != 0 )
        return false;
    // 二维不会沿用三维厚度；原 noteScaleY 仍可独立调整和保存。
    visual.canvasCameraPoseEnabled = false;
    return visual.canvasNoteScaleY() == visual.noteScaleY;
}

/// @brief 比较投影后的屏幕位置，深度区间差异不影响二维视觉兼容。
bool sameScreen(const glm::mat4& a, const glm::mat4& b)
{
    // 中心和四角同时比较，防止只验证中心而遗漏旋转或缩放错误。
    for ( const auto point : { glm::vec4(0, 0, 0, 1),
                               glm::vec4(800, 0, 0, 1),
                               glm::vec4(400, 300, 0, 1),
                               glm::vec4(0, 600, 0, 1),
                               glm::vec4(800, 600, 0, 1) } ) {
        // 先除以齐次分量，再比较实际屏幕投影。
        // 不能直接比较矩阵元素，因为远裁剪面允许不同。
        // 容差覆盖浮点三角函数与四元数转换误差。
        const auto x = a * point, y = b * point;
        if ( glm::length(glm::vec2(x) / x.w - glm::vec2(y) / y.w) > 0.0001F )
            return false;
    }
    return true;
}
/// @brief 旧视角迁移、精确位置与持久化边界的纯数学回归。
bool checkMath()
{
    using namespace MMM::Canvas;
    // 组合旋转必须保持旧画面，而不是只在默认零角度成立。
    // 同时覆盖零俯仰、中间值和旧版最大俯仰。
    // 水平角跨正负方向，能捕获旋转乘法次序错误。
    // 所有样本都加入播放偏移，验证模型变换顺序。
    for ( float angle : { 0.0F, 35.0F, 75.0F } )
        for ( float yaw : { -120.0F, 0.0F, 60.0F } ) {
            MMM::Config::VisualConfig config;
            config.debugCanvasCameraAngleDegrees = angle;
            config.debugCanvasCameraYawDegrees   = yaw;
            config.debugCanvasCameraDistance     = 1.5F;
            // 旧函数作为迁移前的基准，不复制公式建立假基准。
            // 新结果调用生产自由相机投影函数。
            // 比较对象包括四角，因此错误缩放不会被中心点掩盖。
            const auto before =
                planarObservationProjection(800, 600, 13, angle, yaw, 1.5F);
            enableCanvasCameraPose(config);
            if ( !sameScreen(before,
                             canvasCameraPoseProjection(800, 600, 13, config)) )
                return false;
        }
    MMM::Config::VisualConfig config;
    enableCanvasCameraPose(config);
    // 默认相机为后续屏幕映射和背面拒绝提供公共基准。
    // 此处使用真实视觉配置启用函数。
    // 不人为设置测试专用投影矩阵。
    const auto centered = canvasCameraPoseProjection(800, 600, 0, config);
    // 正 X 移动相机应使画布向左，检验相机变换与模型变换方向没有混淆。
    config.canvasCameraPosition[0] = 0.25F;
    const auto shifted = canvasCameraPoseProjection(800, 600, 0, config) *
                         glm::vec4(400, 300, 0, 1);
    if ( shifted.x / shifted.w >= -0.1F ) return false;
    // 序列化走真实 ADL 接口。
    // 读取为新对象，避免原对象残留字段掩盖丢失的键。
    // 精确位置必须随启用标志一起往返保存。
    nlohmann::json json = config;
    config              = json.get<MMM::Config::VisualConfig>();
    if ( !config.canvasCameraPoseEnabled ||
         config.canvasCameraPosition[0] != 0.25F )
        return false;
    // 非数值和越界分量分别回退与限制，不能依靠异常捕获恢复配置。
    json["canvasCameraPosition"] = { "bad", 1000, nullptr };
    config                       = json.get<MMM::Config::VisualConfig>();
    if ( config.canvasCameraPosition != std::array<float, 3>{ 0, 100, 1 } )
        return false;
    // 成功投影的中心应恰好位于半宽半高。
    // 后方样本共用同一矩阵，专门验证齐次深度拒绝。
    // 这两类断言对应可见句柄和不可见句柄的输入边界。
    glm::vec2 screen;
    if ( !projectCameraHandle(
             centered, { 400, 300, 0 }, { 0, 0 }, { 800, 600 }, screen) )
        return false;
    if ( glm::length(screen - glm::vec2(400, 300)) > 0.01F ) return false;
    // 相机后方的几何不可作为句柄，避免透视除法把背后对象映到屏幕。
    if ( projectCameraHandle(
             centered, { 400, 300, 2 }, { 0, 0 }, { 800, 600 }, screen) )
        return false;
    return std::abs(cameraHandleDistance({ 5, 3 }, { 0, 0 }, { 10, 0 }) - 3) <
           0.001F;
}
/// @brief 真实 ImGui 帧检查预览只在摄像机模式存在。
bool checkWindows()
{
    // 使用真实 ImGui 状态机检查窗口是否参与本帧。
    // 不写 imgui.ini，避免污染用户布局。
    // 本测试不启动平台后端，也不创建原生窗口。
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1280, 900 };
    io.DeltaTime   = 1.0F / 60;
    unsigned char* pixels{};
    int            width{}, height{};
    // 字体图集只在初始化时准备，符合真实 NewFrame 前置条件。
    // 测试不上传纹理，预览传入空句柄。
    // 窗口存在性的断言与 GPU 是否可用无关。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    MMM::Canvas::CameraLayoutEditor editor;
    MMM::Config::VisualConfig       visual;
    bool                            passed = true;
    // 连续切换覆盖关闭后重新打开，预览不得遗留独立常驻绘制窗口。
    // 每次切换后完整执行 NewFrame、绘制和 Render。
    // 上一帧的窗口对象可能还缓存于 ImGui，但 Active 应立即清除。
    // 因此检查活动状态，而不是错误地要求内部缓存对象销毁。
    for ( bool mode : { false, true, false, true, false } ) {
        MMM::Canvas::CameraLayoutEditorTestAccess::mode(editor, mode);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 800, 700 });
        ImGui::Begin("LayoutTest");
        // 调用生产入口，不能只测试本地模式布尔值。
        // 元素模式也必须经过控件绘制，以覆盖正常返回路径。
        // 相机模式内部会创建预览，测试不手工 Begin 预览窗口。
        editor.update("test",
                      ImGui::GetCursorScreenPos(),
                      { 780, 650 },
                      ImTextureID{},
                      visual);
        ImGui::End();
        ImGui::Render();
        // 稳定内部 ID 与生产命名保持一致。
        // 窗口隐藏时允许对象仍驻留于 ImGui 缓存。
        // 只有本帧 Active 表示用户仍会看到预览。
        const auto* preview =
            ImGui::FindWindowByName("实际视角预览###CameraLayoutPreview_test");
        passed &= (preview && preview->Active) == mode;
        // 参数窗口必须与预览同帧显示或撤销，不能遗留独立编辑入口。
        // 反复进入模式也覆盖专用 Clay 上下文的释放与重建。
        const auto* parameters =
            ImGui::FindWindowByName("摄像机参数###CameraLayoutParameters_test");
        passed &= (parameters && parameters->Active) == mode;
    }
    // 退出只撤掉辅助视图，不回滚已经确认的位置。
    passed &= visual.canvasCameraPoseEnabled;
    // 键盘手势使用真实事件队列，不能仅检查状态字段变化。
    // 鼠标位于控制面板及预览之外，确保事件归属操作空间。
    MMM::Canvas::CameraLayoutEditorTestAccess::mode(editor, true);
    const auto originalPosition = visual.canvasCameraPosition;
    // 相机初值按值保存，后续断言比较真实变换前后的独立副本。
    // 不读取编辑器内部事务快照，避免两者同时错误而让测试通过。
    /// @brief 推进一帧真实操作视图，固定焦点以模拟用户点击画布。
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 800, 700 });
        ImGui::Begin("LayoutTest");
        ImGui::SetWindowFocus();
        editor.update("test",
                      ImGui::GetCursorScreenPos(),
                      { 780, 650 },
                      ImTextureID{},
                      visual);
        ImGui::End();
        ImGui::Render();
    };
    // 预热窗口悬停状态；工具快捷键不能启动变换事务。
    // 逐个推进按下、释放和移动事件，避免 ImGui 输入节流掩盖行为。
    io.AddMousePosEvent(650, 550);
    frame();
    frame();
    // 初始旋转也独立保存，不能把位置不变当作完全没有副作用。
    const auto originalRotation = visual.canvasCameraRotation;
    for ( const auto key : { ImGuiKey_R, ImGuiKey_G } ) {
        // 先旋转后移动，验证 G 确实能切回平移工具。
        // 每个快捷键都经历按下与释放，不依赖按键重复行为。
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        frame();
        // 没有按下鼠标按钮，往返运动也不应创建隐式变换事务。
        // 两个终点均留在画布内，不能依赖悬停失效来阻止错误变换。
        io.AddMousePosEvent(700, 600);
        frame();
        io.AddMousePosEvent(650, 550);
        frame();
        // 旋转和移动均只改变工具；仅检查位置会漏掉意外旋转。
        // 检查模式状态同时防止通过完全忽略快捷键来满足姿态不变断言。
        passed &= MMM::Canvas::CameraLayoutEditorTestAccess::rotating(editor) ==
                  (key == ImGuiKey_R);
        // 比较完整三分量，防止误改不可见的深度轴而漏报。
        // 这些值来自生产配置，而非编辑器内部缓存。
        passed &= visual.canvasCameraPosition == originalPosition &&
                  visual.canvasCameraRotation == originalRotation;
    }
    // 圆环正反方向及中心退化分别验证，避免旋转方向与取消混淆。
    passed &= std::abs(MMM::Canvas::cameraRingDelta({ 10, 0 }, { 0, 10 }) -
                       1.5707963F) < 0.0001F;
    passed &= std::abs(MMM::Canvas::cameraRingDelta({ 0, 10 }, { 10, 0 }) +
                       1.5707963F) < 0.0001F;
    passed &= MMM::Canvas::cameraRingDelta({ 0, 0 }, { 10, 0 }) == 0;
    // 所有帧都结束后再释放上下文。
    // 相机编辑器不拥有 GPU 资源，因此析构无需设备同步。
    // 测试产生的状态不写入用户软件设置。
    ImGui::DestroyContext();
    return passed;
}
}  // namespace
/// @brief 执行相机迁移和模式生命周期回归，不读写用户配置。
int main()
{
    if ( !checkMath() || !checkCanvasLength() || !checkCompensation() ||
         !checkDynamicThickness() || !checkPartialThickness() ||
         !checkWindows() ) {
        XERROR("CameraLayoutEditorTest failed");
        return 1;
    }
    return 0;
}
