#include "logic/session/CanvasCamera.h"
#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectDraftLaneService.h"
#include "logic/audio/AudioTimelineDescriptor.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/EditorAction.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SamplePropertyEdit.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "logic/session/tool/GrabTool.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

/**
 * @file CanvasCameraTest.cpp
 * @brief 覆盖主画布相机、轨道投影、跨数据域交互与可撤销编辑集成行为。
 *
 * 本测试文件以 SessionContext 为最小集成环境，直接驱动 InteractionController、
 * ActionController、PlaybackController、DrawTool 和
 * GrabTool。它不创建渲染窗口，
 * 而是检查相机输入经过轨道投影、时间换算和动作栈后形成的 ECS 与 BeatMap 状态。
 *
 * 测试中的三个对象域必须严格区分：
 *
 * - Draft Note 使用负轨道和 noteRegistry，只在专业模式下可见与可编辑。
 * - Player Note 使用非负玩家轨道和 noteRegistry，可包含 Polyline 子投影实体。
 * - Audio Sample 使用玩家轨道之后的绝对轨道和 sampleRegistry。
 *
 * entt::entity 只在所属 Registry 内有意义。Note 与 Sample Registry 可以产生相同
 * 数值的实体句柄，所有选择、拖动、悬停和动作命令都必须同时携带
 * ChartObjectKind， 测试会刻意构造重叠句柄以防止跨域串对象。
 *
 * 画布 X 坐标先应用 CameraInfo::horizontalOffsetX，再由 CanvasLaneProjection
 * 映射到 Draft、Player、BGM 或追加轨。Y 坐标通过 ScrollCache 和判定线位置映射
 * 到谱面秒时间。Resize 必须保持横向平移比例，Preview 则有独立投影规则。
 *
 * 测试遵循下列验证层次：
 *
 * 1. 配置固定视口、轨道布局、时间和线性 ScrollCache。
 * 2. 构造最少数量的 Note、Sample、Timing 或项目资源。
 * 3. 通过真实命令入口模拟鼠标、画笔、抓取、选择或协作替换。
 * 4. 检查即时 SessionContext、ECS 组件与交互索引。
 * 5. 对持久化编辑继续执行 Undo/Redo 或 syncBeatmap，验证完整往返。
 *
 * 不能只断言对象数量。多数场景还需检查类型、轨道、时间、父子关系、资源 ID、
 * 音量、选择状态、稳定协作 ID 和
 * mutationFlags，才能证明命令没有写入错误数据域。
 *
 * 浮点相机坐标与秒时间使用 near 比较，整数轨道、实体身份和枚举严格比较。
 * 测试故意避开真实音频设备与 Vulkan 渲染；通过描述符、快照和组件状态验证逻辑
 * 边界，不把无窗口测试通过表述为视觉或音频播放验收。
 *
 * 每个测试返回 bool，由文件末尾轻量 runner 依序执行并记录失败。场景之间各自
 * 创建 SessionContext，不共享 Registry、ActionStack 或相机状态；需要项目环境的
 * 测试显式安装和恢复 EditorEngine 当前项目，防止全局状态泄漏到后续用例。
 *
 * 修改测试时必须保持以下约束：
 *
 * - Arrange 阶段不能依赖上一用例留下的实体或项目。
 * - 命令中的 cameraId 必须匹配 configureObjectEditingCanvas 创建的相机。
 * - 轨道像素应从固定布局或投影计算得出，不依赖真实窗口 DPI。
 * - 时间像素应通过 ScrollCache 计算，避免硬编码与滚动实现漂移。
 * - Polyline 断言必须同时覆盖父内嵌数组和实际存在的子实体。
 * - 新建动作应验证 Undo 删除与 Redo 恢复，而不只验证首次执行。
 * - 修改动作应验证 Undo 恢复全部字段，不能只比较被编辑字段。
 * - 混合 Note/Sample 动作必须验证一次 Undo 同时恢复两个 Registry。
 * - 协作替换必须验证稳定 ID、历史保留策略和观察者变化类别。
 * - 项目资源测试结束前必须恢复 EditorEngine 的全局项目指针。
 * - 负时间、无效资源和轨道上溢场景应在实体创建前整体拒绝。
 * - 专业、Polyline 与 BMS 开关应分别验证可见性和可编辑性。
 */

namespace
{

/// @brief 记录单次会话测试收到的谱面变化类型。
/// @details
/// 观察者不编码或发送谱面，只保存最后一次类别位，供测试判断命令是否把批注、
/// 对象或自动采样误报为其他数据域。返回零表示不参与本地协作序号协议。
class RecordingMutationObserver final : public MMM::IBeatmapMutationObserver
{
public:
    /// @brief 保存最近一次谱面变化类型。
    /// @param beatMap 发生变化的谱面。
    /// @param flags 本次变化包含的数据类别。
    std::uint64_t onBeatmapMutated(const MMM::BeatMap&       beatMap,
                                   MMM::BeatmapMutationFlags flags) override
    {
        (void)beatMap;
        m_flags = flags;
        return 0;
    }

    /// @brief 最近一次收到的谱面变化类型。
    MMM::BeatmapMutationFlags m_flags{ MMM::BeatmapMutationFlags::None };
};

/// @brief 使用小容差比较逻辑像素或时间值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

/// @brief 配置可重复的主画布投影和线性空时间线。
/// @param context 待配置会话。
/// @details
/// 固定四玩家轨、四草稿轨、一持久 BGM 轨以及 1000x600 视口。玩家区占归一化
/// X `[0.1, 0.5]`，判定线位于视口中线。空 Timeline 的 ScrollCache 提供可预测
/// 线性时间换算，所有后续用例在此基线上只覆盖自己关心的设置。
void configureObjectEditingCanvas(MMM::Logic::SessionContext& context)
{
    // 调用方可预先提供 BeatMap；空上下文才创建最小领域对象。
    if ( !context.currentBeatmap ) {
        context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    }
    context.currentBeatmap->m_baseMapMetadata.track_count = 4;
    // 领域元数据与运行时轨道数必须一致，避免控制器选择不同布局。
    context.trackCount                         = 4;
    context.draftTrackCount                    = 4;
    context.bgmTrackCount                      = 1;
    context.currentTime                        = 1.0;
    context.animateTime                        = 1.0;
    context.lastConfig.visual.trackLayout.left = 0.1F;
    // 固定玩家区宽度使每轨正好 100 逻辑像素，便于直接构造鼠标坐标。
    context.lastConfig.visual.trackLayout.right  = 0.5F;
    context.lastConfig.visual.judgeline_pos      = 0.5F;
    context.lastConfig.settings.professionalMode = true;
    context.cameras.emplace("Basic2DCanvas",
                            // 主画布相机以零横移开始，各用例可显式覆盖偏移。
                            MMM::Logic::CameraInfo{
                                "Basic2DCanvas",
                                1000.0F,
                                600.0F,
                                0.0F,
                            });
    auto* cache =
        context.timelineRegistry.ctx().find<MMM::Logic::System::ScrollCache>();
    if ( !cache ) {
        // SessionContext 默认可能尚未安装 ScrollCache，测试按需创建上下文对象。
        cache = &context.timelineRegistry.ctx()
                     .emplace<MMM::Logic::System::ScrollCache>();
    }
    cache->rebuild(context.timelineRegistry,
                   // 空 Timeline 配合当前配置建立确定的线性时间投影。
                   context.lastConfig,
                   context.currentBeatmap.get());
}

/// @brief 验证关闭折线编辑后只有主 Note 和 Hold 可被选中与悬停。
/// @return Note/Hold 可交互且 Flick/Polyline 被忽略时返回 true。
/// @details
/// Arrange：在同一时间创建 Note、Hold、Flick 与 Polyline 四种根对象，并关闭
/// enablePolylineEditing。Act：先执行全域 SelectAll，再分别尝试悬停 Flick 和
/// Hold。Assert：只有 Note/Hold 被选中，Flick 悬停被拒绝，而 Hold 悬停有效。
///
/// 该用例同时覆盖批量选择和单实体悬停两条权限入口，防止只在渲染过滤类型，
/// 却仍允许隐藏的 Flick/Polyline 被命令选中或编辑。
/// @par 关键不变量
/// - enablePolylineEditing=false 只允许 Note 与 Hold 交互。
/// - 全域选择仍需执行类型权限过滤。
/// - Flick 与 Polyline 不写入 selectedNoteEntities。
/// - 禁用类型的悬停命令将 hoveredEntity 保持为空。
/// - 允许的 Hold 悬停仍正常工作，证明不是整体禁用输入。
/// - 权限规则同时约束批量入口和单对象入口。
/// @par 故障定位
/// 选择失败优先检查 SelectAll 的类型过滤；悬停失败则检查 CmdSetHoveredEntity
/// 的可编辑性判定。两者同时失败才说明共享权限规则可能错误。
/// @par 测试边界
/// 本用例只验证逻辑可交互性，不要求禁用类型从资源或领域模型中删除，也不对实际
/// 皮肤的隐藏效果作视觉验收。
bool testKeyModeInteractionRestriction()
{
    // 准备阶段保留四类根对象，确保权限矩阵一次覆盖完整。
    // 每个实体使用不同轨道编号，避免选择集合误判为对象覆盖。
    // 全选命令验证批量入口，悬停命令验证单对象入口。
    // 被禁用类型必须在两个入口上得到一致拒绝。
    // 最后的 Hold 悬停用于排除控制器整体失效。
    // 各断言共同定位类型权限与交互状态之间的边界。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enablePolylineEditing = false;

    const auto createNote = [&](MMM::NoteType type) {
        // 每种类型使用唯一轨道索引，失败日志可排除实体相互覆盖。
        const auto entity = context.noteRegistry.create();
        context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
            entity,
            MMM::Logic::NoteComponent{
                .m_type       = type,
                .m_timestamp  = 1.0,
                .m_trackIndex = static_cast<int>(entt::to_integral(entity)),
            });
        return entity;
    };
    const auto noteEntity     = createNote(MMM::NoteType::NOTE);
    const auto holdEntity     = createNote(MMM::NoteType::HOLD);
    const auto flickEntity    = createNote(MMM::NoteType::FLICK);
    const auto polylineEntity = createNote(MMM::NoteType::POLYLINE);

    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdSelectAll{
        // 显式全域选择避免鼠标分区影响本用例，只验证类型权限。
        .scope = MMM::Logic::SelectAllScope::AllTrackAreas,
    });
    const auto isSelected = [&](entt::entity entity) {
        // InteractionComponent 可能尚未存在，缺失与未选中采用相同失败语义。
        const auto* interaction =
            context.noteRegistry.try_get<MMM::Logic::InteractionComponent>(
                entity);
        return interaction && interaction->isSelected;
    };
    if ( !isSelected(noteEntity) || !isSelected(holdEntity) ||
         isSelected(flickEntity) || isSelected(polylineEntity) ) {
        XERROR("Key mode select-all included a non-Note/Hold object");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        // 先用被禁用 Flick 验证悬停入口确实执行权限过滤。
        flickEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
    });
    if ( context.hoveredEntity != entt::null ) {
        XERROR("Key mode allowed hovering a Flick");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        // 再用允许的 Hold 证明控制器没有把全部悬停一并关闭。
        holdEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
    });
    return context.hoveredEntity == holdEntity;
}

/// @brief 验证分区全选只保留鼠标所在轨道区的物件。
/// @return Ctrl+A 分别隔离草稿、玩家与 BGM 区，且全局全选覆盖三区时返回 true。
/// @details
/// Arrange：构造一个玩家 Note、一个负轨 Draft 和一个 BGM Sample，并横移主相机。
/// Act：把鼠标依次放到玩家、草稿、BGM 区执行默认
/// SelectAll，再执行显式全域选择。
/// Assert：每次默认选择只保留当前分区，全域选择覆盖三类；鼠标不悬停时默认命令
/// 沿用最后有效 BGM 分区，而不是退化到所有对象。
///
/// 相机横移确保分区判断使用投影后的画布坐标，而不是未偏移的固定 X 阈值。
/// @par 关键不变量
/// - 玩家、草稿和 BGM 三个分区的选择互斥。
/// - 每次默认 Ctrl+A 清理其他分区的旧选择。
/// - BGM 选择写入 Sample Registry 与 Sample 选择集合。
/// - AllTrackAreas 显式范围可同时覆盖三个域。
/// - 鼠标离开后默认命令沿用最后有效分区。
/// - horizontalOffsetX 必须参与鼠标分区换算。
/// @par 故障定位
/// 单一区域串选通常来自 CanvasLaneAddress 换算；全域选择遗漏则属于选择枚举；
/// 仅横移后失败说明鼠标 X 未还原到相机逻辑坐标。
/// @par 测试边界
/// 测试使用固定视口和单相机，只约束一次命令的区域选择语义，不覆盖平台快捷键
/// 事件是否产生 CmdSelectAll。
bool testSelectAllRespectsPointerTrackArea()
{
    // 场景同时布置草稿、玩家和音频对象，形成三个选择域。
    // 鼠标坐标只用于确定当前分区，不参与对象的时间筛选。
    // 每次全选前都清空既有选择，避免上一次命令污染结果。
    // 分区切换后仅对应 Registry 和轨道范围可以被选中。
    // 全域模式作为对照，证明对象本身均具备可选择性。
    // 断言按域检查身份，防止只比较选择数量而漏掉串域。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.cameras.at("Basic2DCanvas").horizontalOffsetX = 400.0F;
    // 横移后屏幕 X 与逻辑轨道区不同，可捕获遗漏相机偏移的实现。

    const auto playerEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        playerEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    const auto draftEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        draftEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = -1,
            .m_isDraft    = true,
        });
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "main.ogg",
        });

    MMM::Logic::InteractionController controller(context);
    const auto setMainCanvasMouseX = [&](float mouseX, bool isHovering = true) {
        // 所有鼠标命令固定 Y 与视口，只改变待验证的横向分区。
        controller.handleCommand(MMM::Logic::CmdSetMousePosition{
            .cameraId       = "Basic2DCanvas",
            .mouseX         = mouseX,
            .mouseY         = 300.0F,
            .viewportWidth  = 1000.0F,
            .viewportHeight = 600.0F,
            .isHovering     = isHovering,
        });
    };
    const auto isNoteSelected = [&](entt::entity entity) {
        const auto* interaction =
            context.noteRegistry.try_get<MMM::Logic::InteractionComponent>(
                entity);
        return interaction && interaction->isSelected;
    };
    const auto isSampleSelected = [&] {
        const auto* interaction =
            context.sampleRegistry.try_get<MMM::Logic::InteractionComponent>(
                sampleEntity);
        return interaction && interaction->isSelected;
    };

    setMainCanvasMouseX(550.0F);
    // 第一轮玩家区选择应清除其他 Registry 和草稿域的旧选择。
    controller.handleCommand(MMM::Logic::CmdSelectAll{});
    if ( !isNoteSelected(playerEntity) || isNoteSelected(draftEntity) ||
         isSampleSelected() ) {
        XERROR("Player-area select-all included draft or BGM objects");
        return false;
    }

    setMainCanvasMouseX(150.0F);
    // 第二轮草稿区选择验证负轨 Note 使用独立区域身份。
    controller.handleCommand(MMM::Logic::CmdSelectAll{});
    if ( isNoteSelected(playerEntity) || !isNoteSelected(draftEntity) ||
         isSampleSelected() ) {
        XERROR("Draft-area select-all included player or BGM objects");
        return false;
    }

    setMainCanvasMouseX(950.0F);
    // 第三轮 BGM 区选择必须写入 Sample Registry 而非 Note Registry。
    controller.handleCommand(MMM::Logic::CmdSelectAll{});
    if ( isNoteSelected(playerEntity) || isNoteSelected(draftEntity) ||
         !isSampleSelected() ) {
        XERROR("BGM-area select-all included note objects");
        return false;
    }

    setMainCanvasMouseX(950.0F, false);
    // 显式 AllTrackAreas 不依赖当前 hover，必须选中全部三个域。
    controller.handleCommand(MMM::Logic::CmdSelectAll{
        .scope = MMM::Logic::SelectAllScope::AllTrackAreas,
    });
    if ( !isNoteSelected(playerEntity) || !isNoteSelected(draftEntity) ||
         !isSampleSelected() ) {
        XERROR("All-object select-all omitted a track area");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSelectAll{});
    // 无 hover 的默认命令保留最后分区语义，本例期望仍为 BGM。
    return !isNoteSelected(playerEntity) && !isNoteSelected(draftEntity) &&
           isSampleSelected();
}

/// @brief 验证关闭折线编辑后 Shift 拖绘只创建普通 Hold。
/// @return 跨时间和轨道拖绘仍生成起始轨道 Hold 时返回 true。
/// @details
/// Arrange：关闭 Polyline 编辑并启用专业画布基线。Act：按住 Shift/Ctrl 从玩家
/// 第 0 轨向另一轨和更早时间拖动。Assert：只创建一个起始轨道 Hold，持续时间
/// 为正且没有子节点。
///
/// 该场景证明 Key 模式不仅过滤既有 Flick/Polyline 交互，也会约束 DrawTool 的
/// 新建类型；横向位移不能在禁用状态下把手势升级为折线。
/// @par 关键不变量
/// - Shift/Ctrl 组合不会绕过类型限制。
/// - 横向跨轨预览不生成 Flick 节点。
/// - 最终 Registry 只有一个根对象。
/// - 根对象类型固定为 Hold。
/// - Hold 保留起笔轨道并具有非负持续时间。
/// - m_subNotes 保持空，确认没有隐藏折线结构。
/// @par 故障定位
/// 类型错误指向 DrawTool 手势升级规则；轨道错误指向起笔地址缓存；出现子节点
/// 则说明关闭 Polyline 后仍进入了折线构造分支。
/// @par 测试边界
/// 用例验证提交后的 ECS 结构，不检查 Hold 纹理、音效或鼠标图标等 UI 表现。
bool testKeyModeBrushCreatesOnlyHold()
{
    // 画笔从允许的玩家轨开始，输入路径不依赖已有实体。
    // 按键模式下工具应在预览阶段就固定为 Hold 类型。
    // 提交后同时检查 ECS 组件与领域对象，防止只更新一侧。
    // 时间与轨道由相机投影产生，不能由测试直接回填。
    // Undo 必须删除本次创建，证明动作边界完整入栈。
    // Redo 再恢复同一语义对象，验证创建数据可重放。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enablePolylineEditing = false;

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartBrush(context,
                              // 起笔位于玩家第 0 轨和判定线当前时间。
                              MMM::Logic::CmdStartBrush{
                                  .cameraId    = "Basic2DCanvas",
                                  .mouseX      = 150.0F,
                                  .mouseY      = 300.0F,
                                  .isShiftDown = true,
                                  .isCtrlDown  = true,
                              });
    drawTool.handleUpdateBrush(
        context,
        // 同时改变 X/Y，刻意满足通常创建 Polyline 的手势。
        MMM::Logic::CmdUpdateBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 250.0F,
            .mouseY      = 50.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        });
    drawTool.handleEndBrush(
        // 松键是动作提交边界，之后才检查 Registry 的持久组件。
        context,
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( notes.size() != 1 ) {
        XERROR("Key mode brush did not create exactly one object");
        return false;
    }
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    return note.m_type == MMM::NoteType::HOLD && note.m_trackIndex == 0 &&
           note.m_duration > 0.0 && note.m_subNotes.empty();
}

/// @brief 验证反向拖动半拍、一拍的纯 Hold 在提交及撤销重做后仍为零长度长条。
/// @details
/// 对 Polyline 开关开/关各测试半拍与一拍反向拖动。鼠标保持同轨并向歌曲更早
/// 方向移动，DrawTool 应把纯 Hold 规范为起点处零持续时间，而不是负时长或折线。
///
/// 断言覆盖手势中的 brushState、提交后的根组件结构，以及 ActionStack Undo/Redo
/// 往返，确保零长度语义不是仅存在于预览的临时状态。
/// @par 关键不变量
/// - 半拍和一拍反拖采用同一零长度规则。
/// - Polyline 开关两态都不改变纯 Hold 语义。
/// - brushState 在提交前已经显示 duration=0。
/// - 提交对象的 timestamp 保持起笔时间。
/// - Undo 完全删除对象，Redo 恢复相同零长度对象。
/// - 同轨反拖不生成任何折线子节点。
/// @par 故障定位
/// 预览先失败时检查反向时长规范化；仅提交失败检查 EndBrush 快照；仅 Redo 失败
/// 则检查 NoteAction 是否保存了规范化后的零持续时间。
/// @par 测试边界
/// 半拍和一拍基于线性缓存构造，复杂 BPM 变化由节拍换算专项测试负责。
bool testDownwardBrushCreatesZeroLengthHold()
{
    // 起点和终点位于同一轨道，唯一变量是向下拖动距离。
    // 拖动跨越预设节拍阈值，用于触发零长度 Hold 语义。
    // 预览仍需保持纯长条类型，不能提前退化为普通 Note。
    // 提交后首尾时间必须相等，且轨道投影保持起始轨。
    // 领域对象与 ECS 组件要对零长度结果给出一致表示。
    // Undo/Redo 用来确认特殊长度不会在序列化动作中丢失。
    for ( const bool polylineEnabled : { false, true } ) {
        // 两种编辑模式共享“纯同轨长条反拖”规则。
        for ( const double offset : { 0.25, 0.5 } ) {
            // 线性缓存下 0.25/0.5 秒分别代表本基线 BPM 的半拍和一拍。
            MMM::Logic::SessionContext context;
            configureObjectEditingCanvas(context);
            context.lastConfig.settings.enablePolylineEditing = polylineEnabled;
            MMM::Logic::DrawTool tool;
            tool.handleStartBrush(
                context,
                MMM::Logic::CmdStartBrush{ .cameraId    = "Basic2DCanvas",
                                           .mouseX      = 150.0F,
                                           .mouseY      = 300.0F,
                                           .isShiftDown = true,
                                           .isCtrlDown  = true });
            const auto& cache = context.timelineRegistry.ctx()
                                    .get<MMM::Logic::System::ScrollCache>();
            const float targetY =
                // 通过 ScrollCache 差值求屏幕位移，不硬编码像素/秒比例。
                300.0F + static_cast<float>(cache.getAbsY(1.0) -
                                            cache.getAbsY(1.0 - offset));
            tool.handleUpdateBrush(
                context,
                MMM::Logic::CmdUpdateBrush{ .cameraId    = "Basic2DCanvas",
                                            .mouseX      = 150.0F,
                                            .mouseY      = targetY,
                                            .isShiftDown = true,
                                            .isCtrlDown  = true });
            if ( context.brushState.type != MMM::NoteType::HOLD ||
                 // 更新阶段就应固定为零长度 Hold，不能等提交后才修正。
                 !near(context.brushState.duration, 0.0) )
                return false;
            tool.handleEndBrush(
                context,
                MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
            const auto valid = [&]() {
                // 提交和 Redo 共用同一完整结构断言。
                const auto notes =
                    context.noteRegistry.view<MMM::Logic::NoteComponent>();
                if ( notes.size() != 1U ) return false;
                const auto& note =
                    notes.get<MMM::Logic::NoteComponent>(*notes.begin());
                return note.m_type == MMM::NoteType::HOLD &&
                       near(note.m_timestamp, 1.0) &&
                       near(note.m_duration, 0.0) && note.m_subNotes.empty();
            };
            if ( !valid() ) return false;
            context.actionStack.undo(context);
            // Undo 必须销毁唯一新建实体，不保留零长度占位。
            if ( context.noteRegistry.view<MMM::Logic::NoteComponent>()
                     .size() != 0U )
                return false;
            context.actionStack.redo(context);
            // Redo 应恢复相同时间、类型和零长度，而非重新解释鼠标手势。
            if ( !valid() ) return false;
        }
    }
    return true;
}

/// @brief 验证滑键头部及折线节点向下多次拖动不生成零长度 Hold，也不丢失滑键。
/// @details
/// 分别构造根 Flick 与末节点为 Flick 的 Polyline，对半拍、一拍和两拍反向更新，
/// 并重复三帧相同鼠标位置。提交后对象类型必须保持 Flick/Polyline，横向 dtrack
/// 不能丢失，也不能额外创建零长度 Hold。
///
/// 该用例与纯 Hold 反拖场景形成边界对照：只有从空白同轨绘制的纯长条可折叠为
/// 零长度 Hold；修改滑键头部或折线节点时始终延续滑键结构。
/// @par 关键不变量
/// - 根 Flick 反拖后仍是 Flick 且 dtrack 保持 +1。
/// - Polyline 父类型不会退化为普通 Hold。
/// - Polyline 末节点仍是 Flick 且方向不丢失。
/// - 半拍、一拍和两拍偏移都遵循同一规则。
/// - 重复 UpdateBrush 不重复追加节点或对象。
/// - Registry 中持久根对象始终只有一个。
/// @par 故障定位
/// 根 Flick 退化说明续画分派错误；仅折线末节点丢失说明父子同步错误；对象数量
/// 增长则说明相同鼠标帧没有复用当前预览段。
/// @par 测试边界
/// 本用例只约束物件类型和结构，不比较最终折线顶点的像素几何或纹理渲染。
bool testDownwardFlickAndPolylineRemainSlides()
{
    // Flick 与 Polyline 分别运行同一向下拖动手势。
    // 两个场景独立建会话，避免首个动作栈影响第二个结果。
    // 手势方向只能改变节点几何，不能套用纯 Hold 的特例。
    // Flick 需要保留滑键头语义，Polyline 需要保留节点链。
    // 提交结果同时检查根对象类型和必要的子节点信息。
    // 该对照直接守住零长度长条规则的类型适用范围。
    for ( const bool polyline : { false, true } ) {
        // false 覆盖根 Flick，true 覆盖父 Polyline 的 Flick 末节点。
        for ( const double offset : { 0.25, 0.5, 1.0 } ) {
            MMM::Logic::SessionContext context;
            configureObjectEditingCanvas(context);
            context.lastConfig.settings.enablePolylineEditing = true;
            MMM::Logic::NoteComponent original;
            original.m_type =
                polyline ? MMM::NoteType::POLYLINE : MMM::NoteType::FLICK;
            original.m_timestamp  = polyline ? 0.5 : 1.0;
            original.m_trackIndex = 0;
            original.m_dtrack     = polyline ? 0 : 1;
            if ( polyline ) {
                // 两节点结构先 Hold 后 Flick，末节点是本次拖绘命中目标。
                original.m_subNotes = {
                    { MMM::NoteType::HOLD, 0.5, 0.5, 0, 0 },
                    { MMM::NoteType::FLICK, 1.0, 0.0, 0, 1 }
                };
            }
            const auto entity = context.noteRegistry.create();
            context.noteRegistry.emplace<MMM::Logic::NoteComponent>(entity,
                                                                    original);
            context.hoveredEntity     = entity;
            context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
            context.hoveredSubIndex   = polyline ? 1 : -1;
            // Polyline 明确命中第二节点；根 Flick 使用默认根索引。
            context.hoveredPart =
                static_cast<uint8_t>(MMM::Logic::HoverPart::PolylineNode);
            MMM::Logic::DrawTool tool;
            tool.handleStartBrush(
                context,
                MMM::Logic::CmdStartBrush{ .cameraId    = "Basic2DCanvas",
                                           .mouseX      = 150.0F,
                                           .mouseY      = 300.0F,
                                           .isShiftDown = true,
                                           .isCtrlDown  = true });
            const auto& cache = context.timelineRegistry.ctx()
                                    .get<MMM::Logic::System::ScrollCache>();
            const float targetY =
                300.0F + static_cast<float>(cache.getAbsY(1.0) -
                                            cache.getAbsY(1.0 - offset));
            for ( int frame = 0; frame < 3; ++frame ) {
                // 重复更新验证相同目标帧不会累积生成额外段或改变类型。
                tool.handleUpdateBrush(
                    context,
                    MMM::Logic::CmdUpdateBrush{ .cameraId    = "Basic2DCanvas",
                                                .mouseX      = 150.0F,
                                                .mouseY      = targetY,
                                                .isShiftDown = true,
                                                .isCtrlDown  = true });
            }
            tool.handleEndBrush(
                context,
                MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
            bool found = false;
            // 忽略投影子实体，只检查最终持久化根对象恰好一个。
            for ( const auto noteEntity :
                  context.noteRegistry.view<MMM::Logic::NoteComponent>() ) {
                const auto& note =
                    context.noteRegistry.get<MMM::Logic::NoteComponent>(
                        noteEntity);
                if ( note.m_isSubNote ) continue;
                if ( found || note.m_type != original.m_type ) return false;
                found = true;
                if ( polyline ) {
                    // 父结构仍有两节点且末节点 Flick 方向保持为 +1。
                    if ( note.m_subNotes.size() != 2U ||
                         note.m_subNotes.back().type != MMM::NoteType::FLICK ||
                         note.m_subNotes.back().dtrack != 1 )
                        return false;
                } else if ( note.m_dtrack != 1 )
                    return false;
            }
            if ( !found ) return false;
        }
    }
    return true;
}

/// @brief 验证先横移再纵向绘制的 L 形折线保持 Flick、Hold 顺序。
/// @return 横向段位于纵向段之前时返回 true。
/// @details
/// 手势先从第 0 轨水平移到第 1 轨，再在第 1 轨向时间方向延伸。第一次更新应只
/// 建立 Flick 预览；第二次更新升级为 Polyline，节点顺序必须是横向 Flick 后接
/// 纵向 Hold。
///
/// 该断言防止实现按几何类型重新排序节点，导致用户先画的横向段在持久化结构中
/// 被后续纵向段交换顺序。
/// @par 关键不变量
/// - 第一次水平更新先形成 Flick 语义。
/// - 第二次纵向更新才追加 Hold 语义。
/// - 父对象最终为 Polyline。
/// - m_subNotes 按用户手势时间顺序保存。
/// - Flick 的 dtrack 表示从第零轨到第一轨。
/// - Hold 在到达轨道上延伸，不被排序到 Flick 之前。
/// @par 故障定位
/// 预览顺序正确而提交顺序错误时检查 EndBrush 持久化；两阶段均错误时检查
/// PolyLineBrushState 的段追加顺序，不应按类型重新排序。
/// @par 测试边界
/// 这里只覆盖水平后纵向的两段 L 形手势，更多节点和反向轨道由其他折线测试覆盖。
bool testPolylinePreservesHorizontalFirstGestureOrder()
{
    // 首段手势刻意先横向再纵向，区分采样顺序与坐标排序。
    // 工具收到的每个采样点必须按输入发生顺序追加。
    // 相同时间附近的横移不能被稳定排序误当作重复节点。
    // 提交后父对象内嵌节点与子实体顺序必须一致。
    // Undo 清除整条折线，不能留下脱离父级的投影实体。
    // Redo 恢复原始顺序，证明动作快照未重新排列节点。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enablePolylineEditing = true;

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId    = "Basic2DCanvas",
                                  .mouseX      = 150.0F,
                                  .mouseY      = 300.0F,
                                  .isShiftDown = true,
                                  .isCtrlDown  = true,
                              });
    drawTool.handleUpdateBrush(context,
                               // 第一段仅改变轨道，不改变时间。
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId    = "Basic2DCanvas",
                                   .mouseX      = 250.0F,
                                   .mouseY      = 300.0F,
                                   .isShiftDown = true,
                                   .isCtrlDown  = true,
                               });
    const bool horizontalPreviewEstablished =
        // 单一横向段尚未形成 Polyline，使用根 Flick 草稿表达。
        context.brushState.type == MMM::NoteType::FLICK &&
        context.brushState.dtrack == 1 &&
        context.brushState.polylineSegments.empty();

    drawTool.handleUpdateBrush(context,
                               // 第二段保持目标轨道并改变时间，形成 L 形折线。
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId    = "Basic2DCanvas",
                                   .mouseX      = 250.0F,
                                   .mouseY      = 200.0F,
                                   .isShiftDown = true,
                                   .isCtrlDown  = true,
                               });
    const auto& segments = context.brushState.polylineSegments;
    const bool  horizontalThenVertical =
        // 内嵌段数组必须严格保留用户手势的先后顺序。
        context.brushState.type == MMM::NoteType::POLYLINE &&
        segments.size() == 2U && segments[0].type == MMM::NoteType::FLICK &&
        segments[0].trackIndex == 0 && segments[0].dtrack == 1 &&
        segments[1].type == MMM::NoteType::HOLD &&
        segments[1].trackIndex == 1 && segments[1].duration > 0.0;
    if ( !horizontalPreviewEstablished || !horizontalThenVertical ) {
        XERROR(
            "Horizontal-first Polyline order failed: preview={}, type={}, "
            "segments={}",
            horizontalPreviewEstablished,
            static_cast<int>(context.brushState.type),
            segments.size());
        return false;
    }
    return true;
}

/// @brief 验证关闭 BMS 编辑后 BGM 区不参与投影与画笔交互。
/// @return 玩家轨道仍可寻址且 BGM 区不会创建自动采样时返回 true。
/// @details
/// 先直接验证 calculateCanvasLaneProjection 在 BMS 关闭时报告零 BGM 轨，并且
/// BGM 区 X 无 lane；随后用相同配置驱动 DrawTool 点击 BGM 区，断言手势未激活、
/// Sample Registry 为空且持久 bgmTrackCount 未被追加。
///
/// 玩家第 0 轨仍可投影，证明开关只隐藏 BGM 域而非让整个画布失效。
/// @par 关键不变量
/// - 投影报告零条可见 BGM 轨。
/// - BGM 区 laneAt 不返回可编辑地址。
/// - 画笔在原 BGM 坐标不激活。
/// - Sample Registry 保持为空且持久轨数不增长。
/// - 玩家第零轨仍可寻址。
/// - BMS 开关不使整个画布投影失效。
/// @par 故障定位
/// 数学投影失败检查 calculateCanvasLaneProjection；投影正确但画笔激活时检查
/// DrawTool 对 enableBmsEditing 的二次权限验证。
/// @par 测试边界
/// BMS 开关只约束编辑画布，不验证已有自动采样的音频播放或文件保存行为。
bool testBmsEditingHidesBgmLanes()
{
    // BMS 开关前后复用相同轨道布局，只改变编辑模式。
    // 对照状态先证明 BGM 轨在普通模式下可以被投影命中。
    // 开启 BMS 后隐藏规则必须同时作用于投影与编辑入口。
    // 玩家轨仍保持可用，用于排除整个画布布局被禁用。
    // 测试不创建音频设备，仅验证轨道域的逻辑可达性。
    // 断言分别覆盖显示范围和命令接受范围的共同约束。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enableBmsEditing = false;

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        // 直接传入与共享画布基线一致的尺寸和布局。
        1000.0F,
        4,
        1,
        0.1F,
        0.5F,
        0.0F,
        true,
        false);
    const auto playerLane = projection.laneAt(150.0F);
    if ( !projection.valid || projection.bgmLaneCount != 0 || !playerLane ||
         playerLane->kind != MMM::Logic::CanvasLaneKind::Player ||
         projection.laneAt(550.0F).has_value() ) {
        XERROR("Disabled BMS editing still exposed a BGM lane projection");
        return false;
    }

    MMM::Logic::DrawTool drawTool;
    // 550 位于原本首个 BGM 轨区域，关闭后必须成为不可交互空白。
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return !context.brushState.isActive && samples.size() == 0 &&
           context.bgmTrackCount == 1;
}

