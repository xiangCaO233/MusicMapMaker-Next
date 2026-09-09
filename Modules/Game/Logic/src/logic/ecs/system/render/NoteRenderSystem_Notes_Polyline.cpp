#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/ecs/system/render/NoteLaneGeometry.h"

#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

/// @brief 以普通 Note 纹理为基准，换算目标纹理的绘制尺寸。
/// @param snapshot 提供同一图集内的归一化纹理尺寸。
/// @param id 需要换算的主体或装饰纹理。
/// @param baseW 普通 Note 的当前绘制宽度。
/// @param baseH 普通 Note 的当前绘制高度。
/// @return 目标纹理相对尺寸；任一纹理缺失时保留基础尺寸。
/// @pre snapshot 有效，存在的 Note 图集条目宽高为正。
/// @note 基础尺寸已包含用户缩放；返回值不再次乘 noteScaleX/noteScaleY。
/// @note 缺失纹理时返回尺寸只是布局后备，不意味着对应图集资源已补齐。
/// @note 仅换算几何尺寸，UV 内缩及纹理填充由后续批处理接口负责。
/// @warning 折线逐节点绘制热路径，只查询现有图集，不加载纹理。
static glm::vec2 getDrawSize(RenderSnapshot* snapshot, TextureID id,
                             float baseW, float baseH)
{
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return { baseW, baseH };

    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return { baseW, baseH };

    // 同一图集的归一化因子相消，直接用 UV 宽高比可保留皮肤的相对尺寸。
    float wRatio = it->second.z / itBase->second.z;
    float hRatio = it->second.w / itBase->second.w;

    return { baseW * wRatio, baseH * hRatio };
}

/// @brief 读取纹理宽高比，缺失纹理时按正方形处理。
/// @param snapshot 当前快照的图集信息。
/// @param id 目标纹理身份。
/// @return UV 宽度除以高度的比例。
/// @pre 已存在条目的高度非零，资源有效性不在此逐帧校验。
/// @note 返回的是纹理比例而非几何比例，横纵用户缩放仍可令图元非等比。
/// @pre 当前图集为等边像素规格，UV 宽高比才能直接用作纹理像素宽高比。
/// @warning 绘制热路径只做图集查找与比例计算。
static float getTexAspect(RenderSnapshot* snapshot, TextureID id)
{
    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return 1.0f;
    return it->second.z / it->second.w;
}

/// @brief 获取 Polyline 子物件主体末端时间。
/// @param sub 折线内嵌子物件，时间与持续时间均使用秒。
/// @return Hold 使用持续区间末端，其他类型停留在起点时刻。
/// @pre 时间和时长已由模型校验；本辅助函数不将负时长钳制为零。
/// @warning 热路径：Polyline 几何生成时按子物件调用；保持纯计算，不得分配。
static double getSubCarrierEndTime(const NoteComponent::SubNote& sub)
{
    if ( sub.type == ::MMM::NoteType::HOLD ) {
        return sub.timestamp + sub.duration;
    }
    return sub.timestamp;
}

/// @brief 获取 Polyline 子物件主体末端 HS 锚点时间。
/// @param sub 用于决定末端所属载体的子物件。
/// @return Hold 仍以头部为锚点，不能因末端穿过 HS 事件而改变整个载体的缩放。
/// @note 返回时间键而非 HS 数值，具体效果仍由滚动缓存按该键查询。
/// @note Flick 的轨道终点发生在同一时刻，横向位移不增加这个锚点时间。
/// @warning 热路径：Polyline 几何生成时按子物件调用；保持纯计算，不得访问缓存。
static double getSubCarrierEndAnchorTime(const NoteComponent::SubNote& sub)
{
    if ( sub.type == ::MMM::NoteType::HOLD ) {
        return sub.timestamp;
    }
    return getSubCarrierEndTime(sub);
}

