#include "logic/ecs/system/render/PolylineCarrierAnchor.h"

#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <entt/entity/registry.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace
{
/// @brief 从独立图集区域提取四边形的纵向范围。
/// @param snapshot 已完成生成的渲染快照。
/// @param texture 探针分配了独立 UV 的纹理身份。
/// @return 每项为一个四边形的最小与最大 Y。
/// @note 不使用索引缓冲计数，避免共享顶点被两个三角形重复统计。
/// @note 只用于本探针的互不重叠图集区域，不是通用纹理反查。
/// @note 范围按空间大小排序，负 HS 不改变返回值中上下界的含义。
/// @note 不裁掉离屏端点，长条跨越画布时仍需验证其完整投影跨度。
/// @note 对称装饰的范围中点就是发生时间位置，不依赖纹理的像素高度。
/// @note 此处没有纹理文件访问，图集区域由调用方的测试快照预先提供。
std::vector<glm::dvec2> probeQuadRanges(
    const MMM::Logic::RenderSnapshot& snapshot, MMM::Logic::TextureID texture)
{
    const auto uv = snapshot.uvMap.at(static_cast<uint32_t>(texture));
    std::vector<glm::dvec2> ranges;
    std::size_t             count = 0;
    glm::dvec2              range{};
    // 批处理器按四个连续顶点提交四边形，纹理合批不会重新排序这些顶点。
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.uv.u < uv.x || vertex.uv.u > uv.x + uv.z ||
             vertex.uv.v < uv.y || vertex.uv.v > uv.y + uv.w )
            continue;
        // 第一项初始化上下界，不用零值扩大完全位于正或负半轴的图元。
        if ( count % 4 == 0 ) range = { vertex.pos.y, vertex.pos.y };
        range.x = std::min(range.x, static_cast<double>(vertex.pos.y));
        // 保留实际 float 顶点数值，不从领域时间重算，以免绕过绘制函数。
        range.y = std::max(range.y, static_cast<double>(vertex.pos.y));
        if ( ++count % 4 == 0 ) ranges.push_back(range);
    }
    return ranges;
}