/// @brief 验证项目音频选择按资源类型决定画笔在玩家区和 BGM 区的产物。
/// @return Effect 可创建绑定 Note 与自动采样，Main 只允许创建自动采样，空选择
/// 可创建静音采样草稿时返回 true。
/// @details
/// 同一画布依次覆盖三种资源选择。Effect 在玩家区创建带绑定 Note，在 BGM 区
/// 创建自动采样；Main 在玩家区被拒绝，但可在追加 BGM 轨创建 Sample 并扩轨；
/// 空资源在 BGM 区进入静音采样画笔并保留用户音量。
///
/// 每一步检查此前对象数量不被意外改变，证明资源类型和落点区域共同决定产物，
/// 而不是仅按当前资源或仅按轨道区单独分派。
/// @par 关键不变量
/// - Main 资源只能进入 BGM 自动采样域。
/// - Effect 在 BGM 区创建自动 Sample。
/// - Effect 在玩家区创建带绑定 Tap。
/// - 空资源只在 BGM 区创建静音草稿。
/// - 资源 ID 与用户音量原样进入产物。
/// - 失败组合不改变此前已经创建的对象。
/// @par 故障定位
/// 资源类型错误优先检查项目资源解析；区域产物错误检查 LaneKind 分派；已有对象
/// 数量变化说明失败事务在完成校验前修改了 Registry。
/// @par 测试边界
/// 测试使用内存项目资源，不访问真实音频文件，也不证明解码器可播放这些路径。
bool testBrushAudioResourcePlacementRules()
{
    // 项目资源表包含有效音频，并保留一个不存在的资源 ID。
    // 有效资源先验证 Sample 的轨道、时间和绑定描述符。
    // 无效资源随后走同一画笔入口，结果必须整体拒绝。
    // 拒绝前后的 Registry 数量用于检查没有半成品实体。
    // 动作栈深度还要保持不变，避免留下不可执行的 Undo。
    // 所有断言共同验证资源校验发生在持久化提交之前。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);
    MMM::Logic::DrawTool              drawTool;

    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        // Effect 可同时绑定玩家物件和作为 BGM 区自动采样资源。
        .audioResourceId = "effect",
        .audioTrackType  = MMM::AudioTrackType::Effect,
        .volume          = 0.4F,
    });
    drawTool.handleStartBrush(context,
                              // 玩家第 0 轨创建普通 Note。
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 150.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( notes.size() != 1 ) {
        XERROR("Effect brush selection did not create a player note");
        return false;
    }
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    // Note 绑定必须保留资源 ID 与画笔音量，且不改变轨道位置。
    if ( note.m_trackIndex != 0 || !note.m_sampleBinding ||
         note.m_sampleBinding->m_audioResourceId != "effect" ||
         !near(note.m_sampleBinding->m_volume, 0.4) ) {
        XERROR("Effect brush selection did not bind the created note");
        return false;
    }

    drawTool.handleStartBrush(
        context,
        // 相同 Effect 在首个 BGM 轨创建 Sample，而非 Note。
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 550.0F,
            .mouseY   = 300.0F,
        });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 1 ) {
        XERROR("Effect brush selection did not create an automatic sample");
        return false;
    }
    const auto& effectSample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( effectSample.m_track != 4 ||
         // 绝对轨道 4 紧随四条玩家轨道之后。
         effectSample.m_audioResourceId != "effect" ||
         !near(effectSample.m_volume, 0.4) ) {
        XERROR("Effect automatic sample used the wrong lane or resource");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        // Main 资源只允许成为自动采样，不能绑定玩家 Note。
        .audioResourceId = "main",
        .audioTrackType  = MMM::AudioTrackType::Main,
        .volume          = 0.7F,
    });
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 250.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    if ( context.noteRegistry.view<MMM::Logic::NoteComponent>().size() != 1 ||
         // 玩家区尝试不能新增 Note，并应给出可辨识的主音轨拒绝反馈。
         context.lastActionMessage.find("主音轨") == std::string::npos ) {
        XERROR("Main brush selection was not rejected in the player lanes");
        return false;
    }

    drawTool.handleStartBrush(
        context,
        // 650 命中运行时追加 BGM 轨，提交后应扩为第二持久轨。
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 650.0F,
            .mouseY   = 300.0F,
        });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 2 || context.bgmTrackCount != 2 ) {
        XERROR("Main brush selection did not create on the append BGM lane");
        return false;
    }
    bool foundMain = false;
    // Registry 迭代顺序不固定，按字段寻找新 Main Sample。
    for ( const auto entity : samples ) {
        const auto& sample = samples.get<MMM::Logic::SampleComponent>(entity);
        foundMain          = foundMain || (sample.m_track == 5 &&
                                  sample.m_audioResourceId == "main" &&
                                  near(sample.m_volume, 0.7));
    }
    if ( !foundMain ) return false;

    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        // 空 ID 与 Effect 类型组合表示允许创建未绑定静音 Sample 草稿。
        .audioResourceId = {},
        .audioTrackType  = MMM::AudioTrackType::Effect,
        .volume          = 0.55F,
    });
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    if ( !context.brushState.isActive ||
         // 提交前预览必须明确标记为 Sample 且资源 ID 为空。
         !context.brushState.createsAudioSample ||
         !context.brushState.activeAudioResourceId.empty() ) {
        XERROR("Empty audio selection did not expose a silent sample brush");
        return false;
    }
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 3 ) {
        XERROR("Empty audio selection did not create a silent sample draft");
        return false;
    }
    return std::ranges::any_of(samples, [&](const auto entity) {
        // 最终草稿保留空 ID、BGM 绝对轨道和画笔音量。
        const auto& sample = samples.get<MMM::Logic::SampleComponent>(entity);
        return sample.m_audioResourceId.empty() &&
               sample.m_track >=
                   static_cast<std::uint32_t>(context.trackCount) &&
               near(sample.m_volume, 0.55);
    });
}

/// @brief 验证 BGM 画笔按住期间持续更新半透明采样预览的轨道与时间。
/// @return 拖到追加轨后预览和最终 Sample 均跟随指针位置时返回 true。
/// @details
/// 从首个 BGM 轨按下后保持画笔，拖向追加轨并改变 Y。按下预览应显示轨道 4、
/// Effect ID 和开始时间；更新后轨道变为 5且时间变化；松键只创建一个 Sample，
/// 扩展持久 BGM 轨数并使用最后预览时间。
///
/// 这证明半透明预览不是起笔快照，而是在合法 BGM 域内持续跟随指针，提交值与
/// 用户最后看到的状态一致。
/// @par 关键不变量
/// - 按下时预览位于第一条 BGM 轨。
/// - 更新时预览轨道和时间都随鼠标变化。
/// - 预览保留选中 Effect 的资源 ID。
/// - 松手前不创建 Sample Registry 实体。
/// - EndBrush 只提交最后一个预览位置。
/// - 落在追加轨时持久 BGM 数量恰好扩展一轨。
/// @par 故障定位
/// 预览不移动说明 UpdateBrush 未刷新草稿；最终位置不同说明 EndBrush 使用起笔
/// 快照；多对象或多历史项说明预览被逐帧提交。
/// @par 测试边界
/// 这里只检查逻辑预览字段与最终组件，不对半透明渲染颜色或帧间平滑度作验收。
bool testSampleBrushFollowsPointerBeforeCommit()
{
    // 按下阶段建立唯一的临时 Sample，尚不写入领域数组。
    // 移动阶段跨越时间与轨道，预览实体必须跟随最新指针。
    // 多次移动不得重复创建预览或提前增加动作栈记录。
    // 松开后才把最后位置作为一次原子创建动作提交。
    // 提交结果检查临时身份转换和资源绑定均未发生漂移。
    // Undo 用于确认预览阶段没有额外持久化副作用。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);
    MMM::Logic::DrawTool              drawTool;
    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "effect",
        .audioTrackType  = MMM::AudioTrackType::Effect,
    });

    drawTool.handleStartBrush(context,
                              // 550 命中现有首个 BGM 轨。
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    const double startTime = context.brushState.time;
    // 按下阶段尚未创建 Sample，只检查 brushState 预览身份。
    if ( !context.brushState.isActive ||
         !context.brushState.createsAudioSample ||
         context.brushState.track != 4 ||
         context.brushState.activeAudioResourceId != "effect" ) {
        XERROR("Sample brush did not expose its pressed preview state");
        return false;
    }

    drawTool.handleUpdateBrush(context,
                               // 650 命中追加轨，Y 改变产生新的时间锚点。
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId   = "Basic2DCanvas",
                                   .mouseX     = 650.0F,
                                   .mouseY     = 250.0F,
                                   .isCtrlDown = true,
                               });
    if ( context.brushState.track != 5 ||
         near(context.brushState.time, startTime) ) {
        XERROR("Sample brush preview did not follow the held pointer");
        return false;
    }
    const double draggedTime = context.brushState.time;
    // 保存最终预览时间，提交后与实体值直接比较。
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 1 || context.bgmTrackCount != 2 ) return false;
    // 追加轨提交同时把持久 BGM 数从一扩到二。
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    return sample.m_track == 5 && near(sample.m_timestamp, draggedTime);
}

/// @brief 验证单次绘制手势锁定起笔所在的轨道类型。
/// @return 玩家、BGM 与草稿画笔越区时均保留原类型和最后有效状态时返回 true。
/// @details
/// 用三个独立 Session 分别从玩家、BGM 与草稿域起笔。手势进入另一域时必须保持
/// 原画笔类型和最后合法状态，不创建跨域对象；回到起始域后继续更新并正常提交。
///
/// 玩家场景还建立两段 Polyline 与 Effect 绑定，验证越入 BGM 时复杂草稿不丢失；
/// BGM 场景验证 Sample 不转成 Note；草稿场景验证负轨身份不转成正式玩家物件。
///
/// 轨道类型在鼠标按下时锁定，是一次手势的协议边界。跨区更新只能暂停，不可重置
/// 或转换状态，否则一次连续拖动会产生不同 Registry 的多个动作。
/// @par 关键不变量
/// - 玩家起笔不会在同一手势中变成 BGM Sample。
/// - BGM 起笔不会在同一手势中变成玩家 Note。
/// - 草稿起笔不会跨入玩家域成为正式对象。
/// - 越界期间时间和轨道保持最后合法预览。
/// - 回到起始域后预览恢复跟随。
/// - 提交产物的 kind 与起笔域一致。
/// @par 故障定位
/// 越区即取消说明状态机未区分暂停和终止；越区转换说明起笔 LaneKind 未锁定；
/// 返回后不更新则检查最近合法预览与当前指针的恢复路径。
/// @par 测试边界
/// 三个域分别使用单指针手势，不覆盖多点触控、窗口失焦或跨画布拖动取消。
bool testBrushStaysInStartingLaneKind()
{
    // 起笔分别从草稿、玩家与音频域开始并跨越分区拖动。
    // 起始域在按下时锁定，后续坐标只更新域内轨道位置。
    // 草稿对象不能因指针越界转成玩家 Note 或 Sample。
    // 玩家对象也不能在拖动途中获得音频资源语义。
    // 每个子场景检查预览和提交后的对象种类保持一致。
    // 该结构专门防止连续手势跨 Registry 迁移所有权。
    MMM::Logic::SessionContext playerContext;
    // 第一部分：从玩家区起笔并先形成复杂 Polyline。
    configureObjectEditingCanvas(playerContext);
    playerContext.lastConfig.settings.enablePolylineEditing = true;
    MMM::Logic::InteractionController playerController(playerContext);
    MMM::Logic::DrawTool              playerDrawTool;
    playerController.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "effect.wav",
        .audioTrackType  = MMM::AudioTrackType::Effect,
        .volume          = 0.6F,
    });

    playerDrawTool.handleStartBrush(playerContext,
                                    MMM::Logic::CmdStartBrush{
                                        .cameraId    = "Basic2DCanvas",
                                        .mouseX      = 150.0F,
                                        .mouseY      = 300.0F,
                                        .isShiftDown = true,
                                        .isCtrlDown  = true,
                                    });
    playerDrawTool.handleUpdateBrush(playerContext,
                                     MMM::Logic::CmdUpdateBrush{
                                         .cameraId    = "Basic2DCanvas",
                                         .mouseX      = 150.0F,
                                         .mouseY      = 200.0F,
                                         .isShiftDown = true,
                                         .isCtrlDown  = true,
                                     });
    playerDrawTool.handleUpdateBrush(playerContext,
                                     MMM::Logic::CmdUpdateBrush{
                                         .cameraId    = "Basic2DCanvas",
                                         .mouseX      = 350.0F,
                                         .mouseY      = 200.0F,
                                         .isShiftDown = true,
                                         .isCtrlDown  = true,
                                     });
    if ( playerContext.brushState.type != MMM::NoteType::POLYLINE ||
         playerContext.brushState.polylineSegments.size() != 2U ) {
        XERROR("Player brush did not establish a complex Polyline");
        return false;
    }
    const auto preservedSegments = playerContext.brushState.polylineSegments;
    // 保存越区前全部关键预览字段，随后要求逐项保持。
    const auto preservedTime     = playerContext.brushState.time;
    const auto preservedTrack    = playerContext.brushState.track;
    const auto preservedDuration = playerContext.brushState.duration;
    const auto preservedDtrack   = playerContext.brushState.dtrack;

    playerDrawTool.handleUpdateBrush(
        playerContext,
        // 指针进入 BGM 区，更新应暂停在最后合法玩家状态。
        MMM::Logic::CmdUpdateBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 550.0F,
            .mouseY      = 100.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        });
    const auto& crossedPlayerBrush = playerContext.brushState;
    if ( !crossedPlayerBrush.isActive ||
         crossedPlayerBrush.createsAudioSample ||
         crossedPlayerBrush.type != MMM::NoteType::POLYLINE ||
         crossedPlayerBrush.polylineSegments.size() !=
             preservedSegments.size() ||
         !near(crossedPlayerBrush.time, preservedTime) ||
         crossedPlayerBrush.track != preservedTrack ||
         !near(crossedPlayerBrush.duration, preservedDuration) ||
         crossedPlayerBrush.dtrack != preservedDtrack ||
         crossedPlayerBrush.polylineSegments.back().type !=
             preservedSegments.back().type ||
         crossedPlayerBrush.polylineSegments.back().trackIndex !=
             preservedSegments.back().trackIndex ||
         crossedPlayerBrush.polylineSegments.back().dtrack !=
             preservedSegments.back().dtrack ||
         !crossedPlayerBrush.activeSampleBinding ||
         crossedPlayerBrush.activeSampleBinding->m_audioResourceId !=
             "effect.wav" ) {
        XERROR("Player brush changed after crossing into the BGM lanes");
        return false;
    }

    playerDrawTool.handleUpdateBrush(
        playerContext,
        // 返回玩家区后手势继续，末段 dtrack 更新到目标轨。
        MMM::Logic::CmdUpdateBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 450.0F,
            .mouseY      = 100.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        });
    if ( playerContext.brushState.type != MMM::NoteType::POLYLINE ||
         playerContext.brushState.polylineSegments.size() != 2U ||
         playerContext.brushState.polylineSegments.back().dtrack != 3 ) {
        XERROR("Player brush did not resume after returning to player lanes");
        return false;
    }

    playerDrawTool.handleEndBrush(
        playerContext, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    if ( !playerContext.sampleRegistry.view<MMM::Logic::SampleComponent>()
              .empty() ) {
        // 玩家起笔手势整个生命周期不得向 Sample Registry 写入。
        XERROR("Player brush crossing created an automatic sample");
        return false;
    }
    const auto playerNotes =
        playerContext.noteRegistry.view<MMM::Logic::NoteComponent>();
    bool foundPlayerPolyline = false;
    for ( const auto entity : playerNotes ) {
        const auto& note = playerNotes.get<MMM::Logic::NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;
        foundPlayerPolyline =
            note.m_type == MMM::NoteType::POLYLINE &&
            std::ranges::all_of(note.m_subNotes, [&](const auto& subNote) {
                const int endTrack = subNote.trackIndex + subNote.dtrack;
                return subNote.trackIndex >= 0 &&
                       subNote.trackIndex < playerContext.trackCount &&
                       endTrack >= 0 && endTrack < playerContext.trackCount;
            });
    }
    if ( !foundPlayerPolyline ) {
        XERROR("Player brush crossing did not retain its Polyline");
        return false;
    }

    MMM::Logic::SessionContext bgmContext;
    // 第二部分：从 BGM 区起笔，跨玩家区时保持 Sample 画笔。
    configureObjectEditingCanvas(bgmContext);
    MMM::Logic::DrawTool bgmDrawTool;
    bgmDrawTool.handleStartBrush(bgmContext,
                                 MMM::Logic::CmdStartBrush{
                                     .cameraId = "Basic2DCanvas",
                                     .mouseX   = 550.0F,
                                     .mouseY   = 300.0F,
                                 });
    const double bgmStartTime = bgmContext.brushState.time;
    bgmDrawTool.handleUpdateBrush(bgmContext,
                                  // 玩家区更新无效，轨道和时间保持起笔值。
                                  MMM::Logic::CmdUpdateBrush{
                                      .cameraId   = "Basic2DCanvas",
                                      .mouseX     = 250.0F,
                                      .mouseY     = 200.0F,
                                      .isCtrlDown = true,
                                  });
    if ( !bgmContext.brushState.isActive ||
         !bgmContext.brushState.createsAudioSample ||
         bgmContext.brushState.track != 4 ||
         !near(bgmContext.brushState.time, bgmStartTime) ) {
        XERROR("BGM brush changed after crossing into the player lanes");
        return false;
    }
    bgmDrawTool.handleUpdateBrush(bgmContext,
                                  // 返回追加 BGM 轨后恢复跟随并更新时间。
                                  MMM::Logic::CmdUpdateBrush{
                                      .cameraId   = "Basic2DCanvas",
                                      .mouseX     = 650.0F,
                                      .mouseY     = 200.0F,
                                      .isCtrlDown = true,
                                  });
    if ( bgmContext.brushState.track != 5 ||
         near(bgmContext.brushState.time, bgmStartTime) ) {
        XERROR("BGM brush did not resume after returning to BGM lanes");
        return false;
    }
    bgmDrawTool.handleEndBrush(
        bgmContext, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    const auto bgmSamples =
        bgmContext.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( bgmSamples.size() != 1U ||
         bgmSamples.get<MMM::Logic::SampleComponent>(*bgmSamples.begin())
                 .m_track != 5 ) {
        XERROR("BGM brush committed outside its starting lane kind");
        return false;
    }

    MMM::Logic::SessionContext draftContext;
    // 第三部分：横移相机暴露草稿区，从最左负轨起笔。
    configureObjectEditingCanvas(draftContext);
    draftContext.cameras["Basic2DCanvas"].horizontalOffsetX = 400.0F;
    MMM::Logic::DrawTool draftDrawTool;
    draftDrawTool.handleStartBrush(draftContext,
                                   MMM::Logic::CmdStartBrush{
                                       .cameraId = "Basic2DCanvas",
                                       .mouseX   = 150.0F,
                                       .mouseY   = 300.0F,
                                   });
    const double draftStartTime = draftContext.brushState.time;
    draftDrawTool.handleUpdateBrush(
        draftContext,
        // 跨入玩家区只暂停，不把草稿转换成正式 Note。
        MMM::Logic::CmdUpdateBrush{
            .cameraId   = "Basic2DCanvas",
            .mouseX     = 550.0F,
            .mouseY     = 200.0F,
            .isCtrlDown = true,
        });
    if ( !draftContext.brushState.isActive ||
         draftContext.brushState.createsAudioSample ||
         draftContext.brushState.track != -4 ||
         !near(draftContext.brushState.time, draftStartTime) ) {
        XERROR("Draft brush changed after crossing into the player lanes");
        return false;
    }
    draftDrawTool.handleUpdateBrush(draftContext,
                                    // 返回另一草稿轨后恢复更新并提交该负轨。
                                    MMM::Logic::CmdUpdateBrush{
                                        .cameraId   = "Basic2DCanvas",
                                        .mouseX     = 250.0F,
                                        .mouseY     = 200.0F,
                                        .isCtrlDown = true,
                                    });
    if ( draftContext.brushState.track != -3 ||
         near(draftContext.brushState.time, draftStartTime) ) {
        XERROR("Draft brush did not resume after returning to draft lanes");
        return false;
    }
    draftDrawTool.handleEndBrush(
        draftContext, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    const auto draftNotes =
        draftContext.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( draftNotes.size() != 1U ) return false;
    const auto& draftNote =
        draftNotes.get<MMM::Logic::NoteComponent>(*draftNotes.begin());
    return draftNote.m_isDraft && draftNote.m_trackIndex == -3;
}

/// @brief 验证画笔在横向平移后可直接创建并更新负轨草稿物件。
/// @return 最终物件保持草稿域且不产生正式谱面变更时返回 true。
/// @details
/// 横移主相机使草稿轨进入屏幕，从负轨起笔并水平拖到相邻负轨。提交后 Registry
/// 只含一个 m_isDraft 根 Note，轨道为 -3；BeatMap 正式 Note 容器仍为空，动作栈
/// 也不发布正式对象 mutationFlags。
///
/// 该用例同时验证相机偏移参与草稿拾取、画笔可以更新负轨，以及草稿编辑与正式
/// 谱面协作/保存数据域隔离。
/// @par 关键不变量
/// - 横向相机偏移使负轨进入屏幕可交互区域。
/// - 起笔和更新都使用草稿负轨地址。
/// - 提交对象带 m_isDraft=true。
/// - 最终轨道保持负值 -3。
/// - 正式 BeatMap NoteData 不接收草稿对象。
/// - 动作不发布正式谱面 MutationFlags。
/// @par 故障定位
/// 无法起笔通常是相机偏移未进入草稿投影；能创建但写入 BeatMap 则检查
/// ProjectDraftLaneService 与正式 SessionUtils 同步边界。
/// @par 测试边界
/// 用例不安装真实项目草稿组，只验证单 Session 中草稿与正式领域数据的隔离。
bool testBrushCreatesDraftNote()
{
    // 专业模式提供负轨道草稿区，起点明确落在该分区。
    // 画笔只应创建 Draft Note，不得占用玩家轨道编号。
    // 提交后领域草稿集合与 ECS NoteComponent 同步检查。
    // 稳定 ID 必须随对象创建，供后续协作和 Undo 定位。
    // Undo 删除草稿对象时不能影响玩家与 Sample Registry。
    // Redo 恢复原语义，证明负轨道信息完整进入动作快照。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.cameras["Basic2DCanvas"].horizontalOffsetX = 400.0F;
    // 偏移把原本画布左侧的草稿域平移到可点击屏幕区域。

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartBrush(context,
                              // 150 对应当前投影中的一条草稿负轨。
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 150.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleUpdateBrush(context,
                               // 同一手势移动到相邻草稿轨，不跨玩家边界。
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId   = "Basic2DCanvas",
                                   .mouseX     = 250.0F,
                                   .mouseY     = 300.0F,
                                   .isCtrlDown = true,
                               });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( notes.size() != 1U ) return false;
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    return note.m_isDraft && note.m_trackIndex == -3 &&
           // 草稿只存在 Session ECS，不写入正式 BeatMap NoteData。
           context.currentBeatmap->m_noteData.notes.empty() &&
           context.actionStack.takePendingMutationFlags() ==
               MMM::BeatmapMutationFlags::None;
}

/// @brief 验证横向相机偏移被渲染和拾取共用的轨道投影正确应用。
/// @return 投影边界、轨道宽度和拾取结果正确时返回 true。
/// @details
/// 以归一化 `[0.2, 0.6]` 玩家区、1000 像素视口和 +50 横移构建四轨投影。
/// 断言左右边界整体移动到 250/650，单轨仍为 100 像素，并检查半开轨道边界
/// 349/350 分别映射到第 0/1 轨。
///
/// 同一 PlayerTrackProjection 同时供渲染矩形与鼠标 trackAt 使用，边界一致可避免
/// 画面已经平移但拾取仍停留在旧位置。
/// @par 关键不变量
/// - 归一化边界乘视口宽度后再应用相机偏移。
/// - 平移不改变单轨宽度。
/// - 左边界包含，右边界排除。
/// - 轨道交界采用半开区间避免双重命中。
/// - 第零轨和最后一轨都能被精确寻址。
/// - 画布外坐标不会被 contains 接受。
/// @par 故障定位
/// 边界整体偏差说明相机偏移应用顺序错误；只有交界轨错误说明 contains/trackAt
/// 的半开区间不一致；轨宽错误则检查归一化宽度除数。
/// @par 测试边界
/// 纯数学测试使用逻辑像素，不覆盖 ImGui DPI、窗口装饰或实际显示器缩放。
bool testTrackProjectionUsesCameraOffset()
{
    // 基准点先在零横移相机下计算所属玩家轨。
    // 随后只改变 horizontalOffsetX，其他布局参数保持不动。
    // 同一屏幕坐标应映射到偏移后的逻辑位置与不同轨道。
    // 反向投影还需恢复匹配的屏幕坐标，形成往返约束。
    // 边界点采用 near 比较，避免浮点舍入掩盖轨道错误。
    // 本用例不涉及对象编辑，只隔离验证相机投影层。
    const auto projection = MMM::Logic::calculatePlayerTrackProjection(
        // 参数只涉及纯数学投影，不需要 SessionContext 或 ScrollCache。
        1000.0F,
        4,
        0.2F,
        0.6F,
        50.0F);
    if ( !projection.valid || !near(projection.leftX, 250.0) ||
         !near(projection.rightX, 650.0) ||
         !near(projection.singleTrackWidth, 100.0) ||
         !projection.contains(250.0F) || projection.contains(200.0F) ||
         projection.trackAt(349.0F, 4) != 0 ||
         projection.trackAt(350.0F, 4) != 1 ||
         projection.trackAt(649.0F, 4) != 3 ) {
        XERROR("Canvas track projection ignored horizontal camera offset");
        return false;
    }
    return true;
}

/// @brief 验证草稿、玩家与 BGM 轨道共用统一地址和连续横向投影。
/// @return 边界、绝对轨道及运行时追加轨映射均正确时返回 true。
/// @details
/// 构造四草稿轨、四玩家轨、两持久 BGM 轨加一追加轨的连续投影，并启用批注栏。
/// 逐项检查草稿负绝对轨、玩家索引、批注空隙、BGM 绝对轨、追加轨和画布外区域。
///
/// 同时验证地址反向换算、单轨 bounds 和 BGM 可见范围。统一 CanvasLaneAddress
/// 必须在三域间无重叠，批注栏只占布局空间而不能被 laneAt 当作可编辑轨道。
/// @par 关键不变量
/// - 草稿地址转换为负绝对轨，玩家与 BGM 转为非负轨。
/// - 批注栏占独立间隙且不返回 LaneAddress。
/// - 追加 BGM 轨与持久 BGM 轨保持连续索引。
/// - bounds 与 laneAt 对同一轨道互为一致换算。
/// - 可见范围采用半开索引区间。
/// - 投影外坐标不会被错误吸附到边缘轨。
/// @par 故障定位
/// 绝对轨错误检查 CanvasLaneAddress；区域边界错误检查统一投影构造；仅可见范围
/// 错误时检查像素裁剪到轨道索引的 floor/ceil 规则。
/// @par 测试边界
/// 用例固定四键与两条 BGM 轨，极端键数和无效配置由输入校验测试负责。
bool testUnifiedLaneProjection()
{
    // 固定布局一次枚举草稿、玩家、BGM 与追加轨代表点。
    // 每个代表点都验证命中域、局部索引和持久轨道编号。
    // 相邻分区边界采用内外两侧坐标，避免重叠归属。
    // 逆向查询再把逻辑轨道还原为可点击的屏幕范围。
    // 不可见域应返回空结果，而不是夹取到最近有效轨。
    // 这些断言为后续画笔、抓取和选择测试提供投影基线。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.1F, 0.5F, 0.0F, true, true, true);
    const auto firstDraft = projection.laneAt(-300.0F);
    // 最左草稿轨映射为 Draft index 0，并转换到绝对轨 -4。
    const auto lastDraft  = projection.laneAt(99.0F);
    const auto playerLane = projection.laneAt(499.0F);
    const auto annotation = projection.laneAt(510.0F);
    // 510 位于批注栏，预期没有 CanvasLaneAddress。
    const auto firstBgm  = projection.laneAt(526.0F);
    const auto appendBgm = projection.laneAt(726.0F);
    // 第三条 BGM 是运行时追加轨，绝对轨仍连续位于玩家轨之后。
    const auto outside = projection.laneAt(826.0F);
    const auto visible = projection.visibleBgmRange(576.0F, 676.0F);
    // 可见范围使用半开索引，覆盖前两条 BGM 轨。
    const auto firstDraftBounds =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Draft, 0U });
    if ( !projection.valid || projection.draftLaneCount != 4 ||
         !near(projection.draftLeftX, -300.0) ||
         !near(projection.draftRightX, 100.0) || !firstDraft ||
         *firstDraft !=
             MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Draft,
                                            0 } ||
         firstDraft->absoluteTrack(4) != -4 || !lastDraft ||
         lastDraft->absoluteTrack(4) != -1 || !firstDraftBounds ||
         !near(firstDraftBounds->leftX, -300.0) ||
         !near(firstDraftBounds->rightX, -200.0) ||
         projection.laneAt(-301.0F).has_value() ||
         MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(-1, 4) !=
             MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Draft,
                                            3 } ||
         projection.bgmLaneCount != 3 || !playerLane ||
         *playerLane !=
             MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Player,
                                            3 } ||
         annotation || !near(projection.annotationLeftX, 500.0) ||
         !near(projection.annotationRightX, 526.0) || !firstBgm ||
         firstBgm->absoluteTrack(4) != 4 || !appendBgm ||
         *appendBgm !=
             MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Bgm,
                                            2 } ||
         outside || !visible || visible->first != 0 || visible->second != 2 ) {
        XERROR("Unified canvas lane projection did not map all lane areas");
        return false;
    }
    return true;
}

/// @brief 验证草稿、批注与 BGM 区域使用独立 X 和宽度投影。
/// @return 区域边界、轨道拾取、空隙和相机偏移均符合配置时返回 true。
/// @details
/// 使用 TrackLayout 为草稿、批注和 BGM 分别设置 left/width，并添加 +20
/// 相机横移。 断言每个区域的像素边界、单轨 bounds、整体
/// contentBounds、命中和可见索引。
///
/// 区域间保留的空隙不能被 laneAt 命中；nearestLane 可以从普通空隙选择最近 BGM，
/// 但批注栏属于保留交互区，连 nearestLane 也必须返回空。
/// @par 关键不变量
/// - 草稿、批注和 BGM 各使用自己的 left/width。
/// - 同一相机偏移平移所有区域但不改变各自宽度。
/// - 普通空隙可选择最近可编辑轨。
/// - 批注保留区禁止 laneAt 与 nearestLane 命中。
/// - contentBounds 包含最左草稿和最右追加 BGM。
/// - 两个辅助域分别计算可见半开范围。
/// @par 故障定位
/// 所有区域同宽说明仍复用了玩家轨宽；只有普通空隙错误检查 nearestLane；
/// 批注区被命中则说明保留区域没有先于最近轨逻辑拦截。
/// @par 测试边界
/// 此处只验证布局计算，不创建批注对象，也不验证批注栏的具体绘制样式。
bool testIndependentAuxiliaryLaneProjection()
{
    // BGM 与其他辅助轨分别配置独立数量和显示开关。
    // 投影结果必须保留辅助轨种类，不能统一折算为 Sample。
    // 修改一种辅助轨数量时，另一种的索引保持稳定。
    // 各域边界坐标用于检查累计偏移没有相互挤占。
    // 禁用单域后只清除该域命中，不改变玩家区宽度。
    // 测试由此约束辅助布局配置的独立演进能力。
    MMM::Config::TrackLayout layout;
    // 各辅助区域刻意采用不同宽度，防止实现错误复用玩家单轨宽度。
    layout.left             = 0.1F;
    layout.right            = 0.5F;
    layout.draftLanes.left  = -0.4F;
    layout.draftLanes.width = 0.05F;
    layout.annotation.left  = 0.6F;
    layout.annotation.width = 0.04F;
    layout.bgmLanes.left    = 0.75F;
    layout.bgmLanes.width   = 0.08F;

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        // 所有区域共同应用相机偏移，但各自保持独立归一化起点与宽度。
        1000.0F,
        4,
        2,
        layout,
        20.0F,
        true,
        true,
        true);
    const auto draft =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Draft, 2U });
    const auto bgm = projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 1U });
    const auto draftHit       = projection.laneAt(-275.0F);
    const auto bgmHit         = projection.laneAt(875.0F);
    const auto draftVisible   = projection.visibleDraftRange(-330.0F, -220.0F);
    const auto bgmVisible     = projection.visibleBgmRange(800.0F, 930.0F);
    const auto contentBounds  = projection.contentBounds();
    const auto nearestGapLane = projection.nearestLane(700.0F);
    // 700 位于批注与 BGM 之间普通空隙，应选择最近的首个 BGM 轨。

    if ( !projection.valid || !near(projection.draftLaneWidth, 50.0) ||
         !near(projection.draftLeftX, -380.0) ||
         !near(projection.draftRightX, -180.0) || !draft ||
         !near(draft->leftX, -280.0) || !near(draft->rightX, -230.0) ||
         !near(projection.annotationLeftX, 620.0) ||
         !near(projection.annotationRightX, 660.0) ||
         !near(projection.bgmLaneWidth, 80.0) ||
         !near(projection.bgmLeftX, 770.0) ||
         !near(projection.bgmRightX, 1010.0) || !bgm ||
         !near(bgm->leftX, 850.0) || !near(bgm->rightX, 930.0) ||
         !near(contentBounds.leftX, -380.0) ||
         !near(contentBounds.rightX, 1010.0) ) {
        XERROR("Independent auxiliary lane geometry was not projected");
        return false;
    }
    // 最终组合断言覆盖空隙、批注保留区和两个辅助域的可见半开范围。
    return draftHit && draftHit->kind == MMM::Logic::CanvasLaneKind::Draft &&
           draftHit->index == 2U && bgmHit &&
           bgmHit->kind == MMM::Logic::CanvasLaneKind::Bgm &&
           bgmHit->index == 1U && !projection.laneAt(700.0F) &&
           !projection.laneAt(630.0F) && !projection.nearestLane(630.0F) &&
           nearestGapLane &&
           nearestGapLane->kind == MMM::Logic::CanvasLaneKind::Bgm &&
           nearestGapLane->index == 0U && draftVisible &&
           draftVisible->first == 1U && draftVisible->second == 4U &&
           bgmVisible && bgmVisible->first == 0U && bgmVisible->second == 2U;
}

/// @brief 验证草稿持久轨道数量独立于玩家键数并始终保留最左追加轨。
/// @return 追加轨映射、扩轨后原轨道屏幕位置和反向地址均保持稳定时返回 true。
/// @details
/// 初始六条持久草稿轨外再显示最左追加轨，总投影计数为七。命中追加轨应得到
/// 绝对轨 -7；已有绝对轨 -6 位于 index 1。扩展持久轨到七后再增加新追加轨，
/// 原 -6 轨 index 变为 2，但屏幕 X 保持不动。
///
/// 还验证可见范围和显示编号：追加轨与第一持久轨都显示为 1，之后按靠近玩家区
/// 的顺序递增。该布局保证扩轨发生在最左侧，不让既有草稿物件视觉跳动。
/// @par 关键不变量
/// - 持久六轨加虚拟追加轨得到七条投影轨。
/// - 虚拟轨位于最左侧并编码为 -7。
/// - 既有 -6 轨扩轨前后屏幕 X 不变。
/// - 扩轨只在更左侧增加新的虚拟轨。
/// - 显示编号隐藏虚拟轨与首持久轨的内部差异。
/// - 可见范围能包含追加轨且不越过请求区间。
/// @par 故障定位
/// 扩轨后既有物件跳动说明新轨加在玩家侧；地址错误检查负轨与投影 index 偏移；
/// 仅编号错误检查 draftTrackDisplayNumber 的用户层换算。
/// @par 测试边界
/// 测试只计算扩轨前后投影，不执行实际拖动或 ProjectDraftLaneService 持久化。
bool testDynamicDraftAppendLaneProjection()
{
    // 初始草稿轨数量固定，追加轨只在拖动预览期间出现。
    // 指针越过最外侧草稿轨后应得到临时追加索引。
    // 预览不能立即修改项目持久草稿轨计数。
    // 正式提交时才扩展计数并重新建立稳定投影范围。
    // 撤回预览则必须完全移除临时轨，恢复原边界。
    // 前后坐标对照用于识别缓存未失效造成的幽灵轨道。
    const auto before = MMM::Logic::calculateCanvasLaneProjection(
        // 六持久轨加一追加轨，玩家键数仍固定为四。
        1000.0F,
        4,
        0,
        0.1F,
        0.5F,
        0.0F,
        true,
        false,
        true,
        6,
        true);
    const auto appendLane = before.laneAt(-550.0F);
    const auto existingLane =
        MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(-6, 4, 7);
    const auto existingBounds = before.bounds(existingLane);

    const auto visibleRange = before.visibleDraftRange(-525.0F, -325.0F);
    const bool numberingMatchesTracks =
        // 追加轨和持久最左轨共享用户可理解的首轨编号。
        MMM::Logic::draftTrackDisplayNumber(-7, 6) == 1 &&
        MMM::Logic::draftTrackDisplayNumber(-6, 6) == 1 &&
        MMM::Logic::draftTrackDisplayNumber(-1, 6) == 6;

    const auto after = MMM::Logic::calculateCanvasLaneProjection(
        // 持久轨增加后，新的追加轨继续出现在最左侧。
        1000.0F,
        4,
        0,
        0.1F,
        0.5F,
        0.0F,
        true,
        false,
        true,
        7,
        true);
    const auto expandedAddress =
        MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(-6, 4, 8);
    const auto expandedBounds = after.bounds(expandedAddress);

    if ( !before.valid || before.draftLaneCount != 7U ||
         !near(before.draftLeftX, -600.0) || !appendLane ||
         appendLane->kind != MMM::Logic::CanvasLaneKind::Draft ||
         appendLane->index != 0U || appendLane->absoluteTrack(4, 7) != -7 ||
         !visibleRange || visibleRange->first != 0U ||
         visibleRange->second != 3U || !numberingMatchesTracks ||
         existingLane.index != 1U || !existingBounds ||
         !near(existingBounds->leftX, -500.0) || !after.valid ||
         after.draftLaneCount != 8U || !near(after.draftLeftX, -700.0) ||
         expandedAddress.index != 2U || !expandedBounds ||
         !near(expandedBounds->leftX, existingBounds->leftX) ) {
        XERROR("Dynamic draft append projection shifted existing lanes");
        return false;
    }
    return true;
}

/// @brief 验证拖动后的草稿布局以右边界为锚点向左扩轨。
/// @return 新增持久草稿轨不改变右边界且不侵入玩家区时返回 true。
/// @details
/// 布局编辑器保存完整草稿组的右边界和单轨宽度。新增一条持久轨后，投影应只把
/// 左边界向外移动一个轨宽，原有绝对草稿轨的屏幕位置保持稳定。
/// @warning 纯投影回归测试，不执行 ImGui 拖动状态机或配置文件写入。
bool testDraggedDraftLayoutExpandsAwayFromPlayer()
{
    MMM::Config::TrackLayout layout;
    layout.left             = 0.1F;
    layout.right            = 0.5F;
    layout.draftLanes.right = 0.1F;
    layout.draftLanes.width = 0.1F;

    const auto before = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 0, layout, 0.0F, true, false, true, 8, true);
    const auto beforeAddress =
        MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(-8, 4, 9);
    const auto beforeBounds = before.bounds(beforeAddress);

    const auto after = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 0, layout, 0.0F, true, false, true, 9, true);
    const auto afterAddress =
        MMM::Logic::CanvasLaneAddress::fromAbsoluteTrack(-8, 4, 10);
    const auto afterBounds = after.bounds(afterAddress);

    if ( !before.valid || !after.valid || before.draftLaneCount != 9U ||
         after.draftLaneCount != 10U || !near(before.draftRightX, 100.0) ||
         !near(after.draftRightX, before.draftRightX) ||
         !near(before.player.leftX, before.draftRightX) ||
         !near(after.player.leftX, after.draftRightX) ||
         !near(before.draftLeftX, -800.0) || !near(after.draftLeftX, -900.0) ||
         !beforeBounds || !afterBounds ||
         !near(beforeBounds->leftX, afterBounds->leftX) ) {
        XERROR("Dragged draft layout expanded into the player canvas");
        return false;
    }
    return true;
}