/// @brief 按主体、节点、头部、装饰的叠放顺序生成一条折线。
/// @param cache 已建立的滚动映射，缺失时不输出图元。
/// @param batcher 接收本次折线图元的批处理器。
/// @param note 具有内嵌子节点的折线组件。
/// @param config 提供物件缩放及节点显示配置。
/// @param snapshot 输出几何与可选拾取框，同时提供图集和交互状态。
/// @param currentAbsY 当前画布的滚动空间锚点。
/// @param currentTime 当前可见性判断时间，单位为秒。
/// @param judgmentLineY 当前时间对应的判定线纵坐标。
/// @param leftX 未提供独立投影时的轨道区左边界。
/// @param topY 可见区域上边界。
/// @param bottomY 可见区域下边界。
/// @param singleTrackW 默认轨宽，用于基础物件尺寸和后备布局。
/// @param renderScaleY 滚动距离到画布纵坐标的缩放。
/// @param colorHead 头部乘色。
/// @param colorHoldBody 主体与连接段乘色。
/// @param colorHoldEnd 长条尾部乘色。
/// @param colorNode 中间节点乘色。
/// @param colorArrow 滑键箭头乘色。
/// @param entity 折线根实体身份，用于擦除反馈和拾取结果。
/// @param generateHitboxes 是否同步写入拾取框。
/// @param glowPart 高亮目标部位，None 表示普通绘制。
/// @param glowSubIndex 高亮节点索引；-1 仅放开主体和中间节点，不匹配头尾装饰。
/// @param laneProjection 可选的独立轨道区域投影，借用期间必须有效。
/// @pre batcher 与 snapshot
/// 指向同一批快照输出，不能混用两个快照的图集与命中框。
/// @note 本函数不清空输出，调用方可在同一快照中继续追加其他物件。
/// @note 高亮筛选与拾取开关独立，额外高亮遍应由调用方关闭拾取以免重复登记。
/// @note 颜色按五类部位传入，不在此重新解析物件元数据覆盖色。
/// @pre currentAbsY 与 cache 属于同一滚动状态，不能把未动画缩放的位置混入。
/// @pre 子节点顺序已由编辑模型维护，此处不按时间排序或修复节点拓扑。
/// @note 子节点地址只在本次调用内借用，不存入输出快照或批处理命令。
/// @pre 几何参数均为同一画布的局部像素坐标，不应加入窗口屏幕偏移。
/// @note 各阶段可以各自提前返回；一个部位被剔除不会取消其余阶段。
/// @note 这里没有单独的最终 flush，剩余批次由外层快照生成流程收尾。
/// @warning 快照生成热路径，不允许在逐节点绘制中引入资源加载或阻塞等待。
void NoteRenderSystem::renderPolyline(
    const ScrollCache* cache, Batcher& batcher, const NoteComponent& note,
    const Config::EditorConfig& config, RenderSnapshot* snapshot,
    double currentAbsY, double currentTime, float judgmentLineY, float leftX,
    float topY, float bottomY, float singleTrackW, float renderScaleY,
    glm::vec4 colorHead, glm::vec4 colorHoldBody, glm::vec4 colorHoldEnd,
    glm::vec4 colorNode, glm::vec4 colorArrow, entt::entity entity,
    bool generateHitboxes, HoverPart glowPart, int glowSubIndex,
    const CanvasLaneProjection* laneProjection)
{
    if ( !cache ) return;

    // 先获得普通 Note 的基础大小，各节点再按所在区域与皮肤纹理比例换算。
    float noteW = singleTrackW * config.visual.noteScaleX;
    // 高度以单轨宽度和普通纹理比例为基准，不随 noteScaleX 二次伸缩。
    float noteH = (singleTrackW / getTexAspect(snapshot, TextureID::Note)) *
                  config.visual.noteScaleY;

    // 主体先画，后续节点覆盖连接处，避免粗线压住头部和节点纹理。
    drawPolylineBody(batcher,
                     note,
                     cache,
                     snapshot,
                     judgmentLineY,
                     leftX,
                     singleTrackW,
                     renderScaleY,
                     currentAbsY,
                     currentTime,
                     topY,
                     bottomY,
                     noteW,
                     noteH,
                     colorHoldBody,
                     entity,
                     generateHitboxes,
                     glowPart,
                     glowSubIndex,
                     laneProjection);

    // 中间节点使用专用 Node 纹理，与普通头部的皮肤尺寸可以不同。
    drawPolylineNodes(batcher,
                      note,
                      cache,
                      snapshot,
                      judgmentLineY,
                      leftX,
                      singleTrackW,
                      renderScaleY,
                      currentAbsY,
                      currentTime,
                      topY,
                      bottomY,
                      noteW,
                      noteH,
                      colorNode,
                      config,
                      entity,
                      generateHitboxes,
                      glowPart,
                      glowSubIndex,
                      laneProjection);

    // 首节点最后覆盖连接起点；重合时保留可辨识的 Note 头部外观。
    drawPolylineHead(batcher,
                     note,
                     cache,
                     snapshot,
                     judgmentLineY,
                     leftX,
                     singleTrackW,
                     renderScaleY,
                     currentAbsY,
                     currentTime,
                     topY,
                     bottomY,
                     noteW,
                     noteH,
                     colorHead,
                     config,
                     entity,
                     generateHitboxes,
                     glowPart,
                     glowSubIndex,
                     laneProjection);

    // 尾部装饰最后提交，滑键箭头和长条结束线不被主体覆盖。
    drawPolylineDecoration(batcher,
                           note,
                           cache,
                           snapshot,
                           judgmentLineY,
                           leftX,
                           singleTrackW,
                           renderScaleY,
                           currentAbsY,
                           currentTime,
                           topY,
                           bottomY,
                           noteW,
                           noteH,
                           colorHoldEnd,
                           colorArrow,
                           config,
                           entity,
                           generateHitboxes,
                           glowPart,
                           glowSubIndex,
                           laneProjection);
}

