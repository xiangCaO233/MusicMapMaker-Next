#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/GrabTool.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

/// @file PolylineFirstBodyDragTest.cpp
/// @brief 以真实拖动命令验证折线首段身体与内部节点的编辑语义。
/// @details 首段头部、首段身体和内部节点虽然共用父折线，
/// 但各自需要不同位移范围。测试明确指定按下帧的命中部位，
/// 避免滞后的悬停状态把子段编辑升级成整物件移动。
/// 子段同时存在父内嵌列表和独立 ECS 子实体投影，
/// 拖动、释放、撤销和重做后两份几何都必须一致。
/// 仅检查父列表会漏掉拾取位置滞后的错误。
/// 已选中根时另放一颗已选 Note，作为整组选中误入的哨兵。
/// 未选中根时复用相同命中，覆盖旧的整体移动分支。
/// Hold 节点横移时，前一 Flick 终轨要接上新节点并移动后缀。
/// Flick 节点纵移时，前一 Hold 尾端要接上新时间并移动后缀。
/// 首节点索引零仍用于整条移动，不与内部局部编辑混淆。
/// 测试不启动 UI，也不改动个人项目、布局或配置文件。
/// 一次拖动始终用按下时状态计算位移，不能逐帧累加误差。
/// 根被选中时不得把其它选中 Note 纳入节点的编辑历史。
/// 未选中时也不得以整物件移动代替连接段调整。
/// 前一段的 Flick 轨差与 Hold 时长分别表达横向和纵向接缝。
/// 后续子段整体平移后，其自身长度与轨差仍保持原结构。
/// 命令负载中的部位和子索引应覆盖旧悬停数据。
/// 预览阶段就要同步子实体，使下一帧拾取与视觉一致。
/// 历史动作要覆盖父、子实体，同时不重建未改变的身份。