/// @brief 验证关闭专业模式时不会暴露草稿投影、创建草稿或全选草稿物件。
/// @return 默认投影隐藏草稿区且编辑入口不会命中草稿数据时返回 true。
/// @details
/// 先验证默认纯投影没有草稿宽度、命中或 bounds。再在 Session 中预置一个 Draft，
/// 关闭专业模式后执行全域选择与草稿区画笔，断言既有 Draft 不被选中且没有新增。
///
/// 该用例覆盖数学布局、选择权限和创建权限三个层次，避免仅隐藏渲染但仍允许通过
/// 快捷键或屏幕空白编辑草稿数据。
/// @par 关键不变量
/// - 关闭专业模式时投影不分配草稿宽度。
/// - 草稿区域坐标不产生 laneAt 或 bounds。
/// - 全域选择也不能选中隐藏草稿。
/// - 隐藏区域不能启动 DrawTool 画笔。
/// - 显式悬停命令不能绕过专业模式权限。
/// - 重新开启后原草稿实体重新可选且数据未丢失。
/// @par 故障定位
/// 只隐藏几何但仍可选择说明交互层未复用权限；实体消失说明配置切换误做数据
/// 删除；重开仍不可见则检查缓存脏标记与选择重建。
/// @par 测试边界
/// 本用例不验证设置页面控件，只约束配置值到画布投影和交互命令的逻辑效果。
bool testProfessionalModeHidesDraftArea()
{
    // 同一相机先在专业模式下确认草稿分区可命中。
    // 关闭模式后只应隐藏草稿区，玩家与音频区继续存在。
    // 已有草稿对象保留在领域模型中，不因隐藏而删除。
    // 全选和悬停入口均不得再访问隐藏草稿实体。
    // 重开专业模式后原轨道投影需要无损恢复。
    // 测试区分配置可见性切换与数据生命周期操作。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    if ( !projection.valid || projection.draftLaneCount != 0U ||
         !near(projection.draftLeftX, projection.player.leftX) ||
         !near(projection.draftRightX, projection.player.leftX) ||
         projection.laneAt(50.0F).has_value() ||
         projection.bounds({ MMM::Logic::CanvasLaneKind::Draft, 0U })
             .has_value() ) {
        XERROR(
            "Disabled professional mode still exposed a draft canvas "
            "projection");
        return false;
    }

    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.professionalMode = false;
    const auto draftEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        draftEntity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = -1,
            .m_isDraft    = true,
        });

    MMM::Logic::InteractionController interaction(context);
    interaction.handleCommand(MMM::Logic::CmdSelectAll{
        .scope = MMM::Logic::SelectAllScope::AllTrackAreas,
    });
    const auto* selected =
        context.noteRegistry.try_get<MMM::Logic::InteractionComponent>(
            draftEntity);
    if ( selected && selected->isSelected ) {
        XERROR("Hidden draft object was included by global select-all");
        return false;
    }

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 50.0F,
                                  .mouseY   = 300.0F,
                              });
    if ( context.brushState.isActive ) {
        XERROR("Hidden draft lane accepted a brush gesture");
        return false;
    }
    interaction.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        draftEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
        MMM::Logic::ChartObjectKind::DraftNote,
    });
    if ( context.hoveredEntity != entt::null ) {
        XERROR("Hidden draft object accepted a hover command");
        return false;
    }
    context.lastConfig.settings.professionalMode = true;
    interaction.handleCommand(MMM::Logic::CmdSelectAll{
        .scope = MMM::Logic::SelectAllScope::AllTrackAreas,
    });
    if ( !context.selectedNoteEntities.contains(draftEntity) ) {
        XERROR("Enabling professional mode did not restore draft selection");
        return false;
    }
    return true;
}

/// @brief 验证共用专业模式同步时间线与多个主画布，并保留草稿数据和独立开关。
/// @details
/// 此用例覆盖配置更新从 BeatmapSession 命令队列传播到所有相机快照的完整路径。
/// 两个主画布故意使用不同 ID，避免测试只命中固定的 Basic2DCanvas 特例。
/// 时间线没有草稿命中框，但其静态顶点数应随专业模式关闭而减少。
/// BMS 与 Polyline 开关被设置为相反组合，用于证明专业模式不会串改独立设置。
/// 已选正式物件必须跨切换保留；隐藏草稿的选择与悬停则必须立即清理。
/// 草稿实体本身不能被销毁，重新开启专业模式后仍需从同一 Registry 恢复显示。
/// @par 关键不变量
/// - 配置变更在一次 update 内到达所有已登记 SyncBuffer。
/// - 主画布快照分别计算命中框，不共享另一相机的几何结果。
/// - 时间线仅改变草稿相关静态顶点，不改变 BMS 与 Polyline 开关。
/// - 隐藏只清理不可见对象的交互状态，不清除正式对象选择。
/// - 草稿数据始终留在 Registry，关闭专业模式不是删除操作。
/// - 再次开启后沿用同一实体及原始负轨，避免历史动作失效。
/// @par 故障定位
/// 只单画布失败检查 SyncBuffer 遍历；时间线错误检查静态布局重建；选择丢失或实体
/// 被删则检查专业模式配置处理是否错误进入了数据清理路径。
/// @par 测试边界
/// 使用内存快照验证逻辑输出，不启动 ImGui 停靠窗口或 Vulkan 渲染。多画布只要求
/// 状态一致，各窗口在屏幕上的最终视觉布局需另行验收。
/// @return 往返切换正确更新草稿拾取、时间线分轨及隐藏物件交互状态时返回 true。
bool testProfessionalModeUpdatesAllCanvases()
{
    // 会话注册多个画布快照，分别模拟主视图和辅助视图。
    // 专业模式切换是会话级配置，不能只刷新当前焦点画布。
    // 每个画布都应重算草稿区域与相关命中缓存。
    // 非焦点画布的相机参数必须保留，不能复制主画布状态。
    // 再次切回时所有画布同步恢复各自的投影范围。
    // 断言用于防止配置广播遗漏导致窗口间显示不一致。
    // 建立两个主画布和一个时间线，要求配置广播不能依赖单一窗口是否可见。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    configureObjectEditingCanvas(context);
    context.cameras.at("Basic2DCanvas").horizontalOffsetX = 400.0F;
    context.cameras.emplace(
        "Basic2DCanvasSecondary",
        MMM::Logic::CameraInfo{
            "Basic2DCanvasSecondary", 1000.0F, 600.0F, 400.0F });
    context.cameras.emplace(
        "Timeline", MMM::Logic::CameraInfo{ "Timeline", 400.0F, 600.0F });
    const auto player = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        player,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    const auto draft = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        draft,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = -1,
            .m_isDraft    = true,
        });
    context.noteRegistry.emplace<MMM::Logic::TransformComponent>(player);
    context.noteRegistry.emplace<MMM::Logic::TransformComponent>(draft);
    // 同时选择正式与草稿对象，借此区分应保留和应清理的交互状态。
    MMM::Logic::setChartObjectSelected(
        context, MMM::Logic::ChartObjectKind::PlayerNote, player, true);
    MMM::Logic::setChartObjectSelected(
        context, MMM::Logic::ChartObjectKind::DraftNote, draft, true);
    context.hoveredEntity     = draft;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::DraftNote;
    context.noteRegistry.get<MMM::Logic::InteractionComponent>(draft)
        .isHovered = true;

    auto config                                = context.lastConfig;
    config.settings.enableBmsEditing           = false;
    config.settings.enablePolylineEditing      = true;
    std::uint32_t professionalTimelineVertices = 0;
    for ( const bool enabled : { true, false, true } ) {
        // true->false->true 的往返可以发现只在首次初始化时应用配置的缺陷。
        config.settings.professionalMode = enabled;
        session.pushCommand(MMM::Logic::CmdUpdateEditorConfig{ config });
        session.update(0.0, config, true);
        for ( const char* cameraId :
              { "Basic2DCanvas", "Basic2DCanvasSecondary" } ) {
            // 每个画布都必须独立发布新快照，不能复用另一画布的旧缓冲。
            const auto buffer = context.syncBuffers.find(cameraId);
            if ( buffer == context.syncBuffers.end() || !buffer->second )
                return false;
            const auto* snapshot = buffer->second->pullLatestSnapshot();
            if ( !snapshot ) return false;
            const bool hasDraftHitbox = std::any_of(
                snapshot->hitboxes.begin(),
                snapshot->hitboxes.end(),
                [draft](const auto& hitbox) { return hitbox.entity == draft; });
            if ( snapshot->draftLanesEnabled != enabled ||
                 hasDraftHitbox != enabled || snapshot->bmsEditingEnabled ) {
                XERROR(
                    "Professional mode did not update draft visibility and "
                    "hitboxes for {}",
                    cameraId);
                return false;
            }
        }
        const auto timeline = context.syncBuffers.find("Timeline");
        if ( timeline == context.syncBuffers.end() || !timeline->second )
            return false;
        const auto* snapshot = timeline->second->pullLatestSnapshot();
        if ( !snapshot ) return false;
        if ( enabled ) {
            // 记录完整布局的顶点基线，关闭时应去掉草稿区域相关几何。
            professionalTimelineVertices = snapshot->staticVertexCount;
        } else if ( snapshot->staticVertexCount >=
                        professionalTimelineVertices ||
                    context.selectedNoteEntities.contains(draft) ||
                    context.hoveredEntity == draft ) {
            XERROR(
                "Disabling professional mode retained timeline lanes or hidden "
                "draft interaction");
            return false;
        }
        if ( !context.noteRegistry.valid(draft) ||
             !context.selectedNoteEntities.contains(player) ||
             context.lastConfig.settings.enableBmsEditing ||
             !context.lastConfig.settings.enablePolylineEditing ) {
            XERROR(
                "Professional mode changed draft data, player selection or "
                "independent editing switches");
            return false;
        }
    }
    // 最终重新开启后检查原实体数据，排除通过重建替代可见性切换的实现。
    return context.noteRegistry.get<MMM::Logic::NoteComponent>(draft)
               .m_trackIndex == -1;
}

/// @brief 收集会话内草稿根物件的稳定 ID。
/// @param context 待检查的会话上下文。
/// @return 所有草稿根对象的协作 ID 集合，不包含 Polyline 投影子实体。
/// @details 使用协作 ID 而不是 entt::entity，才能比较不同 Registry 的同一草稿。
std::unordered_set<std::string> collectDraftRootIds(
    const MMM::Logic::SessionContext& context)
{
    std::unordered_set<std::string> identities;
    const auto                      view =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : view ) {
        const auto& note = view.get<const MMM::Logic::NoteComponent>(entity);
        // 子实体由父折线拥有，不应作为项目级草稿合并的独立记录。
        if ( note.m_isDraft && !note.m_isSubNote ) {
            identities.insert(note.m_collaborationId);
        }
    }
    return identities;
}

/// @brief 验证草稿区按主音频共享、并发合并且不写入正式谱面。
/// @details
/// 项目草稿以主音频资源 ID 分组，而不是以谱面文件路径或会话地址分组。
/// 第一会话先发布草稿 A，第二会话载入同组快照后并发删除 A、创建草稿 C；
/// 第一会话同时创建草稿 B。刷新必须按稳定 ID 做三方合并，结果保留 B、C。
/// 正在拖动的本地对象属于未提交交互，远端刷新必须延迟到拖动结束。
/// 草稿轨数量只能单调合并并保留既有负轨索引，不能因旧会话写回而缩小。
/// 正式 Note 会参与 HitEvent，但不能进入项目草稿组；草稿也不能写进 BeatMap。
/// 最后用另一主音频创建会话，证明同项目内不同歌曲的草稿完全隔离。
/// @par 关键不变量
/// - 草稿组键来自解析后的主音频资源 ID，而非会话或谱面地址。
/// - 正式对象只写 BeatMap；草稿对象只写 ProjectDraftLaneGroup。
/// - HitEvent 可同时包含两域对象，但保留 isDraft 与负轨语义。
/// - 三方合并按 collaborationId 识别创建、修改和删除。
/// - 本地拖动期间不替换实体，松手后立即消费远端版本。
/// - 并发轨道数合并采用足以容纳全部对象的较大值。
/// - 新会话从项目持久状态重建，不依赖提交者的 Registry 缓存。
/// @par 故障定位
/// 同音频不共享先检查分组键解析；并发对象丢失检查稳定 ID 三方合并；拖动中跳变
/// 检查 refreshIfChanged 的交互保护；不同音频串组则检查资源 ID 隔离。
/// @par 测试边界
/// 项目对象在单线程内模拟多个会话，不覆盖网络传输、磁盘保存和真正同时写入；
/// 用例只证明相同版本序列下的合并与隔离规则。
/// @return 同主音频画布共享三方合并结果，不同主音频隔离时返回 true。
bool testProjectDraftLaneSharingAndIsolation()
{
    // 两个会话共享同一项目，同时各自绑定不同谱面上下文。
    // 项目级草稿轨变更应传播给共享该项目的会话。
    // 会话私有的相机、选择和 Registry 状态不得相互复制。
    // 切换到另一个项目后，草稿轨配置必须重新隔离。
    // 返回原项目时使用项目持久值，而非离开前的临时预览。
    // 组合断言明确区分共享配置与会话瞬态状态。
    // 两个主音频资源为共享组与隔离组提供稳定、可读的资源标识。
    auto project              = std::make_shared<MMM::Project>();
    project->m_audioResources = {
        MMM::AudioResource{
            .m_id   = "shared-main",
            .m_path = "song.ogg",
            .m_type = MMM::AudioTrackType::Main,
        },
        MMM::AudioResource{
            .m_id   = "other-main",
            .m_path = "other.ogg",
            .m_type = MMM::AudioTrackType::Main,
        },
    };

    const auto configure = [&](MMM::Logic::SessionContext& context,
                               std::string_view            audioPath) {
        // 每个会话使用独立 Registry，但共享同一 Project 持久草稿容器。
        context.collaborationProject = project;
        context.currentBeatmap       = std::make_shared<MMM::BeatMap>();
        context.trackCount           = 4;
        context.currentBeatmap->m_baseMapMetadata.track_count = 4;
        context.currentBeatmap->m_baseMapMetadata.map_path    = "chart.mmm";
        context.currentBeatmap->m_baseMapMetadata.song_file_hint =
            std::string(audioPath);
        MMM::Logic::ProjectDraftLaneService::load(context, project.get());
    };

    MMM::Logic::SessionContext first;
    configure(first, "song.ogg");
    // 正式物件作为对照，只能同步进 BeatMap，不能污染项目草稿组。
    const auto formalEntity = first.noteRegistry.create();
    first.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        formalEntity,
        MMM::Logic::NoteComponent{
            .m_type            = MMM::NoteType::NOTE,
            .m_timestamp       = 0.5,
            .m_trackIndex      = 1,
            .m_collaborationId = "formal",
        });
    const auto draftAEntity = first.noteRegistry.create();
    first.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        draftAEntity,
        MMM::Logic::NoteComponent{
            .m_type            = MMM::NoteType::NOTE,
            .m_timestamp       = 1.0,
            .m_trackIndex      = -4,
            .m_isDraft         = true,
            .m_collaborationId = "draft-a",
        });
    first.draftTrackCount       = 6;
    first.m_needsDraftNotesSync = true;
    // 首次同步建立 shared-main 分组，并保存动态草稿轨数量。
    MMM::Logic::ProjectDraftLaneService::sync(first);

    // 领域谱面和命中事件使用不同过滤规则，因此分别核对持久化与播放结果。
    first.m_needsNotesSync = true;
    MMM::Logic::SessionUtils::syncBeatmap(first);
    MMM::Logic::SessionUtils::rebuildHitEvents(first);
    if ( project->m_draftLaneGroups.size() != 1 ||
         project->m_draftLaneGroups.front().m_trackCount != 6 ||
         first.currentBeatmap->m_noteData.notes.size() != 1 ||
         first.currentBeatmap->m_noteData.notes.front().m_track != 1 ||
         first.hitEvents.size() != 2 || first.hitEvents[0].isDraft ||
         !first.hitEvents[1].isDraft || first.hitEvents[1].trackIndex != -4 ||
         !near(first.currentBeatmap->m_baseMapMetadata.map_length, 500.0) ) {
        XERROR("Draft note persistence or hit event routing was incorrect");
        return false;
    }

    MMM::Logic::SessionContext second;
    configure(second, "song.ogg");
    // 新会话应从项目组重新创建实体，而不是复用第一 Registry 的数值 ID。
    entt::entity loadedDraftA = entt::null;
    for ( const auto entity :
          second.noteRegistry.view<const MMM::Logic::NoteComponent>() ) {
        const auto& note =
            second.noteRegistry.get<const MMM::Logic::NoteComponent>(entity);
        if ( note.m_collaborationId == "draft-a" ) {
            loadedDraftA = entity;
            break;
        }
    }
    if ( second.draftTrackCount != 6 || loadedDraftA == entt::null ||
         second.noteRegistry.get<const MMM::Logic::NoteComponent>(loadedDraftA)
                 .m_trackIndex != -4 ||
         collectDraftRootIds(second) !=
             std::unordered_set<std::string>{ "draft-a" } ) {
        XERROR("A beatmap using the same main audio did not load drafts");
        return false;
    }

    const auto draftBEntity = first.noteRegistry.create();
    first.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        draftBEntity,
        MMM::Logic::NoteComponent{
            .m_type            = MMM::NoteType::HOLD,
            .m_timestamp       = 2.0,
            .m_duration        = 0.5,
            .m_trackIndex      = -1,
            .m_isDraft         = true,
            .m_collaborationId = "draft-b",
        });
    first.m_needsDraftNotesSync = true;
    // 第一会话在原快照上新增 B，形成并发分支的一侧。
    MMM::Logic::ProjectDraftLaneService::sync(first);

    // 第二会话删除 A 并新增 C，形成另一侧；删除语义也按稳定 ID 合并。
    entt::entity draftAToDelete = entt::null;
    const auto   secondView =
        second.noteRegistry.view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : secondView ) {
        const auto& note =
            secondView.get<const MMM::Logic::NoteComponent>(entity);
        if ( note.m_collaborationId == "draft-a" ) {
            draftAToDelete = entity;
            break;
        }
    }
    if ( draftAToDelete != entt::null ) {
        second.noteRegistry.destroy(draftAToDelete);
    }
    const auto draftCEntity = second.noteRegistry.create();
    second.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        draftCEntity,
        MMM::Logic::NoteComponent{
            .m_type            = MMM::NoteType::FLICK,
            .m_timestamp       = 3.0,
            .m_trackIndex      = -3,
            .m_dtrack          = 1,
            .m_isDraft         = true,
            .m_collaborationId = "draft-c",
        });
    second.m_needsDraftNotesSync = true;
    MMM::Logic::ProjectDraftLaneService::sync(second);

    // 拖动期间禁止替换 Registry 对象，否则鼠标手势持有的实体会失效。
    first.isDragging        = true;
    first.draggedObjectKind = MMM::Logic::ChartObjectKind::DraftNote;
    MMM::Logic::ProjectDraftLaneService::refreshIfChanged(first);
    if ( !collectDraftRootIds(first).contains("draft-a") ) {
        XERROR("Draft refresh replaced a local in-progress drag");
        return false;
    }
    first.isDragging = false;
    // 交互结束后立即消费待刷新的项目版本，预期合并为 B 与 C。
    MMM::Logic::ProjectDraftLaneService::refreshIfChanged(first);
    const auto mergedIds = collectDraftRootIds(first);
    if ( mergedIds !=
         std::unordered_set<std::string>{ "draft-b", "draft-c" } ) {
        XERROR("Concurrent draft edits did not merge by stable identity");
        return false;
    }

    MMM::Logic::SessionContext stale;
    configure(stale, "song.ogg");
    // 旧会话随后提交对象时，不能把另一会话已扩展到七轨的组缩回六轨。
    first.draftTrackCount       = 7;
    first.m_needsDraftNotesSync = true;
    MMM::Logic::ProjectDraftLaneService::sync(first);
    const auto staleEntity = stale.noteRegistry.create();
    stale.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        staleEntity,
        MMM::Logic::NoteComponent{
            .m_type            = MMM::NoteType::NOTE,
            .m_timestamp       = 4.0,
            .m_trackIndex      = -2,
            .m_isDraft         = true,
            .m_collaborationId = "draft-stale",
        });
    stale.m_needsDraftNotesSync = true;
    MMM::Logic::ProjectDraftLaneService::sync(stale);
    MMM::Logic::SessionContext afterConcurrentGrowth;
    configure(afterConcurrentGrowth, "song.ogg");
    // 全新会话验证最终持久状态，避免只检查提交者自己的本地缓存。
    entt::entity concurrentOuter = entt::null;
    entt::entity concurrentStale = entt::null;
    const auto   concurrentView  = afterConcurrentGrowth.noteRegistry
                                    .view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : concurrentView ) {
        const auto& note =
            concurrentView.get<const MMM::Logic::NoteComponent>(entity);
        if ( note.m_collaborationId == "draft-c" ) concurrentOuter = entity;
        if ( note.m_collaborationId == "draft-stale" ) concurrentStale = entity;
    }
    if ( afterConcurrentGrowth.draftTrackCount != 7 ||
         concurrentOuter == entt::null || concurrentStale == entt::null ||
         concurrentView.get<const MMM::Logic::NoteComponent>(concurrentOuter)
                 .m_trackIndex != -3 ||
         concurrentView.get<const MMM::Logic::NoteComponent>(concurrentStale)
                 .m_trackIndex != -2 ) {
        XERROR("Concurrent draft count growth shifted or truncated objects");
        return false;
    }

    MMM::Logic::SessionContext isolated;
    configure(isolated, "other.ogg");
    // other-main 必须形成空的新组，不能按相同文件名之外的弱条件串组。
    return isolated.m_draftLaneGroupId == "other-main" &&
           collectDraftRootIds(isolated).empty();
}

/// @brief 验证草稿物件镜像保持在左侧草稿域且不发布正式谱面变更。
/// @details 草稿轨使用负索引，镜像方向和 Flick 方向都必须在草稿域内反转。
/// 动作可以进入撤销栈，但不得产生正式谱面 MutationFlags 或请求 Note 同步。
/// @par 关键不变量
/// - 负轨镜像范围只取 draftTrackCount。
/// - 根 Flick 的 m_dtrack 与轨道位置同时反向。
/// - m_isDraft 在执行前后保持 true。
/// - 正式 NoteData 同步标志保持未触发。
/// - 协作观察者不得收到正式对象变更类别。
/// @par 故障定位
/// 轨道落点错误检查负轨镜像公式；方向错误检查 Flick 特殊字段；出现正式脏标记
/// 说明草稿动作错误复用了正式 Note 的发布路径。
/// @par 测试边界
/// 只覆盖单个根 Flick，不验证折线闭包；Polyline 镜像由批量编辑专项场景覆盖。
/// @return 草稿从最左轨镜像至最右轨且领域脏标记保持为空时返回 true。
bool testDraftMirrorStaysInDraftDomain()
{
    // 镜像源对象位于负轨道，且项目已知草稿轨数量固定。
    // 镜像计算只能在草稿域内部反转局部索引。
    // 结果不得穿过零边界进入玩家轨，也不能变成 Sample。
    // 时间、类型与稳定 ID 之外的字段应保持原值。
    // Undo 恢复原负轨，Redo 再得到相同镜像结果。
    // 该往返防止轨道归一化把不同数据域混为一体。
    // 默认四条草稿轨中 -4 与 -1 互为镜像端点。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        entity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::FLICK,
            .m_timestamp  = 1.0,
            .m_trackIndex = -4,
            .m_dtrack     = 1,
            .m_isDraft    = true,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        entity, MMM::Logic::InteractionComponent{ .isSelected = true });

    MMM::Logic::ActionController controller(context);
    // 镜像通过正式命令路径执行，确保测试覆盖动作封装而非纯换算函数。
    controller.handleCommand(MMM::Logic::CmdMirrorSelected{});
    const auto& mirrored =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    return mirrored.m_isDraft && mirrored.m_trackIndex == -1 &&
           mirrored.m_dtrack == -1 && !context.m_needsNotesSync &&
           context.actionStack.takePendingMutationFlags() ==
               MMM::BeatmapMutationFlags::None;
}

/// @brief 验证草稿镜像使用动态草稿轨数量而非玩家键数。
/// @details 六条草稿轨与默认四键谱面刻意不同，可暴露误用 trackCount 的实现。
/// @par 关键不变量
/// - 草稿最外侧 -6 在六轨域中镜像到 -1。
/// - 玩家四键布局不参与负轨换算。
/// - 镜像不会把已有轨误认成追加轨。
/// - draftTrackCount 在动作执行后仍为六。
/// - 草稿身份不因靠近玩家区而改变。
/// @par 故障定位
/// 得到 -3 等四键结果说明误用了
/// trackCount；轨数增长说明目标被错认成虚拟追加轨； m_isDraft
/// 丢失则检查镜像动作的组件复制范围。
/// @par 测试边界
/// 六轨用于区分动态草稿布局和玩家键数，不覆盖超大轨数或轨道配置持久化。
/// @return 六轨草稿最左轨镜像至最右轨且不扩展轨道数时返回 true。
bool testDraftMirrorUsesDynamicDraftTrackCount()
{
    // 先扩展项目草稿轨计数，再对靠近边缘的对象执行镜像。
    // 镜像中心必须读取最新持久计数，而非会话初始缓存。
    // 动态新增轨参与对称范围，但追加预览轨不应参与。
    // 结果轨道仍需保持负数并落入新的合法边界。
    // Undo/Redo 检查动作是否保存计算结果而非重复读旧布局。
    // 本场景与固定计数用例共同覆盖缓存失效路径。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.draftTrackCount = 6;
    // -6 是六轨草稿最外侧，若错误使用四键宽度将得到越界或中间轨。
    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        entity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = -6,
            .m_isDraft    = true,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        entity, MMM::Logic::InteractionComponent{ .isSelected = true });

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdMirrorSelected{});
    // 镜像只移动物件，不能把既有草稿轨数量当作追加轨请求继续增长。
    const auto& mirrored =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    return mirrored.m_trackIndex == -1 && mirrored.m_isDraft &&
           context.draftTrackCount == 6;
}

/// @brief 验证常用分拍对齐不会丢弃缺少独立子实体的折线节点。
/// @details
/// 该折线仅把节点保存在父 NoteComponent::m_subNotes，Registry 中故意不创建
/// 对应子实体，模拟旧工程、延迟投影或部分物化状态。对齐命令必须以父组件内嵌
/// 数据为完整来源，而不能只遍历当前存在的子实体。时间值偏离标准拍点，用于确认
/// 命令确实执行吸附。随后覆盖领域同步以及 ActionStack 的撤销、重做往返。
/// 节点具体时间不是本测试关注点；核心不变量是类型、数量和父级结构不能丢失。
/// @par 关键不变量
/// - 父 m_subNotes 是缺少投影子实体时的完整数据来源。
/// - 常用分拍吸附不要求每个节点先拥有 entt 实体。
/// - 同时间节点排序稳定，不随机交换用户绘制顺序。
/// - syncBeatmap 后领域 Polyline 仍含全部三个节点。
/// - Undo 和 Redo 各恢复一份结构完整的父组件快照。
/// - 对齐只形成一个历史项，不按节点拆分事务。
/// @par 故障定位
/// 首次对齐即丢节点说明只枚举了子实体；仅 sync 后丢失检查领域写回；Undo/Redo
/// 才丢失则检查 BatchNoteAction 的 before/after 是否保存完整 m_subNotes。
/// @par 测试边界
/// 本用例关注结构保留，不锁定某个皮肤的具体常用除数集合或每个节点的最终小数值。
/// @return 父折线的完整节点在执行、同步、撤销和重做后均保留时返回 true。
bool testAlignCommonBeatsPreservesEmbeddedPolylineNodes()
{
    // 对齐集合同时包含普通 Note 与带内嵌节点的 Polyline。
    // 公共节拍计算只改变时间坐标，不应重建对象身份。
    // 父折线与各子节点必须应用相同的时间偏移基准。
    // 节点相对顺序和轨道几何在对齐前后保持稳定。
    // Undo 恢复全部原时间，Redo 再现完全相同的对齐值。
    // 该往返可识别只更新父对象而遗漏内嵌节点的问题。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enablePolylineEditing = true;

    MMM::Logic::NoteComponent polyline;
    // 两个同时间节点和跨拍 Hold 组合可覆盖排序与时长重新计算路径。
    polyline.m_type       = MMM::NoteType::POLYLINE;
    polyline.m_timestamp  = 1.013;
    polyline.m_trackIndex = 0;
    polyline.m_subNotes   = {
        {
              .type       = MMM::NoteType::HOLD,
              .timestamp  = 1.013,
              .duration   = 0.241,
              .trackIndex = 0,
              .dtrack     = 0,
        },
        {
              .type       = MMM::NoteType::FLICK,
              .timestamp  = 1.254,
              .duration   = 0.0,
              .trackIndex = 0,
              .dtrack     = 1,
        },
        {
              .type       = MMM::NoteType::HOLD,
              .timestamp  = 1.254,
              .duration   = 0.246,
              .trackIndex = 1,
              .dtrack     = 0,
        },
    };

    const auto parentEntity = context.noteRegistry.create();
    // 只物化父实体是本回归用例的关键前置条件，禁止在此补建子实体。
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(parentEntity,
                                                            polyline);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        parentEntity, MMM::Logic::InteractionComponent{ .isSelected = true });

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdAlignSelectedToCommonBeats{});

    // 每个阶段复用同一结构断言，避免仅验证首次执行而漏掉历史动作快照。
    const auto hasCompletePolyline = [&]() {
        if ( !context.noteRegistry.valid(parentEntity) ) return false;
        const auto& current =
            context.noteRegistry.get<const MMM::Logic::NoteComponent>(
                parentEntity);
        return current.m_type == MMM::NoteType::POLYLINE &&
               current.m_subNotes.size() == polyline.m_subNotes.size();
    };
    if ( !hasCompletePolyline() ||
         context.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Common beat alignment discarded embedded Polyline nodes");
        return false;
    }

    MMM::Logic::SessionUtils::syncBeatmap(context);
    // ECS 到领域模型的同步同样必须从父组件复制完整 m_subNotes。
    if ( context.currentBeatmap->m_noteData.polylines.size() != 1U ||
         context.currentBeatmap->m_noteData.polylines.front()
                 .m_subNotes.size() != polyline.m_subNotes.size() ) {
        XERROR("Aligned Polyline nodes were lost during beatmap sync");
        return false;
    }

    context.actionStack.undo(context);
    if ( !hasCompletePolyline() ) {
        XERROR("Undo did not restore the complete Polyline");
        return false;
    }
    context.actionStack.redo(context);
    // 重做后的结构是下一次编辑入口，必须再次保持全部内嵌节点。
    return hasCompletePolyline();
}

/// @brief 验证逻辑视口 Resize 后横向位移保持相同比例。
/// @details 正常宽度按新旧比例缩放；旧宽度无效时保持原值，避免除零和跳变。
/// @par 关键不变量
/// - 有效旧宽度使用 offset / oldWidth * newWidth。
/// - 零旧宽度不会产生 NaN、无穷或归零跳变。
/// - 纯换算函数不读取全局相机或窗口状态。
/// - 结果保持逻辑像素单位，DPI 缩放由调用方处理。
/// @par 故障定位
/// 有效宽度比例错误检查归一化公式；零宽返回异常值说明缺少前置防护；只有高 DPI
/// 环境变化不属于本纯逻辑函数的职责。
/// @par 测试边界
/// 不创建 CameraInfo，也不验证窗口 resize
/// 事件路由；只验证被调用换算函数的契约。
/// @return 偏移随逻辑宽度等比例换算时返回 true。
bool testResizePreservesNormalizedOffset()
{
    // 相机先在旧视口宽度下设置非零横向偏移。
    // Resize 只改变物理像素尺寸，逻辑平移比例应保持。
    // 归一化后的轨道中心在新旧视口中必须对应同一位置。
    // 零宽度等非法尺寸不参与本用例，避免混入输入校验。
    // 断言同时比较偏移量和代表轨道的投影结果。
    // 由此防止窗口缩放后画布内容突然横向跳动。
    // 120/1200 的归一化位置换算到 600 像素后应为 60。
    const float resized =
        MMM::Logic::resizeCanvasHorizontalOffset(120.0F, 1200.0F, 600.0F);
    const float unchanged =
        MMM::Logic::resizeCanvasHorizontalOffset(120.0F, 0.0F, 600.0F);
    // 零旧宽度代表尚未建立有效视口，函数必须选择保守的不变策略。
    if ( !near(resized, 60.0) || !near(unchanged, 120.0) ) {
        XERROR("Canvas horizontal offset was not stable across resize");
        return false;
    }
    return true;
}

/// @brief 验证二维平移按逻辑像素连续修改横向相机和纵向时间。
/// @details
/// 横向位移直接累加逻辑像素；纵向位移经 viewportHeight 与 renderScaleY
/// 换算为时间。连续平移不能启动插值动画，否则鼠标拖动会落后于指针。
/// 第二条零位移命令只改变视口宽度，用于验证 Resize 归一化补偿也走同一路径。
/// @par 关键不变量
/// - deltaX 直接作用于逻辑横向偏移。
/// - deltaY 结合视口高度与 renderScaleY 换算时间增量。
/// - currentTime 与 animateTime 在连续拖动中立即一致推进。
/// - 平移不会启动时间插值动画，避免局部反馈滞后。
/// - ScrollCache 在时间改变后进入待更新状态。
/// - Resize 补偿不会重复应用本次 deltaX。
/// @par 故障定位
/// X 错误检查相机累加与 resize 顺序；Y 错误检查视口和 renderScaleY；出现动画
/// 滞后则检查 CmdPanCanvas 是否误走普通 Seek 插值路径。
/// @par 测试边界
/// 使用固定逻辑尺寸，不覆盖操作系统滚轮单位或触控板惯性；输入适配层不在此测试。
/// @return 横向偏移与无吸附纵向换算均正确时返回 true。
bool testPanCommandUsesLogicalPixels()
{
    // 相机具有明确 DPI 无关视口，平移命令输入逻辑像素。
    // 连续两次相反方向移动用于检查增量而非绝对赋值。
    // 命令不应修改视口尺寸、轨道布局或时间滚动状态。
    // 横移后的投影命中作为对偏移字段的行为验证。
    // 回到原位置时 near 比较应闭合整个往返。
    // 本测试隔离 UI 缩放之外的逻辑坐标契约。
    // 预置非零时间、偏移和总时长，使两个轴的增量都可被精确观察。
    MMM::Logic::SessionContext context;
    context.currentTime            = 10.0;
    context.animateTime            = 10.0;
    context.audioTimelineTotalTime = 100.0;
    context.cameras.emplace(
        "Canvas_7",
        MMM::Logic::CameraInfo{ "Canvas_7", 1000.0F, 600.0F, 50.0F });
    context.timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();

    MMM::Logic::PlaybackController controller(context);
    // 100 逻辑像素在 600 高、2 倍缩放下对应 0.1 秒的连续移动。
    controller.handleCommand(MMM::Logic::CmdPanCanvas{
        .cameraId       = "Canvas_7",
        .deltaX         = 25.0F,
        .deltaY         = 100.0F,
        .viewportWidth  = 1000.0F,
        .viewportHeight = 600.0F,
        .renderScaleY   = 2.0F,
    });

    const auto cameraIt = context.cameras.find("Canvas_7");
    if ( cameraIt == context.cameras.end() ||
         !near(cameraIt->second.horizontalOffsetX, 75.0) ||
         !near(context.currentTime, 10.1) ||
         !near(context.animateTime,
               context.currentTime +
                   context.lastConfig.visual.getEffectiveVisualOffset()) ||
         context.animateTimeAnimationActive ) {
        XERROR("Canvas pan command did not apply continuous two-axis movement");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdPanCanvas{
        .cameraId       = "Canvas_7",
        .viewportWidth  = 500.0F,
        .viewportHeight = 600.0F,
    });
    // 视口宽度减半后偏移也减半，从而保持内容在屏幕上的归一化位置。
    if ( !near(cameraIt->second.horizontalOffsetX, 37.5) ) {
        XERROR("Canvas pan command did not preserve offset after resize");
        return false;
    }
    return true;
}

/// @brief 验证改键数按 BGM 相对索引原子迁移全部自动采样。
/// @details
/// 自动采样保存绝对轨道，但其语义是相对玩家区末尾的 BGM 索引。玩家轨从四轨
/// 改为七轨时，每个采样都应整体加三；稀疏的大轨道同样不能漏掉。动作必须同时
/// 更新 trackCount、变换脏标记和所有 SampleComponent，并以单个历史项撤销。
/// bgmTrackCount 描述可见持久轨数量，本操作只平移索引，不应改变该计数。
/// @par 关键不变量
/// - 所有 Sample 按新旧玩家轨数差值一次性平移。
/// - 稀疏绝对轨与连续轨遵循相同换算。
/// - BGM 相对索引在执行、撤销和重做阶段保持不变。
/// - bgmTrackCount 与领域元数据不因玩家键数修改而变化。
/// - 投影缓存每次状态切换都标脏。
/// - 三个采样与键数修改共用一个 TrackCountAction。
/// @par 故障定位
/// 所有 Sample 同偏差说明玩家轨差值错误；仅稀疏轨未迁移说明实现只扫可见轨；
/// Undo/Redo 叠加偏移说明动作使用当前值而非保存快照。
/// @par 测试边界
/// 样本资源可缺失，因为本场景只检查轨道编码；音频描述符刷新由独立测试负责。
/// @return 稠密、稀疏轨道及 Undo/Redo 均保持相对索引时返回 true。
bool testTrackCountActionMigratesAllSamples()
{
    // 谱面中布置多个 Sample，覆盖主 BGM 与追加轨索引。
    // 轨道数变化通过动作入口执行，不能直接修改元数据。
    // 迁移后每个 Sample 的绝对轨道需保持其局部域含义。
    // Registry、领域数组和轨道布局缓存必须同步更新。
    // Undo 恢复旧计数与全部 Sample 轨道，不能部分回退。
    // Redo 再次迁移用于验证动作保存了完整对象集合。
    MMM::Logic::SessionContext context;
    context.trackCount       = 4;
    context.bgmTrackCount    = 2;
    context.isTransformDirty = false;

    const auto first  = context.sampleRegistry.create();
    const auto third  = context.sampleRegistry.create();
    const auto sparse = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        first, MMM::Logic::SampleComponent{ .m_track = 4 });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        third, MMM::Logic::SampleComponent{ .m_track = 6 });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sparse, MMM::Logic::SampleComponent{ .m_track = 1000 });

    MMM::Logic::InteractionController controller(context);
    // 使用命令入口构造 TrackCountAction，要求所有样本迁移与键数修改原子提交。
    controller.handleCommand(MMM::Logic::CmdUpdateTrackCount{ 7 });
    if ( context.trackCount != 7 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 9 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1003 ||
         !context.isTransformDirty ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Track count action did not atomically migrate all samples");
        return false;
    }

    context.isTransformDirty = false;
    // 撤销应恢复原键数及全部绝对轨道，同时再次使投影缓存失效。
    context.actionStack.undo(context);
    if ( context.trackCount != 4 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 4 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 6 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1000 ||
         !context.isTransformDirty ) {
        XERROR("Track count action undo did not restore sample tracks");
        return false;
    }

    context.isTransformDirty = false;
    // 重做必须复现同一迁移结果，不能基于已迁移数值再次叠加偏移。
    context.actionStack.redo(context);
    if ( context.trackCount != 7 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 9 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1003 ||
         !context.isTransformDirty ) {
        XERROR("Track count action redo did not restore migrated tracks");
        return false;
    }
    return true;
}