/// @brief 绘制子物件自身主体及相邻子物件之间的连接四边形。
/// @param batcher 图元输出器，保留调用方的裁剪状态。
/// @param note 折线组件，空节点列表不生成主体。
/// @param cache 时间到滚动距离的只读映射。
/// @param snapshot 提供图集、擦除状态并接收拾取框。
/// @param judgmentLineY 画布时间原点的纵坐标。
/// @param leftX 默认轨道区左边界。
/// @param singleTrackW 默认轨道宽度。
/// @param renderScaleY 滚动空间到画布空间的倍率，调用方应保证非零。
/// @param currentAbsY 当前滚动锚点。
/// @param currentTime 载体可见性判断的时间。
/// @param topY 可见范围上边界。
/// @param bottomY 可见范围下边界。
/// @param noteW 普通 Note 基础宽度。
/// @param noteH 普通 Note 基础高度。
/// @param colorHold 主体默认乘色，擦除预览可局部替换。
/// @param entity 根实体，用于将子段拾取归属于同一折线。
/// @param generateHitboxes 是否生成主体命中框。
/// @param glowPart 只有普通绘制或 HoldBody 高亮会输出主体。
/// @param glowSubIndex 限制高亮子段，-1 允许全部。
/// @param laneProjection 独立轨道区投影，未提供时使用默认轨道布局。
/// @pre cache 和 snapshot 有效，横向主体纹理存在时普通 Note
/// 参考纹理也必须存在。
/// @note 可见性筛选决定是否提交整个子段，不等同于把每个顶点裁到视口边界。
/// @note 本体和其后过渡段共享当前节点的高亮筛选索引，不能按绘制命令序号解释。
/// @note 过渡段输出一块连接四边形，不在这里按中途时间线事件拆成多个段。
/// @pre 子节点数量及轨道端点加法应在整数索引范围内，渲染不承担溢出恢复。
/// @note 拾取的子索引描述模型位置，不能因某段离屏而压缩重新编号。
/// @warning 逐折线快照热路径，复用滚动和图集缓存，不扫描其他实体。
void NoteRenderSystem::drawPolylineBody(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHold, entt::entity entity, bool generateHitboxes,
    HoverPart glowPart, int glowSubIndex,
    const CanvasLaneProjection* laneProjection)
{
    if ( note.m_subNotes.empty() ) return;
    // 非主体部位的高亮调用不生成连接几何，避免背景主体参与节点光晕叠加。
    if ( glowPart != HoverPart::None && glowPart != HoverPart::HoldBody )
        return;

    for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
        // 部位高亮只生成目标子段，避免一处悬浮令整条折线主体重复叠亮。
        if ( glowPart != HoverPart::None && glowSubIndex != -1 &&
             glowSubIndex != static_cast<int>(i) ) {
            continue;
        }

        const auto& sub = note.m_subNotes[i];

        // 子节点时间是绝对谱面时间，不需要再次加上折线根组件的 timestamp。
        double displayDeltaStart =
            cache->getDisplayDelta(sub.timestamp, currentAbsY, sub.timestamp);
        // 时间端点与 HS 锚点分开传递，长条末端仍继承头部的载体缩放。
        const double subEndTime       = getSubCarrierEndTime(sub);
        const double subEndAnchorTime = getSubCarrierEndAnchorTime(sub);
        double       displayDeltaEnd =
            cache->getDisplayDelta(subEndTime, currentAbsY, subEndAnchorTime);

        // 可见性函数接收滚动距离，先将画布上下界反解到相同单位。
        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        const auto startLane = resolveNoteLaneGeometry(
            sub.trackIndex, laneProjection, leftX, singleTrackW, noteW, noteH);
        // 滑键横向延伸改变后续连接的出发轨道，普通节点与 Hold 留在原轨。
        const std::int32_t subEndTrack = sub.type == ::MMM::NoteType::FLICK
                                             ? sub.trackIndex + sub.dtrack
                                             : sub.trackIndex;
        const auto         endLane     = resolveNoteLaneGeometry(
            subEndTrack, laneProjection, leftX, singleTrackW, noteW, noteH);
        // 将较高端点的纹理高度换回滚动单位，为临近视口边缘的载体留出余量。
        const double padDelta = std::max(startLane.noteH, endLane.noteH) /
                                static_cast<double>(renderScaleY);

        if ( !NoteRenderSystem::isCarrierVisible(sub.timestamp,
                                                 subEndTime,
                                                 currentTime,
                                                 displayDeltaStart,
                                                 displayDeltaEnd,
                                                 maxDelta + padDelta,
                                                 minDelta - padDelta) ) {
            continue;
        }

        float subStartY = judgmentLineY -
                          static_cast<float>(displayDeltaStart) * renderScaleY;

        // 非持续类型首尾同刻；只有正时长 Hold 在下方改写末端纵坐标。
        float subEndY = subStartY;

        // 自身主体与过渡连接分别生成：普通节点无本体，但仍可连接下一节点。
        if ( sub.type == ::MMM::NoteType::FLICK && sub.dtrack != 0 ) {
            auto itBodyH = snapshot->uvMap.find(
                static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
            if ( itBodyH != snapshot->uvMap.end() ) {
                // 横向主体的厚度沿用起始轨尺寸，长度则由两端真实中心距离决定。
                const float drawH =
                    startLane.noteH *
                    (itBodyH->second.w /
                     snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                // 连接体跨越两个真实轨道中心，独立区域间隙也属于连接范围。
                const float drawW =
                    std::abs(endLane.centerX() - startLane.centerX());
                // 向左滑动也使用正宽度矩形，方向信息交给末端箭头表示。
                const float bodyX =
                    std::min(startLane.centerX(), endLane.centerX());

                glm::vec4 finalBodyColor = colorHold;
                // 擦除反馈降低原透明度并改红，只影响命中的节点或整条折线。
                if ( snapshot->erasingEntities.count(entity) &&
                     (snapshot->erasingSubIndex == static_cast<int>(i) ||
                      snapshot->erasingSubIndex == 0 ||
                      snapshot->erasingSubIndex == -1) ) {
                    finalBodyColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
                }

                batcher.setTexture(TextureID::HoldBodyHorizontal);
                // 显式声明本段纹理，不能依赖上一节点恰好也为横向滑键。
                batcher.pushQuad(bodyX,
                                 subStartY + drawH * 0.5f,
                                 drawW,
                                 drawH,
                                 finalBodyColor);

                if ( generateHitboxes && entity != entt::null ) {
                    // 绘制接口以底边定位，拾取框则以左上角定位，二者相差一个高度。
                    // 使用根实体和子索引定位滑键主体，不创建临时子实体身份。
                    snapshot->hitboxes.push_back({ entity,
                                                   HoverPart::HoldBody,
                                                   static_cast<int>(i),
                                                   bodyX,
                                                   subStartY - drawH * 0.5f,
                                                   drawW,
                                                   drawH });
                }
            }
        } else if ( sub.type == ::MMM::NoteType::HOLD && sub.duration > 0 ) {
            // 零时长 Hold 不输出竖直主体，仍可由节点和装饰阶段表现端点。
            subEndY = judgmentLineY -
                      static_cast<float>(cache->getDisplayDelta(
                          subEndTime, currentAbsY, subEndAnchorTime)) *
                          renderScaleY;
            const glm::vec2 bodySize = getDrawSize(snapshot,
                                                   TextureID::HoldBodyVertical,
                                                   startLane.noteW,
                                                   startLane.noteH);
            // 长条主体宽度来自纹理相对比例，不以整条轨道宽度直接填满。
            const float bodyX =
                startLane.leftX + (startLane.width - bodySize.x) * 0.5F;

            glm::vec4 finalBodyColor = colorHold;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == static_cast<int>(i) ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalBodyColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldBodyVertical);
            // 本体的两个横向边缘共享宽度，纵向端点由各自的显示距离决定。
            float sy = judgmentLineY -
                       static_cast<float>(cache->getDisplayDelta(
                           sub.timestamp, currentAbsY, sub.timestamp)) *
                           renderScaleY;
            float ey = judgmentLineY -
                       static_cast<float>(cache->getDisplayDelta(
                           subEndTime, currentAbsY, subEndAnchorTime)) *
                           renderScaleY;
            // 自由四边形保留首尾投影顺序，不假定时间增加时 Y 一定减小。
            batcher.pushFreeQuad({ bodyX, sy },
                                 { bodyX + bodySize.x, sy },
                                 { bodyX + bodySize.x, ey },
                                 { bodyX, ey },
                                 finalBodyColor);

            if ( generateHitboxes && entity != entt::null ) {
                // 投影上下顺序可能反转，命中框统一使用较小 Y 与非负高度。
                // 命中框保持实际持续区间跨度，不包含两端装饰纹理额外伸出的部分。
                float hitY = std::min(subStartY, subEndY);
                float hitH = std::abs(subStartY - subEndY);
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               static_cast<int>(i),
                                               bodyX,
                                               hitY,
                                               bodySize.x,
                                               hitH });
            }
        }

        // 过渡 Body (连接当前子物件末尾到下一个子物件开头)
        if ( i + 1 < note.m_subNotes.size() ) {
            // 只连接相邻数组项，不跳过中间节点寻找另一个可见节点。
            // 下一节点使用自己的 HS 锚点，不延续当前 Hold 头部的锚点。
            const auto& next = note.m_subNotes[i + 1];
            float       nextStartY =
                judgmentLineY -
                static_cast<float>(cache->getDisplayDelta(
                    next.timestamp, currentAbsY, next.timestamp)) *
                    renderScaleY;
            const auto      nextLane = resolveNoteLaneGeometry(next.trackIndex,
                                                          laneProjection,
                                                          leftX,
                                                          singleTrackW,
                                                          noteW,
                                                          noteH);
            const glm::vec2 curBodySize =
                getDrawSize(snapshot,
                            TextureID::HoldBodyVertical,
                            endLane.noteW,
                            endLane.noteH);
            const glm::vec2 nextBodySize =
                getDrawSize(snapshot,
                            TextureID::HoldBodyVertical,
                            nextLane.noteW,
                            nextLane.noteH);
            // 两端分别按所在区域计算宽度，跨独立轨宽区域时允许形成梯形连接。
            const float curBodyX =
                endLane.leftX + (endLane.width - curBodySize.x) * 0.5F;
            const float nextBodyX =
                nextLane.leftX + (nextLane.width - nextBodySize.x) * 0.5F;

            glm::vec4 finalTransColor = colorHold;
            // 当前与下一端不插值两种乘色，整个连接段统一使用主体色。
            // 过渡段擦除反馈关联其到达节点，与子物件自身主体的索引规则不同。
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == static_cast<int>(i + 1) ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalTransColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldBodyVertical);

            // 同一时间的相邻节点仍保留连接请求，不在这里按时长删除拓扑边。
            // 过渡从载体结束处接出：Hold 要跳过自身持续区间，Flick
            // 已完成横向位移。
            double tStart       = getSubCarrierEndTime(sub);
            double tStartAnchor = getSubCarrierEndAnchorTime(sub);
            double tEnd         = next.timestamp;

            float sy =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    tStart, currentAbsY, tStartAnchor)) *
                                    renderScaleY;
            float ey =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    tEnd, currentAbsY, tEnd)) *
                                    renderScaleY;

            float x1 = curBodyX;
            float x2 = nextBodyX;

            // 两端各用自己的宽度和横坐标，不能退化成覆盖整段包围盒的矩形。
            batcher.pushFreeQuad({ x1, sy },
                                 { x1 + curBodySize.x, sy },
                                 { x2 + nextBodySize.x, ey },
                                 { x2, ey },
                                 finalTransColor);

            if ( generateHitboxes && entity != entt::null ) {
                // 拾取采用连接段轴对齐包围盒，不将斜四边形直接作为命中形状。
                // 因而盒内空白区域也可能命中，精确多边形拾取不由本函数提供。
                const float xmin = std::min(curBodyX, nextBodyX);
                // 右界要比较各端点加自身宽度，不能用左界差加某一端宽度代替。
                const float xmax = std::max(curBodyX + curBodySize.x,
                                            nextBodyX + nextBodySize.x);
                float       ymin = std::min(subEndY, nextStartY);
                float       ymax = std::max(subEndY, nextStartY);
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               static_cast<int>(i),
                                               xmin,
                                               ymin,
                                               xmax - xmin,
                                               ymax - ymin });
            }
        }
    }
}