/// @brief 只读检查外部 MMM 谱面中全部折线在事件边界前后的各部位投影。
/// @param path 显式传入的谱面路径；不保存、不打开项目会话。
/// @return 所有画布内横段、竖段、节点与尾部均出现在正确位置时返回真。
/// @details HS 参考值直接从输入事件查找，不调用被测载体锚点函数。
/// @pre 输入为已由项目加载器确认结构有效的 MMM v3 谱面。
/// @pre 折线采用 Hold/Flick 展开形式，正式玩家区为四轨。
/// @note 这是显式启用的开发探针，不承担通用格式兼容或输入修复职责。
/// @note 不创建 BeatmapSession，避免目录监视、最近项目和自动保存副作用。
/// @note 使用 ECS 的秒单位，输入文件中时间和时长均为毫秒。
/// @note 保留原时间精度，不通过整数毫秒取整隐藏边界处错误。
/// @note 参考规则来自 ProcessorFree.AppendExtraNotes 的首尾共用 HS。
/// @note rmslideEXF 的 type 11/12 横段属于其前一竖段载体。
/// @note 此探针检查投影后的平面坐标，不比较游戏透视镜头下的像素位置。
/// @note 不包含声音、判定动画和连击状态，避免混淆视觉时间与判定时间。
/// @note 每根折线独立渲染，防止另一根同位置横段掩盖遗漏。
/// @note 当前根实体内部的同位置横段只验证位置存在，不验证透明度叠加。
/// @note 失败日志保留根时间、子索引和观察时刻，便于重放对应场景。
/// @note 默认 CTest 不启用外部探针，持续集成使用下方的固定回归数据。
bool probeChart(const char* path)
{
    using namespace MMM::Logic;
    // 禁用解析异常，错误输入通过返回值报告；该探针不写入用户资源目录。
    std::ifstream input(path);
    if ( !input ) return false;
    const auto data = nlohmann::json::parse(input, nullptr, false);
    // 无效 JSON 或缺少核心数组不能当作“没有发现不一致”而通过。
    // 进一步的字段类型校验属于正式加载器，探针只接受其有效输出。
    if ( data.is_discarded() || !data.contains("note") ||
         !data.contains("timing") )
        return false;
    entt::registry                         timeline;
    std::vector<std::pair<double, double>> hs;
    std::vector<double>                    times{ 0 };
    // 即使首事件不在零点，也保留启动画面这一观察时刻。
    // 参考 HS 列表与业务缓存独立存储，避免直接复用被测查询结果。
    // 默认 BPM 与谱面元数据一致，后续时间线 BPM 可以覆盖它。
    timeline.emplace<TimelineComponent>(
        timeline.create(),
        TimelineComponent{
            .m_timestamp = 0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value = data.at("metadata").at("base").value("bpm", 120.0) });
    for ( const auto& event : data.at("timing") ) {
        // 只重建影响纵向滚动与载体倍率的事件；颜色等元数据不改变本断言。
        // 事件参数保持原符号，负 HS 和负滚速不能被测试输入阶段规范化。
        TimelineComponent component;
        component.m_timestamp = event.at("timestamp").get<double>() / 1000;
        const auto effect     = event.at("effect").get<std::string>();
        if ( effect == "hs" )
            component.m_effect = MMM::TimingEffect::HS;
        else if ( effect == "bpm" )
            component.m_effect = MMM::TimingEffect::BPM;
        else if ( effect == "scroll" )
            component.m_effect = MMM::TimingEffect::SCROLL;
        else if ( effect == "jump" )
            component.m_effect = MMM::TimingEffect::JUMP;
        else
            continue;
        component.m_value = event.at("param").get<double>();
        timeline.emplace<TimelineComponent>(timeline.create(), component);
        // 保持文件插入顺序，由正式缓存处理同刻事件的生效顺序。
        // 不能预先把所有事件折叠成一条，否则无法覆盖覆盖顺序的回归。
        if ( effect == "hs" )
            hs.emplace_back(component.m_timestamp, component.m_value);
        // 事件前后均取样，捕捉边界两侧的可见性与旧快照残留问题。
        for ( double offset : { -.05, -.001, 0., .001, .05 } )
            times.push_back(component.m_timestamp + offset);
    }
    // 同刻 HS 保持源文件顺序，与游戏事件覆盖顺序相同。
    std::stable_sort(hs.begin(), hs.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    MMM::Config::EditorConfig config;
    config.visual.enableLinearScrollMapping = false;
    // 明确启用谱面变速，不能继承个人配置中的“线性显示”开关。
    // 此处重建一次缓存；之后所有观察帧都只读取同一份静态时间线。
    auto& cache = timeline.ctx().emplace<System::ScrollCache>();
    cache.rebuild(timeline, config, nullptr);
    std::size_t charts = 0, frames = 0, checked = 0;
    std::size_t verticalChecked = 0, nodeChecked = 0, endChecked = 0;
    for ( const auto& source : data.at("note") ) {
        // 普通 Tap/Hold 的显示测试由独立用例负责，本探针聚焦皮肤生成的折线。
        // 过滤根类型也避免把音频采样或虚拟辅助轨当作可玩的横段。
        if ( source.at("type") != "polyline" ) continue;
        NoteComponent note;
        note.m_type       = MMM::NoteType::POLYLINE;
        note.m_timestamp  = source.at("timestamp").get<double>() / 1000;
        note.m_trackIndex = source.at("track").get<int>();
        auto sampleTimes  = times;
        // 每根折线共享事件采样表，再追加自身接点；不要污染下一根的采样范围。
        // 保留重复时刻可重复检查相同输入，不改变测量通过与否的判定。
        for ( const auto& value : source.at("sub_notes") ) {
            NoteComponent::SubNote sub;
            sub.type       = value.at("type") == "hold" ? MMM::NoteType::HOLD
                                                        : MMM::NoteType::FLICK;
            sub.timestamp  = value.at("timestamp").get<double>() / 1000;
            sub.duration   = value.value("duration", 0.0) / 1000;
            sub.trackIndex = value.at("track").get<int>();
            sub.dtrack     = value.value("dtrack", 0);
            note.m_subNotes.push_back(sub);
            // MMM 折线根节点可以没有 duration，粗筛范围应由实际子物件推导。
            // 末端滑键时长为零，仍必须计入整根折线的最晚发生时间。
            note.m_duration =
                std::max(note.m_duration,
                         sub.timestamp + sub.duration - note.m_timestamp);
            // 时间事件之外还覆盖每个物件接点，避免只检查变速处而漏掉横段出现。
            for ( double offset : { -1., -.05, 0., .05, 1. } )
                sampleTimes.push_back(sub.timestamp + offset);
        }
        entt::registry notes, samples;
        // 注册表生命周期覆盖该根折线的所有快照，不跨根复用实体身份。
        // 采样集合为空，背景与音频几何不会贡献横段图集区域。
        const auto entity = notes.create();
        notes.emplace<NoteComponent>(entity, note);
        notes.emplace<TransformComponent>(entity);
        const std::vector<entt::entity> sorted{ entity };
        // 正式入口依赖有序实体表；单根折线天然有序但仍需注册观察指针。
        // 指针只在局部注册表存活期间借用，不转移容器所有权。
        notes.ctx().emplace<const std::vector<entt::entity>*>(&sorted);
        ++charts;
        for ( double now : sampleTimes ) {
            RenderSnapshot snapshot;
            // 新快照防止上一观察时刻的几何残留令缺失断言误通过。
            // 开启播放状态还会覆盖逻辑侧的亚帧补间资格判定。
            snapshot.hasBeatmap = true;
            snapshot.isPlaying  = true;
            snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::None),
                                   glm::vec4{ 0, 0, .01, .01 });
            snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Note),
                                   glm::vec4{ .1, .1, .1, .1 });
            snapshot.uvMap.emplace(
                static_cast<uint32_t>(TextureID::HoldBodyHorizontal),
                glm::vec4{ .5, .5, .1, .01 });
            // 竖段与圆点也使用独立区域，横段通过不能代替其他部位通过。
            snapshot.uvMap.emplace(
                static_cast<uint32_t>(TextureID::HoldBodyVertical),
                glm::vec4{ .3, .3, .01, .1 });
            snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Node),
                                   glm::vec4{ .7, .7, .02, .02 });
            // 每根折线仅有一个尾部，左右箭头与结束线共用探针的末端区域。
            // 这里检查发生时间，左右纹理外观由皮肤绑定测试负责。
            for ( auto id : { TextureID::HoldEnd,
                              TextureID::FlickArrowLeft,
                              TextureID::FlickArrowRight } )
                snapshot.uvMap.emplace(static_cast<uint32_t>(id),
                                       glm::vec4{ .8, .8, .02, .02 });
            // 图集保留基础 Note 尺寸，横段宽度和厚度使用真实换算代码。
            // 横段的 UV 与普通 Note 分离，不依赖绘制命令是否被合批。
            const double judgment = 600 * config.visual.judgeline_pos;
            System::NoteRenderSystem::generateSnapshot(notes,
                                                       samples,
                                                       {},
                                                       {},
                                                       timeline,
                                                       {},
                                                       &snapshot,
                                                       "Basic2DCanvas",
                                                       now,
                                                       800,
                                                       600,
                                                       judgment,
                                                       4,
                                                       0,
                                                       0,
                                                       config,
                                                       600);
            ++frames;
            // 统计实际执行的快照数，不把输入时间点个数误报为渲染验证数。
            // 后续检查从最终 CPU 顶点取值，覆盖主体绘制而非仅覆盖缓存计算。
            std::vector<double> actual;
            double              sum      = 0;
            std::size_t         vertices = 0;
            // 图集区域独立于所有背景与装饰，每四个顶点构成一个横段。
            for ( const auto& vertex : snapshot.vertices ) {
                if ( vertex.uv.u < .5F || vertex.uv.u > .6F ||
                     vertex.uv.v < .5F || vertex.uv.v > .51F )
                    continue;
                sum += vertex.pos.y;
                if ( ++vertices % 4 == 0 ) {
                    // 对四边形求中心，抵消贴图厚度和底边定位约定。
                    // 三角形索引的组织方式不影响这四个几何顶点的顺序。
                    actual.push_back(sum / 4);
                    sum = 0;
                }
            }
            // 横段以完整四边形提交，残缺尾组意味着顶点提取或生成契约被破坏。
            // 不能静默丢掉残余顶点后继续进行中心比较。
            if ( vertices % 4 != 0 ) return false;
            const auto verticals =
                probeQuadRanges(snapshot, TextureID::HoldBodyVertical);
            const auto nodes = probeQuadRanges(snapshot, TextureID::Node);
            const auto ends  = probeQuadRanges(snapshot, TextureID::HoldEnd);
            // 各部位分别提取和计数，圆点仍在正确位置不能掩盖主体缺失。
            // 末端纹理共用探针区域，但不会与中间 Node 区域相交。
            // 这里仍读取完整快照，不根据预计位置先过滤实际图元。
            // 参考投影共用积分原点，但 HS 从独立事件表按载体起点求出。
            const auto referenceY = [&](double time, double anchor) {
                // time 决定端点的积分位置，anchor 只决定该虚拟载体的显示倍率。
                // 两个参数不能合并：跨 HS 的长条尾端正好需要不同的时间值。
                // 遍历仅存在于离线测试中，生产路径仍使用缓存二分查询。
                double multiplier = 1;
                for ( const auto& event : hs ) {
                    if ( event.first > anchor ) break;
                    multiplier = event.second;
                }
                return judgment -
                       (cache.getAbsY(time) - cache.getAbsY(now)) * multiplier;
            };
            // 按领域子索引报告位置，便于直接定位编辑器中的同一物件。
            // 独立载体发生重叠时不改变遍历顺序，不借排序把它们连成一段。
            for ( std::size_t i = 0; i < note.m_subNotes.size(); ++i ) {
                const auto& sub = note.m_subNotes[i];
                if ( i + 1 == note.m_subNotes.size() ) {
                    // 尾部只属于数组最后一个子物件，中间竖段不能产生结束线。
                    // 此分支先于类型过滤，否则以 Hold 结束的折线会漏测。
                    // 根 duration
                    // 不用于推导末端，避免容器时间掩盖真实子物件时间。
                    double anchor = sub.timestamp;
                    if ( sub.type == MMM::NoteType::FLICK && i > 0 &&
                         note.m_subNotes[i - 1].type == MMM::NoteType::HOLD )
                        anchor = note.m_subNotes[i - 1].timestamp;
                    // 长条尾线取结束时间，末端滑键仍取发生时间，但都沿用载体起点
                    // HS。
                    const double expected =
                        referenceY(sub.timestamp + sub.duration, anchor);
                    if ( expected > 1 && expected < 599 ) {
                        ++endChecked;
                        // 尾部检查中点而非上沿，保持与正式居中贴片约定一致。
                        // 同根折线的末端纹理只有一个，不会被另一个尾部误匹配。
                        // 未进入可见区域的尾部不应代替其主体参与覆盖统计。
                        if ( std::none_of(
                                 ends.begin(), ends.end(), [&](const auto& r) {
                                     return std::abs((r.x + r.y) / 2 -
                                                     expected) < .02;
                                 }) ) {
                            XERROR("HS end mismatch: note={}, now={}, y={}",
                                   note.m_timestamp,
                                   now,
                                   expected);
                            return false;
                        }
                    }
                }
                if ( sub.type == MMM::NoteType::HOLD && sub.duration > 0 ) {
                    // 零时长竖段没有主体，其节点或装饰仍由后面的断言验证。
                    // 正时长竖段两端都采用起点 HS，不能在尾端重新读取 HS。
                    // 既比较近端也比较远端，避免只对齐头部却拉长尾部。
                    const double a = referenceY(sub.timestamp, sub.timestamp);
                    const double b =
                        referenceY(sub.timestamp + sub.duration, sub.timestamp);
                    const double low = std::min(a, b), high = std::max(a, b);
                    // 反向 HS 只翻转空间顺序，不交换时间首尾或给尾端重新采样。
                    if ( low < 599 && high > 1 ) {
                        // 使用区间相交，不要求两端同时可见；这覆盖负 HS
                        // 的剔除回归。
                        // 一端越过上边缘、另一端越过下边缘时，主体仍贯穿整个画布。
                        // 断言保留原端点，不以截到视口后的范围掩盖投影错误。
                        ++verticalChecked;
                        if ( std::none_of(verticals.begin(),
                                          verticals.end(),
                                          [&](const auto& r) {
                                              return std::abs(r.x - low) <
                                                         .02 &&
                                                     std::abs(r.y - high) < .02;
                                          }) ) {
                            XERROR(
                                "HS vertical mismatch: note={}, sub={}, "
                                "now={}, range=[{},{}]",
                                note.m_timestamp,
                                i,
                                now,
                                low,
                                high);
                            return false;
                        }
                    }
                }
                if ( i > 0 ) {
                    // 索引零使用独立头部纹理，不计入中间圆点的覆盖数量。
                    // 最后一个子物件仍可有圆点，不能因它另有尾部装饰而跳过。
                    // 横段终点圆点可能由下一 Hold
                    // 节点承载，但应继承横段的投影。
                    std::size_t owner = i;
                    // Hold 紧随 Flick 时，它的圆点表示上一横段的到达轨位置。
                    // 竖段本体是新载体，而这个圆点仍属于上一虚拟对象的横线。
                    // 因而装饰的倍率归属不能直接照抄主体的倍率归属。
                    if ( sub.type == MMM::NoteType::HOLD &&
                         note.m_subNotes[i - 1].type == MMM::NoteType::FLICK )
                        owner = i - 1;
                    double anchor = note.m_subNotes[owner].timestamp;
                    // 横段若又是前一竖段的尾部，还需回到那条竖段起点采样 HS。
                    // 只访问邻接元素，不把整根折线强行统一为根节点的倍率。
                    if ( note.m_subNotes[owner].type == MMM::NoteType::FLICK &&
                         owner > 0 &&
                         note.m_subNotes[owner - 1].type ==
                             MMM::NoteType::HOLD )
                        anchor = note.m_subNotes[owner - 1].timestamp;
                    const double expected = referenceY(sub.timestamp, anchor);
                    if ( expected > 1 && expected < 599 ) {
                        ++nodeChecked;
                        // 圆点贴图可以改变尺寸，中点不应随尺寸改变而漂移。
                        // 这里检验纵向投影，轨道映射由现有布局与交互测试负责。
                        // 首段两端同高度的计数约束另由固定回归用例验证。
                        if ( std::none_of(nodes.begin(),
                                          nodes.end(),
                                          [&](const auto& r) {
                                              return std::abs((r.x + r.y) / 2 -
                                                              expected) < .02;
                                          }) ) {
                            XERROR(
                                "HS node mismatch: note={}, sub={}, now={}, "
                                "y={}",
                                note.m_timestamp,
                                i,
                                now,
                                expected);
                            return false;
                        }
                    }
                }
                if ( sub.type != MMM::NoteType::FLICK || sub.dtrack == 0 )
                    continue;
                // RMSlide 将末端横线放在前一竖段的虚拟对象里，采样该对象起点
                // HS。
                double anchor = sub.timestamp;
                if ( i && note.m_subNotes[i - 1].type == MMM::NoteType::HOLD )
                    anchor = note.m_subNotes[i - 1].timestamp;
                double multiplier = 1;
                // 未出现 HS 时沿用单位倍率；同刻事件依次覆盖，最后一项生效。
                // 不调用 getHsAt，使参考端与实际端不会共享错误的锚点选择。
                for ( const auto& event : hs ) {
                    if ( event.first > anchor ) break;
                    multiplier = event.second;
                }
                const double expected =
                    judgment -
                    (cache.getAbsY(sub.timestamp) - cache.getAbsY(now)) *
                        multiplier;
                // AbsY 的积分由独立滚速用例覆盖，此处检验 HS 乘法及其调用链。
                // 先减当前原点再乘倍率，不能给首尾各乘不同倍率后再相减。
                // 只要求画布内部的横段存在，画布外纹理余量由单元测试另行覆盖。
                // 边缘留一像素容差，不把贴图刚好擦过裁剪边界计作主体可见。
                // 这个内缩只影响覆盖计数，不改变输入时间、HS 或参考坐标。
                // 非有限参考坐标意味着时间线或输入数据无效，不能归入离屏情况。
                if ( !std::isfinite(expected) ) return false;
                if ( expected < 1 || expected > 599 ) continue;
                ++checked;
                // 误差以像素衡量，容纳 double 到 float 的顶点转换舍入。
                // 缺失横段与位置错误都失败，不允许以“未渲染”绕过位置检查。
                // 失败即返回，日志中的观察时刻就是第一处可复现反例。
                // 后续根实体不再执行，避免大量相同错误掩盖最初的偏差。
                // 成功情况下遍历全部候选，不能用首根折线通过代替全谱检查。
                if ( std::none_of(
                         actual.begin(), actual.end(), [expected](double y) {
                             return std::abs(y - expected) < .02;
                         }) ) {
                    XERROR(
                        "HS probe mismatch: note={}, sub={}, now={}, "
                        "expectedY={}, rendered={}",
                        note.m_timestamp,
                        i,
                        now,
                        expected,
                        actual.size());
                    return false;
                }
            }
        }
    }
    XINFO(
        "HS probe passed: polylines={}, snapshots={}, visible horizontal "
        "checks={}, vertical checks={}, node checks={}, end checks={}",
        charts,
        frames,
        checked,
        verticalChecked,
        nodeChecked,
        endChecked);
    // 空谱面或从未产生可见横段时不能宣称验证成功。
    // 这些输入仍可用于其他测试，但不满足本探针的覆盖契约。
    return charts > 0 && checked > 0;
}