namespace
{
/// @brief 首节点身体与头部的回归场景。
enum class DragCase { HoldBody, FlickBody, HoldHead };

/// @brief 为单条折线建立确定的时间、轨道和画布映射。
/// @param ctx 独立测试会话，不加载外部资源或音频设备。
/// @warning 仅在每个用例开始时构建滚动缓存，不进入拖拽热路径。
/// @note 固定玩家轨宽使横移目标不依赖本机屏幕缩放。
/// @note 使用生产 ScrollCache 逆映射，不把像素差误当成秒数。
/// @note 开启专业模式以覆盖可能接管拖动的统一轨道路径。
void configure(MMM::Logic::SessionContext& ctx)
{
    // 四条玩家轨各占 100 像素，150 与 250 分别是轨道零和一。
    // 判定线在 y=300，向上移动鼠标得到晚于当前动画时间的目标。
    ctx.currentBeatmap = std::make_shared<MMM::BeatMap>();
    ctx.currentBeatmap->m_baseMapMetadata.track_count = 4;
    ctx.trackCount                                    = 4;
    ctx.currentTime                                   = 1.0;
    ctx.animateTime                                   = 1.0;
    ctx.lastConfig.visual.trackLayout.left            = 0.1F;
    ctx.lastConfig.visual.trackLayout.right           = 0.5F;
    ctx.lastConfig.visual.judgeline_pos               = 0.5F;
    ctx.lastConfig.settings.enablePolylineEditing     = true;
    ctx.lastConfig.settings.professionalMode          = true;
    ctx.cameras.emplace(
        "Basic2DCanvas",
        MMM::Logic::CameraInfo{ "Basic2DCanvas", 1000.0F, 600.0F, 0.0F });
    auto& cache =
        ctx.timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(
        ctx.timelineRegistry, ctx.lastConfig, ctx.currentBeatmap.get());
}

/// @brief 检查首段身体拖动后新前缀与所有旧子项的关系。
/// @param ctx 已完成拖动的会话。
/// @param root 原折线根实体，整个局部编辑中保持同一身份。
/// @param children 原有子实体身份，必须在插入新前缀后继续有效。
/// @param kind 首段类型，用于选择横向 Flick 或纵向 Hold 前缀。
/// @return 几何、父子索引与独立子实体一致时返回 true。
/// @par 共通结构契约
/// 根实体和全部旧子实体保留原身份，新前缀另占一个新子实体。
/// 根时间与轨道继续指向原头，不等于被拖动的旧首子项位置。
/// 父内嵌列表恰好多一项，旧子实体按原顺序顺延到索引一开始。
/// 子实体父引用仍指向原根，时间和轨道与内嵌列表对应项一致。
/// 新前缀还需要一份索引零的 ECS 子投影，供后续拾取和属性编辑。
/// 索引零不能存在两个子投影，否则鼠标命中会因遍历顺序不稳定。
/// 父内嵌前缀与新投影必须共享同一个稳定协作 ID。
/// 旧子实体的协作 ID 不能与新前缀重复。
/// @par Hold 首段身体
/// 原后缀逐段横移同一轨差，不改变原段间横向布局。
/// 新 Flick 位于旧头时间和轨道，终轨接到已移动的旧 Hold。
/// 原 Hold 的持续时间和后续 Flick 的轨差不因横移改变。
/// @par Flick 首段身体
/// 原后缀逐段纵移同一时间差，不改变原段间节奏间隔。
/// 新 Hold 位于旧头轨道，持续到旧 Flick 的新起点。
/// 原 Flick 的轨差和后续 Hold 的起轨仍为原值。
/// 向下拖动先验证钳制到原起点，再反向拖动验证没有累计误差。
/// 钳制后的预览仍是旧列表，不应在尚未松开时插入负时长段。
/// 向上拖动后的新持续时间使用同一次手势的原始快照计算。
/// @par 失败含义
/// 根锚点变化说明身体命中被误判为整条移动。
/// 旧子身份失效说明实现使用删除重建代替索引顺延。
/// 父子时间或轨道不一致说明预览只更新了一套结构表示。
/// 前缀类型错误说明 Hold 与 Flick 的正交连接规则被交换。
/// 后缀最后一项未移动说明实现只改动了旧首项。
/// 子索引未加一说明新增前缀挤占了原子实体的拾取身份。
/// 前缀起点偏离旧根说明更新把根锚点也一起拖走了。
bool checkBodyResult(const MMM::Logic::SessionContext& ctx, entt::entity root,
                     const std::vector<entt::entity>& children, DragCase kind)
{
    using Note         = MMM::Logic::NoteComponent;
    const auto& parent = ctx.noteRegistry.get<const Note>(root);
    // 新前缀占索引零，旧子项的顺序和实体身份全部顺延一位。
    // 根锚点仍是原先头部，不随被拖动的旧首子项移动。
    if ( parent.m_type != MMM::NoteType::POLYLINE ||
         parent.m_timestamp != 1.0 || parent.m_trackIndex != 0 ||
         parent.m_subNotes.size() != children.size() + 1U ) {
        return false;
    }
    // 新前缀和旧首节点不得共享协作身份。
    if ( parent.m_subNotes.front().collaborationId.empty() ) return false;
    // 前缀必须拥有实际 ECS 子投影，不能只把数组加长而留下不可拾取的空洞。
    // 按父身份与索引识别，避免依赖新实体的 Registry 分配顺序。
    int prefixCount = 0;
    for ( const auto entity : ctx.noteRegistry.view<const Note>() ) {
        const auto& projected = ctx.noteRegistry.get<const Note>(entity);
        if ( projected.m_isSubNote && projected.m_parentPolyline == root &&
             projected.m_subIndex == 0 ) {
            if ( projected.m_collaborationId !=
                 parent.m_subNotes.front().collaborationId ) {
                return false;
            }
            ++prefixCount;
        }
    }
    // 同一父折线只能有一个前缀投影；重复节点会导致双重拾取。
    if ( prefixCount != 1 ) return false;
    for ( std::size_t i = 0; i < children.size(); ++i ) {
        // 原投影不得被删除后重建成新身份，否则选择与协作引用会漂移。
        // 这里用开始手势前保存的实体号核对，而不是按顺序数新实体。
        if ( !ctx.noteRegistry.valid(children[i]) ) return false;
        const auto& child = ctx.noteRegistry.get<const Note>(children[i]);
        // 比较父内嵌列表和投影子实体，防止视觉与拾取使用不同的索引。
        if ( !child.m_isSubNote || child.m_parentPolyline != root ||
             child.m_subIndex != static_cast<int>(i + 1U) ||
             child.m_timestamp != parent.m_subNotes[i + 1U].timestamp ||
             child.m_trackIndex != parent.m_subNotes[i + 1U].trackIndex ||
             child.m_collaborationId !=
                 parent.m_subNotes[i + 1U].collaborationId ||
             child.m_collaborationId ==
                 parent.m_subNotes.front().collaborationId ) {
            return false;
        }
    }
    if ( kind == DragCase::HoldBody ) {
        // 原 Hold 与后续两段均横移一轨；后缀内部轨差和时间未改变。
        // 新 Flick 留在旧头时刻、旧头轨道并向右衔接到原 Hold。
        // 检查末段可防止实现仅移动第一个旧子项而断开后继连接。
        // 第二项是 Flick，其终轨应继续和末段 Hold 的起轨衔接。
        return parent.m_subNotes[0].type == MMM::NoteType::FLICK &&
               parent.m_subNotes[0].timestamp == 1.0 &&
               parent.m_subNotes[0].trackIndex == 0 &&
               parent.m_subNotes[0].dtrack == 1 &&
               parent.m_subNotes[1].type == MMM::NoteType::HOLD &&
               parent.m_subNotes[1].trackIndex == 1 &&
               parent.m_subNotes[2].trackIndex == 1 &&
               parent.m_subNotes[3].trackIndex == 2;
    }
    // 原 Flick 与后续 Hold 同量后移；向下拖动已被钳制在原始时间。
    // 前缀 Hold 的结束时间应精确接上旧 Flick 的新起点。
    const double shift = parent.m_subNotes[1].timestamp - 1.0;
    // 实际吸附时间由缓存求得，测试不假定固定像素与秒的换算比。
    // 前缀时长和旧后缀位移相同才不会留下时间裂缝。
    return parent.m_subNotes[0].type == MMM::NoteType::HOLD &&
           parent.m_subNotes[0].timestamp == 1.0 &&
           parent.m_subNotes[0].duration > 0.0 &&
           std::abs(parent.m_subNotes[0].duration - shift) < 1e-7 &&
           parent.m_subNotes[1].type == MMM::NoteType::FLICK &&
           parent.m_subNotes[1].trackIndex == 0 &&
           parent.m_subNotes[2].trackIndex == 1 &&
           std::abs(parent.m_subNotes[2].timestamp -
                    parent.m_subNotes[1].timestamp) < 1e-7;
}

/// @brief 执行一个真实的 Start/Update/End 拖动事务并往返撤销。
/// @param kind 头部整体移动或两种首段身体局部移动。
/// @return 前缀、后缀、选择集合与历史均符合预期时返回 true。
/// @note Hold 身体场景同时选中另一个物件，确保局部拖动不移动选中组。
/// @par 命中与选择
/// HoldBody/0 与 PolylineNode/0 共享索引，但用户意图不同。
/// 身体局部编辑并插入前缀，头部维持已有的整条拖动语义。
/// Flick 横向身体也由 HoldBody 命中框登记。
/// 已选中根的首段身体仍不展开其他选中物件组成的移动组。
/// @par 历史
/// 一次手势只占一个动作，包含根、旧子实体及新前缀的变更。
/// Undo 恢复旧列表和旧子索引，并撤销新增前缀实体。
/// Redo 再次得到前缀，旧子实体必须保持相同身份。
/// @par 测试边界
/// 此用例覆盖拖动命令到正式画布快照，不模拟 OS 鼠标事件路由。
/// 不覆盖跨域转换，因为首段身体编辑始终留在原有轨道域。
/// 空时间线提供确定的吸附结果，具体像素换算仍由生产缓存完成。
/// @par 预览快照
/// 松键前的根必须保留旧锚点，并已含可见的正交前缀。
/// 快照必须同时具有前缀和旧首项的身体命中，且存在实际绘制命令。
/// 只检查父内嵌列表会漏掉渲染过滤或空纹理造成的不可见预览。
/// 子投影不进入排序绘制列表，避免它代替父根满足身体断言。
/// 鼠标回原点时撤销临时前缀，再前进时只恢复一份前缀。
/// 松手后的 Undo 仍从起笔快照恢复，不受临时列表插入影响。
bool runCase(DragCase kind)
{
    using Note = MMM::Logic::NoteComponent;
    MMM::Logic::SessionContext ctx;
    configure(ctx);
    Note parent;
    parent.m_type       = MMM::NoteType::POLYLINE;
    parent.m_timestamp  = 1.0;
    parent.m_trackIndex = 0;
    if ( kind == DragCase::FlickBody ) {
        parent.m_subNotes = {
            Note::SubNote{ .type       = MMM::NoteType::FLICK,
                           .timestamp  = 1.0,
                           .trackIndex = 0,
                           .dtrack     = 1 },
            Note::SubNote{ .type       = MMM::NoteType::HOLD,
                           .timestamp  = 1.0,
                           .duration   = 0.5,
                           .trackIndex = 1 },
        };
    } else {
        parent.m_subNotes = {
            Note::SubNote{ .type       = MMM::NoteType::HOLD,
                           .timestamp  = 1.0,
                           .duration   = 0.5,
                           .trackIndex = 0 },
            Note::SubNote{ .type       = MMM::NoteType::FLICK,
                           .timestamp  = 1.5,
                           .trackIndex = 0,
                           .dtrack     = 1 },
            Note::SubNote{ .type       = MMM::NoteType::HOLD,
                           .timestamp  = 1.5,
                           .duration   = 0.5,
                           .trackIndex = 1 },
        };
    }
    // 真实已加载折线的父内嵌子项与子投影共享协作 ID。
    // 先统一编号，再构建投影；头部整体移动也据此检查 Undo。
    MMM::Logic::ensureNoteCollaborationIdentity(parent);
    const auto original = parent.m_subNotes;
    // 快照早于真实拖动，不从 Undo 结果反推期望值。
    // 父列表与子实体共同构成一条折线的完整编辑状态。
    const auto root = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(root, parent);
    ctx.noteRegistry.emplace<MMM::Logic::TransformComponent>(root);
    const bool selected = kind == DragCase::HoldBody;
    // 最复杂的 Hold 身体用例同时选中根与另一个独立 Note。
    // 若错误走整组选中路径，另一个 Note 会成为明显的移动哨兵。
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        root, MMM::Logic::InteractionComponent{ .isSelected = selected });
    std::vector<entt::entity> children;
    // 投影子实体按父列表物化，撤销后需逐个恢复原索引和原位置。
    // 只在父数组里放节点会漏掉 ECS 投影索引与拾取状态的回归。
    // 每个子项使用真实的父引用和原索引，供批量动作更新关系。
    for ( std::size_t i = 0; i < original.size(); ++i ) {
        const auto entity = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<Note>(
            entity,
            MMM::Logic::makeNoteComponentFromSubNote(
                original[i], true, root, static_cast<int>(i)));
        ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
        children.push_back(entity);
    }
    const auto other = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(other,
                                   Note{ .m_type       = MMM::NoteType::NOTE,
                                         .m_timestamp  = 3.0,
                                         .m_trackIndex = 3 });
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        other, MMM::Logic::InteractionComponent{ .isSelected = selected });
    // 哨兵位于最右轨，横移一轨会立即暴露错误的选中组联动。
    // 它不属于这条折线，不能被局部编辑收入初始快照。
    ctx.hoveredEntity     = root;
    ctx.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    const auto hitPart    = kind == DragCase::HoldHead
                                ? MMM::Logic::HoverPart::PolylineNode
                                : MMM::Logic::HoverPart::HoldBody;
    // 悬停状态故意留在头部：画布按下帧必须由命令携带的身体命中决定路径。
    ctx.hoveredPart     = static_cast<int>(MMM::Logic::HoverPart::PolylineNode);
    ctx.hoveredSubIndex = -1;
    // 两种手势使用同一个根和子索引，唯独命中部位不同。
    // 因此结构差异不能归因于改变了被抓实体或节点序号。

    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(
        ctx,
        MMM::Logic::CmdStartDrag{ root,
                                  "Basic2DCanvas",
                                  false,
                                  MMM::Logic::ChartObjectKind::PlayerNote,
                                  static_cast<std::uint8_t>(hitPart),
                                  0 });
    if ( kind == DragCase::FlickBody ) {
        // 先尝试向下拉，不能预览负持续时间的前置 Hold。
        // 再向上拉仍应从原始快照计算，不能累计被钳制的错误位移。
        tool.handleUpdateDrag(
            ctx,
            MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 150.0F, 450.0F, true });
        if ( ctx.noteRegistry.get<const Note>(root)
                 .m_subNotes.front()
                 .timestamp != 1.0 ) {
            return false;
        }
    }
    // 横移一个玩家轨；Flick 身体改向上拖动以增加时间。
    // 头部命中仍是整体移动，测试它不会插入局部连接段。
    const float mouseX = kind == DragCase::FlickBody ? 150.0F : 250.0F;
    const float mouseY = kind == DragCase::FlickBody ? 200.0F : 300.0F;
    tool.handleUpdateDrag(
        ctx,
        MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", mouseX, mouseY, true });
    if ( kind != DragCase::HoldHead ) {
        // 先将手势返回原位置，再移动到目标，覆盖前缀撤销与重建。
        // 鼠标折返时不能留下上一次的连接段或重复插入两条前缀。
        // 这也是连续拖动实际会出现的路径，不只测试一次性位移。
        // 前缀消失后子投影索引也必须回到原值，后续插入才不会错位。
        // 回原点并非结束事务，随后仍应可继续向新的吸附位置移动。
        tool.handleUpdateDrag(
            ctx,
            MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", 150.0F, 300.0F, true });
        if ( ctx.noteRegistry.get<const Note>(root).m_subNotes.size() !=
             original.size() ) {
            XERROR(
                "Polyline preview retained prefix after returning to origin");
            return false;
        }
        tool.handleUpdateDrag(
            ctx,
            MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", mouseX, mouseY, true });
        const auto& preview = ctx.noteRegistry.get<const Note>(root);
        // 松键前就应给渲染系统一条完整折线，而不是只有平移的旧后缀。
        // 根时间和根轨号仍是旧锚点；新增前缀填补它与后缀的间隙。
        // 若直接把根跟着后缀移走，松手后的正确提交也无法修复视觉过程。
        if ( preview.m_subNotes.size() != original.size() + 1U ||
             preview.m_timestamp != original.front().timestamp ||
             preview.m_trackIndex != original.front().trackIndex ) {
            XERROR("Polyline first body preview lacks the leading carrier");
            return false;
        }
        // 用正式快照入口检查画布实际拿到前缀与移动后的旧身体。
        // 仅把根放入排序输入；子投影由父折线绘制，不能单独冒充前缀。
        // 本断言覆盖输入后的快照生成，而非只验证历史动作的最终结果。
        // 局部 vector 生命周期覆盖同步快照调用，指针不会逃逸到测试外。
        const std::vector<entt::entity> sortedNotes{ root };
        ctx.noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(
            &sortedNotes);
        MMM::Logic::RenderSnapshot snapshot;
        snapshot.hasBeatmap         = true;
        snapshot.acceptsInteraction = true;
        // 头部、横段和竖段分别给出测试图块，避免空 UV 导致假阴性。
        // 正式渲染器用这些图块计算身体尺寸和对应的命中包围盒。
        // 用互异 UV 区间，调试绘制命令时能够辨认实际使用的纹理。
        // None 图块只服务基础线条，不能充当 Note 身体的可见证据。
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
            glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
            glm::vec4{ 0.1F, 0.1F, 0.1F, 0.1F });
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(MMM::Logic::TextureID::HoldBodyVertical),
            glm::vec4{ 0.2F, 0.1F, 0.1F, 0.1F });
        snapshot.uvMap.emplace(static_cast<std::uint32_t>(
                                   MMM::Logic::TextureID::HoldBodyHorizontal),
                               glm::vec4{ 0.3F, 0.1F, 0.1F, 0.1F });
        const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents;
        MMM::Logic::System::NoteRenderSystem::generateSnapshot(
            ctx.noteRegistry,
            ctx.sampleRegistry,
            {},
            {},
            ctx.timelineRegistry,
            bpmEvents,
            &snapshot,
            "Basic2DCanvas",
            ctx.currentTime,
            1000.0F,
            600.0F,
            300.0F,
            ctx.trackCount,
            ctx.bgmTrackCount,
            ctx.draftTrackCount,
            ctx.lastConfig,
            600.0F);
        // 前缀索引零和旧身体索引一必须同时出现在同一帧。
        // 退化的零宽或零高包围盒不可视，也不算完成预览。
        // 绘制命令须非空，避免只有命中数据而用户看不到连接段。
        // 背景绘制也会产生命令，所以实体命中还必须精确核对根与索引。
        // 不要求特定皮肤像素颜色，只验证几何组成和可交互范围。
        // 这能同时覆盖主画布与后续皮肤缩放的共同几何入口。
        const auto hasBody = [&](int index) {
            return std::ranges::any_of(snapshot.hitboxes, [&](const auto& box) {
                return box.entity == root &&
                       box.part == MMM::Logic::HoverPart::HoldBody &&
                       box.subIndex == index && box.w > 0.0F && box.h > 0.0F;
            });
        };
        if ( snapshot.cmds.empty() || !hasBody(0) || !hasBody(1) ) {
            XERROR("Polyline preview snapshot misses prefix or old body");
            return false;
        }
        // 渲染检查完成后才释放鼠标，不能把提交后的完整结构当作预览。
    }
    tool.handleEndDrag(ctx, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });
    // 插入前缀、顺延旧索引和修改旧后缀必须共用一次历史提交。
    // 选中组中的哨兵仍应停留在原轨，证明身体没有走整体路径。
    if ( ctx.actionStack.getUndoStackSize() != 1U ||
         ctx.noteRegistry.get<const Note>(other).m_trackIndex != 3 ) {
        XERROR("Polyline first body drag moved selection or skipped history");
        return false;
    }
    if ( kind == DragCase::HoldHead ) {
        const auto& moved = ctx.noteRegistry.get<const Note>(root);
        // 只有真实首节点头部命中才沿用整条移动，节点数不变。
        // 根及全部旧节点一起横移一轨，不允许身体专属前缀混入。
        // 此分支也保护既有整条拖动行为不因局部特例而退化。
        if ( moved.m_trackIndex != 1 ||
             moved.m_subNotes.size() != original.size() ||
             moved.m_subNotes[0].trackIndex != 1 ||
             moved.m_subNotes[1].trackIndex != 1 ) {
            return false;
        }
    } else if ( !checkBodyResult(ctx, root, children, kind) ) {
        XERROR("Polyline first body drag did not prepend the correct carrier");
        return false;
    }
    ctx.actionStack.undo(ctx);
    const auto& restored = ctx.noteRegistry.get<const Note>(root);
    // 父列表恢复旧长度之外，还须检查旧起点没有被新前缀污染。
    // 如果只修复视觉几何却留下错位索引，随后拾取仍会指向错误子项。
    if ( restored.m_timestamp != 1.0 || restored.m_trackIndex != 0 ||
         restored.m_subNotes.size() != original.size() ) {
        XERROR(
            "Polyline first body undo did not restore parent: case={} time={} "
            "track={} count={}",
            static_cast<int>(kind),
            restored.m_timestamp,
            restored.m_trackIndex,
            restored.m_subNotes.size());
        return false;
    }
    for ( std::size_t i = 0; i < children.size(); ++i ) {
        // 按旧实体号逐项核对，Undo 不允许生成替身节点代替旧对象。
        // 与父列表单独核对能捕获 ECS 投影和领域列表不同步。
        const auto& child = ctx.noteRegistry.get<const Note>(children[i]);
        if ( child.m_subIndex != static_cast<int>(i) ||
             child.m_trackIndex != original[i].trackIndex ||
             child.m_timestamp != original[i].timestamp ||
             child.m_collaborationId !=
                 restored.m_subNotes[i].collaborationId ) {
            XERROR(
                "Polyline first body undo did not restore child: case={} "
                "index={} subIndex={} time={} track={}",
                static_cast<int>(kind),
                i,
                child.m_subIndex,
                child.m_timestamp,
                child.m_trackIndex);
            return false;
        }
    }
    ctx.actionStack.redo(ctx);
    // 重做时新前缀实体可能换 ID，所以只检查原子实体和父结构。
    // Redo 使用 Action 的 after 快照，不应从最后一次鼠标位置重新吸附。
    // 旧子索引必须再次从一开始，不能在上次重做结果上重复加一。
    return kind == DragCase::HoldHead ||
           checkBodyResult(ctx, root, children, kind);
}