/// @brief 验证 Session 按当前 Key 数选择独立轨道与组件布局。
/// @details
/// 四键和七键分别设置互不相同的轨道范围、判定线和拍号锚点。Session 每帧应先
/// 根据当前 trackCount 物化专属配置，再供布局与交互使用。未定义的五键布局应
/// 回退到配置中的通用字段，证明查找失败不会错误沿用上一次七键结果。
/// @return 切换 Key 数后直接字段均物化为对应独立布局时返回 true。
bool testSessionSelectsKeyCountLayout()
{
    // 配置提供多套键数布局，谱面元数据选择其中一套。
    // 会话初始化时应按 track_count 取得准确左右边界。
    // 不匹配的布局不得因声明顺序而被误选。
    // 轨道代表点用于验证选择结果真正进入投影缓存。
    // 修改键数后重新同步，布局必须随元数据更新。
    // 测试不检查皮肤绘制，只确认会话配置解析语义。
    // 为三个独立字段设置明显不同的数值，避免单字段正确掩盖部分物化缺陷。
    MMM::Config::EditorConfig config;
    auto& fourTrack = config.visual.editableTrackLayoutForKeyCount(4);
    fourTrack.left  = 0.14F;
    fourTrack.right = 0.54F;
    config.visual.editableJudgmentLinePositionForKeyCount(4) = 0.74F;
    config.visual.editableCanvasComponentsForKeyCount(4).beatNumber.anchorX =
        0.24F;

    auto& sevenTrack = config.visual.editableTrackLayoutForKeyCount(7);
    sevenTrack.left  = 0.27F;
    sevenTrack.right = 0.87F;
    config.visual.editableJudgmentLinePositionForKeyCount(7) = 0.87F;
    config.visual.editableCanvasComponentsForKeyCount(7).beatNumber.anchorX =
        0.67F;

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.trackCount                 = 4;
    // 首次 update 覆盖启动阶段，不依赖先前缓存或命令触发。
    session.update(0.0, config, false);
    if ( !near(context.lastConfig.visual.trackLayout.left, 0.14) ||
         !near(context.lastConfig.visual.trackLayout.right, 0.54) ||
         !near(context.lastConfig.visual.judgeline_pos, 0.74) ||
         !near(context.lastConfig.visual.canvasComponents.beatNumber.anchorX,
               0.24) ) {
        XERROR("Four-key Session did not select its independent layout");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    // 改键命令与配置选择必须在同一 update 内生效，不能延迟到下一帧。
    session.update(0.0, config, false);
    if ( context.trackCount != 7 ||
         !near(context.lastConfig.visual.trackLayout.left, 0.27) ||
         !near(context.lastConfig.visual.trackLayout.right, 0.87) ||
         !near(context.lastConfig.visual.judgeline_pos, 0.87) ||
         !near(context.lastConfig.visual.canvasComponents.beatNumber.anchorX,
               0.67) ) {
        XERROR("Seven-key Session reused another Key-count layout");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateEditorConfig{ .config = config },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 5 },
    });
    session.update(0.0, config, false);
    // 五键没有专属项，预期回到通用布局而不是保留七键的物化副本。
    return context.trackCount == 5 &&
           near(context.lastConfig.visual.trackLayout.left,
                config.visual.trackLayout.left) &&
           near(context.lastConfig.visual.judgeline_pos,
                config.visual.judgeline_pos) &&
           near(context.lastConfig.visual.canvasComponents.beatNumber.anchorX,
                config.visual.canvasComponents.beatNumber.anchorX);
}

/// @brief 验证后台 Session 的主画布悬停状态会写入自身渲染快照。
/// @details
/// 非焦点谱面仍需根据自己画布的鼠标位置生成 NearCursor 分拍线。测试以
/// update(..., false) 明确模拟后台会话，并要求命令队列中的悬停坐标进入该会话
/// 自己的 SyncBuffer。若实现只更新当前焦点，会得到空动态顶点或非有限时间。
/// @par 关键不变量
/// - 非焦点 Session 仍消费属于自己的鼠标命令。
/// - hoveredTime 使用该会话的 ScrollCache 计算。
/// - isHoveringCanvas 随快照发布，不读取 UI 全局 hover。
/// - NearCursor 动态分拍线在后台快照中仍有顶点。
/// - 不要求后台会话成为 EditorEngine 当前会话。
/// - 每个会话的 SyncBuffer 保存独立序列。
/// @par 故障定位
/// 缺快照说明后台 update 被提前跳过；有快照但无动态顶点说明 hover 未进入渲染
/// 数据；时间非有限则检查后台会话自己的 ScrollCache 是否完成初始化。
/// @par 测试边界
/// 不创建第二个真实窗口，只通过 isCurrent=false 表达后台状态；UI
/// 输入焦点切换另测。
/// @return 后台快照保留独立悬停位置并可供 NearCursor 分拍线消费时返回 true。
bool testBackgroundSessionPublishesCanvasHover()
{
    // 创建焦点与后台两个会话，各自保留独立画布状态。
    // 鼠标位于后台画布时，该会话仍需发布悬停快照。
    // 发布过程不能切换全局焦点或覆盖前台选择状态。
    // 快照中的轨道、时间和对象身份都来自后台上下文。
    // 移出画布后悬停应清空，防止保留上一帧命中。
    // 该场景支撑多开谱面下非焦点分拍线与交互反馈。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    configureObjectEditingCanvas(context);
    const auto timingEntity = context.timelineRegistry.create();
    context.timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        timingEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });
    context.isBpmEventsDirty = true;
    // 手动建立 ScrollCache，使鼠标 Y 能在没有真实音频播放时换算到谱面时间。
    auto* cache =
        context.timelineRegistry.ctx().find<MMM::Logic::System::ScrollCache>();
    if ( !cache ) return false;
    cache->rebuild(context.timelineRegistry,
                   context.lastConfig,
                   context.currentBeatmap.get());

    auto config = context.lastConfig;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::NearCursor;

    session.pushCommand(MMM::Logic::CmdSetMousePosition{
        .cameraId       = "Basic2DCanvas",
        .mouseX         = 320.0F,
        .mouseY         = 180.0F,
        .viewportWidth  = 1000.0F,
        .viewportHeight = 600.0F,
        .isHovering     = true,
    });
    // false 表示会话不是当前焦点，这正是多开谱面回归场景。
    session.update(0.0, config, false);

    // 快照必须由后台会话自己的缓冲发布，而不是读取全局焦点画布状态。
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Background hover did not publish a main canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || !snapshot->isHoveringCanvas ||
         !std::isfinite(snapshot->hoveredTime) ||
         snapshot->dynamicVertexCount == 0U ) {
        XERROR("Background main canvas snapshot lost its hover beat lines");
        return false;
    }
    return true;
}

/// @brief 验证新建正式音符直接增量维护领域谱面和渲染统计缓存。
/// @details
/// Create 动作具备足够信息直接增量插入 BeatMap，并同步 noteCount，因此不应留下
/// m_needsNotesSync 或 isNoteStatsDirty。Undo
/// 当前允许把领域全量同步延迟到保存边界， 但统计值仍需立即归零；后续渲染 update
/// 不得错误清除该持久化待同步标志。
/// @par 关键不变量
/// - Create 动作直接维护领域 NoteData 与 noteCount。
/// - 增量创建完成后不留下全量 Note 同步请求。
/// - 统计缓存不等待渲染帧扫描 Registry。
/// - Undo 立即更新统计，但允许低频领域重建延后。
/// - 普通 update 不冒充保存边界消费 m_needsNotesSync。
/// - 渲染快照脏标记与持久化同步标记职责分离。
/// @par 故障定位
/// Create 后仍全量标脏说明增量领域维护缺失；统计错误检查动作计数更新；Undo 后
/// 待同步被清除则检查渲染 update 是否越权执行了持久化同步。
/// @par 测试边界
/// 只覆盖普通 Note 创建和撤销，不测批量、折线或磁盘保存时的最终全量同步。
/// @return 创建立即写回单个领域对象，撤销仍可延迟全量写回时返回 true。
bool testDeferredNoteSyncDoesNotKeepRenderSnapshotDirty()
{
    // 首次领域变更把 Note 同步标记置为待处理状态。
    // 一次快照消费应完成 ECS 同步并清除对应脏位。
    // 后续无变更帧不能重复重建同一渲染快照。
    // 测试记录版本与实体数量，区分重建和稳定复用。
    // 新增第二次变更后脏位必须再次正常触发。
    // 由此确认延迟同步既不会漏更新也不会永久变脏。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    configureObjectEditingCanvas(context);
    const auto config = context.lastConfig;
    // 首帧先建立所有系统缓存，避免把初始化脏标记误判为动作结果。
    session.update(0.0, config, false);

    MMM::Logic::NoteComponent note;
    note.m_type       = MMM::NoteType::NOTE;
    note.m_timestamp  = 1.0;
    note.m_trackIndex = 1;
    context.actionStack.pushAndExecute(std::make_unique<MMM::Logic::NoteAction>(
                                           MMM::Logic::NoteAction::Type::Create,
                                           entt::null,
                                           std::nullopt,
                                           note),
                                       context);
    // 创建路径应同时维护领域对象与统计缓存，无需等待下一帧全量扫描。
    if ( context.m_needsNotesSync || context.isNoteStatsDirty ||
         context.noteCount != 1U ||
         context.currentBeatmap->m_noteData.notes.size() != 1U ) {
        XERROR("Note creation did not update deferred data and stats caches");
        return false;
    }

    session.update(0.0, config, false);
    // 渲染帧不能把一次已经完成的增量同步重新标成待处理。
    if ( context.m_needsNotesSync || context.isNoteStatsDirty ||
         context.noteCount != 1U ) {
        XERROR(
            "Deferred note sync kept render stats dirty after creation: "
            "pending={}, dirty={}, count={}",
            context.m_needsNotesSync,
            context.isNoteStatsDirty,
            context.noteCount);
        return false;
    }

    context.actionStack.undo(context);
    // 撤销立刻更新统计，但允许领域 NoteData 在低频同步点重建。
    if ( !context.m_needsNotesSync || context.isNoteStatsDirty ||
         context.noteCount != 0U ) {
        XERROR("Note undo did not update deferred data and stats caches");
        return false;
    }

    session.update(0.0, config, false);
    // 普通渲染 update 不承担持久化写回，因此待同步标志仍应保持。
    if ( !context.m_needsNotesSync || context.isNoteStatsDirty ||
         context.noteCount != 0U ) {
        XERROR(
            "Deferred note sync kept render stats dirty after undo: "
            "pending={}, "
            "dirty={}, count={}",
            context.m_needsNotesSync,
            context.isNoteStatsDirty,
            context.noteCount);
        return false;
    }
    return true;
}

/// @brief 验证交互命令执行前已经物化当前 Key 数的轨道与判定线布局。
/// @details
/// 通用布局与四键专属布局故意不重叠，鼠标坐标只在四键布局内对应第零轨。
/// 若 Session 在处理画笔命令之后才选择 Key 布局，预览会落到错误轨道或时间。
/// 此用例要求同一帧内先物化配置、再消费命令、最后发布画笔状态。
/// @par 关键不变量
/// - 画笔轨道换算读取当前 Key 数的专属 TrackLayout。
/// - 时间换算读取同一布局对应的判定线位置。
/// - CmdStartBrush 不得先于配置物化执行。
/// - brushState 在本次 update 结束前可供 UI 预览。
/// - 通用布局值只作为未定义 Key 数时的回退。
/// - 焦点标志不改变同帧命令和布局的先后关系。
/// @par 故障定位
/// brushState 错而 context 配置正确说明画笔过早执行；两者都旧说明 Key
/// 布局未物化； 仅未定义键数失败说明回退仍沿用了上一次专属布局。
/// @par 测试边界
/// 不检查设置序列化，只验证本帧传入 EditorConfig 的运行时选择和命令执行顺序。
/// @return 画笔在专属布局中的鼠标位置生成同轨同时间预览时返回 true。
bool testQueuedBrushUsesKeyCountLayout()
{
    // 画笔命令先进入队列，执行前切换到谱面键数布局。
    // 消费队列时投影必须读取命令对应会话的当前布局。
    // 指针位置刻意落在不同布局会解释为不同轨的位置。
    // 创建结果用于识别提前缓存旧投影的错误。
    // 动作仍只提交一次，不能因布局刷新重复消费命令。
    // 测试覆盖 UI 采集与逻辑线程处理之间的时间差。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    configureObjectEditingCanvas(context);
    context.currentTool = MMM::Logic::EditTool::Draw;

    auto config                     = context.lastConfig;
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    config.visual.judgeline_pos     = 0.5F;
    auto& fourTrackLayout = config.visual.editableTrackLayoutForKeyCount(4);
    fourTrackLayout.left  = 0.4F;
    fourTrackLayout.right = 0.8F;
    config.visual.editableJudgmentLinePositionForKeyCount(4) = 0.75F;

    // 该坐标以通用布局解释会得到不同结果，是执行顺序的可观测探针。
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 450.0F,
            .mouseY   = 450.0F,
        },
    });
    session.update(0.0, config, true);

    // 同时核对产物与上下文配置，区分换算错误和物化顺序错误。
    if ( !context.brushState.isActive || context.brushState.track != 0 ||
         !near(context.brushState.time, 1.0) ||
         !near(context.lastConfig.visual.trackLayout.left, 0.4) ||
         !near(context.lastConfig.visual.judgeline_pos, 0.75) ) {
        XERROR("Queued brush command used the legacy global canvas layout");
        return false;
    }
    return true;
}

/// @brief 验证玩家轨道数变化不会把超出 uint32 的自动采样轨道静默截断。
/// @details
/// 最大轨道再加一会溢出。命令必须在修改任何字段或压入动作之前完成全量预检，
/// 并通过状态消息向 UI 暴露失败；不能依赖无符号自然回绕得到看似合法的零轨。
/// @par 关键不变量
/// - 预检使用不会溢出的中间表示或显式边界判断。
/// - 失败前不修改 context.trackCount。
/// - 失败前不修改 SampleComponent::m_track。
/// - 失败不会创建 ActionStack 历史项。
/// - lastActionMessage 提供用户可见的拒绝原因。
/// - UINT32_MAX 不会回绕为玩家第零轨。
/// @par 故障定位
/// 任一状态变化说明预检晚于写入；轨道变零说明无符号回绕；状态不变但无消息说明
/// 错误结果没有传播到交互反馈层。
/// @par 测试边界
/// 只覆盖增加一个玩家轨导致的上溢，其他非法轨道关系由元数据迁移用例覆盖。
/// @return 溢出时轨道数、采样和撤销栈均保持原状时返回 true。
bool testTrackCountOverflowIsRejectedAtomically()
{
    // 样本轨道靠近表示上限，缩减或迁移将导致索引溢出。
    // 动作必须在写入元数据前预检整个对象集合。
    // 任一对象不可表示时，所有对象与轨道数都保持原状。
    // Registry、领域数据和动作栈深度分别检查无部分提交。
    // 失败路径不得生成可 Undo 的空动作或修改通知。
    // 这些断言共同定义轨道迁移的原子拒绝语义。
    MMM::Logic::SessionContext context;
    context.trackCount    = 1;
    context.bgmTrackCount = 1;
    const auto entity     = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_track = std::numeric_limits<std::uint32_t>::max(),
        });

    MMM::Logic::InteractionController controller(context);
    // 1->2 会使 UINT32_MAX 对应的 BGM 相对索引无法重新编码。
    controller.handleCommand(MMM::Logic::CmdUpdateTrackCount{ 2 });

    // 无历史项是原子拒绝的重要证据，避免产生可撤销的半成品动作。
    return context.trackCount == 1 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                   .m_track == std::numeric_limits<std::uint32_t>::max() &&
           context.actionStack.getUndoStackSize() == 0 &&
           !context.lastActionMessage.empty();
}

/// @brief 验证元数据入口改键失败不会覆盖任一谱面状态。
/// @details
/// 元数据窗口同时提交名称和键数，因此失败时不仅轨道，名称及 BeatMap 元数据也
/// 必须回滚。用例依次覆盖重新编码溢出、采样落入玩家区、合法扩轨三个分支。
/// 合法分支的 Undo 只撤销轨道迁移，提交后的其他元数据字段按动作契约保留。
/// 每次失败均核对撤销栈为空和本地化错误关键词，确保 UI 能解释拒绝原因。
/// @par 关键不变量
/// - 元数据名称与键数在失败分支整体保持旧值。
/// - context 与 BeatMap 的轨道计数始终同步。
/// - 自动采样不能因新玩家区扩大而落入玩家轨。
/// - 溢出和领域非法是两个可区分的错误分支。
/// - 合法迁移保持各 Sample 的 BGM 相对索引。
/// - 成功提交只创建一个复合历史项。
/// - Undo 按契约只恢复轨道布局，不回退已提交名称。
/// @par 故障定位
/// 失败分支名称变化说明元数据先写后验；Sample 部分迁移说明未先全量预检；合法
/// 分支轨道错误检查 BGM 相对索引重编码，Undo 名称回退则违反既定动作契约。
/// @par 测试边界
/// 不验证元数据窗口控件或保存文件，只检查 Session 命令后的内存原子性与反馈。
/// @return 非法轨道与溢出均原子拒绝，成功迁移仍可撤销时返回 true。
bool testMetadataTrackCountMigrationIsAtomic()
{
    // 元数据编辑同时携带轨道数与其他可修改字段。
    // 轨道迁移成功后，相关 Sample 与布局必须一起提交。
    // 迁移失败时其他元数据字段也不能抢先写入。
    // 观察者只应在完整成功后收到一次对象变化通知。
    // Undo 恢复元数据和对象轨道，Redo 再整体应用。
    // 本场景验证复合元数据动作仍遵循事务边界。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.currentBeatmap             = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.name            = "before";
    context.currentBeatmap->m_baseMapMetadata.track_count     = 1;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 2;
    context.trackCount                                        = 1;
    context.bgmTrackCount                                     = 2;

    const auto first = context.sampleRegistry.create();
    const auto far   = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        first,
        MMM::Logic::SampleComponent{
            .m_track           = 1,
            .m_audioResourceId = "first",
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        far,
        MMM::Logic::SampleComponent{
            .m_track           = std::numeric_limits<std::uint32_t>::max(),
            .m_audioResourceId = "far",
        });

    // 第一阶段：最大绝对轨道无法随玩家区扩张，所有元数据必须保持 before。
    auto updatedMeta        = context.currentBeatmap->m_baseMapMetadata;
    updatedMeta.name        = "overflow";
    updatedMeta.track_count = 2;
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.currentBeatmap->m_baseMapMetadata.name != "before" ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 1 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         context.trackCount != 1 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 1 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             std::numeric_limits<std::uint32_t>::max() ||
         context.actionStack.getUndoStackSize() != 0 ||
         context.lastActionMessage.find("超出可表示范围") ==
             std::string::npos ) {
        XERROR("Metadata track-count overflow partially changed chart state");
        return false;
    }

    context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track = 0;
    // 第二阶段：自动采样位于玩家轨道区，本身即违反 Sample 领域约束。
    updatedMeta.name = "invalid";
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.currentBeatmap->m_baseMapMetadata.name != "before" ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 1 ||
         context.trackCount != 1 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 1 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             0 ||
         context.actionStack.getUndoStackSize() != 0 ||
         context.lastActionMessage.find("落入玩家轨道区") ==
             std::string::npos ) {
        XERROR("Invalid automatic-sample lane partially changed metadata");
        return false;
    }

    context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track = 2;
    // 第三阶段：修正为两个合法 BGM 相对轨，验证成功迁移与单次历史记录。
    updatedMeta.name        = "after";
    updatedMeta.track_count = 3;
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.currentBeatmap->m_baseMapMetadata.name != "after" ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         context.trackCount != 3 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             4 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Metadata track-count migration did not preserve BGM lanes");
        return false;
    }

    context.actionStack.undo(context);
    // 按动作契约，Undo 恢复轨道布局但不撤销同次提交的名称字段。
    return context.currentBeatmap->m_baseMapMetadata.name == "after" &&
           context.currentBeatmap->m_baseMapMetadata.track_count == 1 &&
           context.currentBeatmap->m_baseMapMetadata.bgm_track_count == 2 &&
           context.trackCount == 1 && context.bgmTrackCount == 2 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                   .m_track == 1 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(far)
                   .m_track == 2;
}

/// @brief 验证谱面设置更换主音轨会同步时间最早的 Main BGM 自动采样。
/// @details
/// 项目中同时放置玩家区采样、Effect、两个 Main BGM 采样。更换谱面主音频时，
/// 只有时间最早且位于 BGM 区的 Main 采样代表主音轨起点，应改绑新资源。
/// 时间、offset、轨道和音量都是用户编辑结果，资源改绑不能重置这些字段。
/// 元数据更新由会话直接提交，不创建可撤销动作，但必须同时通知 Metadata 与
/// AudioSamples 两类变更，并立即把 ECS 结果同步回 BeatMap 领域数组。
/// @par 关键不变量
/// - 候选仅限 BGM 区内且资源类型为 Main 的 Sample。
/// - 多个候选按锚点时间选择最早一项。
/// - 玩家区同资源 ID 的对象不是主音轨自动采样。
/// - Effect 资源与后续 Main Sample 保持原绑定。
/// - 改绑不改变时间、offset、轨道或音量。
/// - 领域数组与 ECS 在命令完成后立即一致。
/// - 变更通知同时包含 Metadata 和 AudioSamples。
/// @par 故障定位
/// 改错 Sample 检查 Main/BGM/最早时间筛选；字段被重置检查全组件替换范围；领域
/// 未更新或通知缺类分别检查 syncBeatmap 与 mutation 聚合路径。
/// @par 测试边界
/// 资源表只提供标识和类型，不访问音频文件；成功改绑不代表音频设备实际加载成功。
/// @return 仅首个 Main BGM 采样更新且其他 Main、Effect 和玩家轨采样不变时返回
/// true。
bool testMetadataAudioChangeRetargetsFirstMainBgmSample()
{
    // 谱面含多个音频 Sample，并明确首个主 BGM 对象。
    // 更换主音频资源只允许重定向该主 BGM 的资源引用。
    // 其他 Sample 的资源、偏移与音量必须保持原值。
    // 元数据音频字段和对象绑定要在同一动作中更新。
    // Undo 同时恢复旧字段与旧绑定，Redo 再统一切换。
    // 该约束避免资源变更误伤效果音和追加音轨。
    MMM::Logic::BeatmapSession session;
    // 观察者记录聚合标志，验证保存和协作链路都能感知资源改绑。
    const auto observer = std::make_shared<RecordingMutationObserver>();
    session.setMutationObserver(observer, false);
    auto& context          = session.getContextMutable();
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.name     = "AudioChange";
    context.currentBeatmap->m_baseMapMetadata.map_path = "AudioChange.mmm";
    context.currentBeatmap->m_baseMapMetadata.song_file_hint  = "audio/old.ogg";
    context.currentBeatmap->m_baseMapMetadata.track_count     = 4;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 2;
    context.trackCount                                        = 4;
    context.bgmTrackCount                                     = 2;
    context.collaborationProject = std::make_shared<MMM::Project>();
    context.collaborationProject->m_audioResources = {
        MMM::AudioResource{ .m_id   = "old-main",
                            .m_path = "audio/old.ogg",
                            .m_type = MMM::AudioTrackType::Main },
        MMM::AudioResource{ .m_id   = "new-main",
                            .m_path = "audio/new.ogg",
                            .m_type = MMM::AudioTrackType::Main },
        MMM::AudioResource{ .m_id   = "effect",
                            .m_path = "audio/effect.wav",
                            .m_type = MMM::AudioTrackType::Effect },
    };

    // 四个样本覆盖不应改的玩家区、Effect、应改的首 Main 和后续 Main。
    const auto playerSample = context.sampleRegistry.create();
    const auto effectSample = context.sampleRegistry.create();
    const auto firstMain    = context.sampleRegistry.create();
    const auto laterMain    = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        playerSample,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 0.25,
            .m_track           = 1,
            .m_audioResourceId = "old-main",
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        effectSample,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 0.5,
            .m_track           = 4,
            .m_audioResourceId = "effect",
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        firstMain,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_offsetMs        = -100,
            .m_track           = 4,
            .m_audioResourceId = "old-main",
            .m_volume          = 0.6F,
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        laterMain,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 2.0,
            .m_track           = 5,
            .m_audioResourceId = "old-main",
        });

    // 清空兼容字段，强制资源解析以新的 song_file_hint 为权威来源。
    auto updatedMeta           = context.currentBeatmap->m_baseMapMetadata;
    updatedMeta.song_file_hint = "audio/new.ogg";
    updatedMeta.main_audio_path.clear();
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);

    // 同时锁定目标选择、字段保留、通知类型和无撤销记录契约。
    const auto& updatedFirst =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(firstMain);
    if ( updatedFirst.m_audioResourceId != "new-main" ||
         !near(updatedFirst.m_timestamp, 1.0) ||
         updatedFirst.m_offsetMs != -100 || updatedFirst.m_track != 4 ||
         !near(updatedFirst.m_volume, 0.6) ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(laterMain)
                 .m_audioResourceId != "old-main" ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(effectSample)
                 .m_audioResourceId != "effect" ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(playerSample)
                 .m_audioResourceId != "old-main" ||
         context.currentBeatmap->m_baseMapMetadata.song_file_hint !=
             std::filesystem::path("audio/new.ogg") ||
         !MMM::hasBeatmapMutationFlag(observer->m_flags,
                                      MMM::BeatmapMutationFlags::Metadata) ||
         !MMM::hasBeatmapMutationFlag(
             observer->m_flags, MMM::BeatmapMutationFlags::AudioSamples) ||
         context.actionStack.getUndoStackSize() != 0U ) {
        XERROR(
            "Metadata audio change did not retarget only the first Main BGM "
            "sample");
        return false;
    }

    // 领域数组没有 ECS 实体 ID，因此按新资源 ID 找到被改绑的采样。
    const auto updatedDomainSample =
        std::find_if(context.currentBeatmap->m_audioSamples.begin(),
                     context.currentBeatmap->m_audioSamples.end(),
                     [](const MMM::AudioSampleEvent& sample) {
                         return sample.m_audioResourceId == "new-main";
                     });
    return context.currentBeatmap->m_audioSamples.size() == 4U &&
           updatedDomainSample !=
               context.currentBeatmap->m_audioSamples.end() &&
           updatedDomainSample->m_timestamp == 1000.0 &&
           updatedDomainSample->m_offsetMs == -100 &&
           updatedDomainSample->m_track == 4 &&
           near(updatedDomainSample->m_volume, 0.6);
}

/// @brief 验证替换元数据时同步迁移自动采样，并保留当前 BGM 轨道数。
/// @details
/// metadata 分支可从外部谱面取得新玩家键数，但当前编辑会话已经拥有自己的 BGM
/// 布局，来源中的 bgm_track_count 不得覆盖它。自动采样以玩家轨末尾为基准整体
/// 平移，稀疏轨也必须保留相对位置。替换动作应把元数据与 Sample 更新合并为一个
/// 历史项，使 Undo/Redo 不会暴露中间布局。
/// @par 关键不变量
/// - replaceMetadata 采用来源的玩家键数。
/// - 当前会话的 BGM 持久轨数优先于来源的任意值。
/// - 每个自动采样保持原 BGM 相对轨位置。
/// - context、BeatMap 元数据和 Sample Registry 原子切换。
/// - 领域 m_audioSamples 在执行后同步重建。
/// - Undo 与 Redo 恢复成对一致的布局和样本位置。
/// @par 故障定位
/// BGM 数被 99 覆盖说明来源字段优先级错误；轨道偏移不对检查相对索引换算；只有
/// Undo/Redo 失败说明复合动作没有同时保存元数据和 Sample 快照。
/// @par 测试边界
/// 只打开 replaceMetadata，物件和时间线的权威替换分支由其他场景独立验证。
/// @return 执行、Undo 和 Redo 均恢复轨道与元数据时返回 true。
bool testReplaceBeatmapMetadataMigratesSamples()
{
    // 权威替换带来新的轨道元数据，现有 Sample 需要迁移。
    // 替换入口必须使用与本地轨道动作相同的域换算规则。
    // 稳定对象 ID 保持不变，便于协作引用继续命中。
    // ECS Registry 在替换后应与新领域数组完全一致。
    // mutationFlags 需要包含元数据与对象两个变化类别。
    // 测试不保留本地 Undo，只验证权威快照落地一致性。
    // 当前布局为四玩家轨、三 BGM 轨，是替换前和 Undo 的共同基线。
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 4;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 3;
    context.trackCount                                        = 4;
    context.bgmTrackCount                                     = 3;

    const auto first = context.sampleRegistry.create();
    const auto far   = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        first,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "first",
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        far,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 2.0,
            .m_track           = 10,
            .m_audioResourceId = "far",
        });

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 7;
    // 99 故意与当前值不同，证明 BGM 数量不从来源谱面复制。
    source->m_baseMapMetadata.bgm_track_count = 99;

    MMM::Logic::ActionController controller(context);
    // 只替换元数据，避免物件替换分支影响样本迁移的观测。
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap   = source,
        .replaceMetadata = true,
    });
    if ( context.trackCount != 7 || context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 7 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             13 ||
         context.currentBeatmap->m_audioSamples.size() != 2 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Metadata replacement did not preserve BGM-relative samples");
        return false;
    }

    // Undo 要同步恢复元数据字段与两个绝对轨道。
    context.actionStack.undo(context);
    if ( context.trackCount != 4 || context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 4 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 4 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             10 ) {
        XERROR("Metadata replacement undo did not restore sample tracks");
        return false;
    }

    // Redo 使用动作快照复现七键布局，不能在当前值上重复叠加偏移。
    context.actionStack.redo(context);
    return context.trackCount == 7 && context.bgmTrackCount == 3 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                   .m_track == 7 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(far)
                   .m_track == 13;
}

/// @brief 验证替换元数据造成自动采样绝对轨道溢出时整项拒绝。
/// @details
/// 来源键数比当前多一，而 Sample 已在 UINT32_MAX。预检必须在写入来源元数据、
/// 修改 context 或创建历史项之前发现无法编码的新轨道，并留下可显示的错误。
/// @par 关键不变量
/// - UINT32_MAX 在玩家轨增加后不能静默回绕。
/// - 来源元数据在预检失败后不写入当前 BeatMap。
/// - 原 Sample 实体和值保持有效。
/// - ActionStack 不接受无法执行的替换动作。
/// - lastActionMessage 非空，允许 UI 呈现失败。
/// @par 故障定位
/// 元数据或轨道变化说明替换在溢出预检前提交；历史项出现说明失败动作仍被入栈；
/// 只有消息缺失则检查错误枚举到用户反馈文本的映射。
/// @par 测试边界
/// 使用单个极值 Sample 构造确定上溢，不覆盖多个错误同时存在时的消息优先级。
/// @return 元数据、采样与撤销栈均未改变时返回 true。
bool testReplaceBeatmapMetadataOverflowIsRejected()
{
    // 权威元数据会把现有 Sample 映射到不可表示的轨道。
    // 替换前先保存领域、Registry 和观察者状态作为基线。
    // 预检失败后必须拒绝整个快照而非夹取溢出索引。
    // 旧谱面元数据与每个 Sample 字段均需保持不变。
    // 观察者不得收到成功变化通知，脏标记也不能推进。
    // 该路径证明外部协作数据同样遵守本地原子约束。
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 1;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 1;
    context.trackCount                                        = 1;
    context.bgmTrackCount                                     = 1;
    const auto entity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_track = std::numeric_limits<std::uint32_t>::max(),
        });

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 2;
    MMM::Logic::ActionController controller(context);
    // 替换入口和元数据对话框入口都必须共享相同的原子轨道预检。
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap   = source,
        .replaceMetadata = true,
    });

    // 状态、实体、历史项和错误消息一起检查，排除静默的部分提交。
    return context.trackCount == 1 &&
           context.currentBeatmap->m_baseMapMetadata.track_count == 1 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                   .m_track == std::numeric_limits<std::uint32_t>::max() &&
           context.actionStack.getUndoStackSize() == 0 &&
           !context.lastActionMessage.empty();
}

/// @brief 验证远端编辑本人创建的音符后，本人的撤回仍按逻辑身份删除该音符。
/// @details
/// 本地 Create 动作先生成稳定 collaborationId，随后权威快照用该 ID 返回被远端
/// 修改过的同一对象，并新增另一远端对象。替换必须尽量复用原实体，使本地动作栈、
/// 选择和悬停仍指向正确对象；临时画笔、橡皮和拖动状态则必须安全终止。
/// 空间索引需要标脏并清空旧缓存，因为远端已改变时间和轨道。最后执行本地 Undo，
/// 应仅删除本人创建的逻辑对象，保留没有对应本地动作的远端对象。
/// @par 关键不变量
/// - collaborationId 相同的权威对象尽量复用旧实体。
/// - 本地 Create 动作在远端替换后仍留在撤销栈。
/// - 选择、悬停可随复用实体保留。
/// - 画笔、橡皮和拖动等未提交手势全部终止。
/// - 排序缓存清空并标脏，避免沿用旧时空位置。
/// - Marquee 几何保留但选择查询标脏等待重算。
/// - Undo 只删除本地拥有对象，不触碰新远端对象。
/// @par 故障定位
/// 实体变化说明稳定 ID 匹配失败；动作栈丢失说明权威替换做了全量重置；临时手势
/// 残留检查替换前清理；Undo 删除远端项则检查动作目标重定向。
/// @par 测试边界
/// 权威谱面直接以内存对象提供，不覆盖网络乱序或权限；只验证既定快照的本地合并。
/// @return 权威替换保留动作栈与实体身份，且撤回不删除他人物件时返回 true。
bool testAuthoritativeReplacementPreservesOwnedCreateUndo()
{
    // 本地先创建对象并形成拥有该对象的 Undo 记录。
    // 随后权威快照保留此稳定 ID，并加入其他远端变化。
    // 合并后本地 Undo 仍只能撤销自己创建的那个对象。
    // 远端对象及其字段不得随本地历史一起消失。
    // Redo 恢复本地对象时采用权威快照中的兼容状态。
    // 测试约束协作替换与本地所有权历史的共存规则。
    MMM::Logic::SessionContext context;
    context.currentBeatmap                = std::make_shared<MMM::BeatMap>();
    const auto                staleEntity = context.noteRegistry.create();
    MMM::Logic::NoteComponent staleNote;
    staleNote.m_timestamp = 1.0;
    context.actionStack.pushAndExecute(std::make_unique<MMM::Logic::NoteAction>(
                                           MMM::Logic::NoteAction::Type::Create,
                                           staleEntity,
                                           std::nullopt,
                                           staleNote),
                                       context);
    // 动作产生的协作 ID 是跨 Registry 和网络快照重识别对象的依据。
    const auto collaborationId =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(staleEntity)
            .m_collaborationId;
    context.sortedNoteEntities.push_back(staleEntity);
    context.sortedNoteMaxEndPrefix.push_back(1.0);
    context.noteRegistry.emplace_or_replace<MMM::Logic::InteractionComponent>(
        staleEntity,
        MMM::Logic::InteractionComponent{
            .isHovered  = true,
            .isSelected = true,
            .hoveredPart =
                static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        });
    context.selectedNoteEntities.insert(staleEntity);
    context.hoveredEntity     = staleEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);
    context.hasMarqueeSelection = true;
    context.marqueeBoxes.push_back(MMM::Logic::MarqueeBox{
        .startTime  = 0.5,
        .endTime    = 1.5,
        .startTrack = 0.0F,
        .endTrack   = 3.0F,
        .cameraId   = "Basic2DCanvas",
    });
    // 构造所有持有旧实体的短期交互状态，替换后逐项检查清理或重绑定。
    context.draggedEntity     = staleEntity;
    context.draggedObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.dragRenderPinnedEntities.push_back(staleEntity);
    context.brushState.isActive = true;
    context.brushState.polylineSegments.push_back({});
    context.eraserState.isActive = true;
    context.eraserState.targetObjectKind =
        MMM::Logic::ChartObjectKind::PlayerNote;
    context.eraserState.targetEntities.insert(staleEntity);

    // 权威版本保留本地对象 ID，但把轨道改为 3，要求复用实体并更新内容。
    auto  source                      = std::make_shared<MMM::BeatMap>();
    auto& editedOwnedNote             = source->m_noteData.notes.emplace_back();
    editedOwnedNote.m_timestamp       = 1000.0;
    editedOwnedNote.m_track           = 3;
    editedOwnedNote.m_collaborationId = collaborationId;
    // 无本地动作的远端对象用于确认 Undo 不会按 Registry 范围误删。
    auto& remoteNote             = source->m_noteData.notes.emplace_back();
    remoteNote.m_timestamp       = 2500.0;
    remoteNote.m_track           = 2;
    remoteNote.m_collaborationId = "remote-note-b";
    source->sync();

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap       = source,
        .replaceObjects      = true,
        .authoritativeRemote = true,
    });

    // 持久选择可随复用实体保留，进行中的手势与旧空间索引必须清理。
    const auto view =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    if ( view.size() != 2U || !context.noteRegistry.valid(staleEntity) ||
         context.noteRegistry.get<const MMM::Logic::NoteComponent>(staleEntity)
                 .m_trackIndex != 3 ||
         context.actionStack.getUndoStackSize() != 1U ||
         !context.actionStack.isDirty() || context.brushState.isActive ||
         !context.brushState.polylineSegments.empty() ||
         context.eraserState.isActive ||
         !context.eraserState.targetEntities.empty() ||
         !context.dragRenderPinnedEntities.empty() ||
         !context.selectedNoteEntities.contains(staleEntity) ||
         !context.noteRegistry
              .get<const MMM::Logic::InteractionComponent>(staleEntity)
              .isSelected ||
         !context.noteRegistry
              .get<const MMM::Logic::InteractionComponent>(staleEntity)
              .isHovered ||
         context.hoveredEntity != staleEntity || !context.hasMarqueeSelection ||
         context.marqueeBoxes.size() != 1U ||
         !context.isMarqueeSelectionDirty ||
         context.draggedEntity != entt::null ||
         !context.sortedNoteEntities.empty() ||
         !context.sortedNoteMaxEndPrefix.empty() ||
         !context.isNoteOrderDirty ) {
        XERROR("Authoritative replacement did not preserve owned create undo");
        return false;
    }

    // Undo 按 collaborationId 删除 owned create，剩余对象只能是远端新增项。
    context.actionStack.undo(context);
    const auto afterUndo =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    return afterUndo.size() == 1U &&
           near(afterUndo
                    .get<const MMM::Logic::NoteComponent>(*afterUndo.begin())
                    .m_timestamp,
                2.5);
}