/// @brief 检查负 HS 将长条尾端翻回画布时，主体不会因首尾顺序反转被剔除。
/// @return 已过时间范围但仍穿过画布的主体存在时返回真。
/// @details 使用正式快照入口，覆盖粗筛与逐载体可见性两层检查。
/// @note 对照对象是 RMSlide type 32 的虚拟竖段，不要求首端也在视口内。
/// @note 时间区间已经结束，保留主体的依据必须是空间相交而非时间容差。
/// @note 只设置 HS，省略 BPM 时使用 ScrollCache 的基础速度。
/// @note 不含横段和圆点，避免其他可见部位掩盖竖段被丢弃的问题。
/// @note 几何是否越过视口边缘由绘制裁剪处理，本函数检查提交前剔除。
/// @note 该用例不触发资源重载、拾取输入或窗口生命周期管理。
bool testReverseCarrierVisibility()
{
    using namespace MMM::Logic;
    // 负 HS 与普通负 SV 不同：积分仍向前，只有载体相对距离反向。
    // 因此不能通过把时间线 SCROLL 改成负数来替代此用例。
    entt::registry notes, samples, timeline;
    timeline.emplace<TimelineComponent>(
        timeline.create(),
        TimelineComponent{ .m_timestamp = 0,
                           .m_effect    = MMM::TimingEffect::HS,
                           .m_value     = -1 });
    MMM::Config::EditorConfig config;
    config.visual.enableLinearScrollMapping = false;
    auto& cache = timeline.ctx().emplace<System::ScrollCache>();
    cache.rebuild(timeline, config, nullptr);
    // 采用长于整个视口跨度的竖段，保证可以构造一端离屏、一端可见。
    // 时间戳保持递增，不允许以交换首尾时间来规避反向投影。
    // 测试的是显示倍率翻转，原始谱面时长仍必须为正。
    NoteComponent note;
    note.m_type       = MMM::NoteType::POLYLINE;
    note.m_timestamp  = 1;
    note.m_duration   = 3;
    note.m_trackIndex = 1;
    note.m_subNotes   = { { .type       = MMM::NoteType::HOLD,
                            .timestamp  = 1,
                            .duration   = 3,
                            .trackIndex = 1 } };
    const auto entity = notes.create();
    notes.emplace<NoteComponent>(entity, note);
    notes.emplace<TransformComponent>(entity);
    // 与真实会话一样提供基础变换组件，不提前把负向范围改成正向。
    // 可见性最终由 NoteRenderSystem 按当前播放时刻重新求值。
    const std::vector<entt::entity> sorted{ entity };
    notes.ctx().emplace<const std::vector<entt::entity>*>(&sorted);
    // 根实体与子长条覆盖同一区间，不依赖错误的根时长把图元强行保活。
    RenderSnapshot snapshot;
    snapshot.hasBeatmap = true;
    // 图集只保留区分背景、基础尺寸和竖段所需的三个条目。
    // 基础 Note 尺寸仍存在，主体宽度换算不会走缺省资源分支。
    snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::None),
                           glm::vec4{ 0, 0, .01, .01 });
    snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Note),
                           glm::vec4{ .1, .1, .1, .1 });
    snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::HoldBodyVertical),
                           glm::vec4{ .4, .4, .01, .1 });
    // 当前时间已超过尾端半秒，不能靠结束时刻附近的容差保留主体。
    // 尾端经负 HS 翻转到判定线上方，首端则已经越过画布顶部。
    System::NoteRenderSystem::generateSnapshot(
        notes,
        samples,
        {},
        {},
        timeline,
        {},
        &snapshot,
        "Basic2DCanvas",
        4.5,
        800,
        600,
        600 * config.visual.judgeline_pos,
        4,
        0,
        0,
        config,
        600);
    // 使用独立主体 UV；头部、尾部或背景的存在不能算作主体绘制成功。
    // 首端在画布之外不等于整个载体离屏，这正是旧顺序判断漏掉的情况。
    // 顶点仍保留原始首尾顺序，不能依靠排序顶点使可见性测试通过。
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.uv.u >= .4F && vertex.uv.u <= .41F && vertex.uv.v >= .4F &&
             vertex.uv.v <= .5F )
            return true;
    }
    // 没有找到主体说明它在快照生成前被剔除，视口裁剪无法再恢复它。
    // 与其他测试返回码分离，便于 CI 识别负 HS 可见性回归。
    return false;
}