/// @brief 检查内部节点拖动只改变相连子段和后缀，不移动整条折线。
/// @param selected 根折线与另一 Note 是否同时被选中。
/// @param flick 抓取横移子段节点；否则抓取竖向子段节点。
/// @return 父子投影、选择组、撤销与重做均保持同一局部语义时返回 true。
/// @details 横向拖动 Hold 节点时，前一 Flick 的终轨须接上新节点；
/// 纵向拖动 Flick 节点时，前一 Hold 的尾端须接上新节点时间。
/// 两种场景的后缀均跟随移动，根部与其它选中物件仍停在原位置。
/// @warning 仅测试命令级手势，不改变测试用户配置或项目资源。
bool runInternalNodeCase(bool selected, bool flick)
{
    // 每个场景建立新会话，前一场景的选择与历史不影响本次结果。
    // 玩家轨道投影与首段身体场景一致，直接使用生产时间逆映射。
    using Note = MMM::Logic::NoteComponent;
    MMM::Logic::SessionContext ctx;
    configure(ctx);
    Note parent;
    parent.m_type       = MMM::NoteType::POLYLINE;
    parent.m_timestamp  = 1.0;
    parent.m_trackIndex = 0;
    // 两组正交连接给目标节点的前后都提供有效子段。
    // 末段是后缀哨兵，防止实现只移动被抓的单个节点。
    // 根锚点固定在第零轨，局部拖动不能改变它。
    parent.m_subNotes = {
        Note::SubNote{ .type       = MMM::NoteType::HOLD,
                       .timestamp  = 1.0,
                       .duration   = 0.5,
                       .trackIndex = 0 },
        Note::SubNote{ .type       = MMM::NoteType::FLICK,
                       .timestamp  = 1.5,
                       .trackIndex = 0,
                       .dtrack     = 1 },
        Note::SubNote{ .type       = MMM::NoteType::HOLD,
                       .timestamp  = 1.5,
                       .duration   = 0.5,
                       .trackIndex = 1 },
        Note::SubNote{ .type       = MMM::NoteType::FLICK,
                       .timestamp  = 2.0,
                       .trackIndex = 1,
                       .dtrack     = 1 },
        Note::SubNote{ .type       = MMM::NoteType::HOLD,
                       .timestamp  = 2.0,
                       .duration   = 0.5,
                       .trackIndex = 2 },
    };
    MMM::Logic::ensureNoteCollaborationIdentity(parent);
    // original 独立于被编辑组件，后续 Undo 不以预览状态作期望值。
    // 父子投影复用同一协作身份，撤销后仍要对应原来的实体。
    const auto original = parent.m_subNotes;
    const auto root     = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(root, parent);
    ctx.noteRegistry.emplace<MMM::Logic::TransformComponent>(root);
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        root, MMM::Logic::InteractionComponent{ .isSelected = selected });
    std::vector<entt::entity> children;
    // 真实加载的折线有子实体投影；只核对父列表会漏掉拾取与撤销错误。
    // 子实体直接从父子段构造，不能在测试中修补生产逻辑的输出。
    for ( std::size_t index = 0; index < original.size(); ++index ) {
        const auto entity = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<Note>(
            entity,
            MMM::Logic::makeNoteComponentFromSubNote(
                original[index], true, root, static_cast<int>(index)));
        ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
        children.push_back(entity);
    }
    const auto other = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(other,
                                   Note{ .m_type       = MMM::NoteType::NOTE,
                                         .m_timestamp  = 3.0,
                                         .m_trackIndex = 3 });
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        other, MMM::Logic::InteractionComponent{ .isSelected = selected });
    // 索引一是 Flick，索引二是其终轨上的 Hold。
    // 使用正交拖动分别检验时间连接与轨道连接。
    const int subIndex  = flick ? 1 : 2;
    ctx.hoveredPart     = static_cast<int>(MMM::Logic::HoverPart::Head);
    ctx.hoveredSubIndex = -1;
    MMM::Logic::GrabTool tool;
    // 按下帧明确传入节点身份，不能依赖上一帧悬停的 Head 状态。
    // 即使根和哨兵均已选中，本次手势仍只能编辑这一条折线的内部连接。
    tool.handleStartDrag(
        ctx,
        MMM::Logic::CmdStartDrag{
            root,
            "Basic2DCanvas",
            false,
            MMM::Logic::ChartObjectKind::PlayerNote,
            static_cast<std::uint8_t>(MMM::Logic::HoverPart::PolylineNode),
            subIndex });
    if ( !ctx.isDragging ) return false;
    // Ctrl 关闭吸附；Flick 向时间后方移动，Hold 向右移动一轨。
    // 目标点跨过相应节点，但没有跨出当前谱面的玩家轨道域。
    tool.handleUpdateDrag(ctx,
                          MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas",
                                                     flick ? 150.0F : 350.0F,
                                                     flick ? 0.0F : 300.0F,
                                                     true });
    const auto&  preview = ctx.noteRegistry.get<const Note>(root);
    const double shift   = preview.m_subNotes[1].timestamp - 1.5;
    // 父根与第一子段固定；横向调整改变前一 Flick 的终轨，
    // 纵向调整改变前一 Hold 的长度，两个方向都不需要搬移整条折线。
    // 两种路径均检查后缀末段，避免只改当前子段却留下断开的路径。
    // 只要误入整条位移分支，根时间或轨道的断言就会失败。
    const bool connected =
        flick ? shift > 0.0 &&
                    std::abs(preview.m_subNotes[0].duration - (0.5 + shift)) <
                        1e-7 &&
                    std::abs(preview.m_subNotes[2].timestamp - (1.5 + shift)) <
                        1e-7 &&
                    std::abs(preview.m_subNotes[4].timestamp - (2.0 + shift)) <
                        1e-7
              : preview.m_subNotes[1].dtrack == 2 &&
                    preview.m_subNotes[2].trackIndex == 2 &&
                    preview.m_subNotes[3].trackIndex == 2 &&
                    preview.m_subNotes[4].trackIndex == 3;
    if ( !connected || preview.m_timestamp != 1.0 ||
         preview.m_trackIndex != 0 ||
         ctx.noteRegistry.get<const Note>(other).m_trackIndex != 3 ||
         ctx.noteRegistry.get<const Note>(other).m_timestamp != 3.0 )
        return false;
    // 子实体与父内嵌列表同步，释放后仍须保持原实体身份。
    // 预览帧就应同步拾取位置，不能等待松手才修复 ECS 投影。
    for ( std::size_t index = 0; index < children.size(); ++index ) {
        const auto& child = ctx.noteRegistry.get<const Note>(children[index]);
        if ( child.m_timestamp != preview.m_subNotes[index].timestamp ||
             child.m_trackIndex != preview.m_subNotes[index].trackIndex ||
             child.m_dtrack != preview.m_subNotes[index].dtrack ||
             child.m_duration != preview.m_subNotes[index].duration )
            return false;
    }
    tool.handleEndDrag(ctx, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });
    // 单次手势只能占一个历史动作，否则 Undo 会恢复半条折线。
    if ( ctx.actionStack.getUndoStackSize() != 1U ) return false;
    ctx.actionStack.undo(ctx);
    const auto& restored = ctx.noteRegistry.get<const Note>(root);
    // 根锚点、父子段和子实体都回到按下前，不能只回退视觉父根。
    // 独立哨兵从未进入本次批次，不依赖撤销再移回原位。
    if ( restored.m_timestamp != 1.0 || restored.m_trackIndex != 0 )
        return false;
    for ( std::size_t index = 0; index < children.size(); ++index ) {
        const auto& child = ctx.noteRegistry.get<const Note>(children[index]);
        if ( restored.m_subNotes[index].timestamp !=
                 original[index].timestamp ||
             restored.m_subNotes[index].trackIndex !=
                 original[index].trackIndex ||
             child.m_timestamp != original[index].timestamp ||
             child.m_trackIndex != original[index].trackIndex )
            return false;
    }
    ctx.actionStack.redo(ctx);
    const auto& redone = ctx.noteRegistry.get<const Note>(root);
    // Redo 使用已保存的 after 状态，不再读取当前鼠标位置或吸附。
    // 同时核对前连接段和末端后缀，防止历史只记录其中一部分。
    return flick ? redone.m_subNotes[1].timestamp > 1.5 &&
                       redone.m_subNotes[0].duration > 0.5
                 : redone.m_subNotes[1].dtrack == 2 &&
                       redone.m_subNotes[4].trackIndex == 3;
}

