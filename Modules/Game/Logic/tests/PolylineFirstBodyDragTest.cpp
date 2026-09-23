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
}  // namespace

/// @brief 运行首段两种身体拖动与头部整体拖动的回归矩阵。
/// @return 任一结构或撤销断言失败时返回非零。
int main()
{
    return runCase(DragCase::HoldBody) && runCase(DragCase::FlickBody) &&
                   runCase(DragCase::HoldHead)
               ? 0
               : 1;
}