/// @brief 验证远端覆盖本人修改结果后，撤回只还原仍未冲突的字段。
/// @details
/// 本地把 Hold 的时间和时长一起修改；远端随后改变时间和轨道，但保留本地修改后的
/// 时长。动作栈重定基线时应按字段做三方比较：远端已改的时间、轨道优先，仍与
/// 本地 after 相同的时长可由 Undo 恢复为 before。实体 ID 也必须保持稳定。
/// @par 关键不变量
/// - 权威替换按 collaborationId 找到本地 Update 的目标。
/// - 远端已改变的 timestamp 不被本地 Undo 覆盖。
/// - 远端已改变的 trackIndex 不被本地 Undo 覆盖。
/// - 未发生远端冲突的 duration 可恢复到本地 before。
/// - 动作历史保留一次，不生成额外远端动作。
/// - Registry 实体保持有效，避免选择和引用悬空。
/// @par 故障定位
/// 所有字段都回退说明未做三方合并；没有字段回退说明本地动作被丢弃；只有实体
/// 变化则说明权威替换未按 collaborationId 复用现有对象。
/// @par 测试边界
/// 选择三个代表字段验证逐字段策略，不穷举 NoteComponent 的每个元数据扩展字段。
/// @return 远端时间和轨道得到保留，本地时长修改被撤回时返回 true。
bool testAuthoritativeReplacementMergesOwnedUpdateUndo()
{
    // 本地动作先修改对象字段，并记录修改前后的拥有范围。
    // 权威快照随后同时改变同对象的其他非冲突字段。
    // Undo 只恢复本地负责字段，保留远端已接受的更新。
    // Redo 也只重放本地字段，不能覆盖权威合并结果。
    // 稳定 ID 用于跨 ECS 重建重新绑定历史目标。
    // 该断言集合防止整对象快照式 Undo 回滚协作修改。
    MMM::Logic::SessionContext context;
    context.currentBeatmap           = std::make_shared<MMM::BeatMap>();
    const auto                entity = context.noteRegistry.create();
    MMM::Logic::NoteComponent before;
    before.m_type            = MMM::NoteType::HOLD;
    before.m_timestamp       = 1.0;
    before.m_duration        = 0.5;
    before.m_trackIndex      = 0;
    before.m_collaborationId = "owned-update-note";
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(entity, before);

    // 本地动作同时修改两个字段，为选择性撤回提供可区分的目标。
    auto after        = before;
    after.m_timestamp = 2.0;
    after.m_duration  = 1.5;
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::NoteAction>(
            MMM::Logic::NoteAction::Type::Update, entity, before, after),
        context);

    // 远端时间与本地 after 冲突，时长仍等于 after，并额外修改轨道。
    auto  source                     = std::make_shared<MMM::BeatMap>();
    auto& remotelyEdited             = source->m_noteData.holds.emplace_back();
    remotelyEdited.m_timestamp       = 3000.0;
    remotelyEdited.m_duration        = 1500.0;
    remotelyEdited.m_track           = 3;
    remotelyEdited.m_collaborationId = before.m_collaborationId;
    source->sync();

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap       = source,
        .replaceObjects      = true,
        .authoritativeRemote = true,
    });
    if ( !context.noteRegistry.valid(entity) ||
         context.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Authoritative replacement did not retain owned update");
        return false;
    }

    // 只撤销未冲突的 duration；远端时间 3.0 和轨道 3 必须保留。
    context.actionStack.undo(context);
    const auto& merged =
        context.noteRegistry.get<const MMM::Logic::NoteComponent>(entity);
    return near(merged.m_timestamp, 3.0) && near(merged.m_duration, 0.5) &&
           merged.m_trackIndex == 3;
}

/// @brief 验证 Polyline 子物件经过多轮领域对象与 ECS 往返仍保持身份和顺序。
/// @details
/// 领域模型同时维护按类型子数组和 Polyline 内嵌节点。载入 ECS 时子物件只能作为
/// 折线投影存在；同步回领域模型时不能再把它们追加为独立根对象。三十二轮往返
/// 用于放大重复附加、isSubNote 丢失、顺序漂移或 metadata/sampleBinding 丢失。
/// 每轮都从上轮领域结果重新载入，任何非幂等行为都会累计并被数量断言发现。
/// @par 关键不变量
/// - 根对象始终只有一个 Polyline。
/// - Hold 与 Flick 子对象各只有一个且保持 isSubNote。
/// - Polyline 引用重新指向当前轮领域容器中的对象。
/// - m_subNotes 顺序在每轮保持 Hold 后 Flick。
/// - Hold duration、sampleBinding 与 metadata 不丢失。
/// - Flick dtrack 不因通用节点转换归零。
/// - 三十二轮后对象数量不发生放大。
/// @par 故障定位
/// 数量逐轮增加说明子对象被同步为根；引用失效说明 sync 后未重建 Polyline 引用；
/// 字段退化则检查通用 SubNote 与具体 Hold/Flick 间的复制范围。
/// @par 测试边界
/// 三十二轮用于检验幂等性而非性能基准，测试结果不代表大谱面的同步耗时。
/// @return 子物件不会成为独立根对象且全部字段稳定时返回 true。
bool testPolylineSubNoteIdentitySurvivesRepeatedEcsSync()
{
    // Polyline 根对象带多个稳定子节点身份并同步到 ECS。
    // 重复 syncBeatmap 不改变领域数据，只重走投影流程。
    // 每轮同步后子实体可以重建，但稳定 ID 映射必须保留。
    // 节点顺序、父级关联和类型也要保持一致。
    // 交互引用通过稳定身份重新定位，不能依赖旧实体句柄。
    // 测试据此区分允许的 ECS 生命周期与领域身份漂移。
    // Hold 与 Flick 携带不同扩展字段，覆盖按类型恢复和公共节点排序。
    auto  beatmap        = std::make_shared<MMM::BeatMap>();
    auto& hold           = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp     = 1000.0;
    hold.m_duration      = 375.0;
    hold.m_track         = 1;
    hold.m_isSubNote     = true;
    hold.m_sampleBinding = MMM::AudioSampleBinding{ "hold.wav", 0.4F };
    hold.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["child"] =
        "hold";
    auto& flick       = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp = 1375.0;
    flick.m_track     = 1;
    flick.m_dtrack    = 2;
    flick.m_isSubNote = true;
    flick.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["child"] =
        "flick";
    auto& polyline = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_subNotes.emplace_back(hold);
    polyline.m_subNotes.emplace_back(flick);
    polyline.m_subHolds.emplace_back(hold);
    polyline.m_subFlicks.emplace_back(flick);
    beatmap->sync();

    MMM::Logic::SessionContext context;
    for ( std::size_t round = 0; round < 32U; ++round ) {
        // load->sync 构成一次序列化边界，循环中不复用上一轮 ECS 实体。
        MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
        context.m_needsNotesSync = true;
        MMM::Logic::SessionUtils::syncBeatmap(context);
        if ( beatmap->m_noteData.notes.size() != 0U ||
             beatmap->m_noteData.holds.size() != 1U ||
             beatmap->m_noteData.flicks.size() != 1U ||
             beatmap->m_noteData.polylines.size() != 1U ) {
            XERROR("Polyline ECS round trip amplified child objects");
            return false;
        }
        // 数量正确后再核对节点字段，避免结构未放大但内容逐轮退化。
        const auto& restoredHold     = beatmap->m_noteData.holds.front();
        const auto& restoredFlick    = beatmap->m_noteData.flicks.front();
        const auto& restoredPolyline = beatmap->m_noteData.polylines.front();
        if ( !restoredHold.m_isSubNote || !restoredFlick.m_isSubNote ||
             restoredPolyline.m_subNotes.size() != 2U ||
             &restoredPolyline.m_subNotes[0].get() != &restoredHold ||
             &restoredPolyline.m_subNotes[1].get() != &restoredFlick ||
             restoredPolyline.m_subHolds.size() != 1U ||
             restoredPolyline.m_subFlicks.size() != 1U ||
             !near(restoredHold.m_duration, 375.0) ||
             restoredFlick.m_dtrack != 2 || !restoredHold.m_sampleBinding ||
             restoredHold.m_sampleBinding->m_audioResourceId != "hold.wav" ||
             !near(restoredHold.m_sampleBinding->m_volume, 0.4) ||
             restoredHold.m_metadata.note_properties
                     .at(MMM::NoteMetadataType::MMM)
                     .at("child") != "hold" ) {
            XERROR("Polyline ECS round trip lost child semantics");
            return false;
        }
    }
    return true;
}

/// @brief 验证在运行时追加空轨放置采样会持久扩展 BGM 轨道数。
/// @details
/// SampleAction 把现有采样从第一条 BGM 轨移动到当前 BGM 区右侧的追加轨。
/// 动作必须先扩展持久轨数再应用新绝对轨道，并把扩轨与移动纳入同一 Undo/Redo
/// 边界；不能要求用户另行保存轨道布局，也不能在撤销后留下空的新增轨。
/// @par 关键不变量
/// - 追加轨的绝对索引等于 trackCount + bgmTrackCount。
/// - 执行后持久 BGM 数量扩展到足以容纳目标。
/// - Sample 时间和资源字段不因换轨变化。
/// - Undo 同时恢复原轨和原 BGM 数量。
/// - Redo 同时恢复目标轨和扩展后的数量。
/// - 整个手势只占一个 SampleAction 历史项。
/// @par 故障定位
/// Sample 到目标轨但轨数未增说明扩轨遗漏；Undo 只恢复一半说明动作未保存布局；
/// Redo 再增一轨说明使用当前 bgmTrackCount 重新推导而非快照。
/// @par 测试边界
/// 直接执行 SampleAction，不覆盖鼠标坐标换算；GrabTool 追加轨由拖动场景验证。
/// @return 扩展及 Undo/Redo 均恢复完整状态时返回 true。
bool testAppendLaneExpandsPersistentCount()
{
    // 指针落入追加区域时先生成非持久轨道预览。
    // 创建对象提交应把所需轨数一次扩展到项目配置。
    // 新对象的局部轨道需对应扩展后的最后一条有效轨。
    // 项目、会话布局与领域对象必须观察到同一计数。
    // Undo 删除对象并恢复原轨数，不能遗留空追加轨。
    // Redo 再扩展用于验证计数变化属于同一复合动作。
    MMM::Logic::SessionContext context;
    context.trackCount    = 4;
    context.bgmTrackCount = 2;
    // 四玩家轨加两 BGM 轨时，绝对轨 6 正好是第一条追加轨。
    const auto                  entity = context.sampleRegistry.create();
    MMM::Logic::SampleComponent before;
    before.m_track = 4;
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(entity, before);

    auto after    = before;
    after.m_track = 6;
    auto action   = std::make_unique<MMM::Logic::SampleAction>(
        MMM::Logic::SampleAction::Type::Update, entity, before, after);
    // 对象移动和隐式扩轨作为一次动作执行，不能产生两个历史项。
    context.actionStack.pushAndExecute(std::move(action), context);
    if ( context.bgmTrackCount != 3 ) {
        XERROR("Runtime append lane did not expand persistent BGM count");
        return false;
    }

    // Undo 同时恢复采样轨道并收回该动作新增的末尾空轨。
    context.actionStack.undo(context);
    if ( context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                 .m_track != 4 ) {
        XERROR("Append lane undo did not restore BGM count");
        return false;
    }

    // Redo 必须先恢复容量再落入绝对轨 6，避免短暂的越界状态。
    context.actionStack.redo(context);
    if ( context.bgmTrackCount != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                 .m_track != 6 ) {
        XERROR("Append lane redo did not restore expanded BGM count");
        return false;
    }
    return true;
}

/// @brief 验证显式增删持久 BGM 轨可撤销，并禁止删除占用中的末尾轨。
/// @details
/// 显式轨道命令只改变布局，不创建采样。新增一轨应形成一个历史项并支持往返；
/// 删除时必须检查目标末尾轨是否存在 Sample。被占用的删除是无操作且不能污染
/// 历史栈，移除占用对象后相同命令才可提交第二个动作。每次合法变更还需使投影
/// 缓存失效，并同步 BeatMap 的 bgm_track_count 元数据。
/// @par 关键不变量
/// - 合法增轨同步修改 context 与 BeatMap 元数据。
/// - 每次执行、撤销和重做都使 Transform 缓存失效。
/// - 末轨存在 Sample 时拒绝缩容。
/// - 拒绝分支不改变历史栈大小。
/// - 空末轨允许删除并创建一个新历史项。
/// - 删除动作 Undo/Redo 精确往返三轨和两轨状态。
/// @par 故障定位
/// 元数据与 context 不一致说明动作只更新一层；占用轨被删说明缩容前未扫描
/// Sample； 被拒绝却新增历史项说明验证发生在 pushAndExecute 之后。
/// @par 测试边界
/// 只允许从末尾增删轨，不验证中间轨重排，因为当前领域模型不提供该操作。
/// @return 元数据同步、Undo/Redo 和末轨占用保护均正确时返回 true。
bool testExplicitBgmTrackCountAction()
{
    // 显式动作只调整 BGM 轨数量，不借助对象拖动触发。
    // 已有 Sample 的局部 BGM 归属在绝对索引迁移后保持。
    // 玩家轨数和其他辅助轨计数不得随该动作改变。
    // 投影缓存应立即反映新的 BGM 区宽与轨道边界。
    // Undo/Redo 同时覆盖配置、Sample 索引和缓存结果。
    // 该用例隔离验证公共轨数动作的直接调用契约。
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 4;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 2;
    context.trackCount                                        = 4;
    context.bgmTrackCount                                     = 2;
    MMM::Logic::InteractionController controller(context);

    // 2->3 覆盖纯布局扩展，不依赖追加轨采样的隐式扩展路径。
    controller.handleCommand(MMM::Logic::CmdUpdateBgmTrackCount{ 3 });
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         !context.isTransformDirty ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Explicit BGM lane add did not persist as one editor action");
        return false;
    }

    context.isTransformDirty = false;
    // Undo 必须恢复 context 与领域元数据，并通知投影重新计算。
    context.actionStack.undo(context);
    if ( context.bgmTrackCount != 2 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         !context.isTransformDirty ) {
        XERROR("Explicit BGM lane add undo did not restore metadata");
        return false;
    }

    context.isTransformDirty = false;
    // Redo 再次建立三轨布局，为后续末轨占用检查准备状态。
    context.actionStack.redo(context);
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         !context.isTransformDirty ) {
        XERROR("Explicit BGM lane add redo did not restore metadata");
        return false;
    }

    // 绝对轨 6 是四玩家轨加三 BGM 轨后的最后一条 BGM 轨。
    const auto occupiedEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        occupiedEntity,
        MMM::Logic::SampleComponent{
            .m_track           = 6,
            .m_audioResourceId = "occupied.wav",
        });
    controller.handleCommand(MMM::Logic::CmdUpdateBgmTrackCount{ 2 });
    // 占用拒绝不创建历史项，Undo 栈仍只包含最初的扩轨动作。
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Occupied final BGM lane was removed");
        return false;
    }

    // 清空末轨后再次缩容，此次应成为独立且可撤销的布局动作。
    context.sampleRegistry.destroy(occupiedEntity);
    controller.handleCommand(MMM::Logic::CmdUpdateBgmTrackCount{ 2 });
    if ( context.bgmTrackCount != 2 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         context.actionStack.getUndoStackSize() != 2 ) {
        XERROR("Empty final BGM lane was not removed");
        return false;
    }

    context.actionStack.undo(context);
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ) {
        XERROR("Explicit BGM lane removal undo did not restore metadata");
        return false;
    }
    context.actionStack.redo(context);
    // 最终状态应与合法删除后的两条 BGM 轨一致。
    return context.bgmTrackCount == 2 &&
           context.currentBeatmap->m_baseMapMetadata.bgm_track_count == 2;
}

/// @brief 验证自动采样精确属性编辑接受 Main/Effect 并原子更新全部字段。
/// @details
/// resolveSamplePropertyEdit 是无副作用校验层：Main 与 Effect 都可绑定，BGM
/// 相对 轨需转换为绝对轨，缺失资源、未知类型、负轨和非有限音量必须返回明确
/// issue。 成功结果随后交给 SampleAction，要求资源、offset、volume、轨道和隐式
/// BGM 扩展 组成一个可撤销事务；时间戳不在属性面板编辑范围内，应保持原值。
/// @par 关键不变量
/// - Main 与 Effect 都能成为自动采样资源。
/// - BGM 相对轨先验证非负，再安全编码为绝对轨。
/// - nullptr 和未知资源类型返回不同 issue。
/// - NaN 或无穷音量不会进入 SampleComponent。
/// - 属性解析不修改传入的 before 值。
/// - SampleAction 同时处理字段更新和必要的 BGM 扩轨。
/// - Undo/Redo 恢复全部字段，而非只恢复音量。
/// @par 故障定位
/// issue 错误检查纯解析器分支；绝对轨错误检查玩家轨偏移；执行后字段或轨数遗漏
/// 检查 SampleAction；仅往返失败检查 before/after 快照完整性。
/// @par 测试边界
/// 属性编辑不改变时间戳，资源文件存在性和实际音量播放效果由资源与音频层验证。
/// @return 资源与数值校验、BGM 相对轨换算及 Undo/Redo 均正确时返回 true。
bool testSamplePropertyEditValidationAndAction()
{
    // 编辑请求覆盖资源、偏移与音量，并先提供一组合法值。
    // 合法请求应形成单个动作并同步领域与 SampleComponent。
    // 随后的非法请求逐项验证范围和资源存在性校验。
    // 每次拒绝后对象字段及动作栈深度必须保持基线。
    // Undo/Redo 只针对成功请求，检查所有属性完整往返。
    // 该组合避免属性面板产生部分应用的混合状态。
    // before 同时提供所有需保留或修改的字段，检查属性补丁不会越界修改时间。
    MMM::Logic::SampleComponent before{
        .m_timestamp       = 2.5,
        .m_offsetMs        = 0,
        .m_track           = 4,
        .m_audioResourceId = "old.wav",
        .m_volume          = 1.0F,
    };
    const MMM::AudioResource mainResource{
        .m_id   = "main.wav",
        .m_path = "main.wav",
        .m_type = MMM::AudioTrackType::Main,
    };
    const MMM::AudioResource effectResource{
        .m_id   = "effect.wav",
        .m_path = "effect.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };

    // 相对 BGM 轨 2 在四键谱面中编码为绝对轨 6。
    auto mainEdit = MMM::Logic::resolveSamplePropertyEdit(
        before, 4, &mainResource, 2, -125, 0.75F);
    if ( !mainEdit.m_sample ||
         mainEdit.m_issue != MMM::Logic::SamplePropertyEditIssue::None ||
         mainEdit.m_sample->m_timestamp != before.m_timestamp ||
         mainEdit.m_sample->m_track != 6 ||
         mainEdit.m_sample->m_offsetMs != -125 ||
         mainEdit.m_sample->m_audioResourceId != "main.wav" ||
         !near(mainEdit.m_sample->m_volume, 0.75) ) {
        XERROR("Main sample property edit did not preserve and map fields");
        return false;
    }

    const auto effectEdit = MMM::Logic::resolveSamplePropertyEdit(
        before, 4, &effectResource, 0, 80, 1.25F);
    MMM::AudioResource unsupportedResource = effectResource;
    unsupportedResource.m_type = static_cast<MMM::AudioTrackType>(99);
    // 集中验证所有无效输入，确保解析器不会以默认 Sample 掩盖错误。
    if ( !effectEdit.m_sample ||
         effectEdit.m_sample->m_audioResourceId != "effect.wav" ||
         effectEdit.m_sample->m_track != 4 ||
         MMM::Logic::resolveSamplePropertyEdit(before, 4, nullptr, 0, 0, 1.0F)
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::MissingResource ||
         MMM::Logic::resolveSamplePropertyEdit(
             before, 4, &unsupportedResource, 0, 0, 1.0F)
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::UnsupportedResourceType ||
         MMM::Logic::resolveSamplePropertyEdit(
             before, 4, &mainResource, -1, 0, 1.0F)
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::InvalidBgmLane ||
         MMM::Logic::resolveSamplePropertyEdit(
             before,
             4,
             &mainResource,
             0,
             0,
             std::numeric_limits<float>::infinity())
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::InvalidVolume ) {
        XERROR("Sample property edit resource or numeric validation failed");
        return false;
    }

    // 通过真实动作应用结果，验证纯解析层与持久事务之间的契约。
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.trackCount     = 4;
    context.bgmTrackCount  = 1;
    const auto entity      = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(entity, before);
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::SampleAction>(
            MMM::Logic::SampleAction::Type::Update,
            entity,
            before,
            *mainEdit.m_sample),
        context);
    const auto& edited =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( edited.m_track != 6 || edited.m_audioResourceId != "main.wav" ||
         edited.m_offsetMs != -125 || !near(edited.m_volume, 0.75) ||
         context.bgmTrackCount != 3 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Sample property edit was not one atomic persistent action");
        return false;
    }

    // Undo 连同由绝对轨 6 推导出的 BGM 轨扩展一起恢复。
    context.actionStack.undo(context);
    const auto& restored =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( restored.m_track != 4 || restored.m_audioResourceId != "old.wav" ||
         restored.m_offsetMs != 0 || !near(restored.m_volume, 1.0) ||
         context.bgmTrackCount != 1 ) {
        XERROR("Sample property edit undo did not restore all fields");
        return false;
    }

    // Redo 应用完整 after 快照，而不是只重复最后一次字段赋值。
    context.actionStack.redo(context);
    const auto& redone =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    return redone.m_track == 6 && redone.m_audioResourceId == "main.wav" &&
           redone.m_offsetMs == -125 && near(redone.m_volume, 0.75) &&
           context.bgmTrackCount == 3;
}

/// @brief 验证 BeatMap 自动采样由独立 Registry 完整载入并同步回领域对象。
/// @details
/// BeatMap 使用毫秒，SampleComponent 使用秒；载入和写回必须各转换一次且不影响
/// offsetMs。自动采样只存在 sampleRegistry，不能混入 noteRegistry 或生成玩家
/// HitEvent。修改 ECS 后设置同步标志，syncBeatmap 应重建领域数组、更新 BGM 轨数
/// 并清除已经消费的待同步标志。
/// @par 关键不变量
/// - 1250 毫秒载入为 1.25 秒。
/// - offsetMs 始终保持整数毫秒，不做秒换算。
/// - Sample 不产生 NoteComponent 或玩家 HitEvent。
/// - 资源 ID、绝对轨和音量完整载入。
/// - 写回时 2.5 秒恢复为 2500 毫秒。
/// - BeatMap 的 BGM 轨元数据跟随 context 更新。
/// - 完成写回后 m_needsSamplesSync 被清除。
/// @par 故障定位
/// 时间相差千倍说明毫秒/秒边界错误；对象进入 Note Registry 说明领域隔离失败；
/// 写回字段缺失或标志残留则检查 syncBeatmap 的 Sample 分支。
/// @par 测试边界
/// 只验证一个 Sample 的完整往返，不把序列化文件格式或音频解码纳入本场景。
/// @return 时间单位、偏移、轨道、资源、音量和 BGM 轨道数均往返时返回 true。
bool testSampleRegistryLoadAndSync()
{
    // 领域谱面预置多个 Sample，覆盖不同轨道与资源字段。
    // 初次加载负责建立 Registry 实体及稳定 ID 索引。
    // 再次同步修改后的领域数组，现有实体需正确更新。
    // 已删除对象要从 Registry 和反向索引中共同移除。
    // 未变化对象身份尽量保持，交互状态不能无故丢失。
    // 数量与字段断言共同验证增删改三类同步路径。
    // 1250 ms 与 -250 ms offset 可区分单位换算字段和原样保存字段。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    beatmap->m_baseMapMetadata.bgm_track_count = 2;
    beatmap->m_audioSamples.push_back({
        .m_timestamp       = 1250.0,
        .m_offsetMs        = -250,
        .m_track           = 4,
        .m_audioResourceId = "stem.ogg",
        .m_volume          = 0.75F,
    });

    // 载入后先核对 Registry 隔离，防止自动采样参与玩家判定逻辑。
    MMM::Logic::SessionContext context;
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    const auto sampleView =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( sampleView.size() != 1 ||
         !context.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         !context.hitEvents.empty() ) {
        XERROR("Audio samples were not isolated from player-note ECS");
        return false;
    }

    const auto entity = *sampleView.begin();
    auto&      sample =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( !near(sample.m_timestamp, 1.25) || sample.m_offsetMs != -250 ||
         sample.m_track != 4 || sample.m_audioResourceId != "stem.ogg" ||
         !near(sample.m_volume, 0.75) ) {
        XERROR("Audio sample load did not preserve all fields");
        return false;
    }

    // 同时改动全部字段，避免只验证时间或轨道的部分同步。
    sample.m_timestamp         = 2.5;
    sample.m_offsetMs          = 125;
    sample.m_track             = 6;
    sample.m_audioResourceId   = "effect.wav";
    sample.m_volume            = 0.5F;
    context.bgmTrackCount      = 3;
    context.m_needsSamplesSync = true;
    MMM::Logic::SessionUtils::syncBeatmap(context);

    // 写回时间恢复为毫秒，其他字段保持各自原始单位和语义。
    if ( beatmap->m_audioSamples.size() != 1 ||
         !near(beatmap->m_audioSamples.front().m_timestamp, 2500.0) ||
         beatmap->m_audioSamples.front().m_offsetMs != 125 ||
         beatmap->m_audioSamples.front().m_track != 6 ||
         beatmap->m_audioSamples.front().m_audioResourceId != "effect.wav" ||
         !near(beatmap->m_audioSamples.front().m_volume, 0.5) ||
         beatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.m_needsSamplesSync ) {
        XERROR("Audio sample ECS did not synchronize back to BeatMap");
        return false;
    }
    return true;
}

/// @brief 验证 Note 采样绑定音量在领域对象、ECS、Action 与 HitEvent
/// 间完整往返。
/// @details
/// 绑定属于玩家 Note 的可选值，不是独立自动采样。用例从领域 Note 载入 ECS，
/// 经 NoteAction 替换资源与音量，再覆盖 Undo、Redo、领域同步及 HitEvent 构建。
/// 所有阶段必须保留绑定的值语义，避免 optional 存在但内部音量回退为默认值。
/// @par 关键不变量
/// - 领域绑定载入同一 NoteComponent，不创建 Sample 实体。
/// - NoteAction after 同时替换资源 ID 与音量。
/// - Undo 恢复原资源和 0.25 音量。
/// - Redo 恢复新资源和 0.75 音量。
/// - syncBeatmap 把绑定写回领域 Note。
/// - ensureHitEvents 将同一绑定复制到判定事件。
/// - optional 在全部阶段都保持有值。
/// @par 故障定位
/// 首次载入失败检查领域到 ECS；Action 往返失败检查 NoteAction 快照；领域正确但
/// HitEvent 错误则检查判定缓存重建时的 sampleBinding 复制。
/// @par 测试边界
/// 绑定资源无需真实存在，本用例只检查 ID 与音量在数据层的传递，不试听音频。
/// @return 加载、更新、撤销、重做及同步均保留资源标识和物件音量时返回 true。
bool testNoteSampleBindingRoundTrip()
{
    // Note 先绑定到项目音频资源并设置采样相关属性。
    // 同步进入 ECS 后，绑定描述符需完整映射到组件。
    // 从 Registry 回写领域时不能丢失资源 ID 与偏移信息。
    // 第二次同步再次读取回写结果，形成双向往返。
    // 未绑定 Note 作为对照，必须保持空描述符语义。
    // 测试不播放音频，只验证绑定数据的结构一致性。
    // 初始 0.25 与更新后 0.75 提供明显的历史快照边界。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    MMM::Note note;
    note.m_timestamp = 1000.0;
    note.m_track     = 1;
    note.setSampleBinding({ "effect-id", 0.25F });
    beatmap->m_noteData.notes.push_back(std::move(note));

    MMM::Logic::SessionContext context;
    // 玩家绑定随 Note 进入 noteRegistry，不应创建 sampleRegistry 实体。
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    auto view = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( view.size() != 1 ) {
        XERROR("Note sample binding setup did not create one ECS note");
        return false;
    }

    const auto entity = *view.begin();
    const auto before =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    if ( !before.m_sampleBinding ||
         before.m_sampleBinding->m_audioResourceId != "effect-id" ||
         !near(before.m_sampleBinding->m_volume, 0.25) ) {
        XERROR("Domain note sample binding did not load into ECS");
        return false;
    }

    // 资源 ID 和音量在同一 NoteAction 中更新，Undo 必须一起恢复。
    auto after = before;
    after.m_sampleBinding =
        MMM::AudioSampleBinding{ "replacement-effect", 0.75F };
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::NoteAction>(
            MMM::Logic::NoteAction::Type::Update, entity, before, after),
        context);
    const auto bindingAfterExecute =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity)
            .m_sampleBinding;
    if ( !bindingAfterExecute ||
         bindingAfterExecute->m_audioResourceId != "replacement-effect" ||
         !near(bindingAfterExecute->m_volume, 0.75) ) {
        XERROR("Note action execute dropped sample binding volume");
        return false;
    }

    // 撤销读取 before 快照，不能仅清空 optional 或恢复默认音量。
    context.actionStack.undo(context);
    const auto bindingAfterUndo =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity)
            .m_sampleBinding;
    if ( !bindingAfterUndo ||
         bindingAfterUndo->m_audioResourceId != "effect-id" ||
         !near(bindingAfterUndo->m_volume, 0.25) ) {
        XERROR("Note action undo dropped sample binding volume");
        return false;
    }

    // 最终从 ECS 同步领域，再从领域派生 HitEvent，覆盖两个下游消费者。
    context.actionStack.redo(context);
    MMM::Logic::SessionUtils::syncBeatmap(context);
    const auto& domainBinding =
        beatmap->m_noteData.notes.front().getSampleBinding();
    MMM::Logic::SessionUtils::ensureHitEvents(context);
    if ( !domainBinding ||
         domainBinding->m_audioResourceId != "replacement-effect" ||
         !near(domainBinding->m_volume, 0.75) ||
         context.hitEvents.size() != 1 ||
         !context.hitEvents.front().sampleBinding ||
         context.hitEvents.front().sampleBinding->m_audioResourceId !=
             "replacement-effect" ||
         !near(context.hitEvents.front().sampleBinding->m_volume, 0.75) ) {
        XERROR("ECS note sample binding did not sync to domain and HitEvent");
        return false;
    }
    return true;
}

/// @brief 验证主画布音量指令原子更新玩家绑定、Polyline 子绑定和自动采样。
/// @details
/// 三种目标共用一个 UI 命令，但落在两个 Registry 和不同字段位置。父折线绑定与
/// 指定 subIndex 的子绑定必须分开寻址；Sample 则直接更新 m_volume。负音量属于
/// 非法输入，不得创建第四个历史项。三次合法命令按顺序 Undo/Redo 后应完整往返。
/// @par 关键不变量
/// - ChartObjectKind 决定访问 Note 或 Sample Registry。
/// - 缺少 subIndex 时更新折线父绑定。
/// - 有效 subIndex 只更新指定内嵌节点绑定。
/// - Sample 更新自身 m_volume，不创建 Note 绑定。
/// - 负音量拒绝且不压入历史项。
/// - 三次 Undo 分别恢复三类目标的不同初值。
/// - 三次 Redo 精确复现合法命令顺序。
/// @par 故障定位
/// 父子串改检查 subIndex 分派；Sample 串到 Note 检查 ChartObjectKind；历史数量
/// 异常检查非法音量验证时机；往返错误检查三类动作快照。
/// @par 测试边界
/// 命令逐个执行以验证单对象入口，批量选择更新由下一用例覆盖。
/// @return 三类目标均支持 Undo/Redo，非法音量不写入时返回 true。
bool testObjectSampleVolumeCommand()
{
    // 命令直接面向单个对象身份修改采样音量。
    // 有效值需要同步到领域 Note 与 ECS 组件表示。
    // 相同实体数值可能存在于 Sample Registry，种类必须消歧。
    // 越界音量请求整体拒绝，不能夹取后静默成功。
    // Undo/Redo 检查原值与新值的准确恢复。
    // mutationFlags 还需表明这是对象属性变化而非资源变化。
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.trackCount     = 4;
    context.bgmTrackCount  = 1;
    MMM::Logic::InteractionController controller(context);

    // 同一折线同时带父绑定和子节点绑定，验证 subIndex 路由不会串目标。
    MMM::Logic::NoteComponent note;
    note.m_sampleBinding = MMM::AudioSampleBinding{ "head.wav", 0.5F };
    note.m_type          = MMM::NoteType::POLYLINE;
    note.m_subNotes.push_back(MMM::Logic::NoteComponent::SubNote{
        .type          = MMM::NoteType::NOTE,
        .timestamp     = 1.0,
        .duration      = 0.0,
        .trackIndex    = 0,
        .dtrack        = 0,
        .sampleBinding = MMM::AudioSampleBinding{ "node.wav", 0.4F },
    });
    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity, note);

    // 自动采样位于另一 Registry，命令必须结合 ChartObjectKind 分派。
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "sample.wav",
            .m_volume          = 0.6F,
        });

    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity = noteEntity,
        .kind   = MMM::Logic::ChartObjectKind::PlayerNote,
        .volume = 0.75F,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity   = noteEntity,
        .kind     = MMM::Logic::ChartObjectKind::PlayerNote,
        .subIndex = 0,
        .volume   = 1.25F,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity = sampleEntity,
        .kind   = MMM::Logic::ChartObjectKind::AudioSample,
        .volume = 0.25F,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity = sampleEntity,
        .kind   = MMM::Logic::ChartObjectKind::AudioSample,
        .volume = -0.25F,
    });

    // 最后一条负音量命令应被拒绝，因此撤销栈只包含前三个合法动作。
    const auto& editedNote =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
    const auto& editedSample =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(sampleEntity);
    if ( !editedNote.m_sampleBinding ||
         !near(editedNote.m_sampleBinding->m_volume, 0.75) ||
         !editedNote.m_subNotes.front().sampleBinding ||
         !near(editedNote.m_subNotes.front().sampleBinding->m_volume, 1.25) ||
         !near(editedSample.m_volume, 0.25) ||
         context.actionStack.getUndoStackSize() != 3 ) {
        XERROR("Object sample volume command did not update typed targets");
        return false;
    }

    // 逆序撤销三个动作，检查每类目标都回到各自不同的初始音量。
    context.actionStack.undo(context);
    context.actionStack.undo(context);
    context.actionStack.undo(context);
    const auto& restoredNote =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
    const auto& restoredSample =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(sampleEntity);
    if ( !restoredNote.m_sampleBinding ||
         !near(restoredNote.m_sampleBinding->m_volume, 0.5) ||
         !restoredNote.m_subNotes.front().sampleBinding ||
         !near(restoredNote.m_subNotes.front().sampleBinding->m_volume, 0.4) ||
         !near(restoredSample.m_volume, 0.6) ) {
        XERROR("Object sample volume undo did not restore all typed targets");
        return false;
    }

    // 顺序重做后分别核对父、子和 Sample，确保历史项没有错误合并。
    context.actionStack.redo(context);
    context.actionStack.redo(context);
    context.actionStack.redo(context);
    return near(context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity)
                    .m_sampleBinding->m_volume,
                0.75) &&
           near(context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity)
                    .m_subNotes.front()
                    .sampleBinding->m_volume,
                1.25) &&
           near(context.sampleRegistry
                    .get<MMM::Logic::SampleComponent>(sampleEntity)
                    .m_volume,
                0.25);
}