/// @brief 绘制除首节点之外的折线节点及可选拾取框。
/// @param batcher 节点几何输出器。
/// @param note 内嵌节点所属的折线组件。
/// @param cache 节点时间的滚动映射，调用方保证有效。
/// @param snapshot 图集、擦除状态与拾取结果所在快照。
/// @param judgmentLineY 当前时间的纵向画布锚点。
/// @param leftX 后备轨道布局左边界。
/// @param singleTrackW 后备轨道宽度。
/// @param renderScaleY 滚动距离的显示倍率，需非零。
/// @param currentAbsY 当前画布的滚动空间锚点。
/// @param currentTime 节点可见性判断时间。
/// @param topY 可见区域上边界。
/// @param bottomY 可见区域下边界。
/// @param noteW 普通 Note 参考宽度。
/// @param noteH 普通 Note 参考高度。
/// @param colorNode 节点默认乘色。
/// @param config 节点纹理的填充模式设置。
/// @param entity 拾取与擦除反馈所属根实体。
/// @param generateHitboxes 是否输出节点命中框。
/// @param glowPart 只接受普通绘制或 PolylineNode 高亮。
/// @param glowSubIndex 高亮节点索引，-1 表示不限中间节点。
/// @param laneProjection 可选独立轨道投影，保持节点与所在区域尺寸一致。
/// @note 首节点由头部阶段单独使用 Note 纹理绘制，不在这里重复提交。
/// @note 中间节点的类型不改变 Node 纹理选择，持续区间由主体阶段负责。
/// @note 仅一个子节点的折线不进入本阶段，由头部及可选尾部阶段处理。
/// @note 节点的可见性独立判断，主体不可见并不直接阻止该节点阶段执行。
/// @pre 传入的参考尺寸与投影对象使用相同的玩家轨宽基准。
/// @warning 逐折线渲染热路径，只读取缓存与组件，不修改节点数据。
void NoteRenderSystem::drawPolylineNodes(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorNode, const Config::EditorConfig& config,
    entt::entity entity, bool generateHitboxes, HoverPart glowPart,
    int glowSubIndex, const CanvasLaneProjection* laneProjection)
{
    if ( note.m_subNotes.empty() ) return;
    if ( glowPart != HoverPart::None && glowPart != HoverPart::PolylineNode )
        return;

    // 首节点由单独阶段绘制；遍历索引仍保持原节点序号以对应编辑命令。
    for ( size_t i = 1; i < note.m_subNotes.size(); ++i ) {
        if ( glowPart != HoverPart::None && glowSubIndex != -1 &&
             glowSubIndex != static_cast<int>(i) ) {
            continue;
        }

        const auto& sub = note.m_subNotes[i];
        // 节点按自身起点定位，不能因为它是 Hold 而在末端再画一个 Node。
        double displayDeltaStart =
            cache->getDisplayDelta(sub.timestamp, currentAbsY, sub.timestamp);
        // 节点是单时刻载体，两端距离相等仍需保留纹理高度的可见性余量。
        double displayDeltaEnd = displayDeltaStart;

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        const auto lane = resolveNoteLaneGeometry(
            sub.trackIndex, laneProjection, leftX, singleTrackW, noteW, noteH);
        const double padDelta = lane.noteH / static_cast<double>(renderScaleY);

        // 剔除余量使用参考 Note 高度；实际 Node 尺寸在通过筛选后才按皮肤换算。
        if ( !NoteRenderSystem::isCarrierVisible(sub.timestamp,
                                                 sub.timestamp,
                                                 currentTime,
                                                 displayDeltaStart,
                                                 displayDeltaEnd,
                                                 maxDelta + padDelta,
                                                 minDelta - padDelta) ) {
            continue;
        }

        float subStartY = judgmentLineY -
                          static_cast<float>(displayDeltaStart) * renderScaleY;
        const glm::vec2 nodeSize =
            getDrawSize(snapshot, TextureID::Node, lane.noteW, lane.noteH);
        // Node 纹理可宽于轨道，围绕轨中心扩展而非固定左边缘。
        const float nodeX = lane.leftX + (lane.width - nodeSize.x) * 0.5F;

        // 擦除反馈是局部副本，不修改传入颜色或影响后面节点的默认乘色。
        glm::vec4 finalNodeColor = colorNode;
        if ( snapshot->erasingEntities.count(entity) &&
             (snapshot->erasingSubIndex == static_cast<int>(i) ||
              snapshot->erasingSubIndex == 0 ||
              snapshot->erasingSubIndex == -1) ) {
            finalNodeColor = { 1.0f, 0.2f, 0.2f, colorNode.a * 0.5f };
        }

        batcher.setTexture(TextureID::Node);
        // 物件填充模式只交给图元生成器处理，不改变节点的节奏时间或轨号。
        batcher.pushFilledQuad(
            nodeX,
            subStartY + nodeSize.y * 0.5f,
            nodeSize.x,
            nodeSize.y,
            { getTexAspect(snapshot, TextureID::Node), 1.0f },
            config.visual.noteFillMode,
            finalNodeColor);

        // 拾取框使用节点外接矩形，不随 Fit 留白缩到实际纹理不透明区域。
        if ( generateHitboxes && entity != entt::null ) {
            // 一个可见节点对应一个框，不把其后连接体合并成同一拾取区域。
            snapshot->hitboxes.push_back({ entity,
                                           HoverPart::PolylineNode,
                                           static_cast<int>(i),
                                           nodeX,
                                           subStartY - nodeSize.y * 0.5f,
                                           nodeSize.x,
                                           nodeSize.y });
        }
    }
}