/// @brief 验证折线末端参数拖动、零参数退化及结构历史。
/// @param draft 是否在草稿轨道编辑。
/// @param flick 末端是 Flick 箭头；否则是 Hold 尾端。
/// @param selected 根折线开始拖动前是否已选中。
/// @return 参数预览同步且归零后的折线退化可撤销、可重做时返回 true。
/// @note 两节点结构覆盖尾项归零移除、普通 Note 退化及历史往返。
/// @note 鼠标坐标由统一轨道投影和 ScrollCache 反算，命中信息在 Start 固定。
/// @note 父锚点与子实体投影需在预览、提交、Undo 和 Redo 各阶段保持一致。
bool runPolylineEndpointCase(bool draft, bool flick, bool selected)
{
    // Arrange 阶段构造父折线、两个子节点及实际 ECS 子投影。
    // Act 阶段只通过 GrabTool 的开始、更新、结束命令修改数据。
    // 根实体保留为历史批次主键，子实体负责暴露即时拾取同步状态。
    // 鼠标轨道取自统一画布投影，Draft 与 Player 各自使用实际轨宽。
    // 时间位置通过 ScrollCache 逆映射生成，避免把秒数近似为像素。
    // Ctrl 修饰绕过吸附，让两次位置更新保持确定的目标时间。
    // Start 显式锁定部位与子索引，不依赖先前帧留下的 hover 状态。
    // 首次更新将端点参数带离零值，验证局部字段确实收到输入。
    // 预览同时核对父列表和子投影，防止松键才补齐视觉状态。
    // 第二次更新回到零值，仍属于同一个历史事务。
    // 释放后检查退化根与子实体清理，确认结构只由收尾阶段转换。
    // Undo 对照按下前结构，Redo 对照提交后普通 Note。
    // 选择状态和对象域组合变化，确保端点不会误入整体移动路径。
    // 只创建内存会话，避免外部谱面或皮肤影响拖动路径。
    using Note = MMM::Logic::NoteComponent;
    MMM::Logic::SessionContext ctx;
    // 共用现有画布布局和滚动缓存，隔离选择及端点类型差异。
    configure(ctx);
    // 投影包含追加草稿槽，编辑域仍由会话中的持久草稿轨数限制。
    ctx.draftTrackCount                      = 4;
    ctx.lastConfig.settings.professionalMode = true;

    // 根保持在各自区域内部，末端更新不应改变其逻辑地址。
    const int track = draft ? -2 : 1;
    Note      parent;
    // 首节点用于降级后的独立 Note，尾节点单独承载端部参数。
    // 用单根和两节点构成最小有效折线，根位置在两种域中均合法。
    // 尾 Hold 的零值由持续时间表达；Flick 的零值由终轨与起轨重合表达。
    parent.m_type       = MMM::NoteType::POLYLINE;
    parent.m_timestamp  = 1.0;
    parent.m_trackIndex = track;
    parent.m_isDraft    = draft;
    // 头节点保留原始根锚点，尾节点是此手势唯一的编辑目标。
    // 两种端点都接在普通头部之后，归零时尾段清理会将子列表缩为单项。
    // 子项共用根起点，便于把参数变化与对象整体平移区分开来。
    // Flick 轨差初值向右，Draft/Player 用相同绝对轨语义表达方向。
    parent.m_subNotes = {
        Note::SubNote{ .type       = MMM::NoteType::NOTE,
                       .timestamp  = 1.0,
                       .trackIndex = track },
        Note::SubNote{ .type =
                           flick ? MMM::NoteType::FLICK : MMM::NoteType::HOLD,
                       .timestamp  = 1.0,
                       .duration   = flick ? 0.0 : 0.5,
                       .trackIndex = track,
                       .dtrack     = flick ? 1 : 0 },
    };
    MMM::Logic::ensureNoteCollaborationIdentity(parent);
    // 先分配协作身份再创建投影，历史比较不依赖 ECS 分配顺序。
    // 先生成稳定协作 ID 再建立子投影，使历史结构核对不受实体编号影响。
    const auto root = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(root, parent);
    ctx.noteRegistry.emplace<MMM::Logic::TransformComponent>(root);
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        root, MMM::Logic::InteractionComponent{ .isSelected = selected });

    std::vector<entt::entity> children;
    // 父数据和子投影以相同索引创建，确保操作输入能定位尾节点。
    // 子组件暂不预设 Draft 标志，拖动同步应从父根复制真实域状态。
    // 这样草稿用例可以检测同步逻辑是否更新了非几何字段。
    // 保存真实子实体句柄，后续同步断言直接检查原投影。
    // 记录按原始索引建立的子实体，后续预览通过原身份检查同步结果。
    for ( std::size_t index = 0; index < parent.m_subNotes.size(); ++index ) {
        // 生产转换函数补齐颜色、绑定、父实体和子索引等投影字段。
        const auto entity = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<Note>(
            entity,
            MMM::Logic::makeNoteComponentFromSubNote(
                parent.m_subNotes[index], true, root, static_cast<int>(index)));
        ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
        children.push_back(entity);
    }

    const auto& camera = ctx.cameras.at("Basic2DCanvas");
    // 和主画布统一目标使用相同的分区开关及追加草稿槽。
    // 各测试会话使用相同生产布局入口；Draft 与 Player 不共享轨宽假设。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        camera.viewportWidth,
        ctx.trackCount,
        ctx.bgmTrackCount,
        ctx.lastConfig.visual.trackLayout,
        camera.horizontalOffsetX,
        true,
        true,
        true,
        ctx.draftTrackCount,
        true);
    // 绝对轨必须按本次投影的草稿轨数换算，包含追加槽后的实际地址布局。
    const auto address = MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(
        track, projection.playerLaneCount, projection.draftLaneCount);
    const auto laneBounds = projection.bounds(address);
    // 检查布局和地址有效后，才用该轨中心生成按下位置。
    if ( !projection.valid || !laneBounds ) return false;
    // 取区域内中心点，避免坐标恰好落在相邻轨的半开边界上。
    const float rootX = (laneBounds->leftX + laneBounds->rightX) * 0.5F;

    const auto& cache =
        ctx.timelineRegistry.ctx().get<MMM::Logic::System::ScrollCache>();
    // 将歌曲时间映射成主画布鼠标位置，避免直接假定滚动积分是线性的。
    // 滚动绝对位置以动画时间为锚点，鼠标 y 对应测试指定的歌曲时刻。
    const float judgmentY =
        camera.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    // getTime 的输入是绝对滚动位置；此差值恰好抵消锚点所在时间。
    // 因此生成的 mouseY 经生产逆映射后仍回到指定 time。
    const auto yForTime = [&](double time) {
        // 把目标时间相对当前动画锚点的积分差反算成视口坐标。
        return static_cast<float>(judgmentY + cache.getAbsY(ctx.animateTime) -
                                  cache.getAbsY(time));
    };

    const auto part = flick ? MMM::Logic::HoverPart::FlickArrow
                            : MMM::Logic::HoverPart::HoldEnd;
    const auto kind = draft ? MMM::Logic::ChartObjectKind::DraftNote
                            : MMM::Logic::ChartObjectKind::PlayerNote;
    // 两种根类别刻意共享相同节点和坐标，隔离类别路由对端点操作的影响。
    // Draft 输入从命中的末端子实体开始，Player 输入仍从折线根开始。
    const auto           hitEntity = draft ? children[1] : root;
    MMM::Logic::GrabTool tool;
    // 命中部位和索引按按下帧固定，不依赖可能滞后的悬停状态。
    // 命中信息来自按下帧，尾部索引固定为父折线的第二个子项。
    tool.handleStartDrag(ctx,
                         MMM::Logic::CmdStartDrag{
                             hitEntity,
                             "Basic2DCanvas",
                             false,
                             kind,
                             static_cast<std::uint8_t>(part),
                             1,
                         });
    // Draft 子实体应提升到父实体，整个事务都围绕原根记录。
    // Draft 子实体应提升到其父折线；Player 的根身份则原样保留。
    if ( !ctx.isDragging || ctx.draggedEntity != root ) return false;

    // 先把尾端拉到非零参数，确认根锚点不变且独立子投影即时跟随。
    // Hold 只改时间端点，Flick 只改箭头终轨，两者都不移动根锚点。
    float expandedX = rootX;
    float expandedY = yForTime(2.0);
    if ( flick ) {
        // Flick 通过目标终轨改写 dtrack；起点仍保持在根所在轨道。
        // 目标选在 Draft 内另一轨或 Player 轨 3，保持完整落在各自编辑域。
        const int  expandedTrack = draft ? -3 : 3;
        const auto expandedAddress =
            MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(
                expandedTrack,
                projection.playerLaneCount,
                projection.draftLaneCount);
        const auto expandedBounds = projection.bounds(expandedAddress);
        // 用目标轨道自己的边界取中心，不混用草稿和玩家轨宽。
        if ( !expandedBounds ) return false;
        expandedX = (expandedBounds->leftX + expandedBounds->rightX) * 0.5F;
        expandedY = yForTime(1.0);
    }
    tool.handleUpdateDrag(ctx,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas", expandedX, expandedY, true });
    // 第一次更新只看预览数据，保证鼠标仍按住时拾取位置已同步。
    // 第一次更新必须可见地改变参数，但不应把整个根移到指针位置。
    // 在预览中立刻核对双表示，不能只在释放时重新构建出正确状态。
    const auto& expanded      = ctx.noteRegistry.get<const Note>(root);
    const auto& expandedChild = ctx.noteRegistry.get<const Note>(children[1]);
    const bool  expandedCorrect =
        expanded.m_type == MMM::NoteType::POLYLINE &&
        expanded.m_timestamp == 1.0 && expanded.m_trackIndex == track &&
        expanded.m_subNotes.size() == 2U &&
        // Hold 参数必须延长；Flick 轨差必须精确对应选定的目标轨。
        (flick ? expanded.m_subNotes[1].dtrack == (draft ? -1 : 2)
               : expanded.m_subNotes[1].duration > 0.5) &&
        expandedChild.m_dtrack == expanded.m_subNotes[1].dtrack &&
        expandedChild.m_duration == expanded.m_subNotes[1].duration &&
        expandedChild.m_trackIndex == expanded.m_subNotes[1].trackIndex &&
        // 子投影的 Draft 状态属于同步字段，不能沿用默认组件值。
        expandedChild.m_isDraft == draft;
    if ( !expandedCorrect ) return false;

    // 参数归零后释放；尾子项应从父列表和子实体投影一并移除。
    // 连续手势回到根轨与起始时间，分别对应 Flick 零轨差和 Hold 零时长。
    // 同一手势返回起始端点位置，覆盖连续拖动中的正反向变化。
    const float zeroX = rootX;
    // 目标退回初始末端，验证连续移动后参数可精确回到零。
    const float zeroY = yForTime(1.0);
    // 两个端点分别按当前字段单位归零，而非把对象移到负时间或其他轨。
    tool.handleUpdateDrag(
        ctx, MMM::Logic::CmdUpdateDrag{ "Basic2DCanvas", zeroX, zeroY, true });
    tool.handleEndDrag(ctx, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto& collapsed = ctx.noteRegistry.get<const Note>(root);
    // 注册表计数只纳入子投影，不把其他类型组件或普通根 Note 混在一起。
    // 仅统计引用当前根的投影，不把其他 Note 实体计入退化结果。
    const auto countChildren = [&ctx, root]() {
        // 结构收尾会销毁投影，故以父关系计数而不使用之前的句柄数。
        std::size_t count = 0;
        // 只读查询不改动注册表，撤销和重做均可重复执行该断言。
        for ( const auto entity : ctx.noteRegistry.view<const Note>() ) {
            const auto& note = ctx.noteRegistry.get<const Note>(entity);
            if ( note.m_isSubNote && note.m_parentPolyline == root ) ++count;
        }
        return count;
    };
    // 仅剩头部 Note 时由原根接管普通物件语义，尾投影必须销毁。
    if ( collapsed.m_type != MMM::NoteType::NOTE ||
         collapsed.m_timestamp != 1.0 || collapsed.m_trackIndex != track ||
         !collapsed.m_subNotes.empty() || countChildren() != 0U ||
         ctx.actionStack.getUndoStackSize() != 1U || ctx.isDragging ) {
        return false;
    }

    // Root 仍然有效，但其 NoteType、参数字段与子列表已变为独立普通物件。
    // 原末端子投影删除后不得继续参与命中或后续结构同步。
    // Undo 应恢复完整折线和两个子投影；Redo 再次退化为普通 Note。
    // 先撤销完整结构，再核对 Redo 是否能恢复已提交的退化态。
    ctx.actionStack.undo(ctx);
    // 根实体保留身份，撤销只恢复其原折线字段和子实体关系。
    // Undo 需恢复同一根实体中的完整节点列表与对应子投影集合。
    const auto& restored = ctx.noteRegistry.get<const Note>(root);
    if ( restored.m_type != MMM::NoteType::POLYLINE ||
         restored.m_timestamp != 1.0 || restored.m_trackIndex != track ||
         restored.m_subNotes.size() != 2U || countChildren() != 2U ||
         // 撤销比较原始尾参数，排除历史仅保存归零后的当前组件副本。
         restored.m_subNotes[1].type !=
             (flick ? MMM::NoteType::FLICK : MMM::NoteType::HOLD) ||
         (flick ? restored.m_subNotes[1].dtrack != 1
                : restored.m_subNotes[1].duration != 0.5) ) {
        return false;
    }
    // Redo 应直接使用历史 after 快照，不读取当前鼠标与工具缓存。
    ctx.actionStack.redo(ctx);
    // 重做后再验证子投影集合为空，避免只检查根的类型字段。
    // Redo 再次检查最终领域形态，不能只验证 Undo 可逆。
    const auto& redone = ctx.noteRegistry.get<const Note>(root);
    return redone.m_type == MMM::NoteType::NOTE && redone.m_timestamp == 1.0 &&
           redone.m_trackIndex == track && redone.m_subNotes.empty() &&
           countChildren() == 0U;
}

/// @brief 验证草稿折线内部 Hold 越域拖动不会丢失或转成整根移动。
/// @return Hold 后缀被限制在草稿轨、根保持原位且可撤销时返回 true。
/// @note 目标位于有效玩家轨，起始对象却处于草稿域；局部编辑应保持可撤销。
bool runDraftInternalHoldBoundaryCase()
{
    // 起始对象属于草稿域，鼠标目标则落在可交互的玩家轨上。
    // 两者的轨道几何来自统一投影，不依赖屏幕上固定的像素常量。
    // 节点和后缀共享初始轨差，拖动必须保持段间连接。
    // 根的锚点和域标记不随内部节点目标一同改变。
    // 子实体投影用于验证预览中的拾取位置与父列表一致。
    // 超出草稿边界的增量应按域限制，不应迁移或隐藏整个物件。
    // 目标时间固定在内部 Hold 起点，只改变轨道变量。
    // 释放提交一条动作，Undo 从独立初始结构恢复所有节点。
    // 每个历史断言都通过原始实体句柄读取，不依据枚举顺序。
    // 此用例不构造选择组，隔离单个内部节点的拖动语义。
    // 预览与撤销都检查 Draft 标记，防止只保留负轨号但切换领域。
    // 玩家区域内的目标仍是合法输入，不能触发越界坐标的早退。
    using Note = MMM::Logic::NoteComponent;
    MMM::Logic::SessionContext ctx;
    configure(ctx);
    ctx.draftTrackCount = 4;

    Note parent;
    // 三节点结构让内部 Hold 带有后缀哨兵，可发现只约束当前节点的错位。
    parent.m_type       = MMM::NoteType::POLYLINE;
    parent.m_timestamp  = 1.0;
    parent.m_trackIndex = -2;
    parent.m_isDraft    = true;
    parent.m_subNotes   = {
        Note::SubNote{
              .type = MMM::NoteType::NOTE, .timestamp = 1.0, .trackIndex = -2 },
        Note::SubNote{ .type       = MMM::NoteType::HOLD,
                         .timestamp  = 1.5,
                         .duration   = 0.5,
                         .trackIndex = -3 },
        Note::SubNote{
              .type = MMM::NoteType::NOTE, .timestamp = 2.0, .trackIndex = -3 },
    };
    MMM::Logic::ensureNoteCollaborationIdentity(parent);
    // 父根保持未选中，命令明确命中内部节点而不是进入整组拖动。
    const auto root = ctx.noteRegistry.create();
    ctx.noteRegistry.emplace<Note>(root, parent);
    ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(root);
    std::vector<entt::entity> children;
    for ( std::size_t index = 0; index < parent.m_subNotes.size(); ++index ) {
        const auto entity = ctx.noteRegistry.create();
        ctx.noteRegistry.emplace<Note>(
            entity,
            MMM::Logic::makeNoteComponentFromSubNote(
                parent.m_subNotes[index], true, root, static_cast<int>(index)));
        ctx.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
        children.push_back(entity);
    }

    const auto& camera = ctx.cameras.at("Basic2DCanvas");
    // 计算合法玩家目标中心，越域意图不依赖玩家轨宽的硬编码像素值。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        camera.viewportWidth,
        ctx.trackCount,
        ctx.bgmTrackCount,
        ctx.lastConfig.visual.trackLayout,
        camera.horizontalOffsetX,
        true,
        true,
        true,
        ctx.draftTrackCount,
        true);
    const auto playerBounds = projection.bounds(
        MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Player, 1 });
    if ( !projection.valid || !playerBounds ) return false;
    const float outsideDraftX =
        (playerBounds->leftX + playerBounds->rightX) * 0.5F;
    const auto& cache =
        ctx.timelineRegistry.ctx().get<MMM::Logic::System::ScrollCache>();
    const float judgmentY =
        camera.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    const float holdY = static_cast<float>(
        judgmentY + cache.getAbsY(ctx.animateTime) - cache.getAbsY(1.5));

    MMM::Logic::GrabTool tool;
    // DraftNote 命令和 PolylineNode/1 一起锁定草稿内部编辑语义。
    tool.handleStartDrag(
        ctx,
        MMM::Logic::CmdStartDrag{
            root,
            "Basic2DCanvas",
            false,
            MMM::Logic::ChartObjectKind::DraftNote,
            static_cast<std::uint8_t>(MMM::Logic::HoverPart::PolylineNode),
            1,
        });
    if ( !ctx.isDragging || ctx.draggedEntity != root ) return false;
    tool.handleUpdateDrag(ctx,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas", outsideDraftX, holdY, true });

    // 同时检查 Hold、后缀和父锚点，以发现整根位移或结构丢失。
    const auto& preview = ctx.noteRegistry.get<const Note>(root);
    const auto& child   = ctx.noteRegistry.get<const Note>(children[1]);
    if ( !preview.m_isDraft || preview.m_trackIndex != -2 ||
         preview.m_timestamp != 1.0 || preview.m_subNotes.size() != 3U ||
         preview.m_subNotes[1].trackIndex >= 0 ||
         preview.m_subNotes[2].trackIndex >= 0 ||
         child.m_trackIndex != preview.m_subNotes[1].trackIndex ||
         child.m_duration != preview.m_subNotes[1].duration ) {
        return false;
    }

    tool.handleEndDrag(ctx, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });
    // 越域边界钳制仍是一项有效局部编辑，因此只产生一条历史记录。
    if ( ctx.actionStack.getUndoStackSize() != 1U ||
         ctx.noteRegistry.get<const Note>(root).m_type !=
             MMM::NoteType::POLYLINE ) {
        return false;
    }
    ctx.actionStack.undo(ctx);
    // 通过既有实体句柄验证子投影撤销，而不是仅数父列表元素。
    // 时间值也逐个回到初始位置，防止撤销只修复轨道投影。
    const auto& restored = ctx.noteRegistry.get<const Note>(root);
    for ( std::size_t index = 0; index < children.size(); ++index ) {
        const auto& restoredChild =
            ctx.noteRegistry.get<const Note>(children[index]);
        if ( restoredChild.m_trackIndex !=
                 parent.m_subNotes[index].trackIndex ||
             restoredChild.m_timestamp != parent.m_subNotes[index].timestamp ) {
            return false;
        }
    }
    return restored.m_isDraft && restored.m_trackIndex == -2 &&
           restored.m_subNotes.size() == parent.m_subNotes.size() &&
           restored.m_subNotes[1].trackIndex == -3 &&
           restored.m_subNotes[2].trackIndex == -3;
}
}  // namespace

/// @brief 运行折线身体、内部节点、末端和草稿边界的回归矩阵。
/// @return 任一结构或撤销断言失败时返回非零。
/// @note 端点覆盖两域、两类型和两种选择状态的全组合，避免选择状态抢占局部手势。
/// @note 所有场景分别创建会话，不把前一场景的撤销记录或拖动标记带入后一场景。
int main()
{
    return runCase(DragCase::HoldBody) && runCase(DragCase::FlickBody) &&
                   runCase(DragCase::HoldHead) &&
                   runInternalNodeCase(false, false) &&
                   runInternalNodeCase(true, false) &&
                   runInternalNodeCase(true, true) &&
                   runPolylineEndpointCase(false, false, false) &&
                   runPolylineEndpointCase(false, false, true) &&
                   runPolylineEndpointCase(false, true, false) &&
                   runPolylineEndpointCase(false, true, true) &&
                   runPolylineEndpointCase(true, false, false) &&
                   runPolylineEndpointCase(true, false, true) &&
                   runPolylineEndpointCase(true, true, false) &&
                   runPolylineEndpointCase(true, true, true) &&
                   runDraftInternalHoldBoundaryCase()
               ? 0
               : 1;
}