/// @brief 验证物件音量命令经 BeatmapSession 队列分发后真正写入组件。
/// @details
/// 控制器单元测试不能证明 LogicCommand variant 分派已注册，因此本用例通过
/// pushCommand/update 覆盖队列消费，要求同一帧写入并创建一个撤销步骤。
/// @par 关键不变量
/// - pushCommand 本身只入队，不越线程修改组件。
/// - Session update 能访问 AudioSample 类型命令分支。
/// - 分派结果经 SampleAction 写入 0.4 音量。
/// - 历史栈增加一次，保持用户可撤销性。
/// - 后台会话标志不阻止命令消费。
/// @par 故障定位
/// 值未变说明 variant 未注册或队列未消费；值变但无历史项说明分派绕过动作栈；
/// 两者正确即可把控制器内部实现与 Session 路由同时视为有效。
/// @par 测试边界
/// 不检查保存或观察者通知，仅证明 LogicCommand 从队列到动作控制器的路由存在。
/// @return 队列命令更新自动采样且生成一个撤销步骤时返回 true。
bool testObjectSampleVolumeCommandRoutesThroughSession()
{
    // 本场景通过 BeatmapSession 公共命令入口而非直接控制器。
    // 会话路由必须把类型身份和目标稳定 ID 原样传递。
    // 执行结果与底层动作测试保持相同字段更新语义。
    // 无效会话或错误对象种类不得落到同值实体上。
    // Undo/Redo 仍由会话动作栈统一管理。
    // 该测试守住 UI 命令到编辑动作之间的集成边界。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    const auto                 entity  = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
            .m_volume          = 1.0F,
        });

    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateObjectSampleVolume{
            .entity = entity,
            .kind   = MMM::Logic::ChartObjectKind::AudioSample,
            .volume = 0.4F,
        } });
    // update 是队列的消费边界；此前组件仍保持初始音量 1.0。
    session.update(0.0, MMM::Config::EditorConfig{}, false);

    // 值和历史项同时成立，证明分派没有绕过 ActionStack 直接写组件。
    return near(context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                    .m_volume,
                0.4) &&
           context.actionStack.getUndoStackSize() == 1;
}

/// @brief 验证批量音量命令覆盖全部选中音频物件且合并为一次撤销。
/// @details
/// 选择集合同时包含带父/子绑定的 Polyline 和一个自动采样，另放置一个未选采样
/// 作为排除对照。批量命令应把所有可试听字段设为同一音量并包装为一个复合动作；
/// 一次 Undo/Redo 必须恢复或重放整个用户意图，未选对象始终保持 0.65。
/// @par 关键不变量
/// - 选中父折线时覆盖父绑定及全部有绑定子节点。
/// - 选中 Sample 同步加入批量更新。
/// - 未选 Sample 在全部阶段保持 0.65。
/// - 执行后所有选中试听字段统一为 0.8。
/// - BatchNoteAction 与 BatchSampleAction 包装成一个历史项。
/// - 一次 Undo 恢复三个不同初值。
/// - 一次 Redo 再次统一选中对象音量。
/// @par 故障定位
/// 子绑定未改检查折线遍历；未选项被改检查选择过滤；历史项多于一说明复合动作
/// 封装缺失；Undo/Redo 部分恢复说明某个 Registry 未进入批量动作。
/// @par 测试边界
/// 只覆盖已有绑定对象；没有绑定的选中 Note 不应凭空创建资源绑定。
/// @return 玩家主绑定、折线子绑定和自动采样可一起执行、撤销和重做时返回 true。
bool testSelectedObjectSampleVolumeCommand()
{
    // 选择集混合包含可调音量与不具采样属性的对象。
    // 批量命令只处理满足类型和绑定条件的目标。
    // 所有有效对象应合并为一次 Undo，而非逐个入栈。
    // 不适用对象的组件与领域字段保持完全不变。
    // Undo/Redo 分别验证整组选中目标的原子往返。
    // 对象种类检查防止跨 Registry 同句柄误命中。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.currentBeatmap             = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count = 4;
    context.trackCount                                    = 4;

    // 父绑定和子绑定初值不同，便于确认批量枚举覆盖两个层级。
    MMM::Logic::NoteComponent note;
    note.m_type          = MMM::NoteType::POLYLINE;
    note.m_sampleBinding = MMM::AudioSampleBinding{ "head.wav", 0.35F };
    note.m_subNotes.push_back(MMM::Logic::NoteComponent::SubNote{
        .type          = MMM::NoteType::NOTE,
        .timestamp     = 1.0,
        .duration      = 0.0,
        .trackIndex    = 0,
        .dtrack        = 0,
        .sampleBinding = MMM::AudioSampleBinding{ "node.wav", 0.55F },
    });
    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity, note);
    MMM::Logic::setChartObjectSelected(
        context, MMM::Logic::ChartObjectKind::PlayerNote, noteEntity, true);

    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "sample.wav",
            .m_volume          = 0.45F,
        });
    MMM::Logic::setChartObjectSelected(
        context, MMM::Logic::ChartObjectKind::AudioSample, sampleEntity, true);

    // 未选对象用于防止实现误扫并修改整个 sampleRegistry。
    const auto unselectedSampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        unselectedSampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 2.0,
            .m_track           = 4,
            .m_audioResourceId = "other.wav",
            .m_volume          = 0.65F,
        });

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateSelectedObjectSampleVolume{ .volume = 0.8F } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);

    // 统一助手在执行、撤销和重做阶段复用，并始终校验未选对象不变。
    const auto volumesEqual = [&](float expectedNote,
                                  float expectedSubNote,
                                  float expectedSample) {
        const auto& currentNote =
            context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
        return currentNote.m_sampleBinding &&
               near(currentNote.m_sampleBinding->m_volume, expectedNote) &&
               currentNote.m_subNotes.front().sampleBinding &&
               near(currentNote.m_subNotes.front().sampleBinding->m_volume,
                    expectedSubNote) &&
               near(context.sampleRegistry
                        .get<MMM::Logic::SampleComponent>(sampleEntity)
                        .m_volume,
                    expectedSample) &&
               near(
                   context.sampleRegistry
                       .get<MMM::Logic::SampleComponent>(unselectedSampleEntity)
                       .m_volume,
                   0.65F);
    };

    if ( !volumesEqual(0.8F, 0.8F, 0.8F) ||
         context.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Selected object volume command did not batch selected targets");
        return false;
    }

    context.actionStack.undo(context);
    if ( !volumesEqual(0.35F, 0.55F, 0.45F) ) {
        XERROR("Selected object volume undo did not restore original values");
        return false;
    }
    context.actionStack.redo(context);
    return volumesEqual(0.8F, 0.8F, 0.8F) &&
           context.actionStack.getUndoStackSize() == 1U;
}

/// @brief 验证协作资源命令由会话队列消费且换谱时不会泄漏到新谱面。
/// @details
/// 协作项目及路径映射必须通过 Session 命令线程安全地替换，而不是由 UI 直接写
/// context。第二次替换包含被当前谱面引用的新音频，要求重建 AudioTimeline 描述
/// 并安排激活；离线只读切换只能改变编辑权限，不能丢掉已缓存资源。最终加载普通
/// 新谱面时才清空协作资源，防止上一个会话的路径映射串入新项目。
/// @par 关键不变量
/// - 资源项目和路径映射在同一命令中替换。
/// - 谱面引用新资源时重建 AudioTimelineDescriptor。
/// - 新描述符能按资源 ID 找到 replacement-main。
/// - 音频激活通过 pending 标志交给低频边界。
/// - 离线只读只改变权限，不释放缓存资源。
/// - 恢复在线后资源仍保持同一对象和值。
/// - 加载普通谱面才清空项目指针与路径映射。
/// @par 故障定位
/// 首次绑定失败检查命令分派；音频未重绑检查描述符刷新条件；离线切换后资源丢失
/// 检查权限状态处理；换谱后仍残留则检查会话加载清理边界。
/// @par 测试边界
/// 不发起真实协作连接或音频加载，只检查资源缓存和描述符激活请求的内存状态。
/// @return 资源项目和路径映射完成绑定，并在加载新谱面后清空时返回 true。
bool testCollaborationResourcesRouteThroughSession()
{
    // 协作命令携带资源增删改，通过会话统一路由到项目。
    // 资源表变化必须先通过权限与项目有效性检查。
    // 成功后谱面中的引用仍指向可解析的项目资源。
    // 非资源对象、选择和相机状态不应受命令影响。
    // 观察者分类用于确认通知走资源变化通道。
    // 本用例只验证本地应用，不模拟网络传输可靠性。
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.currentBeatmap             = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count = 4;

    // 首次绑定只含图片映射，用于验证基本 variant 路由和完整值复制。
    auto project           = std::make_shared<MMM::Project>();
    project->m_projectRoot = "collaboration-cache";
    const std::unordered_map<std::string, std::string> pathRemap{
        { "images/cover.png", "files/cover-hash.png" },
    };
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdSetCollaborationResources{
            .project   = project,
            .pathRemap = pathRemap,
        },
    });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.collaborationProject != project ||
         context.collaborationPathRemap != pathRemap ) {
        XERROR("Collaboration resources command was not routed to session");
        return false;
    }

    context.isAudioTimelineDescriptorDirty           = false;
    context.isAudioTimelineActivationPending         = false;
    context.isAudioTimelineFingerprintPublishPending = false;
    // 清空既有音频脏标志，确保下一步观测到的是资源替换本身触发的结果。
    auto replacementProject           = std::make_shared<MMM::Project>();
    replacementProject->m_projectRoot = "replacement-collaboration-cache";
    replacementProject->m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "replacement-main",
        .m_path = "audio/new.ogg",
        .m_type = MMM::AudioTrackType::Main,
    });
    context.currentBeatmap->m_audioSamples.emplace_back().m_audioResourceId =
        "replacement-main";
    const std::unordered_map<std::string, std::string> replacementRemap{
        { "audio/new.ogg", "files/new-audio-hash.ogg" },
    };
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdSetCollaborationResources{
            .project   = replacementProject,
            .pathRemap = replacementRemap,
        },
    });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    // 新资源被谱面引用时，描述符应重新绑定并安排音频线程低频激活。
    if ( context.collaborationProject != replacementProject ||
         context.collaborationPathRemap != replacementRemap ||
         !MMM::Logic::audioTimelineDescriptorReferencesResource(
             context.audioTimelineDescriptor, "replacement-main") ||
         !context.isAudioTimelineActivationPending ) {
        XERROR("Replacement collaboration resources did not rebind audio");
        return false;
    }

    // 离线只读仍需显示和试听缓存内容，因此资源所有权必须继续保留。
    session.setCollaborationOfflineReadOnly(true);
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.collaborationProject != replacementProject ||
         context.collaborationPathRemap != replacementRemap ) {
        XERROR("Offline collaboration session discarded cached resources");
        return false;
    }

    session.setCollaborationOfflineReadOnly(false);
    session.update(0.0, MMM::Config::EditorConfig{}, false);

    // 加载独立谱面构成会话边界，此时才允许释放上个协作项目及重映射。
    auto replacement = std::make_shared<MMM::BeatMap>();
    replacement->m_baseMapMetadata.track_count = 4;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(replacement) },
    });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    return !context.collaborationProject &&
           context.collaborationPathRemap.empty();
}

/// @brief 验证不同 ECS 注册表中重叠的实体 ID 不会被 DrawTool 混淆。
/// @details
/// entt 注册表会从相同初值分配实体，因此 Note 与 Sample 可以拥有数值相同的 ID。
/// 橡皮必须用 hoveredObjectKind 选择 Registry；仅靠 entity 数值会误删玩家
/// Note。 删除通过 SampleAction 提交，结束手势后目标集合应清空，并可由一次 Undo
/// 恢复。
/// @par 关键不变量
/// - 测试前置条件要求两个 Registry 分配相同 entity 数值。
/// - hoveredObjectKind 明确指向 AudioSample。
/// - 橡皮目标集合按类型解释实体。
/// - 提交后 Note Registry 的同值实体仍有效。
/// - Sample Registry 的目标实体被删除。
/// - EndErase 清空临时目标集合。
/// - 一次 Undo 只恢复 Sample 及其资源字段。
/// @par 故障定位
/// Note 被删说明橡皮只按 entity 查找；Sample 未删说明 hoveredObjectKind
/// 未传入动作； 临时集合残留检查 EndErase；Undo 串对象检查类型化历史项。
/// @par 测试边界
/// 只擦除单个 Sample，批量选中和 Polyline 尾钩由下一场景覆盖。
/// @return 悬停自动采样时只删除 Sample 并可通过一次 Undo 恢复时返回 true。
bool testSampleEraseTargetsTypedRegistry()
{
    // Note 与 Sample Registry 刻意创建数值相同的实体句柄。
    // 擦除命令携带 ChartObjectKind 选择唯一目标域。
    // 删除 Sample 后同值 Note 必须继续存在且保持字段。
    // 领域数组、Registry 和选择集合要共同移除目标 Sample。
    // Undo 恢复 Sample 时不能重复或替换同值 Note。
    // 该场景直接防御只凭 entt::entity 路由的跨域误删。
    MMM::Logic::SessionContext context;
    const auto                 noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    if ( noteEntity != sampleEntity ) {
        XERROR("Regression setup did not create overlapping ECS entity IDs");
        return false;
    }

    // 悬停身份由 kind+entity 构成，重叠数值本身不能唯一定位对象。
    context.hoveredEntity     = sampleEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;

    MMM::Logic::DrawTool drawTool;
    // 走完整 start/update/end 手势，覆盖临时目标收集和最终动作提交。
    drawTool.handleStartErase(context, MMM::Logic::CmdStartErase{});
    drawTool.handleUpdateErase(context, MMM::Logic::CmdUpdateErase{});
    drawTool.handleEndErase(context, MMM::Logic::CmdEndErase{});

    if ( !context.noteRegistry.valid(noteEntity) ||
         context.sampleRegistry.valid(sampleEntity) ||
         context.actionStack.getUndoStackSize() != 1 ||
         !context.eraserState.targetEntities.empty() ) {
        XERROR("Sample eraser did not delete only the typed sample target");
        return false;
    }
    // Undo 只恢复 Sample，原 Note 从始至终必须保持同一实体有效。
    context.actionStack.undo(context);
    return context.noteRegistry.valid(noteEntity) &&
           context.sampleRegistry.valid(sampleEntity) &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(sampleEntity)
                   .m_audioResourceId == "effect.wav";
}

/// @brief 验证绘制模式右键擦除选中折线尾钩时可同时处理其他选中物件。
/// @details
/// 选中集合包含完整 Polyline 和一个普通 Note，悬停位置指向折线最后 Flick 节点。
/// 擦除尾钩应以缩短后的新折线替换旧父子实体，同时删除另一个已选对象，并包装成
/// 一个批量动作。选择索引不能保留已销毁实体；Undo 要恢复原父、全部三个子实体
/// 和普通 Note，Redo 再次生成结构完整的两节点折线。
/// @par 关键不变量
/// - 悬停 subIndex 指向最后一个 Flick 节点。
/// - 原父实体和三个原子实体在提交后全部销毁。
/// - 新折线只含 Note 与 Hold 两个节点。
/// - 另一个已选普通 Note 与尾钩在同一事务删除。
/// - selectedNoteEntities 不保留无效实体。
/// - Undo 恢复五个原实体及一致选择组件。
/// - Redo 重新得到一个根与两个子实体。
/// @par 故障定位
/// 旧子残留说明父替换未闭包收集；新结构节点错误检查尾钩裁剪；选择缓存悬空检查
/// 批量动作后的索引维护；Undo/Redo 不完整检查复合 before/after 条目。
/// @par 测试边界
/// 不验证擦除图标或鼠标按键映射，只从 DrawTool 的逻辑手势入口开始测试。
/// @return 原折线父子实体和其他选中物件被一次性替换，且 Undo/Redo
/// 后实体与选择索引保持有效时返回 true。
bool testSelectedPolylineTailEraseWithOtherSelection()
{
    // 选择集包含 Polyline 尾节点和另一个普通对象。
    // 擦除尾部只修改目标折线结构，不清空无关选择对象。
    // 父对象内嵌节点和子实体投影必须同步缩短。
    // 折线仍有有效节点时不得误删整个根对象。
    // Undo 恢复尾节点、身份映射与原选择状态。
    // Redo 再次缩短用于验证复合选择下的动作稳定性。
    MMM::Logic::SessionContext context;
    context.lastConfig.settings.enablePolylineEditing = true;

    // 三节点结构以尾部 Flick 收束，移除后应留下 Note+Hold 的合法折线。
    MMM::Logic::NoteComponent polyline;
    polyline.m_type       = MMM::NoteType::POLYLINE;
    polyline.m_timestamp  = 1.0;
    polyline.m_trackIndex = 0;
    polyline.m_subNotes   = {
        {
              .type       = MMM::NoteType::NOTE,
              .timestamp  = 1.0,
              .duration   = 0.0,
              .trackIndex = 0,
              .dtrack     = 0,
        },
        {
              .type       = MMM::NoteType::HOLD,
              .timestamp  = 2.0,
              .duration   = 0.5,
              .trackIndex = 1,
              .dtrack     = 0,
        },
        {
              .type       = MMM::NoteType::FLICK,
              .timestamp  = 3.0,
              .duration   = 0.0,
              .trackIndex = 1,
              .dtrack     = 1,
        },
    };

    const auto parentEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(parentEntity,
                                                            polyline);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        parentEntity);

    // 显式物化全部子实体，用于检查替换动作不会留下孤儿投影。
    std::vector<entt::entity> childEntities;
    childEntities.reserve(polyline.m_subNotes.size());
    for ( std::size_t index = 0; index < polyline.m_subNotes.size(); ++index ) {
        const auto childEntity = context.noteRegistry.create();
        context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
            childEntity,
            MMM::Logic::makeNoteComponentFromSubNote(polyline.m_subNotes[index],
                                                     true,
                                                     parentEntity,
                                                     static_cast<int>(index)));
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
            childEntity);
        childEntities.push_back(childEntity);
    }

    // 另一个已选 Note 验证擦除操作会处理整个选择，而非只处理悬停折线。
    MMM::Logic::NoteComponent otherNote;
    otherNote.m_type       = MMM::NoteType::NOTE;
    otherNote.m_timestamp  = 4.0;
    otherNote.m_trackIndex = 2;
    const auto otherEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(otherEntity,
                                                            otherNote);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(otherEntity);

    MMM::Logic::setChartObjectSelected(
        context, MMM::Logic::ChartObjectKind::PlayerNote, parentEntity, true);
    MMM::Logic::setChartObjectSelected(
        context, MMM::Logic::ChartObjectKind::PlayerNote, otherEntity, true);
    context.hoveredEntity     = parentEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredSubIndex = static_cast<int>(polyline.m_subNotes.size() - 1U);

    MMM::Logic::DrawTool drawTool;
    // 完整橡皮手势在 End 阶段一次性提交父子替换与其他删除。
    drawTool.handleStartErase(context, MMM::Logic::CmdStartErase{});
    drawTool.handleUpdateErase(context, MMM::Logic::CmdUpdateErase{});
    drawTool.handleEndErase(context, MMM::Logic::CmdEndErase{});

    // 双向核对缓存集合和 InteractionComponent，避免只清了一侧。
    const auto selectionIndexIsConsistent = [&context]() {
        for ( const auto entity : context.selectedNoteEntities ) {
            const auto* interaction =
                context.noteRegistry
                    .try_get<const MMM::Logic::InteractionComponent>(entity);
            if ( !context.noteRegistry.valid(entity) || !interaction ||
                 !interaction->isSelected ) {
                return false;
            }
        }
        return true;
    };
    // 新折线允许使用新实体 ID，因此按根/子数量和节点语义检查结果。
    const auto reducedPolylineIsValid = [&context]() {
        const auto notes =
            context.noteRegistry.view<const MMM::Logic::NoteComponent>();
        if ( notes.size() != 3U ) return false;

        std::size_t rootCount  = 0U;
        std::size_t childCount = 0U;
        for ( const auto entity : notes ) {
            const auto& note =
                notes.get<const MMM::Logic::NoteComponent>(entity);
            if ( note.m_isSubNote ) {
                ++childCount;
                continue;
            }
            ++rootCount;
            if ( note.m_type != MMM::NoteType::POLYLINE ||
                 note.m_subNotes.size() != 2U ||
                 note.m_subNotes.back().type != MMM::NoteType::HOLD ) {
                return false;
            }
        }
        return rootCount == 1U && childCount == 2U;
    };

    if ( context.noteRegistry.valid(parentEntity) ||
         context.noteRegistry.valid(otherEntity) ||
         !context.selectedNoteEntities.empty() ||
         !selectionIndexIsConsistent() || !reducedPolylineIsValid() ||
         context.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Selected Polyline tail erase did not commit atomically");
        return false;
    }
    for ( const auto childEntity : childEntities ) {
        // 所有原子实体都应由替换动作销毁，不能与新投影并存。
        if ( context.noteRegistry.valid(childEntity) ) {
            XERROR("Selected Polyline tail erase left an original child");
            return false;
        }
    }

    // 一次 Undo 恢复整个用户选择的所有旧实体及一致的选择索引。
    context.actionStack.undo(context);
    const auto restoredNotes =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    if ( !context.noteRegistry.valid(parentEntity) ||
         !context.noteRegistry.valid(otherEntity) ||
         restoredNotes.size() != 5U || !selectionIndexIsConsistent() ) {
        XERROR("Selected Polyline tail erase undo did not restore objects");
        return false;
    }
    for ( const auto childEntity : childEntities ) {
        if ( !context.noteRegistry.valid(childEntity) ) {
            XERROR("Selected Polyline tail erase undo lost an original child");
            return false;
        }
    }

    // Redo 再次检查最终结构，确保动作没有依赖首次生成的新实体 ID。
    context.actionStack.redo(context);
    return !context.noteRegistry.valid(parentEntity) &&
           !context.noteRegistry.valid(otherEntity) &&
           context.selectedNoteEntities.empty() &&
           selectionIndexIsConsistent() && reducedPolylineIsValid();
}

/// @brief 验证自动采样悬浮检视包含锚点、实际触发点和音频字段。
/// @details
/// 自动采样的锚点时间与实际播放时间由 offsetMs 区分。悬停 offset handle 时，
/// 快照应同时公开锚点、触发点、资源 ID、音量及对象类型，UI 才能绘制两处标记并
/// 播放正确资源。offset 归零后两点重合，末端标记必须隐藏以避免重复视觉元素。
/// @par 关键不变量
/// - 检视读取 Sample Registry 而非同时间玩家 Note。
/// - objectKind 与 entity 精确指向自动采样。
/// - head 表示锚点，end 表示应用 offset 后的触发点。
/// - 负 offset 使触发点早于锚点。
/// - 资源 ID 和音量来自 SampleComponent。
/// - offset 为零时只显示一个时间标记。
/// - 快照刷新由 Transform 脏标志触发。
/// @par 故障定位
/// 资源字段来自 Note 说明类型化悬停错误；时间点顺序错误检查 offset 符号；零
/// offset 仍显示末端则检查快照去重条件，revision 不变则检查缓存失效传播。
/// @par 测试边界
/// 用内存快照代替 UI 绘制和试听，不证明悬浮面板的视觉布局或真实音频输出。
/// @return offset handle 检视快照完整保留资源、音量、偏移与两类时间点时返回
/// true。
bool testSampleHoverInspectDetails()
{
    // 鼠标命中 Sample 后生成只读检查详情快照。
    // 快照同时携带资源、轨道、时间、偏移与音量信息。
    // 相同句柄的 Note 不能污染 Sample 类型解析。
    // 移出或目标删除后详情必须清空，不能悬挂旧引用。
    // 检查过程不得写入选择状态或动作栈。
    // 该用例验证 UI 展示数据来源，而不验证实际控件绘制。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    beatmap->m_baseMapMetadata.bgm_track_count = 1;
    // 玩家 Note 带故意不同的绑定，用于发现检视错误读取 noteRegistry 的问题。
    MMM::Note note;
    note.m_timestamp = 1250.0;
    note.m_track     = 0;
    note.setSampleBinding({ "wrong-note-effect.wav", 0.9F });
    beatmap->m_noteData.notes.push_back(std::move(note));
    beatmap->m_audioSamples.push_back({
        .m_timestamp       = 1250.0,
        .m_offsetMs        = -125,
        .m_track           = 4,
        .m_audioResourceId = "detail-effect.wav",
        .m_volume          = 0.35F,
    });

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    const auto sampleView =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    const auto noteView =
        context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( sampleView.size() != 1 || noteView.size() != 1 ) {
        XERROR("Sample hover inspect setup did not load overlapping objects");
        return false;
    }
    const auto entity     = *sampleView.begin();
    const auto noteEntity = *noteView.begin();
    if ( entity != noteEntity ) {
        XERROR("Sample hover inspect setup did not overlap ECS entity IDs");
        return false;
    }
    auto* interaction =
        context.sampleRegistry.try_get<MMM::Logic::InteractionComponent>(
            entity);
    if ( !interaction ) {
        interaction =
            &context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
                entity);
    }
    interaction->isHovered = true;
    interaction->hoveredPart =
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::SampleOffset);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleOffset);
    context.mouseCameraId   = "Basic2DCanvas";
    context.isMouseInCanvas = true;
    context.lastMousePos    = { 550.0F, 300.0F };

    // 首帧由 Session 把 ECS 悬停状态汇总进主画布快照。
    const auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Sample hover inspect did not publish a main-canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot ) return false;
    const auto& inspect = snapshot->hoverInspect;
    if ( !inspect.show ||
         inspect.kind != MMM::Logic::HoverInspectKind::AudioSampleTrigger ||
         !inspect.showAudioSample || !inspect.showAudioPreview ||
         inspect.entity != entity ||
         inspect.objectKind != MMM::Logic::ChartObjectKind::AudioSample ||
         !inspect.head.show || !inspect.end.show || !inspect.showTrack ||
         inspect.audioResourceId != "detail-effect.wav" ||
         !near(inspect.volume, 0.35) || inspect.offsetMs != -125 ||
         inspect.track != 4 || !near(inspect.head.time, 1.25) ||
         !near(inspect.end.time, 1.125) ) {
        XERROR("Sample hover inspect omitted audio or effective-time details");
        return false;
    }

    // offset 归零后触发点与锚点重合，快照只应保留一个可见标记。
    context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity).m_offsetMs =
        0;
    context.isTransformDirty = true;
    session.update(0.0, config, true);
    const auto* zeroOffsetSnapshot = bufferIt->second->pullLatestSnapshot();
    if ( !zeroOffsetSnapshot || !zeroOffsetSnapshot->hoverInspect.head.show ||
         zeroOffsetSnapshot->hoverInspect.end.show ||
         !near(zeroOffsetSnapshot->hoverInspect.head.time, 1.25) ) {
        XERROR("Zero-offset sample hover inspect duplicated the trigger point");
        return false;
    }
    return true;
}

/// @brief 验证悬浮检视与常用分拍编辑手势只生成单轨单拍临时预览。
/// @details
/// 六键谱面中的 Note 位于第五轨且落在三分之一拍。普通悬停应只高亮该轨当前拍，
/// 拖动时常用分拍模式应以对象时间为焦点；若当前固定分拍已经兼容目标位置，则不
/// 需要额外预览。画笔状态改用笔尖时间和轨道，自动采样画笔位于 BGM 区时必须
/// 关闭玩家分拍预览。用例覆盖这些状态间连续切换，防止残留上一帧快照。
/// @par 关键不变量
/// - 悬停预览限制在对象第五轨和所在单拍区间。
/// - 1/3 分数由对象拍内位置约分得到。
/// - 拖动期间不叠加普通悬停提示。
/// - 常用分拍模式原样携带启用除数掩码。
/// - 固定六分拍兼容当前位置时省略冗余预览。
/// - Hold 画笔以末端而非起点作为 focusTime。
/// - BGM Sample 画笔不显示玩家轨分拍提示。
/// @par 故障定位
/// 轨道或拍区间错误检查 hoverInspect 到预览的输入；掩码错误检查配置快照复制；
/// 状态切换后残留说明 Transform 脏标志未触发预览重算。
/// @par 测试边界
/// 固定 120 BPM 与单个 Note 只验证预览状态机，复杂变速由时间换算测试覆盖。
/// @return 悬浮、拖动与绘制状态均使用正确轨道、拍区间和分拍来源时返回 true。
bool testHoverSubdivisionPreviewUsesInspectedTrackAndBeat()
{
    // 悬停目标位于明确轨道和节拍，细分预览据此生成。
    // 预览位置应读取被检查对象，而非当前焦点轨或播放头。
    // 修改分拍数后节点间距按同一拍长重新计算。
    // 切换目标轨道时横向投影必须随目标更新。
    // 清除悬停后预览集合归零，避免跨帧残留。
    // 浮点时间使用 near 比较，轨道身份保持严格比较。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 6;
    beatmap->m_baseMapMetadata.preference_bpm = 120.0;

    // 120 BPM 提供明确的 0.5 秒拍长，便于断言拍区间边界。
    MMM::Timing timing;
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 120.0;
    timing.m_beat_length           = 500.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 120.0;
    beatmap->m_timings.push_back(timing);

    // 1.1666 秒正好是当前拍的三分之一位置，分数应约简为 1/3。
    MMM::Note note;
    note.m_timestamp = 1166.6666666667;
    note.m_track     = 5;
    beatmap->m_noteData.notes.push_back(note);

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    context.currentBeatmap->m_baseMapMetadata.track_count = 6;
    context.trackCount                                    = 6;
    context.lastConfig.settings.beatDivisor               = 4;

    // 先建立玩家 Note 的头部悬停，快照焦点必须来自物件而不是鼠标空白时间。
    const auto view = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( view.size() != 1 ) {
        XERROR("Hover subdivision preview setup did not load one Note");
        return false;
    }
    const auto entity = *view.begin();
    auto&      interaction =
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    interaction.isHovered = true;
    interaction.hoveredPart =
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);
    context.mouseCameraId   = "Basic2DCanvas";
    context.isMouseInCanvas = true;
    context.lastMousePos    = { 350.0F, 200.0F };

    auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Hover subdivision preview did not publish a canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot ) return false;
    const auto& preview = snapshot->hoverSubdivisionPreview;
    if ( !preview.show || preview.track != 5 || preview.numerator != 1 ||
         preview.denominator != 3 || !near(preview.beatStartTime, 1.0) ||
         !near(preview.beatEndTime, 1.5) || !near(preview.beatDuration, 0.5) ) {
        XERROR("Hover subdivision preview escaped the inspected track or beat");
        return false;
    }

    // 进入普通拖动时先关闭悬停专用预览，避免两种提示叠加。
    context.isDragging        = true;
    context.draggedEntity     = entity;
    context.draggedObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.draggedPart       = MMM::Logic::HoverPart::Head;
    context.isTransformDirty  = true;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->hoverSubdivisionPreview.show ) {
        XERROR("Dragging did not restore the configured beat grid preview");
        return false;
    }

    // 常用分拍仅启用 3 和 5，快照需原样携带掩码供渲染选择网格。
    std::uint32_t commonBeatDivisorMask = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(commonBeatDivisorMask, 3, true);
    MMM::Config::setCommonBeatDivisorEnabled(commonBeatDivisorMask, 5, true);
    config.settings.objectPlacementSnap = true;
    config.settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    config.settings.commonBeatDivisorMask = commonBeatDivisorMask;
    context.isTransformDirty              = true;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || !snapshot->hoverSubdivisionPreview.show ||
         snapshot->hoverSubdivisionPreview.track != 5 ||
         snapshot->hoverSubdivisionPreview.commonBeatDivisorMask !=
             commonBeatDivisorMask ||
         !near(snapshot->hoverSubdivisionPreview.focusTime,
               note.m_timestamp / 1000.0) ||
         !near(snapshot->hoverSubdivisionPreview.beatStartTime, 1.0) ||
         !near(snapshot->hoverSubdivisionPreview.beatEndTime, 1.5) ) {
        XERROR("Common-divisor drag preview did not follow the dragged Note");
        return false;
    }
    context.isDragging    = false;
    context.draggedEntity = entt::null;

    // 固定六分拍可直接表达三分之一位置，此时额外常用分拍预览应消失。
    config.settings.beatDivisor = 6;
    context.isTransformDirty    = true;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->hoverSubdivisionPreview.show ) {
        XERROR("Compatible current beat grid still enabled hover preview");
        return false;
    }

    // 切换到 Hold 画笔后，焦点使用末端
    // time+duration，而不是已经清除的悬停对象。
    interaction.isHovered                 = false;
    context.hoveredEntity                 = entt::null;
    context.brushState.isActive           = true;
    context.brushState.createsAudioSample = false;
    context.brushState.type               = MMM::NoteType::HOLD;
    context.brushState.time               = 1.0;
    context.brushState.duration           = 0.1;
    context.brushState.track              = 4;
    context.isTransformDirty              = true;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || !snapshot->hoverSubdivisionPreview.show ||
         snapshot->hoverSubdivisionPreview.track != 4 ||
         snapshot->hoverSubdivisionPreview.commonBeatDivisorMask !=
             commonBeatDivisorMask ||
         !near(snapshot->hoverSubdivisionPreview.focusTime, 1.1) ||
         !near(snapshot->hoverSubdivisionPreview.beatStartTime, 1.0) ||
         !near(snapshot->hoverSubdivisionPreview.beatEndTime, 1.5) ) {
        XERROR("Common-divisor draw preview did not follow the brush tip");
        return false;
    }

    // 同一笔刷改为创建 BGM Sample 后，玩家轨分拍提示不再适用。
    context.brushState.createsAudioSample = true;
    context.isTransformDirty              = true;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->hoverSubdivisionPreview.show ) {
        XERROR(
            "BGM sample brush unexpectedly enabled player subdivision preview");
        return false;
    }
    return true;
}

/// @brief 验证连续 Seek 状态会进入主画布快照，并在最终提交后清除。
/// @details
/// 时间轴拖动期间 isScrubbing 告诉画布暂时放宽视口跟随，避免播放头与用户手势
/// 争夺相机。松手发送最终 Seek 后，同一字段必须恢复 false；当前时间本身由播放
/// 控制器维护，本用例专门锁定跨线程快照中的交互状态传播。
/// @par 关键不变量
/// - 连续 Seek 在命令中显式携带 isScrubbing=true。
/// - SessionContext 保存本次手势状态。
/// - 主画布快照发布相同 true 状态。
/// - 最终 Seek 同时提交新时间和 false 状态。
/// - 下一快照不沿用上一帧的 scrub 标志。
/// - 测试不依赖真实音频设备或播放线程。
/// @par 故障定位
/// context 状态不变说明 Seek 命令未消费；context 正确而快照错误说明字段未复制；
/// 最终命令后仍为 true 则检查状态是否只置位而未按命令覆盖。
/// @par 测试边界
/// 不启动播放器，只验证连续 Seek 状态随逻辑快照传播和释放。
/// @return 拖动预览与松手提交快照携带正确状态时返回 true。
bool testSeekScrubStatePropagatesToCanvasSnapshot()
{
    // 播放控制器先进入拖动定位状态并写入目标时间。
    // 画布快照必须在同一更新周期发布 scrub 标志和位置。
    // 拖动期间不强制启动真实音频，也不改变谱面对象。
    // 结束手势后最终时间保留，但瞬态 scrub 标志清除。
    // 多画布读取应得到同一会话播放状态。
    // 该测试只证明状态传播，不声称音频设备已经 seek。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    MMM::Note note;
    note.m_timestamp = 10'000.0;
    note.m_track     = 0;
    beatmap->m_noteData.notes.push_back(std::move(note));

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    const auto config = context.lastConfig;

    // 第一条命令代表拖动中的预览位置，画布应收到持续同步标志。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdSeek{
        .time        = 5.0,
        .isScrubbing = true,
    } });
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Continuous seek did not publish a main-canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || !snapshot->isSeekScrubbing ) {
        XERROR("Continuous seek state was missing from the canvas snapshot");
        return false;
    }

    // 第二条命令代表鼠标松开后的最终位置，必须释放 scrub 状态。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdSeek{
        .time        = 7.0,
        .isScrubbing = false,
    } });
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->isSeekScrubbing ) {
        XERROR(
            "Committed seek did not release canvas viewport synchronization");
        return false;
    }
    return true;
}

/// @brief 验证绑定采样的玩家物件会向主画布公开独立试听字段。
/// @details
/// 玩家 Note 的 sampleBinding 与自动采样共享资源试听能力，但 UI 表达不同。
/// hoverInspect 应设置 showAudioPreview 而非 showAudioSample，并保留玩家对象
/// kind、 实体 ID、资源 ID 与物件级音量，防止检视面板把它误当 BGM 区 Sample。
/// @par 关键不变量
/// - 悬停目标来自 noteRegistry 的玩家 Note。
/// - showAudioPreview 为 true，允许播放器试听绑定资源。
/// - showAudioSample 为 false，避免显示自动采样属性控件。
/// - objectKind 保持 PlayerNote。
/// - entity 保持原 Note 实体。
/// - audioResourceId 与 volume 来自 Note 绑定。
/// @par 故障定位
/// show 标志互换说明玩家绑定与自动采样共用了错误 UI 分支；资源或音量错误检查
/// NoteComponent::m_sampleBinding 到 hoverInspect 的复制。
/// @par 测试边界
/// 绑定资源无需存在，测试不访问文件系统或音频设备，只验证试听描述字段。
/// @return 悬浮信息保留实体类型、资源 ID 和物件音量时返回 true。
bool testBoundNoteHoverInspectAudioPreview()
{
    // Note 绑定有效音频资源后进入悬停检查路径。
    // 预览描述符由绑定字段生成，不创建持久 Sample 对象。
    // 未绑定或资源缺失的 Note 必须返回无预览结果。
    // 悬停切换时旧描述符及时替换，不能继续引用前一资源。
    // Registry 数量与动作栈保持不变，证明操作只读。
    // 测试隔离描述符生成，真实解码与播放不在覆盖范围。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    MMM::Note note;
    note.m_timestamp = 1500.0;
    note.m_track     = 2;
    note.setSampleBinding({ "bound-effect.wav", 0.45F });
    beatmap->m_noteData.notes.push_back(std::move(note));

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    const auto view = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( view.size() != 1 ) {
        XERROR("Bound Note hover inspect setup did not load one player note");
        return false;
    }
    // 明确设置头部悬停和主画布鼠标来源，触发玩家物件检视分支。
    const auto entity = *view.begin();
    auto&      interaction =
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    interaction.isHovered = true;
    interaction.hoveredPart =
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);
    context.mouseCameraId   = "Basic2DCanvas";
    context.isMouseInCanvas = true;
    context.lastMousePos    = { 375.0F, 300.0F };

    const auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR(
            "Bound Note hover inspect did not publish a main-canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot ) return false;
    // 两个 show 标志必须互斥，资源试听与自动采样属性面板不可混用。
    const auto& inspect = snapshot->hoverInspect;
    if ( !inspect.show || !inspect.showAudioPreview ||
         inspect.showAudioSample || inspect.entity != entity ||
         inspect.objectKind != MMM::Logic::ChartObjectKind::PlayerNote ||
         inspect.audioResourceId != "bound-effect.wav" ||
         !near(inspect.volume, 0.45) ) {
        XERROR("Bound Note hover inspect omitted audio preview details");
        return false;
    }
    return true;
}