/// @brief 从正式快照验证两个横段仍在同一虚拟载体的投影尺度中。
/// @param note 截图中首条折线的领域组件。
/// @param timeline 含跨段 HS 变化的时间线。
/// @param config 与缓存一致的非线性映射配置。
/// @return 两个横段均可见且纵向位置符合皮肤同倍率公式时返回真。
/// @details 使用独立 UV 提取最终顶点，避免只测锚点函数而漏掉绘制调用方。
/// @note 对照 rmslideEXF.lua 的 type 10 与 type 11/12，而非原生 Slide 节点。
/// @note 此用例比较编辑画布的滚动距离，不要求复现游戏的透视镜头。
/// @note 不启动窗口或音频设备，因此可在无图形驱动的 CI 中执行。
/// @note Node 使用另一独立图集区域，分别检查横段主体与圆点的位置。
/// @note 横段厚度非零，以覆盖批处理器以底边定位的坐标约定。
/// @note 所有 UV 区域彼此分离，背景填充不会贡献被统计的顶点。
/// @note 此处不覆盖纹理文件加载及透明边缘，它们不决定 HS 采样时间。
/// @note 末端主体是否出现比截图颜色更稳定，适合跨平台回归比较。
/// @pre timeline 尚未注册 ScrollCache 上下文，由此函数创建并持有。
bool testRenderedCarriers(const MMM::Logic::NoteComponent& note,
                          entt::registry&                  timeline,
                          const MMM::Config::EditorConfig& config)
{
    using namespace MMM::Logic;
    // 快照入口要求真实实体、变换组件和有序观察表；这些对象在测试期间存活。
    entt::registry notes, samples;
    const auto     entity = notes.create();
    notes.emplace<NoteComponent>(entity, note);
    notes.emplace<TransformComponent>(entity);
    // 单实体天然有序，避免额外排序代码掩盖粗筛路径的问题。
    // 快照生成只借用向量地址，不允许在其运行中搬移该容器。
    const std::vector<entt::entity> sorted{ entity };
    notes.ctx().emplace<const std::vector<entt::entity>*>(&sorted);
    auto& cache = timeline.ctx().emplace<System::ScrollCache>();
    cache.rebuild(timeline, config, nullptr);
    // 不手工填缓存内部段表；事件排序与生效时间沿用业务入口。
    // 只选头部尚未到判定线的时刻，避免把游戏按住状态与编辑器几何混为一谈。
    for ( double now : { 0.0, 0.5, 3.0 } ) {
        RenderSnapshot snapshot;
        snapshot.hasBeatmap = true;
        // 当前时刻即使 HS=1，视口中也可能已有未来 HS=0.1 的虚拟载体。
        // 所以不能用当前滚动段稳定来证明整批顶点可共用同一个亚帧位移。
        snapshot.isPlaying = true;
        // 每个观察时刻使用新快照，防止上一帧残留顶点凑足预期数量。
        // 横段 UV 与普通 Note、无纹理几何隔离；只提取横向主体的四边形。
        snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::None),
                               glm::vec4{ 0, 0, .01, .01 });
        snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Note),
                               glm::vec4{ .1, .1, .1, .1 });
        snapshot.uvMap.emplace(
            static_cast<uint32_t>(TextureID::HoldBodyHorizontal),
            glm::vec4{ .5, .5, .1, .01 });
        // 两端圆点不能只跟随下一竖段，否则跨 HS 时会从横线末端消失。
        snapshot.uvMap.emplace(static_cast<uint32_t>(TextureID::Node),
                               glm::vec4{ .7, .7, .02, .02 });
        const float judgment = 600 * config.visual.judgeline_pos;
        // 使用主画布路径，预览区压缩比例不参与本次公式对照。
        // 空背景和采样集合排除无关几何，但仍保留正式轨道绘制。
        // 四轨与原谱一致，两个相反方向横段都必须参与输出。
        System::NoteRenderSystem::generateSnapshot(notes,
                                                   samples,
                                                   {},
                                                   {},
                                                   timeline,
                                                   {},
                                                   &snapshot,
                                                   "Basic2DCanvas",
                                                   now,
                                                   800,
                                                   600,
                                                   judgment,
                                                   4,
                                                   0,
                                                   0,
                                                   config,
                                                   600);
        if ( snapshot.allowUiPlaybackInterpolation ) return false;
        std::vector<float> ys;
        std::vector<float> nodeYs;
        // 图集内缩只改变 UV 边缘，仍落在分配给横段的区域内部。
        // 不按顶点在总缓冲中的绝对序号定位，避免依赖轨道背景数量。
        for ( const auto& vertex : snapshot.vertices ) {
            if ( vertex.uv.u >= .5F && vertex.uv.u <= .6F &&
                 vertex.uv.v >= .5F && vertex.uv.v <= .51F )
                ys.push_back(vertex.pos.y);
            if ( vertex.uv.u >= .7F && vertex.uv.u <= .72F &&
                 vertex.uv.v >= .7F && vertex.uv.v <= .72F )
                nodeYs.push_back(vertex.pos.y);
        }
        // 修复前尾部横段按恢复后的 HS=1 投影，可能整段被裁掉，不能接受空结果。
        if ( ys.size() != 8 ) return false;
        const double headY = (ys[0] + ys[1] + ys[2] + ys[3]) / 4;
        // 对四边形取均值消除纹理厚度，不拿某条边当作音乐时间位置。
        // 两个横段按子物件顺序提交；纹理批处理只合并命令而不重排顶点。
        const double tailY = (ys[4] + ys[5] + ys[6] + ys[7]) / 4;
        // 首横段左端一个圆点，次横段两端各一个；尾端不能用新 Hold 的 HS。
        if ( nodeYs.size() != 12 ) return false;
        // 圆点顶点同样按四边形求中心，不依赖 Node 纹理与 Note 的比例。
        // 分别检查起点和终点，防止只有一侧修正而仍留下断头横线。
        // 允许百分之一像素的误差，以容纳 double 投影转 float 的舍入。
        for ( std::size_t i : { 4U, 8U } ) {
            const double nodeY =
                (nodeYs[i] + nodeYs[i + 1] + nodeYs[i + 2] + nodeYs[i + 3]) / 4;
            if ( std::abs(nodeY - tailY) > .01 ) return false;
        }
        // 消去画布缩放，只比较相对判定线的投影比例；不依赖皮肤尺寸。
        // 参考式直接使用起止时间的滚动积分，不调用被测锚点函数。
        const double origin = cache.getAbsY(now);
        // 所选时刻严格早于首节点，分母不为零，也不会经过判定线钳制。
        const double ratio =
            (cache.getAbsY(note.m_subNotes[2].timestamp) - origin) /
            (cache.getAbsY(note.m_timestamp) - origin);
        if ( std::abs((judgment - headY) * ratio - (judgment - tailY)) > .01 )
            return false;
    }
    return true;
}
}  // namespace