/// @brief 在第一个内嵌节点的位置绘制折线头部。
/// @param batcher 头部图元输出器。
/// @param note 折线组件，首内嵌节点才是实际头部定位来源。
/// @param cache 已建立的滚动映射。
/// @param snapshot 纹理及交互状态来源，同时接收拾取框。
/// @param judgmentLineY 当前时间对应的画布纵坐标。
/// @param leftX 默认轨道区起点。
/// @param singleTrackW 默认轨道宽度。
/// @param renderScaleY 滚动到像素空间的非零倍率。
/// @param currentAbsY 滚动空间原点。
/// @param currentTime 头部的可见性判断时间。
/// @param topY 可见区域上边界。
/// @param bottomY 可见区域下边界。
/// @param noteW 普通头部的参考宽度。
/// @param noteH 普通头部的参考高度。
/// @param colorHead 头部乘色。
/// @param config 普通 Note 纹理的填充模式。
/// @param entity 根实体标识，不为首节点另造实体。
/// @param generateHitboxes 是否生成首节点命中框。
/// @param glowPart 高亮时必须为 PolylineNode，而非普通音符的 Head。
/// @param glowSubIndex 高亮时必须明确指向首节点 0。
/// @param laneProjection 首节点轨道的可选独立投影。
/// @note 头部命中仍编码为 PolylineNode/0，交互端据此访问内嵌节点。
/// @note 无实体身份的预览仍可输出图元，但不会产生可编辑的拾取记录。
/// @note 第一个节点的类型不改变头部纹理；Hold 或 Flick 头也使用普通 Note。
/// @note 头部高亮要求明确索引 0，和中间节点允许 -1 的筛选规则不同。
/// @warning 渲染热路径只读取首节点，不遍历整条折线重新推导头部。
void NoteRenderSystem::drawPolylineHead(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHead, const Config::EditorConfig& config,
    entt::entity entity, bool generateHitboxes, HoverPart glowPart,
    int glowSubIndex, const CanvasLaneProjection* laneProjection)
{
    if ( glowPart != HoverPart::None &&
         !(glowPart == HoverPart::PolylineNode && glowSubIndex == 0) )
        return;

    if ( note.m_subNotes.empty() ) return;

    // 根组件时间可承担容器语义；绘制实际头部使用首节点自身的时间与轨道。
    const auto& firstSub = note.m_subNotes.front();

    double displayDeltaStart = cache->getDisplayDelta(
        firstSub.timestamp, currentAbsY, firstSub.timestamp);
    // 只判断头部时刻可见性；首节点为 Hold 也不让其尾部替头部保活。
    double displayDeltaEnd = displayDeltaStart;

    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    const auto lane = resolveNoteLaneGeometry(
        firstSub.trackIndex, laneProjection, leftX, singleTrackW, noteW, noteH);
    const double padDelta = lane.noteH / static_cast<double>(renderScaleY);

    if ( !NoteRenderSystem::isCarrierVisible(firstSub.timestamp,
                                             firstSub.timestamp,
                                             currentTime,
                                             displayDeltaStart,
                                             displayDeltaEnd,
                                             maxDelta + padDelta,
                                             minDelta - padDelta) ) {
        return;
    }

    float headY =
        judgmentLineY - static_cast<float>(displayDeltaStart) * renderScaleY;
    const glm::vec2 headSize =
        getDrawSize(snapshot, TextureID::Note, lane.noteW, lane.noteH);
    const float headX = lane.leftX + (lane.width - headSize.x) * 0.5F;

    // 头部几何以轨中心与时间中心对齐，用户缩放不改变它对应的拍位。
    glm::vec4 finalHeadColor = colorHead;
    // 只擦除其他节点时不将头部染红；0 和 -1 分别包含头部或整条折线反馈。
    if ( snapshot->erasingEntities.count(entity) &&
         (snapshot->erasingSubIndex == 0 || snapshot->erasingSubIndex == -1) ) {
        finalHeadColor = { 1.0f, 0.2f, 0.2f, colorHead.a * 0.5f };
    }

    batcher.setTexture(TextureID::Note);
    // 填充模式可能留白或裁纹理，头部命中框仍按完整布局矩形登记。
    batcher.pushFilledQuad(headX,
                           headY + headSize.y * 0.5f,
                           headSize.x,
                           headSize.y,
                           { getTexAspect(snapshot, TextureID::Note), 1.0f },
                           config.visual.noteFillMode,
                           finalHeadColor);

    if ( generateHitboxes && entity != entt::null ) {
        snapshot->hitboxes.push_back({ entity,
                                       HoverPart::PolylineNode,
                                       0,
                                       headX,
                                       headY - headSize.y * 0.5f,
                                       headSize.x,
                                       headSize.y });
    }
}