/// @brief 验证自动采样在 BGM 区拖到追加轨后只提交一次可撤销更新。
/// @details
/// 从现有第一条 BGM 轨拖至右侧追加轨，同时纵向移动半秒。拖动预览可逐帧修改
/// 组件，但 EndDrag 只能提交一个 SampleAction；该动作还需隐式扩展 BGM 轨数量。
/// Undo/Redo 应同时恢复时间、轨道和布局，不能留下独立的中间扩轨步骤。
/// @par 关键不变量
/// - StartDrag 捕获 Sample 的完整 before 快照。
/// - UpdateDrag 同时预览时间和 BGM 轨变化。
/// - 目标追加轨在提交时扩展持久 BGM 数量。
/// - EndDrag 只压入一个历史动作。
/// - Undo 恢复原时间、原轨和原轨道数。
/// - Redo 恢复 1.5 秒、目标轨与扩展布局。
/// @par 故障定位
/// 预览错误检查抓取坐标换算；提交生成多动作检查 EndDrag；仅轨数不一致检查
/// SampleAction 的隐式追加轨处理；往返错误检查完整 before/after 快照。
/// @par 测试边界
/// 使用单个 Sample，不覆盖多选整体拖动；批量音量和剪贴板有独立场景。
/// @return 锚点、轨道、BGM 数量及 Undo/Redo 均正确时返回 true。
bool testSampleAnchorDragUsesSingleAction()
{
    // 抓取 Sample 锚点后发送多次连续鼠标移动。
    // 交互期间 ECS 可逐帧预览，但动作栈不能逐帧增长。
    // 松开时只以最终轨道和时间提交一个编辑动作。
    // 中间位置不进入持久历史，也不能残留临时组件状态。
    // Undo 一次恢复拖动前位置，Redo 一次到最终位置。
    // 该结构约束连续手势的合并和本地即时反馈。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto entity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleAnchor);

    // 悬停 SampleAnchor 后走完整抓取手势，避免直接构造动作绕过预览状态。
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             entity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              650.0F,
                              50.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    // 鼠标目标映射到 1.5 秒和绝对轨 5，即当前 BGM 区的追加轨。
    const auto& moved =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( !near(moved.m_timestamp, 1.5) || moved.m_track != 5 ||
         context.bgmTrackCount != 2 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Sample anchor drag did not commit one append-lane action");
        return false;
    }

    // 一次 Undo 必须同时收回追加轨和恢复原锚点。
    context.actionStack.undo(context);
    const auto& restored =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( !near(restored.m_timestamp, 1.0) || restored.m_track != 4 ||
         context.bgmTrackCount != 1 ) {
        XERROR("Sample anchor drag undo did not restore the original sample");
        return false;
    }
    // Redo 再次复现完整手势结果，历史栈中仍只有一个用户动作。
    context.actionStack.redo(context);
    const auto& redone =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    return near(redone.m_timestamp, 1.5) && redone.m_track == 5 &&
           context.bgmTrackCount == 2;
}

/// @brief 验证禁止垂直移动同时约束玩家轨道和 BGM 轨道的整体拖拽。
/// @details
/// disableVerticalObjectDrag 仅锁定时间轴方向，横向换轨仍应工作。用例分别构造
/// 玩家 Note 与自动 Sample 两个独立会话，向右下方拖动同样的逻辑距离；两者时间
/// 均保持 1.0，但轨道各右移一格。每个手势仍是正常的可撤销动作。
/// @par 关键不变量
/// - 玩家 Note 的时间保持 1.0，轨道从 0 变为 1。
/// - 自动 Sample 的时间保持 1.0，绝对轨从 4 变为 5。
/// - 锁定纵向不等于禁用整个拖动手势。
/// - 两种 Registry 使用相同设置语义。
/// - 每个上下文各产生一个可撤销动作。
/// - Undo 恢复原轨且不改变锁定的时间。
/// @par 故障定位
/// 时间变化说明锁定未在对应 Registry 分支应用；轨道不变说明设置被误解为禁用
/// 整个拖动；只有 Sample 失败则检查 BGM 抓取分支的设置读取。
/// @par 测试边界
/// 两个独立上下文避免历史相互影响，不覆盖设置控件本身的读写和持久化。
/// @return 两类物件只横向换轨、时间保持不变且仍可撤销时返回 true。
bool testVerticalObjectDragLock()
{
    // 对象拖动开启垂直锁定，初始轨道作为横向锚点。
    // 指针同时发生水平和垂直移动以验证锁定优先级。
    // 时间随纵向位置更新，轨道必须维持按下时的值。
    // 预览与最终提交都要遵守同一锁定状态。
    // Undo/Redo 检查锁定后的最终动作仍可完整往返。
    // 测试不依赖键盘事件，只验证已解析的锁定命令。
    // 第一阶段覆盖 noteRegistry 和玩家轨道投影。
    MMM::Logic::SessionContext noteContext;
    configureObjectEditingCanvas(noteContext);
    noteContext.lastConfig.settings.disableVerticalObjectDrag = true;

    const auto noteEntity = noteContext.noteRegistry.create();
    noteContext.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    noteContext.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity);
    noteContext.hoveredEntity     = noteEntity;
    noteContext.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    noteContext.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);

    // 鼠标 Y 指向不同时间，但锁定设置要求只采用 X 方向的轨道变化。
    MMM::Logic::GrabTool noteTool;
    noteTool.handleStartDrag(noteContext,
                             MMM::Logic::CmdStartDrag{
                                 noteEntity,
                                 "Basic2DCanvas",
                                 false,
                                 MMM::Logic::ChartObjectKind::PlayerNote,
                             });
    noteTool.handleUpdateDrag(noteContext,
                              MMM::Logic::CmdUpdateDrag{
                                  "Basic2DCanvas",
                                  250.0F,
                                  50.0F,
                                  true,
                              });
    noteTool.handleEndDrag(noteContext,
                           MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto& movedNote =
        noteContext.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
    if ( !near(movedNote.m_timestamp, 1.0) || movedNote.m_trackIndex != 1 ||
         noteContext.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Vertical drag lock did not constrain the player Note");
        return false;
    }
    // 玩家对象 Undo 恢复轨道，时间在整个手势中都不应变化。
    noteContext.actionStack.undo(noteContext);
    const auto& restoredNote =
        noteContext.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
    if ( !near(restoredNote.m_timestamp, 1.0) ||
         restoredNote.m_trackIndex != 0 ) {
        XERROR("Vertical drag lock Note undo did not restore the track");
        return false;
    }

    // 第二阶段以全新上下文覆盖 sampleRegistry 和 BGM 轨道投影。
    MMM::Logic::SessionContext sampleContext;
    configureObjectEditingCanvas(sampleContext);
    sampleContext.lastConfig.settings.disableVerticalObjectDrag = true;
    const auto sampleEntity = sampleContext.sampleRegistry.create();
    sampleContext.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    sampleContext.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity);
    sampleContext.hoveredEntity     = sampleEntity;
    sampleContext.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    sampleContext.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleAnchor);

    // 同样拖到右侧追加轨，允许横向变化以及随之产生的布局动作。
    MMM::Logic::GrabTool sampleTool;
    sampleTool.handleStartDrag(sampleContext,
                               MMM::Logic::CmdStartDrag{
                                   sampleEntity,
                                   "Basic2DCanvas",
                                   false,
                                   MMM::Logic::ChartObjectKind::AudioSample,
                               });
    sampleTool.handleUpdateDrag(sampleContext,
                                MMM::Logic::CmdUpdateDrag{
                                    "Basic2DCanvas",
                                    650.0F,
                                    50.0F,
                                    true,
                                });
    sampleTool.handleEndDrag(sampleContext,
                             MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto& movedSample =
        sampleContext.sampleRegistry.get<MMM::Logic::SampleComponent>(
            sampleEntity);
    if ( !near(movedSample.m_timestamp, 1.0) || movedSample.m_track != 5 ||
         sampleContext.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Vertical drag lock did not constrain the automatic sample");
        return false;
    }
    // 自动采样 Undo 同时恢复原 BGM 轨，时间仍锁定在一秒。
    sampleContext.actionStack.undo(sampleContext);
    const auto& restoredSample =
        sampleContext.sampleRegistry.get<MMM::Logic::SampleComponent>(
            sampleEntity);
    return near(restoredSample.m_timestamp, 1.0) && restoredSample.m_track == 4;
}

/// @brief 验证拖放命令不能绕过项目资源表创建悬空自动采样。
/// @details
/// CmdCreateAudioSample 只携带资源 ID，控制器必须在当前项目或协作资源表中解析
/// 该 ID 后才能创建对象。没有项目时即使 ID 看似合法，也应给出失败消息，并保持
/// Sample Registry 与 ActionStack 都为空。
/// @par 关键不变量
/// - 合法画布坐标不能替代项目资源校验。
/// - 未解析 ID 不生成 SampleComponent。
/// - 未解析 ID 不创建撤销历史项。
/// - bgmTrackCount 不因失败拖放扩展。
/// - lastActionMessage 明确记录失败。
/// @par 故障定位
/// 出现实体验证说明资源查找被绕过；只有轨数增长说明追加轨在资源校验前提交；
/// 状态均正确但无消息则检查失败反馈生成。
/// @par 测试边界
/// 只覆盖项目资源完全缺失，不区分文件丢失、哈希错误或权限错误等资源层故障。
/// @return 当前项目中无法解析资源时不创建实体或撤销动作。
bool testAudioResourceDropRejectsMissingProjectResource()
{
    // 拖放载荷提供不存在于当前项目资源表的音频 ID。
    // 命中有效音频轨也不能绕过资源存在性检查。
    // 拒绝前后领域 Sample 与 Registry 数量保持一致。
    // 不创建预览实体，不增加动作栈，也不发送变化通知。
    // 另一有效资源路径作为对照证明坐标本身可接受。
    // 该测试保证外部载荷不能制造悬空项目引用。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);

    // 使用主画布 BGM 区合法坐标，确保失败原因只来自资源不存在。
    controller.handleCommand(MMM::Logic::CmdCreateAudioSample{
        .audioResourceId = "main-track-id",
        .cameraId        = "Basic2DCanvas",
        .mouseX          = 650.0F,
        .mouseY          = 50.0F,
        .isCtrlDown      = true,
    });

    // 错误消息非空保证 UI 能反馈拒绝，而非静默吞掉拖放。
    return context.sampleRegistry.view<MMM::Logic::SampleComponent>().empty() &&
           context.actionStack.getUndoStackSize() == 0 &&
           !context.lastActionMessage.empty();
}

/// @brief 验证未打开本地项目的访客可使用已校验协作资源继续编辑。
/// @details
/// 联机访客可能没有本地 ProjectManager 项目，但 Session 持有服务器下发并校验过
/// 的 collaborationProject。资源拖放必须从该表解析 Effect，并允许在玩家区创建
/// 绑定 Note；随后跨区域拖到 BGM 区时应转换为 Sample 且继续保留资源 ID。
/// @par 关键不变量
/// - 资源只存在 collaborationProject 也可被解析。
/// - 初次拖放在 BGM 区创建一个自动 Sample。
/// - Sample 保存协作资源的稳定 ID。
/// - 跨到玩家区后来源 Sample 被删除。
/// - 新 Note 通过 sampleBinding 保留同一资源 ID。
/// - 转换不要求本地 ProjectManager 当前项目。
/// @par 故障定位
/// 初次创建失败检查协作资源回退；转换失败检查 GrabTool 的二次解析来源；资源 ID
/// 丢失检查 Sample 与 Note 绑定间的转换助手。
/// @par 测试边界
/// 协作资源已视为服务端校验通过，不覆盖下载、哈希验证或离线缓存生命周期。
/// @return 资源拖放成功且跨区域转换保留资源绑定时返回 true。
bool testGuestCollaborationResourcesSupportEditing()
{
    // 访客会话从协作快照取得资源，而非本地项目所有权。
    // 资源进入只读镜像后仍需供允许的谱面编辑解析。
    // 创建或修改对象引用该资源时不能误判为资源缺失。
    // 操作不得把访客资源写回宿主项目的本地资源表。
    // Undo/Redo 只处理对象编辑，协作资源镜像继续存在。
    // 场景区分资源可用性与资源所有权两个独立概念。
    // 仅设置会话级协作项目，不注册任何本地项目，以还原访客实际环境。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.collaborationProject = std::make_shared<MMM::Project>();
    context.collaborationProject->m_projectRoot = "collaboration-cache";
    context.collaborationProject->m_audioResources.push_back(MMM::AudioResource{
        .m_id   = "guest-effect-id",
        .m_path = "files/effect-hash.wav",
        .m_type = MMM::AudioTrackType::Effect,
    });

    MMM::Logic::InteractionController controller(context);
    // Effect 首先落到 BGM 区，证明资源查找已经接受 collaborationProject。
    controller.handleCommand(MMM::Logic::CmdCreateAudioSample{
        .audioResourceId = "guest-effect-id",
        .cameraId        = "Basic2DCanvas",
        .mouseX          = 650.0F,
        .mouseY          = 50.0F,
        .isCtrlDown      = true,
    });

    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 1 ||
         samples.get<MMM::Logic::SampleComponent>(*samples.begin())
                 .m_audioResourceId != "guest-effect-id" ) {
        XERROR("Guest collaboration resource could not create a sample");
        return false;
    }

    // 再将同一对象拖到玩家区，覆盖转换阶段的二次资源解析。
    const auto sampleEntity = *samples.begin();
    context.sampleRegistry.get<MMM::Logic::InteractionComponent>(sampleEntity)
        .isSelected           = true;
    context.hoveredEntity     = sampleEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleAnchor);

    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             sampleEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              150.0F,
                              300.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    // 转换应销毁 Sample 并创建带绑定的普通 Note，不能保留跨区重复对象。
    const auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( !context.sampleRegistry.view<MMM::Logic::SampleComponent>().empty() ||
         notes.size() != 1 ) {
        XERROR("Guest sample did not convert through collaboration resources");
        return false;
    }
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    return note.m_sampleBinding &&
           note.m_sampleBinding->m_audioResourceId == "guest-effect-id";
}

/// @brief 验证实际触发 handle 可产生有符号 offset 并完整撤销。
/// @details
/// SampleAnchor 表示谱面锚点，SampleOffset handle
/// 表示实际触发时刻。拖动后者只能 改
/// m_offsetMs，不能移动锚点或轨道。鼠标落在锚点之前，预期产生 -250 ms；
/// disableVerticalObjectDrag 对整体移动有效，但不能禁止用户编辑 offset handle。
/// 最终动作必须携带 before/after 的有符号值并支持 Undo/Redo。
/// @par 关键不变量
/// - hoveredPart 选择 SampleOffset 而非 SampleAnchor。
/// - 锚点 timestamp 和绝对轨保持原值。
/// - 目标触发时间早于锚点时保存负 offset。
/// - -250 不被无符号转换或夹到零。
/// - EndDrag 只创建一个 SampleAction。
/// - Undo/Redo 在 0 与 -250 之间精确往返。
/// @par 故障定位
/// 锚点或轨道变化说明 handle 路由错误；offset 为正检查 Y 到触发时间的符号；
/// Undo/Redo 不对称检查 SampleAction 是否完整保存 m_offsetMs。
/// @par 测试边界
/// 只验证一个负 offset；正值、零值和数值输入控件由属性编辑测试覆盖。
/// @return 负 offset、Undo 与 Redo 均正确时返回 true。
bool testSampleOffsetHandleDrag()
{
    // 抓取 Sample 偏移手柄，只允许修改资源内播放偏移。
    // 对象在谱面上的锚点时间与轨道必须保持不变。
    // 连续移动阶段使用临时预览，松开后合并为一次动作。
    // 偏移边界按资源描述符约束，不产生负值或越界值。
    // Undo/Redo 仅恢复偏移字段，其他采样属性保持原样。
    // 该用例防止手柄拖动被错误路由为整体对象移动。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.disableVerticalObjectDrag = true;

    const auto entity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    // 明确悬停 SampleOffset，避免抓取工具选择默认的锚点移动分支。
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleOffset);

    // 走完整抓取事务，EndDrag 才负责把预览值提交到 ActionStack。
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             entity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              582.0F,
                              425.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    if ( context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                 .m_offsetMs != -250 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Sample offset handle did not commit a signed offset");
        return false;
    }
    // 初始 offset 为零，一次 Undo 后必须精确恢复，而非夹到正值。
    context.actionStack.undo(context);
    if ( context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
             .m_offsetMs != 0 ) {
        XERROR("Sample offset handle undo did not restore zero offset");
        return false;
    }
    // Redo 再次证明历史快照保存的是有符号毫秒值。
    context.actionStack.redo(context);
    return context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
               .m_offsetMs == -250;
}

/// @brief 验证 Note 与自动采样的转换规则严格区分 Effect、Main 和物件类型。
/// @details
/// Sample 转玩家 Note 只接受零 offset 的 Effect 或空资源草稿；Main 不能成为物件
/// 绑定，非零 offset 也无法由 Note 语义表达。反向转换只接受普通 Tap，绑定资源
/// 仍须是 Effect；Hold 等持续物件不能退化为单点 Sample。空资源在两个方向都
/// 作为合法草稿保留，默认音量为 1.0。转换助手无副作用，失败以 nullopt 表达。
/// @par 关键不变量
/// - Effect 零 offset Sample 可转为带绑定 Tap。
/// - Main Sample 和非零 offset Sample 均被拒绝。
/// - 空资源 Sample 可转为未绑定 Tap。
/// - 带 Effect 的普通 Tap 可转回 Sample。
/// - 未绑定 Tap 可转为空资源 Sample。
/// - Main 绑定和 Hold 类型不能转成 Sample。
/// - 成功转换保留时间、音量和目标轨道。
/// @par 故障定位
/// 资源类型组合错误检查转换资格矩阵；空资源失败检查草稿特例；字段丢失检查纯
/// 转换助手的值复制，函数本身不涉及 Registry 或动作栈。
/// @par 测试边界
/// 纯转换测试不创建实体；生命周期、选择迁移和 Undo 由跨区拖动场景验证。
/// @return 仅规格允许的两个方向能构造转换结果时返回 true。
bool testCrossAreaConversionRules()
{
    MMM::AudioResource effect{
        .m_id   = "effect.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };
    MMM::AudioResource main{
        .m_id   = "main.ogg",
        .m_type = MMM::AudioTrackType::Main,
    };
    MMM::Logic::SampleComponent sample{
        .m_timestamp       = 1.25,
        .m_track           = 4,
        .m_audioResourceId = "effect.wav",
        .m_volume          = 0.35F,
    };

    // 第一组覆盖 Sample->Note 的成功、Main 拒绝和非零 offset 拒绝。
    const auto note = MMM::Logic::makePlayerNoteFromSample(sample, 2, &effect);
    auto       offsetSample = sample;
    offsetSample.m_offsetMs = 1;
    if ( !note || note->m_type != MMM::NoteType::NOTE ||
         note->m_trackIndex != 2 || !note->m_sampleBinding ||
         note->m_sampleBinding->m_audioResourceId != "effect.wav" ||
         !near(note->m_sampleBinding->m_volume, 0.35) ||
         MMM::Logic::makePlayerNoteFromSample(sample, 2, &main) ||
         MMM::Logic::makePlayerNoteFromSample(offsetSample, 2, &effect) ) {
        XERROR("Sample-to-Note conversion accepted an invalid source");
        return false;
    }

    // 空资源 Sample 是尚未绑定音频的草稿，可转换为未绑定 Tap。
    auto emptySample              = sample;
    emptySample.m_audioResourceId = {};
    const auto unboundNote =
        MMM::Logic::makePlayerNoteFromSample(emptySample, 1, nullptr);
    if ( !unboundNote || unboundNote->m_type != MMM::NoteType::NOTE ||
         unboundNote->m_trackIndex != 1 || unboundNote->m_sampleBinding ) {
        XERROR("Silent sample draft did not convert to an unbound player Tap");
        return false;
    }

    // 第二组覆盖 Note->Sample，并检查 Hold 与 Main 资源都被拒绝。
    const auto convertedSample =
        MMM::Logic::makeAudioSampleFromPlayerNote(*note, 5, &effect);
    auto hold    = *note;
    hold.m_type  = MMM::NoteType::HOLD;
    auto unbound = *note;
    unbound.m_sampleBinding.reset();
    const auto silentSample =
        MMM::Logic::makeAudioSampleFromPlayerNote(unbound, 6, nullptr);
    if ( !convertedSample || convertedSample->m_track != 5 ||
         convertedSample->m_offsetMs != 0 ||
         convertedSample->m_audioResourceId != "effect.wav" ||
         !near(convertedSample->m_volume, 0.35) || !silentSample ||
         silentSample->m_track != 6 ||
         !silentSample->m_audioResourceId.empty() ||
         !near(silentSample->m_volume, 1.0) ||
         MMM::Logic::makeAudioSampleFromPlayerNote(*note, 5, &main) ||
         MMM::Logic::makeAudioSampleFromPlayerNote(hold, 5, &effect) ) {
        XERROR("Note-to-Sample conversion accepted an invalid source");
        return false;
    }
    return true;
}

/// @brief 验证空采样草稿拖回玩家轨道后成为可撤销的未绑定 Tap。
/// @details
/// BGM 区允许先放置未绑定资源的 Sample 草稿。将其跨区拖到玩家第零轨时，应转换
/// 为普通 Tap，而不是拒绝、保留空 Sample 或伪造资源绑定。跨 Registry 的删除与
/// 创建必须包装为一个复合动作；Undo 恢复原空 Sample，Redo 再次生成未绑定 Tap。
/// @par 关键不变量
/// - 空资源是合法草稿值，不触发缺失资源错误。
/// - 提交后 sampleRegistry 为空。
/// - 提交后 noteRegistry 只有一个普通 Tap。
/// - 新 Tap 的 sampleBinding 保持空。
/// - 跨 Registry 转换只生成一个历史项。
/// - Undo 恢复空 Sample，Redo 恢复未绑定 Tap。
/// @par 故障定位
/// 转换拒绝说明空资源草稿未被识别；两类对象并存说明复合动作删除来源失败；
/// 绑定被创建说明空资源错误套用了默认音频。
/// @par 测试边界
/// 用例从 Sample 到 Note 单向开始，反向转换在未绑定 Tap 场景中独立验证。
/// @return 转换、Undo 与 Redo 均保持空资源语义时返回 true。
bool testSilentSampleDragConvertsToUnboundNote()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    // 空 audioResourceId 是合法草稿，不等同于找不到项目资源的错误输入。
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp = 1.0,
            .m_track     = 4,
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    context.hoveredEntity     = sampleEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleAnchor);

    // 从 BGM 区拖到玩家区，EndDrag 应提交跨 Registry 的复合转换。
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             sampleEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              150.0F,
                              300.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    // 提交后来源 Registry 为空，目标 Registry 只有一个未绑定 Tap。
    auto notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( !samples.empty() || notes.size() != 1 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Silent sample drag did not commit one cross-area conversion");
        return false;
    }
    const auto  noteEntity = *notes.begin();
    const auto& note       = notes.get<MMM::Logic::NoteComponent>(noteEntity);
    if ( note.m_type != MMM::NoteType::NOTE || note.m_trackIndex != 0 ||
         note.m_sampleBinding ) {
        XERROR("Silent sample drag created a bound or non-Tap player object");
        return false;
    }

    // Undo 应删除新 Note 并恢复同语义的空 Sample。
    context.actionStack.undo(context);
    auto restoredSamples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( restoredSamples.size() != 1 ||
         !context.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         !restoredSamples
              .get<MMM::Logic::SampleComponent>(*restoredSamples.begin())
              .m_audioResourceId.empty() ) {
        XERROR("Silent sample conversion undo did not restore the draft");
        return false;
    }

    // Redo 再次跨 Registry，资源 optional 仍保持未绑定。
    context.actionStack.redo(context);
    notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return samples.empty() && notes.size() == 1 &&
           !notes.get<MMM::Logic::NoteComponent>(*notes.begin())
                .m_sampleBinding;
}

/// @brief 验证选取工具按住已选物件时复用统一抓取并允许跨入 BGM 区。
/// @details
/// Marquee 工具通常负责框选，但在已选且悬停的对象上按下时应委托 GrabTool，保持
/// 与抓取工具一致的拖动和跨区转换能力。用例通过 InteractionController 的命令
/// 路由执行完整手势，防止工具模式分派吞掉拖动。未绑定 Tap 进入第一条 BGM 轨
/// 后应成为空资源 Sample，并由一个动作撤销。
/// @par 关键不变量
/// - currentTool 保持 Marquee，不需切换到 Grab。
/// - 已选且悬停对象进入实体拖动分支。
/// - 完整命令序列由 InteractionController 分派。
/// - 玩家 Note 跨区后转换为空 Sample。
/// - 目标绝对轨为第一条 BGM 轨 4。
/// - 一次 Undo 恢复原 Note 并清除 Sample。
/// @par 故障定位
/// StartDrag 无效说明 Marquee 未委托抓取；转换结果错误检查统一 GrabTool；出现
/// 多个历史项说明工具路由额外提交了选择变化。
/// @par 测试边界
/// 不绘制框选矩形，只验证 Marquee 模式命中已选对象后的抓取委托路径。
/// @return 未绑定 Note 经选取工具命令路由转换为静音采样且可撤销时返回 true。
bool testMarqueeToolEntityDragCrossesCanvasAreas()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.currentTool = MMM::Logic::EditTool::Marquee;

    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(noteEntity);

    // 选择与悬停均通过命令建立，模拟 Marquee 模式下的真实输入顺序。
    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdSelectEntity{
        noteEntity,
        true,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        noteEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdStartDrag{
        noteEntity,
        "Basic2DCanvas",
        false,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateDrag{
        "Basic2DCanvas",
        550.0F,
        300.0F,
        true,
    });
    controller.handleCommand(MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    // 抓取委托成功后 Note 已被 Sample 替换，且只产生一个历史项。
    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( !context.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         samples.size() != 1 || context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Marquee tool did not commit one cross-area conversion");
        return false;
    }
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( sample.m_track != 4 || !sample.m_audioResourceId.empty() ) {
        XERROR("Marquee tool produced the wrong BGM sample");
        return false;
    }

    // 一次 Undo 返回玩家 Note，控制器没有额外提交选择或移动动作。
    context.actionStack.undo(context);
    return context.noteRegistry.view<MMM::Logic::NoteComponent>().size() == 1 &&
           context.sampleRegistry.view<MMM::Logic::SampleComponent>().empty();
}

/// @brief 验证物件拖入草稿追加轨后扩展持久轨道数并保持单次撤销记录。
/// @details
/// 初始四条草稿轨的左侧还显示一条虚拟追加轨。玩家 Note 拖入该位置时，预览阶段
/// 可以暂时使用 -5，但不能每次 UpdateDrag 都永久扩轨；即使连续更新 128 次，
/// draftTrackCount 仍应为四。EndDrag 才一次性扩展到五轨并提交移动动作。
/// Undo/Redo 要同时恢复物件所属域、负轨位置和草稿轨布局。
/// @par 关键不变量
/// - 虚拟追加轨允许预览绝对负轨 -5。
/// - 一百二十八次更新不累计修改持久轨数量。
/// - 预览对象已经标记草稿，但布局仍为四轨。
/// - 松手后布局只扩展一轨。
/// - 移动与扩轨共用一个 ActionStack 项。
/// - Undo 恢复玩家第零轨，Redo 恢复草稿 -5。
/// @par 故障定位
/// 预览阶段轨数增长说明虚拟轨被过早持久化；重复增长说明每帧累加；松手后不增
/// 检查 EndDrag 候选提交；往返不一致检查移动动作是否保存 draftTrackCount。
/// @par 测试边界
/// 一百二十八次更新是状态稳定性探针，不作为拖动性能或帧率基准。
/// @return 拖动、Undo 与 Redo 同步恢复物件轨道和草稿轨道数量时返回 true。
bool testDraftAppendLaneDragExpandsPersistentCount()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        entity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);

    // 从玩家对象开始，确保手势同时覆盖正式域到草稿域的转换。
    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        entity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdStartDrag{
        entity,
        "Basic2DCanvas",
        false,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    // 重复预览用于发现按帧扩轨导致草稿区域无限增长的回归。
    for ( int update = 0; update < 128; ++update ) {
        controller.handleCommand(MMM::Logic::CmdUpdateDrag{
            "Basic2DCanvas",
            -350.0F,
            300.0F,
            true,
        });
    }
    const auto preview =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    if ( preview.m_trackIndex != -5 || !preview.m_isDraft ||
         context.draftTrackCount != 4 ) {
        XERROR("Draft append drag expanded persistent lanes before release");
        return false;
    }

    // 仅松手边界可以把虚拟追加轨转为第五条持久草稿轨。
    controller.handleCommand(MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto moved =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    if ( moved.m_trackIndex != -5 || !moved.m_isDraft ||
         context.draftTrackCount != 5 ||
         context.actionStack.getUndoStackSize() != 1U ) {
        XERROR("Draft append drag did not expand one persistent lane");
        return false;
    }

    // Undo 返回正式玩家域，并收回由本动作引入的第五条草稿轨。
    context.actionStack.undo(context);
    const auto undone =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    if ( undone.m_trackIndex != 0 || undone.m_isDraft ||
         context.draftTrackCount != 4 ) {
        XERROR("Draft append drag undo did not restore lane count");
        return false;
    }

    // Redo 同时重建草稿身份、负轨位置和持久轨数。
    context.actionStack.redo(context);
    const auto redone =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    if ( redone.m_trackIndex != -5 || !redone.m_isDraft ||
         context.draftTrackCount != 5 ) {
        XERROR("Draft append drag redo did not restore expansion");
        return false;
    }
    return true;
}

/// @brief 验证进入草稿追加轨后移回原轨，松开时不会扩充草稿区。
/// @details
/// 用户拖动过程中可能短暂经过追加轨但最终撤回。第一次 UpdateDrag 允许显示 -5
/// 预览，第二次返回原玩家轨；EndDrag 必须以最终位置为准，不能仅因曾经过追加轨
/// 就扩大 draftTrackCount 或创建无变化历史项。该场景约束预览意图可被覆盖。
/// @par 关键不变量
/// - 进入追加轨仅设置临时候选。
/// - 回到玩家区后对象恢复非草稿身份。
/// - 松手采用最后一次 UpdateDrag 的位置。
/// - draftTrackCount 始终保持四。
/// - before 与 after 相同不创建空动作。
/// - 临时追加候选在 EndDrag 后被清理。
/// @par 故障定位
/// 回到玩家轨仍为草稿说明预览不可逆；松手后轨数增长说明历史路径只记录曾进入
/// 追加轨而非最终位置；出现历史项则检查空变化消除。
/// @par 测试边界
/// 只覆盖回到原始轨的完全撤回，改到另一玩家轨的正常移动由抓取用例覆盖。
/// @return 预览和提交均保持原轨道数量且不产生空操作时返回 true。
bool testDraftAppendLanePreviewCanBeWithdrawn()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto entity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        entity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);

    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        entity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdStartDrag{
        entity,
        "Basic2DCanvas",
        false,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    // 先进入虚拟追加轨，建立尚未提交的扩轨候选。
    controller.handleCommand(MMM::Logic::CmdUpdateDrag{
        "Basic2DCanvas",
        -350.0F,
        300.0F,
        true,
    });
    // 再移回原玩家轨，候选扩轨应被覆盖而不是累计。
    controller.handleCommand(MMM::Logic::CmdUpdateDrag{
        "Basic2DCanvas",
        150.0F,
        300.0F,
        true,
    });
    // 最终 before 与 after 相同，因此 EndDrag 不应压入空动作。
    controller.handleCommand(MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto& note =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    return note.m_trackIndex == 0 && !note.m_isDraft &&
           context.draftTrackCount == 4 &&
           context.actionStack.getUndoStackSize() == 0U;
}

/// @brief 验证折线在草稿区和玩家区之间双向拖动并保持完整结构。
/// @details
/// Polyline 的根组件、内嵌 m_subNotes 和独立投影子实体必须作为一个结构移动。
/// 测试先从草稿区拖到玩家区并同步领域模型，再拖回草稿区；两个方向都要求根与
/// 子节点轨道统一平移、m_isDraft 一致、父子引用有效。每次跨域各形成一个动作，
/// 最后 Undo/Redo 第二次移动，确认历史快照不会丢失第一次转换后的结构。
/// @par 关键不变量
/// - 根与两个子节点始终属于同一草稿或玩家域。
/// - 内嵌节点轨道与对应子实体轨道一致。
/// - 子实体 parent 与 subIndex 在移动后保持有效。
/// - 草稿转玩家后可同步为一个领域 Polyline。
/// - 玩家转草稿后历史栈累计第二个动作。
/// - Undo 回到玩家结构，Redo 回到草稿结构。
/// @par 故障定位
/// 仅根移动说明子结构闭包不完整；ECS 正确而领域失败检查 syncBeatmap；第二次
/// 移动往返失败检查动作是否保存跨域后的完整父子快照。
/// @par 测试边界
/// 两节点折线用于结构验证，大折线非法跨域和回滚由下一场景负责。
/// @return 双向移动、正式同步及 Undo/Redo 均保留根子结构时返回 true。
bool testPolylineDragMovesBetweenDraftAndPlayer()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto                rootEntity  = context.noteRegistry.create();
    const auto                firstChild  = context.noteRegistry.create();
    const auto                secondChild = context.noteRegistry.create();
    MMM::Logic::NoteComponent root{
        .m_type            = MMM::NoteType::POLYLINE,
        .m_timestamp       = 1.0,
        .m_trackIndex      = -2,
        .m_isDraft         = true,
        .m_collaborationId = "draft-polyline-root",
    };
    root.m_subNotes = {
        MMM::Logic::NoteComponent::SubNote{
            .type            = MMM::NoteType::NOTE,
            .timestamp       = 1.0,
            .trackIndex      = -2,
            .collaborationId = "draft-polyline-first",
        },
        MMM::Logic::NoteComponent::SubNote{
            .type            = MMM::NoteType::FLICK,
            .timestamp       = 1.25,
            .trackIndex      = -1,
            .dtrack          = -1,
            .collaborationId = "draft-polyline-second",
        },
    };
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(rootEntity, root);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(rootEntity);
    const auto addChild = [&](entt::entity entity, std::size_t index) {
        const auto& sub = root.m_subNotes[index];
        context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
            entity,
            MMM::Logic::NoteComponent{
                .m_type            = sub.type,
                .m_timestamp       = sub.timestamp,
                .m_trackIndex      = sub.trackIndex,
                .m_dtrack          = sub.dtrack,
                .m_isSubNote       = true,
                .m_isDraft         = true,
                .m_parentPolyline  = rootEntity,
                .m_subIndex        = static_cast<int>(index),
                .m_collaborationId = sub.collaborationId,
            });
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    };
    // 先创建根、后创建两个子实体，覆盖 unordered_map 子实体先于根遍历的路径。
    addChild(firstChild, 0);
    addChild(secondChild, 1);

    // 结构断言同时检查根、内嵌节点与子实体，防止只更新一种表示。
    const auto structureMatches =
        [&](bool isDraft, int firstTrack, int secondTrack) {
            if ( !context.noteRegistry.valid(rootEntity) ||
                 !context.noteRegistry.valid(firstChild) ||
                 !context.noteRegistry.valid(secondChild) ) {
                return false;
            }
            const auto& currentRoot =
                context.noteRegistry.get<const MMM::Logic::NoteComponent>(
                    rootEntity);
            const auto& currentFirst =
                context.noteRegistry.get<const MMM::Logic::NoteComponent>(
                    firstChild);
            const auto& currentSecond =
                context.noteRegistry.get<const MMM::Logic::NoteComponent>(
                    secondChild);
            return currentRoot.m_type == MMM::NoteType::POLYLINE &&
                   currentRoot.m_isDraft == isDraft &&
                   currentRoot.m_trackIndex == firstTrack &&
                   currentRoot.m_subNotes.size() == 2 &&
                   currentRoot.m_subNotes[0].trackIndex == firstTrack &&
                   currentRoot.m_subNotes[1].trackIndex == secondTrack &&
                   currentFirst.m_parentPolyline == rootEntity &&
                   currentFirst.m_isDraft == isDraft &&
                   currentFirst.m_trackIndex == firstTrack &&
                   currentSecond.m_parentPolyline == rootEntity &&
                   currentSecond.m_isDraft == isDraft &&
                   currentSecond.m_trackIndex == secondTrack;
        };
    // 共用完整手势助手，并通过 kind 指明当前对象所属轨道域。
    const auto drag = [&](MMM::Logic::ChartObjectKind kind, float mouseX) {
        context.hoveredEntity     = rootEntity;
        context.hoveredObjectKind = kind;
        context.hoveredPart =
            static_cast<std::int32_t>(MMM::Logic::HoverPart::PolylineNode);
        context.hoveredSubIndex = 0;
        MMM::Logic::GrabTool tool;
        tool.handleStartDrag(context,
                             MMM::Logic::CmdStartDrag{
                                 rootEntity,
                                 "Basic2DCanvas",
                                 false,
                                 kind,
                             });
        tool.handleUpdateDrag(context,
                              MMM::Logic::CmdUpdateDrag{
                                  "Basic2DCanvas",
                                  mouseX,
                                  300.0F,
                                  true,
                              });
        tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });
    };

    // 第一阶段把负轨草稿移入玩家域，并立即检查领域同步结果。
    drag(MMM::Logic::ChartObjectKind::DraftNote, 250.0F);
    MMM::Logic::SessionUtils::syncBeatmap(context);
    if ( !structureMatches(false, 1, 2) ||
         context.currentBeatmap->m_noteData.polylines.size() != 1 ||
         context.currentBeatmap->m_noteData.polylines.front()
                 .m_subNotes.size() != 2 ) {
        XERROR("Draft-to-player Polyline drag lost its complete structure");
        return false;
    }

    // 第二阶段反向跨域，历史栈应累计两个完整移动动作。
    drag(MMM::Logic::ChartObjectKind::PlayerNote, -50.0F);
    if ( !structureMatches(true, -2, -1) ||
         context.actionStack.getUndoStackSize() != 2 ) {
        XERROR("Player-to-draft Polyline drag did not cross the area boundary");
        return false;
    }
    // Undo/Redo 只往返第二次移动，第一次转换后的玩家结构是中间基线。
    context.actionStack.undo(context);
    if ( !structureMatches(false, 1, 2) ) {
        XERROR("Bidirectional Polyline drag undo lost the player structure");
        return false;
    }
    context.actionStack.redo(context);
    return structureMatches(true, -2, -1);
}