/// @brief 用截图谱面的首个跨 HS 折线检查 RMSlide 虚拟载体投影。
/// @return 所有载体使用正确锚点且投影符合首尾同倍率规则时返回零。
/// @details 固定真实时间值，不打开或重写用户谱面；避免测试污染最近项目。
/// @note 数据摘自《玩具狂奏曲-終焉-》的首根折线，时间精度与导入后 ECS 一致。
/// @note 外侧独立长条并不参与本回归，避免无关主体影响顶点计数。
/// @note 返回码区分锚点归属、投影公式、断开连接和实际绘制失败。
int main()
{
    // 外部全谱探针为显式选择，不让普通 CTest 依赖用户本机目录。
    if ( const char* path = std::getenv("MMM_HS_PROBE_FILE");
         path && !probeChart(path) )
        return 9;
    using namespace MMM::Logic;
    using namespace MMM::Logic::System;
    if ( !testReverseCarrierVisibility() ) return 5;
    // 领域文件以毫秒存储，ECS 边界换成秒；保留小数防止事件边界被截断。
    constexpr double start = 3.418202916142714;
    constexpr double joint = 3.879774313969562;
    NoteComponent    note;
    // 根组件决定快照粗筛范围，必须与子物件覆盖的真实区间一致。
    note.m_type       = MMM::NoteType::POLYLINE;
    note.m_timestamp  = start;
    note.m_duration   = joint + 1.1539284945671197 - start;
    note.m_trackIndex = 2;
    // 首个零时长左滑对应虚拟 type 10，后接竖段和末端右滑。
    // 末端右滑在领域中独立存储，但皮肤把它作为前一载体的尾部绘制。
    // 最后一段是新虚拟载体，用于验证不能把整条面条都锁到首个 HS。
    note.m_subNotes = { { .type       = MMM::NoteType::FLICK,
                          .timestamp  = start,
                          .trackIndex = 2,
                          .dtrack     = -1 },
                        { .type       = MMM::NoteType::HOLD,
                          .timestamp  = start,
                          .duration   = joint - start,
                          .trackIndex = 1 },
                        { .type       = MMM::NoteType::FLICK,
                          .timestamp  = joint,
                          .trackIndex = 1,
                          .dtrack     = 1 },
                        { .type       = MMM::NoteType::HOLD,
                          .timestamp  = joint,
                          .duration   = 1.1539284945671197,
                          .trackIndex = 2 } };
    // 虚拟横段属于前一竖段，但后一个 Hold 不能继承整根面条的倍率。
    if ( polylineCarrierAnchor(note, 2) != start ||
         polylineCarrierAnchor(note, 3) != joint )
        return 1;

    entt::registry timeline;
    // 固定基础 BPM 以隔离 HS 归属；Scroll 与 Jump 的积分由既有缓存负责。
    // 三个事件不共享时间戳，测试结果不依赖同刻事件的插入顺序。
    // HS 在竖段内部从 0.1 恢复到 1，准确覆盖原来末端横段飞出屏幕的条件。
    for ( const auto& event :
          { TimelineComponent{ .m_timestamp = 0,
                               .m_effect    = MMM::TimingEffect::BPM,
                               .m_value     = 86.66048240494622 },
            TimelineComponent{ .m_timestamp = start,
                               .m_effect    = MMM::TimingEffect::HS,
                               .m_value     = 0.1 },
            TimelineComponent{ .m_timestamp = 3.76438146451285,
                               .m_effect    = MMM::TimingEffect::HS,
                               .m_value     = 1.0 } } )
        timeline.emplace<TimelineComponent>(timeline.create(), event);
    MMM::Config::EditorConfig config;
    config.visual.enableLinearScrollMapping = false;
    // 编辑器的线性显示开关会主动忽略变速，不属于本次游戏语义对照。
    ScrollCache cache;
    cache.rebuild(timeline, config, nullptr);
    // 改变当前播放时间也不能改变虚拟载体自身的 HS 归属。
    // 参考式来自 AppendExtraNotes 首尾共用 noteCarrier.hs，而非节点各自取值。
    for ( double now : { 0.0, start, 3.76438146451285, joint, 5.0 } ) {
        // 同时覆盖播放进入载体、跨过 HS 事件及载体结束后的负距离。
        // 这里不做可见性筛选，让已经离屏的投影也接受公式检查。
        const double origin    = cache.getAbsY(now);
        const double reference = (cache.getAbsY(joint) - origin) * 0.1;
        const double actual    = cache.getDisplayDelta(
            joint, origin, polylineCarrierAnchor(note, 2));
        if ( std::abs(actual - reference) > 1e-7 ) return 2;
    }
    // 锚点正确还不够，正式渲染入口也必须使用同一规则。
    if ( !testRenderedCarriers(note, timeline, config) ) return 4;
    // 脱离前一竖段的滑键不应被误认为同一虚拟对象的末端。
    note.m_subNotes[2].timestamp += 0.01;
    // 超过浮点相接容差的真实间隔必须保留，不能借前一长条的 HS 填平。
    if ( polylineCarrierAnchor(note, 2) != note.m_subNotes[2].timestamp )
        return 3;
    // 切换线性显示会忽略 HS，缓存标志必须随实际投影更新，而非只看事件存在。
    // 否则用户关闭变速后仍会永久失去原有亚帧补间。
    config.visual.enableLinearScrollMapping = true;
    cache.rebuild(timeline, config, nullptr);
    if ( cache.hasNonUnitHs() ) return 6;
    // 恢复变速应重新禁止统一位移；验证同一缓存实例能双向切换。
    config.visual.enableLinearScrollMapping = false;
    cache.rebuild(timeline, config, nullptr);
    if ( !cache.hasNonUnitHs() ) return 7;
    // 删除最后一个时间线事件后走空表分支，也必须清除旧谱面的 HS 状态。
    // 这模拟清空时间线，不能只用新建缓存来规避状态残留。
    timeline.clear();
    cache.rebuild(timeline, config, nullptr);
    if ( cache.hasNonUnitHs() ) return 8;
    return 0;
}