/// @brief 按最后一个子物件类型绘制滑键箭头或长条结束线。
/// @param batcher 装饰图元输出器。
/// @param note 提供最终子物件的折线组件。
/// @param cache 末端滚动距离及 HS 映射来源。
/// @param snapshot 图集、擦除状态及命中框输出。
/// @param judgmentLineY 当前画布判定线位置。
/// @param leftX 默认轨道区左边界。
/// @param singleTrackW 后备轨道宽度。
/// @param renderScaleY 滚动空间到画布的非零倍率。
/// @param currentAbsY 当前滚动位置锚点。
/// @param currentTime 末端可见性判断时间。
/// @param topY 可见区域上边界。
/// @param bottomY 可见区域下边界。
/// @param noteW 普通 Note 参考宽度。
/// @param noteH 普通 Note 参考高度。
/// @param colorHoldEnd 长条结束线乘色。
/// @param colorArrow 滑键箭头乘色。
/// @param config 装饰纹理填充模式。
/// @param entity 整条折线所属实体。
/// @param generateHitboxes 是否为末端装饰输出独立部位命中框。
/// @param glowPart 高亮请求的部位，必须与末端类型对应。
/// @param glowSubIndex 高亮时必须指向最后节点，不将 -1 作为全部装饰匹配。
/// @param laneProjection 用于末端真实轨道的可选投影。
/// @note 普通 Note 尾节点不追加装饰；中间节点的箭头和结束线不由本函数绘制。
/// @note 装饰的拾取框归属于根实体与末节点索引，不生成独立 ECS 实体。
/// @note 首节点为最终节点时也允许尾部装饰，头部与末端身份并不互斥。
/// @note 末端可见性筛选先于类型分支，缺少可见末端时不追加装饰命中框。
/// @note 即使主体纹理缺失，装饰阶段仍按自身纹理和可见性独立尝试绘制。
/// @warning 折线渲染热路径只处理末节点，不引入资源查询之外的阻塞工作。
void NoteRenderSystem::drawPolylineDecoration(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHoldEnd, glm::vec4 colorArrow,
    const Config::EditorConfig& config, entt::entity entity,
    bool generateHitboxes, HoverPart glowPart, int glowSubIndex,
    const CanvasLaneProjection* laneProjection)
{
    if ( note.m_subNotes.empty() ) return;

    int lastIdx = static_cast<int>(note.m_subNotes.size() - 1);
    // 装饰仅属于最终节点，先筛选索引再按类型决定使用哪种末端图元。
    const auto& last = note.m_subNotes.back();
    // 普通遍不限制部位，高亮遍必须同时命中末节点索引与可接受的装饰部位。
    bool isLastGlow =
        (glowPart == HoverPart::None) ||
        (glowPart == HoverPart::FlickArrow && glowSubIndex == lastIdx) ||
        (glowPart == HoverPart::HoldEnd && glowSubIndex == lastIdx);

    if ( !isLastGlow ) return;

    // 先按末端本身进行剔除，主体仍在视口内并不意味着离屏装饰也要提交。
    double targetTime = getSubCarrierEndTime(last);
    // 长条末端取结束时间但沿用头部 HS 锚点，滑键则仍位于起点时间。
    double targetAnchorTime = getSubCarrierEndAnchorTime(last);
    double displayDelta =
        cache->getDisplayDelta(targetTime, currentAbsY, targetAnchorTime);
    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    const std::int32_t decorationTrack = last.type == ::MMM::NoteType::FLICK
                                             ? last.trackIndex + last.dtrack
                                             : last.trackIndex;
    // 箭头放在滑键到达轨，不以滑键起始轨的独立区域宽度计算末端尺寸。
    const auto lane = resolveNoteLaneGeometry(
        decorationTrack, laneProjection, leftX, singleTrackW, noteW, noteH);
    const double padDelta = lane.noteH / static_cast<double>(renderScaleY);

    if ( !NoteRenderSystem::isCarrierVisible(targetTime,
                                             targetTime,
                                             currentTime,
                                             displayDelta,
                                             displayDelta,
                                             maxDelta + padDelta,
                                             minDelta - padDelta) ) {
        return;
    }

    float lStartY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            last.timestamp, currentAbsY, last.timestamp)) *
                            renderScaleY;

    if ( last.type == ::MMM::NoteType::FLICK ) {
        if ( glowPart == HoverPart::None ||
             glowPart == HoverPart::FlickArrow ) {
            // 方向由带符号轨差决定，零轨差保留既有右箭头选择。
            TextureID arrowId = (last.dtrack < 0) ? TextureID::FlickArrowLeft
                                                  : TextureID::FlickArrowRight;
            const glm::vec2 arrowSize =
                getDrawSize(snapshot, arrowId, lane.noteW, lane.noteH);
            // 左右箭头可能有不同皮肤尺寸，各自以落点轨道中心对齐。
            const float arrowX = lane.leftX + (lane.width - arrowSize.x) * 0.5F;

            glm::vec4 finalArrowColor = colorArrow;
            // 装饰保留调用方透明度，只在擦除反馈中额外衰减，不重算全局 alpha。
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == lastIdx ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalArrowColor = { 1.0f, 0.2f, 0.2f, colorArrow.a * 0.5f };
            }

            batcher.setTexture(arrowId);
            // 选择左右纹理而非翻转顶点顺序，保留皮肤分别提供两种箭头的能力。
            batcher.pushFilledQuad(arrowX,
                                   lStartY + arrowSize.y * 0.5f,
                                   arrowSize.x,
                                   arrowSize.y,
                                   { getTexAspect(snapshot, arrowId), 1.0f },
                                   config.visual.noteFillMode,
                                   finalArrowColor);

            if ( generateHitboxes && entity != entt::null ) {
                // 箭头命中在落点轨道，仍通过末节点索引编辑原滑键的轨差。
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::FlickArrow,
                                               lastIdx,
                                               arrowX,
                                               lStartY - arrowSize.y * 0.5f,
                                               arrowSize.x,
                                               arrowSize.y });
            }
        }
    } else if ( last.type == ::MMM::NoteType::HOLD ) {
        if ( glowPart == HoverPart::None || glowPart == HoverPart::HoldEnd ) {
            // 即使时长为零也允许绘制结束线，此处不复用正时长主体的筛选条件。
            float subEndY = judgmentLineY -
                            static_cast<float>(cache->getDisplayDelta(
                                targetTime, currentAbsY, targetAnchorTime)) *
                                renderScaleY;
            const glm::vec2 endSize = getDrawSize(
                snapshot, TextureID::HoldEnd, lane.noteW, lane.noteH);
            // 结束线使用独立纹理比例，不强制沿用竖直主体宽度。
            const float endX = lane.leftX + (lane.width - endSize.x) * 0.5F;

            glm::vec4 finalEndColor = colorHoldEnd;
            // 尾部反馈允许由整条擦除或末节点擦除触发，不读取中间节点的独立状态。
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == lastIdx ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalEndColor = { 1.0f, 0.2f, 0.2f, colorHoldEnd.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldEnd);
            // 尾线是以结束时刻居中的独立贴片，不沿持续区间拉伸整张纹理。
            batcher.pushFilledQuad(
                endX,
                subEndY + endSize.y * 0.5f,
                endSize.x,
                endSize.y,
                { getTexAspect(snapshot, TextureID::HoldEnd), 1.0f },
                config.visual.noteFillMode,
                finalEndColor);

            if ( generateHitboxes && entity != entt::null ) {
                // 使用 HoldEnd 而非主体部位，让尾部持续时间编辑能识别这个区域。
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldEnd,
                                               lastIdx,
                                               endX,
                                               subEndY - endSize.y * 0.5f,
                                               endSize.x,
                                               endSize.y });
            }
        }
    }
}

}  // namespace MMM::Logic::System