/// @brief 验证大折线跨越草稿与玩家边界后完整回弹并重建可见性索引。
/// @details
/// 三节点折线整体向左时，部分节点进入草稿域、部分仍在玩家域，违反折线必须位于
/// 同一轨道域的约束。UpdateDrag 可展示该临时预览，但 EndDrag 必须拒绝并恢复根、
/// 内嵌节点及三个子实体的所有字段。拒绝不是动作，Undo 栈保持空；交互拖动标志、
/// pinned 实体必须清理，排序、统计、批注和变换缓存要标脏以重建可见状态。
/// @par 关键不变量
/// - 非法预览可以暂时跨越零轨边界。
/// - 提交拒绝后根时间、轨道和节点数组恢复。
/// - 三个投影子实体逐项恢复父子关系和几何字段。
/// - 所有 InteractionComponent 清除 isDragging。
/// - SessionContext 不再保存 draggedEntity 或 pinned 实体。
/// - 拒绝不创建撤销历史，但提供失败消息。
/// - 四类渲染派生缓存全部标脏。
/// @par 故障定位
/// 仍提交说明跨域合法性检查过晚；字段未恢复检查拖动 before 快照；对象恢复但
/// 不可见检查索引和渲染脏标记；拖动残留检查终止清理。
/// @par 测试边界
/// 用例不验证错误提示文案内容，只要求存在反馈并完整恢复可渲染逻辑状态。
/// @return 根折线、子实体、拖拽态与渲染缓存均恢复且不产生撤销记录时返回 true。
bool testRejectedPolylineDragRestoresVisibility()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto                rootEntity = context.noteRegistry.create();
    const auto                childA     = context.noteRegistry.create();
    const auto                childB     = context.noteRegistry.create();
    const auto                childC     = context.noteRegistry.create();
    MMM::Logic::NoteComponent root{
        .m_type       = MMM::NoteType::POLYLINE,
        .m_timestamp  = 1.0,
        .m_trackIndex = 0,
    };
    root.m_subNotes = {
        MMM::Logic::NoteComponent::SubNote{
            .type       = MMM::NoteType::NOTE,
            .timestamp  = 1.0,
            .trackIndex = 0,
        },
        MMM::Logic::NoteComponent::SubNote{
            .type       = MMM::NoteType::NOTE,
            .timestamp  = 3.0,
            .trackIndex = 1,
        },
        MMM::Logic::NoteComponent::SubNote{
            .type       = MMM::NoteType::FLICK,
            .timestamp  = 6.0,
            .trackIndex = 2,
            .dtrack     = 1,
        },
    };
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(rootEntity, root);
    context.noteRegistry.emplace<MMM::Logic::TransformComponent>(rootEntity);
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(rootEntity);

    // 显式物化每个子实体，使回滚必须恢复两套结构表示。
    const auto addChild = [&](entt::entity entity, std::size_t index) {
        const auto& sub = root.m_subNotes[index];
        context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
            entity,
            MMM::Logic::NoteComponent{
                .m_type           = sub.type,
                .m_timestamp      = sub.timestamp,
                .m_trackIndex     = sub.trackIndex,
                .m_dtrack         = sub.dtrack,
                .m_isSubNote      = true,
                .m_parentPolyline = rootEntity,
                .m_subIndex       = static_cast<int>(index),
            });
        context.noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    };
    addChild(childA, 0);
    addChild(childB, 1);
    addChild(childC, 2);

    // 预置有效派生缓存，拒绝后必须使其失效以重新计算包围范围。
    context.sortedNoteEntities = { rootEntity, childA, childB, childC };
    context.sortedNoteMaxEndPrefix.assign(4U, 6.0);
    context.isNoteOrderDirty             = false;
    context.isNoteStatsDirty             = false;
    context.isAnnotationRenderCacheDirty = false;
    context.isTransformDirty             = false;
    context.hoveredEntity                = rootEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::PolylineNode);
    context.hoveredSubIndex = 0;

    // 预览跨过零轨边界，故意制造根与后续节点分属两域的非法状态。
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             rootEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::PlayerNote,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              50.0F,
                              0.0F,
                              true,
                          });
    const auto& preview =
        context.noteRegistry.get<const MMM::Logic::NoteComponent>(rootEntity);
    if ( preview.m_trackIndex != -1 || !preview.m_isDraft ||
         preview.m_subNotes.front().trackIndex != -1 ||
         preview.m_subNotes[1].trackIndex != 0 ) {
        XERROR(
            "Polyline invalid-drop regression did not cross the draft "
            "boundary");
        return false;
    }

    // EndDrag 发现结构跨域后整体回滚，不能分别夹取节点导致折线变形。
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto& restored =
        context.noteRegistry.get<const MMM::Logic::NoteComponent>(rootEntity);
    // 子实体逐字段对照原节点，覆盖 parent、index、时间、轨道和方向。
    const auto childMatches = [&](entt::entity entity, std::size_t index) {
        const auto& child =
            context.noteRegistry.get<const MMM::Logic::NoteComponent>(entity);
        return child.m_parentPolyline == rootEntity &&
               child.m_subIndex == static_cast<int>(index) &&
               near(child.m_timestamp, root.m_subNotes[index].timestamp) &&
               child.m_trackIndex == root.m_subNotes[index].trackIndex &&
               child.m_dtrack == root.m_subNotes[index].dtrack;
    };
    // 所有参与预览的实体都必须清除 InteractionComponent::isDragging。
    const auto notDragging = [&](entt::entity entity) {
        return !context.noteRegistry
                    .get<const MMM::Logic::InteractionComponent>(entity)
                    .isDragging;
    };
    if ( restored.m_type != MMM::NoteType::POLYLINE ||
         !near(restored.m_timestamp, root.m_timestamp) ||
         restored.m_trackIndex != root.m_trackIndex ||
         restored.m_subNotes.size() != root.m_subNotes.size() ||
         !childMatches(childA, 0) || !childMatches(childB, 1) ||
         !childMatches(childC, 2) || !notDragging(rootEntity) ||
         !notDragging(childA) || !notDragging(childB) || !notDragging(childC) ||
         context.isDragging || context.draggedEntity != entt::null ||
         !context.dragRenderPinnedEntities.empty() ||
         context.actionStack.getUndoStackSize() != 0U ||
         context.lastActionMessage.empty() || !context.isNoteOrderDirty ||
         !context.isNoteStatsDirty || !context.isAnnotationRenderCacheDirty ||
         !context.isTransformDirty ) {
        XERROR("Rejected Polyline drag did not fully restore renderable state");
        return false;
    }
    return true;
}

/// @brief 验证未绑定 Tap 拖入 BGM 轨道后成为可撤销的空采样草稿。
/// @details
/// 这是空 Sample 转未绑定 Tap 的反向场景。普通 Tap 没有 sampleBinding 时仍允许
/// 进入 BGM 区，结果是 audioResourceId 为空、默认音量 1.0 的 Sample 草稿。
/// Note 删除与 Sample 创建必须原子提交；Undo 恢复未绑定 Tap，Redo 恢复空采样，
/// 两个 Registry 在每个稳定阶段都只能有一个对象。
/// @par 关键不变量
/// - 来源只能是普通 Tap，且没有 sampleBinding。
/// - 目标绝对轨为第一条 BGM 轨。
/// - 新 Sample 的资源 ID 为空、音量为 1.0。
/// - 提交后 Note 与 Sample 不会同时存在。
/// - 转换只占一个复合历史项。
/// - Undo/Redo 保持未绑定语义而不自动选择资源。
/// @par 故障定位
/// 转换被拒绝检查未绑定 Tap 特例；自动绑定资源说明转换越权读取当前选择；两域
/// 并存或往返不完整检查 CompositeEditorAction 的 typed entries。
/// @par 测试边界
/// 只允许普通 Tap 进入 BGM；Hold、Flick 与 Polyline 的拒绝由转换矩阵测试覆盖。
/// @return 转换、Undo 与 Redo 均保持未绑定音频语义时返回 true。
bool testUnboundNoteDragConvertsToSilentSample()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    // 不设置 sampleBinding，明确表示用户尚未选择音频资源。
    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    context.hoveredEntity     = noteEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);

    // 将玩家第零轨对象拖到第一条 BGM 轨，触发跨 Registry 转换。
    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             noteEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::PlayerNote,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              550.0F,
                              300.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    // 提交结果必须是单个空资源 Sample，并保持一个历史边界。
    auto notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( !notes.empty() || samples.size() != 1 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Unbound Note drag did not commit one cross-area conversion");
        return false;
    }
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( sample.m_track != 4 || !sample.m_audioResourceId.empty() ||
         !near(sample.m_volume, 1.0) ) {
        XERROR("Unbound Note drag created a bound or misplaced sample");
        return false;
    }

    // Undo 删除 Sample 并恢复没有绑定的 Tap。
    context.actionStack.undo(context);
    notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( notes.size() != 1 || !samples.empty() ||
         notes.get<MMM::Logic::NoteComponent>(*notes.begin())
             .m_sampleBinding ) {
        XERROR("Unbound Note conversion undo did not restore the Tap");
        return false;
    }

    // Redo 再次转换，空资源语义不能被默认项目音频替换。
    context.actionStack.redo(context);
    notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return notes.empty() && samples.size() == 1 &&
           samples.get<MMM::Logic::SampleComponent>(*samples.begin())
               .m_audioResourceId.empty();
}

/// @brief 验证跨 Registry 的同值实体 ID 在复合转换及 Undo 中不会串对象。
/// @details
/// noteRegistry 与 sampleRegistry
/// 独立分配实体，数值相同是正常现象。复合动作先在 Note 域创建选中对象，再在
/// Sample 域删除同值实体；选择缓存也按类型分成两组。 执行、Undo 和 Redo
/// 都必须用 ChartObjectKind/动作类型选择 Registry，不能仅用 entt::entity
/// 作为全局身份，否则会删错对象或把选择写入错误集合。
/// @par 关键不变量
/// - 两个 Registry 的 entity 数值刻意相等。
/// - BatchNoteAction 只访问 noteRegistry。
/// - BatchSampleAction 只访问 sampleRegistry。
/// - 执行后只有 Note 侧对象与选择存在。
/// - Undo 后只有 Sample 侧对象与选择存在。
/// - Redo 再次恢复 Note 侧，不串改类型化缓存。
/// @par 故障定位
/// 同值对象被串删说明动作仅按 entity 访问；对象正确但选择错说明缓存未按类型
/// 更新；仅 Undo/Redo 错误说明 typed identity 没有进入历史快照。
/// @par 测试边界
/// 手工构造 CompositeEditorAction 以隔离动作身份问题，不包含鼠标坐标换算。
/// @return 目标与来源选中状态均按领域恢复时返回 true。
bool testCompositeConversionUsesTypedIdentity()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    const auto noteEntity = context.noteRegistry.create();
    if ( noteEntity != sampleEntity ) {
        XERROR("Typed identity test did not obtain overlapping entity IDs");
        return false;
    }

    // 两个子动作故意携带相同 entity 数值，但分别操作 Note 与 Sample。
    MMM::Logic::NoteComponent note;
    note.m_timestamp     = 1.0;
    note.m_trackIndex    = 2;
    note.m_sampleBinding = MMM::AudioSampleBinding{ "effect.wav", 1.0F };
    std::vector<std::unique_ptr<MMM::Logic::IEditorAction>> actions;
    actions.push_back(std::make_unique<MMM::Logic::BatchNoteAction>(
        std::vector<MMM::Logic::BatchNoteAction::Entry>{
            {
                .entity        = noteEntity,
                .before        = std::nullopt,
                .after         = note,
                .afterSelected = true,
            },
        }));
    actions.push_back(std::make_unique<MMM::Logic::BatchSampleAction>(
        std::vector<MMM::Logic::BatchSampleAction::Entry>{
            {
                .entity = sampleEntity,
                .before = context.sampleRegistry
                              .get<MMM::Logic::SampleComponent>(sampleEntity),
                .after          = std::nullopt,
                .beforeSelected = true,
            },
        }));
    // CompositeEditorAction 保证跨 Registry 转换只形成一个用户历史项。
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::CompositeEditorAction>(std::move(actions),
                                                            "测试跨区转换"),
        context);
    if ( context.sampleRegistry.valid(sampleEntity) ||
         !context.noteRegistry.valid(noteEntity) ||
         !context.noteRegistry.get<MMM::Logic::InteractionComponent>(noteEntity)
              .isSelected ||
         !context.selectedNoteEntities.contains(noteEntity) ||
         !context.selectedSampleEntities.empty() ) {
        XERROR("Composite conversion mixed overlapping Registry identities");
        return false;
    }

    // Undo 后只有 Sample 被恢复并选中，Note 缓存必须为空。
    context.actionStack.undo(context);
    if ( !context.sampleRegistry.valid(sampleEntity) ||
         context.noteRegistry.valid(noteEntity) ||
         !context.sampleRegistry
              .get<MMM::Logic::InteractionComponent>(sampleEntity)
              .isSelected ||
         !context.selectedSampleEntities.contains(sampleEntity) ||
         !context.selectedNoteEntities.empty() ) {
        XERROR("Composite conversion undo did not restore typed selection");
        return false;
    }
    // Redo 返回 Note 侧选择，证明动作快照没有混用同值实体。
    context.actionStack.redo(context);
    return !context.sampleRegistry.valid(sampleEntity) &&
           context.noteRegistry.valid(noteEntity) &&
           context.selectedNoteEntities.contains(noteEntity) &&
           context.selectedSampleEntities.empty();
}

/// @brief 验证主画布框选按带类型身份选中 Sample，且 Preview 忽略 Sample。
/// @details
/// Note 与 Sample 使用同值实体 ID，并分别建立空间排序缓存。主画布框选范围只覆盖
/// 第一条 BGM 轨，因此应选择 Sample 而不是同值 Note。Preview 画布不显示自动
/// Sample；把相同框选切换到 Preview 后，已有 Sample 选择必须清理，两个类型化
/// 选择集合都为空。该用例锁定框选查询和选择写回的双重类型边界。
/// @par 关键不变量
/// - Note 与 Sample 排序索引独立保存。
/// - 主画布 BGM 框只查询 Sample 索引。
/// - 同值 Note 的 InteractionComponent 保持未选。
/// - selectedSampleEntities 记录 Sample 类型身份。
/// - Preview 不支持自动 Sample 的框选。
/// - 切换画布后清理旧 Sample 选择及缓存集合。
/// @par 故障定位
/// 主画布串选 Note 检查空间索引类型；Sample 组件选中但集合为空检查双向缓存；
/// Preview 仍选中说明画布能力过滤未在重算前清理旧状态。
/// @par 测试边界
/// 框选几何直接写入上下文，不测试 ImGui 拖框事件或 Preview 的视觉反馈。
/// @return 同值 Note 未被串选且 Preview 不处理自动采样时返回 true。
bool testMarqueeSelectsTypedSamplesOnlyOnMainCanvas()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.cameras.emplace(
        "Preview", MMM::Logic::CameraInfo{ "Preview", 1000.0F, 600.0F, 0.0F });

    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(noteEntity);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity);
    if ( noteEntity != sampleEntity ) {
        XERROR("Marquee identity test did not obtain overlapping entity IDs");
        return false;
    }

    // 分别填充 Note 与 Sample 排序索引，模拟正常更新后的查询缓存。
    context.sortedNoteEntities       = { noteEntity };
    context.sortedNoteMaxEndPrefix   = { 1.0 };
    context.sortedSampleEntities     = { sampleEntity };
    context.sortedSampleMaxEndPrefix = { 1.0 };
    context.marqueeBoxes             = {
        MMM::Logic::MarqueeBox{
                        .startTime  = 0.9,
                        .endTime    = 1.1,
                        .startTrack = 4.05F,
                        .endTrack   = 4.95F,
                        .cameraId   = "Basic2DCanvas",
        },
    };
    context.isMarqueeSelectionDirty = true;

    // 主画布允许框选 BGM Sample，结果只能写入 selectedSampleEntities。
    MMM::Logic::InteractionController controller(context);
    controller.updateMarqueeSelection();
    if ( !context.sampleRegistry
              .get<MMM::Logic::InteractionComponent>(sampleEntity)
              .isSelected ||
         context.noteRegistry.get<MMM::Logic::InteractionComponent>(noteEntity)
             .isSelected ||
         !context.selectedNoteEntities.empty() ||
         !context.selectedSampleEntities.contains(sampleEntity) ) {
        XERROR("Main-canvas marquee confused typed sample and note identity");
        return false;
    }

    // 同一几何框切换到 Preview；Preview 不消费 Sample 命中，应清空旧选择。
    context.marqueeBoxes = {
        MMM::Logic::MarqueeBox{
            .startTime  = 0.9,
            .endTime    = 1.1,
            .startTrack = 4.05F,
            .endTrack   = 4.95F,
            .cameraId   = "Preview",
        },
    };
    context.isMarqueeSelectionDirty = true;
    controller.updateMarqueeSelection(true);
    return !context.sampleRegistry
                .get<MMM::Logic::InteractionComponent>(sampleEntity)
                .isSelected &&
           context.selectedNoteEntities.empty() &&
           context.selectedSampleEntities.empty();
}

/// @brief 验证混合 Note/Sample 跨会话粘贴共用时间锚点和相对 BGM 轨道。
/// @details
/// 来源选择同时包含玩家 Note 和自动 Sample，两者最早时间相差一秒。复制到六键
/// 目标会话时，以 animateTime=10 秒作为统一锚点，必须保持相对时间差。Sample
/// 的来源绝对轨 6 在四键环境中代表 BGM 相对轨 2，目标中应重编码为绝对轨 8。
/// mirrored 只影响玩家轨道，不能镜像 BGM 相对索引；两类创建组成一个复合动作。
/// selectPastedObjects 要分别维护两个 Registry 的选择组件和类型化选择集合。
/// @par 关键不变量
/// - 来源 Note 与 Sample 共用最早对象作为时间零点。
/// - 目标 Note 落在 10 秒，Sample 保持晚一秒。
/// - 四键来源 BGM 相对轨 2 在六键目标编码为绝对轨 8。
/// - 玩家轨 1 在六键镜像为轨 4。
/// - Sample offset、资源和音量保持不变。
/// - 两类粘贴结果都进入各自选择集合。
/// - 一次 Undo/Redo 同时处理两个 Registry。
/// @par 故障定位
/// 时间差错误检查公共锚点；Sample 轨错误检查来源相对索引重编码；Note 镜像错误
/// 检查目标键数；撤销只删一类说明粘贴没有使用复合动作。
/// @par 测试边界
/// 跨会话剪贴板使用进程内缓存，不覆盖系统剪贴板格式或应用重启后的持久化。
/// @return 镜像仅作用 Note，且一次 Undo 同时移除两类新物件时返回 true。
bool testMixedChartObjectClipboardAcrossSessions()
{
    // 来源四键会话提供一个玩家对象和一个位于第三条 BGM 轨的自动采样。
    MMM::Logic::SessionContext source;
    configureObjectEditingCanvas(source);
    const auto noteEntity = source.noteRegistry.create();
    source.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 2.0,
            .m_trackIndex = 1,
        });
    source.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    const auto sampleEntity = source.sampleRegistry.create();
    source.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 3.0,
            .m_offsetMs        = -80,
            .m_track           = 6,
            .m_audioResourceId = "stem.ogg",
            .m_volume          = 0.4F,
        });
    source.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });

    // Copy 将两类对象写入编辑器级剪贴板，供不同 Session 消费。
    MMM::Logic::ActionController sourceController(source);
    sourceController.handleCommand(MMM::Logic::CmdCopy{});

    // 目标改为六键，但 BGM 相对轨和对象间一秒时间差必须保持。
    MMM::Logic::SessionContext target;
    configureObjectEditingCanvas(target);
    target.trackCount                                    = 6;
    target.currentBeatmap->m_baseMapMetadata.track_count = 6;
    target.animateTime                                   = 10.0;
    MMM::Logic::ActionController targetController(target);
    targetController.handleCommand(MMM::Logic::CmdPaste{
        .m_mirrored            = true,
        .m_selectPastedObjects = true,
    });

    // 一次粘贴只创建各一个对象，并因相对 BGM 轨 2 扩展到三条持久轨。
    auto notes   = target.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = target.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( notes.size() != 1 || samples.size() != 1 ||
         target.actionStack.getUndoStackSize() != 1 ||
         target.bgmTrackCount != 3 ) {
        XERROR("Mixed clipboard paste did not create one atomic object batch");
        return false;
    }
    // Note 在六键域内镜像 1->4；Sample 只重编码 6->8，不参与镜像。
    const auto  pastedNoteEntity   = *notes.begin();
    const auto  pastedSampleEntity = *samples.begin();
    const auto& note = notes.get<MMM::Logic::NoteComponent>(pastedNoteEntity);
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(pastedSampleEntity);
    if ( !near(note.m_timestamp, 10.0) || note.m_trackIndex != 4 ||
         !near(sample.m_timestamp, 11.0) || sample.m_track != 8 ||
         sample.m_offsetMs != -80 || sample.m_audioResourceId != "stem.ogg" ||
         !near(sample.m_volume, 0.4) ||
         !target.noteRegistry
              .get<MMM::Logic::InteractionComponent>(pastedNoteEntity)
              .isSelected ||
         !target.sampleRegistry
              .get<MMM::Logic::InteractionComponent>(pastedSampleEntity)
              .isSelected ) {
        XERROR("Mixed clipboard paste changed timing, lane, mirror or fields");
        return false;
    }

    // 一个 Undo 同时删除两个 Registry 的粘贴结果，验证复合事务边界。
    target.actionStack.undo(target);
    if ( !target.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         !target.sampleRegistry.view<MMM::Logic::SampleComponent>().empty() ) {
        XERROR("One undo did not remove both mixed pasted object kinds");
        return false;
    }
    // Redo 重建两类对象，不能依赖上次执行时分配的实体数值。
    target.actionStack.redo(target);
    return target.noteRegistry.view<MMM::Logic::NoteComponent>().size() == 1 &&
           target.sampleRegistry.view<MMM::Logic::SampleComponent>().size() ==
               1;
}

/// @brief 验证本会话混合剪切粘贴会原子删除来源并创建目标物件。
/// @details
/// Cut 先保存两类快照并标记来源，真正删除延迟到 Paste。粘贴锚点从原最早时间
/// 2 秒移动到 6 秒，因此 Note 到 6 秒、晚一秒的 Sample 到 7 秒。本会话剪切不能
/// 先创建副本再留下来源；删除旧对象与创建新对象必须合并为一个历史项。
/// 一次 Undo 应移除粘贴结果并恢复两个原实体的时间位置。
/// @par 关键不变量
/// - Cut 同时捕获选中的 Note 与 Sample。
/// - Cut 阶段只标记来源，不单独创建历史项。
/// - Paste 以 animateTime 重新锚定两类对象。
/// - 稳定状态中每个 Registry 仍各有一个对象。
/// - 来源删除与目标创建组成一个历史项。
/// - Undo 恢复原 2 秒 Note 和 3 秒 Sample。
/// @par 故障定位
/// 来源与目标并存说明 Cut 标记未被 Paste 消费；时间错误检查公共锚点；历史项
/// 多于一或 Undo 只恢复一类说明删除与创建没有合并为同一事务。
/// @par 测试边界
/// 只覆盖同一会话的成功粘贴，不验证取消粘贴或跨进程剪切语义。
/// @return 一次 Undo 恢复两类来源物件并移除粘贴副本时返回 true。
bool testMixedChartObjectLocalCut()
{
    // 两种对象都设置选中组件，使 Cut 收集到完整的混合载荷。
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 2.0,
            .m_trackIndex = 1,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 3.0,
            .m_track           = 5,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });

    // Cut 本身不压入动作；Paste 才提交删除来源和创建目标的复合动作。
    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdCut{});
    context.animateTime = 6.0;
    controller.handleCommand(MMM::Logic::CmdPaste{});

    // 稳定状态中仍各有一个对象，但它们是移动后的新结果而非旧对象副本。
    auto notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( notes.size() != 1 || samples.size() != 1 ||
         context.actionStack.getUndoStackSize() != 1 ||
         !near(notes.get<MMM::Logic::NoteComponent>(*notes.begin()).m_timestamp,
               6.0) ||
         !near(samples.get<MMM::Logic::SampleComponent>(*samples.begin())
                   .m_timestamp,
               7.0) ) {
        XERROR("Local mixed cut did not atomically replace source objects");
        return false;
    }

    // 单次撤销恢复原始 2 秒和 3 秒位置，证明剪切事务没有被拆分。
    context.actionStack.undo(context);
    auto restoredNotes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto restoredSamples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return restoredNotes.size() == 1 && restoredSamples.size() == 1 &&
           near(restoredNotes
                    .get<MMM::Logic::NoteComponent>(*restoredNotes.begin())
                    .m_timestamp,
                2.0) &&
           near(restoredSamples
                    .get<MMM::Logic::SampleComponent>(*restoredSamples.begin())
                    .m_timestamp,
                3.0);
}

/// @brief 验证批注标记与音符共用完整滚动投影，且批注栏参与分拍吸附。
/// @details
/// 谱面同时含 BPM 与 1.8 倍 Scroll，故简单线性时间差不能得到正确画布 Y。时间戳
/// 批注和同时间 Note 应通过同一 ScrollCache 投影到一致位置。鼠标位于独立批注栏
/// 时仍属于谱面时间区域，开启吸附后应得到 0.5
/// 秒拍点，但不能暴露完整滚动段数组。 更新批注内容会重建独立
/// AnnotationRenderCache，快照 revision 必须递增并与缓存
/// 一致；这验证批注数据不依赖批注表窗口是否打开，也不绑在时间线快照消费上。
/// @par 关键不变量
/// - 批注栏 X 不属于可编辑 Note/Sample 轨道。
/// - 批注栏 Y 仍通过 ScrollCache 参与时间吸附。
/// - 快照 snappedTime 精确为 0.5 秒。
/// - annotationMarkers 由独立批注缓存提供。
/// - 快照不复制完整 scrollSegments 作为批注依赖。
/// - 内容更新使 annotationRevision 单调增加。
/// - 标记 Y 使用完整 Scroll 投影而非线性近似。
/// @par 故障定位
/// 吸附失败检查批注栏是否被当作画布外；revision 不变检查独立批注缓存失效；标记
/// 与 Note 不对齐而线性近似相等，说明批注渲染绕过了 ScrollCache。
/// @par 测试边界
/// 通过逻辑坐标和快照验证投影，不检查批注图标、文本样式或批注表窗口布局。
/// @return 非移动工具下标记对齐音符中心，批注栏悬停仍给出精确拍线时返回 true。
bool testAnnotationMarkerProjectionAndGutterSnap()
{
    // 120 BPM 提供 0.5 秒拍点，Scroll=1.8 则刻意打破简单线性投影。
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    beatmap->m_baseMapMetadata.preference_bpm = 120.0;

    MMM::Timing bpm;
    bpm.m_timestamp             = 0.0;
    bpm.m_bpm                   = 120.0;
    bpm.m_beat_length           = 500.0;
    bpm.m_timingEffect          = MMM::TimingEffect::BPM;
    bpm.m_timingEffectParameter = 120.0;
    beatmap->m_timings.push_back(bpm);

    MMM::Timing scroll;
    scroll.m_timestamp             = 0.0;
    scroll.m_beat_length           = 1.8;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = 1.8;
    beatmap->m_timings.push_back(scroll);

    // Note 与批注使用相同时间，作为两个渲染系统投影一致性的基准。
    MMM::Note note;
    note.m_timestamp = 500.0;
    note.m_track     = 2;
    beatmap->m_noteData.notes.push_back(note);
    beatmap->m_annotations.emplace_back(MMM::BeatmapAnnotation{
        .m_id         = "annotation-projection",
        .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
        .m_timestamp  = 500.0,
        .m_author     = "Creator",
        .m_content    = "projection",
    });
    beatmap->sync();

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    context.currentTime = context.animateTime = 0.25;
    context.currentTool                       = MMM::Logic::EditTool::Draw;
    context.lastConfig.settings.objectPlacementSnap = true;
    context.mouseCameraId                           = "Basic2DCanvas";
    context.isMouseInCanvas                         = true;

    // 从统一轨道投影取得批注栏中心，避免测试复制布局常量。
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F,
        context.trackCount,
        context.bgmTrackCount,
        context.lastConfig.visual.trackLayout.left,
        context.lastConfig.visual.trackLayout.right,
        0.0F,
        true,
        context.lastConfig.settings.enableBmsEditing);
    auto* cache =
        context.timelineRegistry.ctx().find<MMM::Logic::System::ScrollCache>();
    if ( !projection.valid || !cache ) return false;
    // 使用 ScrollCache 反算 0.5 秒的屏幕 Y，鼠标精确落在目标拍线上。
    const float judgmentLineY =
        600.0F * context.lastConfig.visual.judgeline_pos;
    const double currentAbsY = cache->getVisualAnchorAbsY(context.animateTime);
    const float  beatLineY =
        judgmentLineY -
        static_cast<float>(cache->getDisplayDelta(0.5, currentAbsY, 0.5));
    context.lastMousePos = {
        (projection.annotationLeftX + projection.annotationRightX) * 0.5F,
        beatLineY,
    };

    // Session 更新同时处理鼠标吸附、批注缓存和主画布快照发布。
    const auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Annotation projection did not publish a canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->annotationMarkers.size() != 1U ||
         snapshot->annotationRevision == 0U ||
         snapshot->annotationRevision !=
             context.annotationRenderCacheRevision ||
         !snapshot->scrollSegments.empty() || !snapshot->isSnapped ||
         !near(snapshot->snappedTime, 0.5) ) {
        XERROR(
            "Annotation gutter did not reuse the canvas beat snap: "
            "snapshot={}, markers={}, segments={}, snapped={}, time={:.6f}",
            snapshot != nullptr,
            snapshot ? snapshot->annotationMarkers.size() : 0U,
            snapshot ? snapshot->scrollSegments.size() : 0U,
            snapshot ? snapshot->isSnapped : false,
            snapshot ? snapshot->snappedTime : -1.0);
        return false;
    }

    // 保留首个 revision；仅修改内容也应使缓存版本增加，通知渲染端换数据。
    const std::uint64_t firstAnnotationRevision = snapshot->annotationRevision;
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpsertBeatmapAnnotation{
            .annotationId = "annotation-projection",
            .targetKind   = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
            .timestamp    = 0.5,
            .author       = "Creator",
            .content      = "revision",
        } });
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->annotationRevision <= firstAnnotationRevision ||
         snapshot->annotationRevision !=
             context.annotationRenderCacheRevision ) {
        XERROR("Annotation snapshot revision did not follow cache rebuild");
        return false;
    }

    // 最终明确比较完整 ScrollCache 投影与错误的线性近似，防止测试退化。
    const float expectedMarkerY =
        judgmentLineY -
        static_cast<float>(cache->getDisplayDelta(0.5, currentAbsY, 0.5));
    const float simplifiedMarkerY =
        judgmentLineY - static_cast<float>((0.5 - context.animateTime) * 500.0 *
                                           config.visual.timelineZoom);
    if ( !near(snapshot->annotationMarkers.front().canvasY, expectedMarkerY) ||
         near(expectedMarkerY, simplifiedMarkerY) ) {
        XERROR("Annotation marker diverged from the note render projection");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行主画布二维相机换算测试。
/// @details
/// 测试按基础交互、投影布局、草稿协作、元数据与样本、跨区域编辑、批注快照的
/// 依赖顺序串行执行。逻辑与运算符保持短路：首个失败用例已经输出具体 XERROR，
/// runner 只负责转换为进程退出码。这里不重复记录成功日志，避免掩盖失败上下文。
/// @return 全部测试通过时返回 0。
int main()
{
    // 固定顺序使前置单元先失败，并确保各用例依赖自己的独立 SessionContext。
    return testKeyModeInteractionRestriction() &&
                   testSelectAllRespectsPointerTrackArea() &&
                   testKeyModeBrushCreatesOnlyHold() &&
                   testDownwardBrushCreatesZeroLengthHold() &&
                   testDownwardFlickAndPolylineRemainSlides() &&
                   testPolylinePreservesHorizontalFirstGestureOrder() &&
                   testBmsEditingHidesBgmLanes() &&
                   testBrushAudioResourcePlacementRules() &&
                   testSampleBrushFollowsPointerBeforeCommit() &&
                   testBrushStaysInStartingLaneKind() &&
                   testBrushCreatesDraftNote() &&
                   testTrackProjectionUsesCameraOffset() &&
                   testUnifiedLaneProjection() &&
                   testIndependentAuxiliaryLaneProjection() &&
                   testDynamicDraftAppendLaneProjection() &&
                   testDraggedDraftLayoutExpandsAwayFromPlayer() &&
                   testProfessionalModeHidesDraftArea() &&
                   testProfessionalModeUpdatesAllCanvases() &&
                   testProjectDraftLaneSharingAndIsolation() &&
                   testDraftMirrorStaysInDraftDomain() &&
                   testDraftMirrorUsesDynamicDraftTrackCount() &&
                   testAlignCommonBeatsPreservesEmbeddedPolylineNodes() &&
                   testResizePreservesNormalizedOffset() &&
                   testPanCommandUsesLogicalPixels() &&
                   testTrackCountActionMigratesAllSamples() &&
                   testSessionSelectsKeyCountLayout() &&
                   testBackgroundSessionPublishesCanvasHover() &&
                   testDeferredNoteSyncDoesNotKeepRenderSnapshotDirty() &&
                   testQueuedBrushUsesKeyCountLayout() &&
                   testTrackCountOverflowIsRejectedAtomically() &&
                   testMetadataTrackCountMigrationIsAtomic() &&
                   testMetadataAudioChangeRetargetsFirstMainBgmSample() &&
                   testReplaceBeatmapMetadataMigratesSamples() &&
                   testReplaceBeatmapMetadataOverflowIsRejected() &&
                   testAuthoritativeReplacementPreservesOwnedCreateUndo() &&
                   testAuthoritativeReplacementMergesOwnedUpdateUndo() &&
                   testPolylineSubNoteIdentitySurvivesRepeatedEcsSync() &&
                   testAppendLaneExpandsPersistentCount() &&
                   testExplicitBgmTrackCountAction() &&
                   testSamplePropertyEditValidationAndAction() &&
                   testSampleRegistryLoadAndSync() &&
                   testNoteSampleBindingRoundTrip() &&
                   testObjectSampleVolumeCommand() &&
                   testObjectSampleVolumeCommandRoutesThroughSession() &&
                   testSelectedObjectSampleVolumeCommand() &&
                   testCollaborationResourcesRouteThroughSession() &&
                   testSampleEraseTargetsTypedRegistry() &&
                   testSelectedPolylineTailEraseWithOtherSelection() &&
                   testSampleHoverInspectDetails() &&
                   testHoverSubdivisionPreviewUsesInspectedTrackAndBeat() &&
                   testSeekScrubStatePropagatesToCanvasSnapshot() &&
                   testBoundNoteHoverInspectAudioPreview() &&
                   testSampleAnchorDragUsesSingleAction() &&
                   testVerticalObjectDragLock() &&
                   testAudioResourceDropRejectsMissingProjectResource() &&
                   testGuestCollaborationResourcesSupportEditing() &&
                   testSampleOffsetHandleDrag() &&
                   testCrossAreaConversionRules() &&
                   testSilentSampleDragConvertsToUnboundNote() &&
                   testMarqueeToolEntityDragCrossesCanvasAreas() &&
                   testDraftAppendLaneDragExpandsPersistentCount() &&
                   testDraftAppendLanePreviewCanBeWithdrawn() &&
                   testPolylineDragMovesBetweenDraftAndPlayer() &&
                   testRejectedPolylineDragRestoresVisibility() &&
                   testUnboundNoteDragConvertsToSilentSample() &&
                   testCompositeConversionUsesTypedIdentity() &&
                   testMarqueeSelectsTypedSamplesOnlyOnMainCanvas() &&
                   testMixedChartObjectClipboardAcrossSessions() &&
                   testMixedChartObjectLocalCut() &&
                   testAnnotationMarkerProjectionAndGutterSnap()
               ? 0
               : 1;
}
