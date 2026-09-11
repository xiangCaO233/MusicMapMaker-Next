/// @file Basic2DCanvas_Interaction.cpp
/// @brief 实现主画布输入路由、编辑手势、布局编辑和批注交互。
///
/// 本文件只消费 UI 主线程可见的不可变 RenderSnapshot，
/// 所有谱面修改均转换为 LogicCommandEvent 交给逻辑线程执行。
/// 输入处理按固定顺序拆分为拖放、快捷键、修饰键滚轮、
/// 布局编辑、批注侧栏以及普通物件/画布手势，
/// 高优先级区域一旦消费输入，低优先级路径不得再次解释同一手势。
///
/// 连续拖动命令通过位置、视觉时间、缩放和修饰键缓存去重，
/// 鼠标松开或状态取消时立即发布最终命令，不采用 sleep 或固定等待窗口。
/// 框选越界滚动使用每帧 deltaTime 推进视觉目标，
/// 只覆盖最新待处理状态且不阻塞本地选框反馈。
///
/// 组件布局编辑使用像素几何做命中和吸附，
/// 再将结果规范化回 CanvasComponentLayoutConfig；
/// 批注详情卡片完全基于快照数据绘制，不查询 ECS 或项目文件。
/// 文件拖放只收集系统事件，在对应画布获得处理机会时再分类发布。
#include "canvas/Basic2DCanvasInteraction.h"

#include "audio/AudioManager.h"
#include "canvas/AnnotationDetailLayout.h"
#include "canvas/AnnotationTargetHint.h"
#include "canvas/CanvasBlockedGesture.h"
#include "canvas/HoverLayerSelection.h"
#include "canvas/ObjectDragAutoPan.h"
#include "common/AudioResourceDragPayload.h"
#include "common/CanvasComponentLayout.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/CanvasCamera.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/CanvasContentVisibility.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 连续拖动编辑命令的像素去重阈值。
/// @details 小于该距离的触控板或高 DPI 抖动不重复跨线程发布，
/// 离散状态变化和鼠标释放仍会绕过去重提交最终值。
constexpr float CONTINUOUS_EDIT_MOUSE_EPSILON = 0.75f;

/// @brief 画布悬浮信息相对鼠标的屏幕偏移。
/// @details 偏移方向会按鼠标所在 viewport 象限翻转，避免窗口越界。
constexpr float CANVAS_HOVER_OVERLAY_OFFSET = 15.0f;

/// @brief 画布悬浮信息窗口背景透明度。
/// @details 保留谱面可见性，同时为文字提供跨皮肤的稳定对比底色。
constexpr float CANVAS_HOVER_OVERLAY_BACKGROUND_ALPHA = 0.70F;

/// @brief 画布悬浮信息窗口的内边距。
/// @details 同一值也用于批注详情卡片正文，使悬浮层视觉节奏一致。
constexpr float CANVAS_HOVER_OVERLAY_PADDING = 12.0F;

/// @brief 画布悬浮信息窗口的横向元素间距。
/// @details 用于标签、轨道信息及操作提示之间的水平分隔。
constexpr float CANVAS_HOVER_OVERLAY_ITEM_SPACING_X = 8.0F;

/// @brief 画布悬浮信息窗口的纵向元素间距。
/// @details 批注卡片标题、元数据与 Markdown 正文共用该垂直节距。
constexpr float CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y = 6.0F;

/// @brief 按轨道区域绘制一基轨道编号。
/// @param labelPrefix 可选的物件部件说明。
/// @param track 统一画布有符号轨道。
/// @param trackCount 玩家轨道数量。
/// @param draftTrackCount 持久化草稿轨道数量。
/// @param color 文本颜色；为空时使用 ImGui 默认文本颜色。
/// @details 统一有符号轨道空间中，负值表示草稿轨，
/// [0, trackCount) 表示玩家轨，之后的值表示 BGM 轨。
/// 显示编号始终转换为一基序号，labelPrefix 用于折线节点等子部件说明。
/// @warning UI 热路径：悬浮信息可见时每帧调用，只执行常量级格式化与绘制。
void renderHoverTrack(const char* labelPrefix, std::int32_t track,
                      std::int32_t trackCount, std::int32_t draftTrackCount,
                      const ImVec4* color = nullptr)
{
    // 内部绘制器统一处理可选前缀和颜色的四种组合，避免三个轨道
    // 分支各自维护不同格式字符串。
    const auto draw = [&](const char* label, std::int32_t number) {
        if ( color ) {
            if ( labelPrefix ) {
                // 子部件和轨道类型同时显示，帮助区分折线节点与主体。
                ImGui::TextColored(
                    *color, "%s %s: %d", labelPrefix, label, number);
            } else {
                // 没有子部件前缀时保持简洁的“轨道: 编号”格式。
                ImGui::TextColored(*color, "%s: %d", label, number);
            }
        } else if ( labelPrefix ) {
            ImGui::Text("%s %s: %d", labelPrefix, label, number);
        } else {
            ImGui::Text("%s: %d", label, number);
        }
    };

    if ( track < 0 ) {
        // 草稿轨的内部负索引并非简单取绝对值，公共 helper 按当前
        // 草稿轨数量转换为与画布左右顺序一致的显示编号。
        draw(TR("ui.canvas.draft_track").data(),
             Logic::draftTrackDisplayNumber(track, draftTrackCount));
    } else if ( track >= trackCount ) {
        // BGM 轨紧接玩家轨编码，减去玩家轨数量后再转为一基编号。
        draw(TR("ui.canvas.bgm_track").data(), track - trackCount + 1);
    } else {
        // 玩家轨内部零基，UI 统一显示为一基编号。
        draw(TR("ui.canvas.track").data(), track + 1);
    }
}

/// @brief 绘制当前悬浮批注所指向物件的高对比几何提示。
/// @param bounds 批注目标在画布局部坐标中的提示边界。
/// @param canvasPosition 画布左上角屏幕坐标。
/// @param canvasWidth 画布可见宽度。
/// @param canvasHeight 画布可见高度。
/// @details 将逻辑快照提供的局部矩形转换为屏幕坐标，
/// 先绘制低透明度填充，再叠加黑色粗描边与主题强调色细描边。
/// 顶部双层三角标记指向目标中心，使窄物件也容易定位。
/// 所有几何裁剪在画布范围内，不能覆盖相邻 Dock 窗口。
/// @warning UI 热路径：悬浮批注详情卡片时每帧调用一次，只追加固定数量 ImGui
/// 几何。
void renderAnnotationTargetHint(const AnnotationTargetHintBounds& bounds,
                                ImVec2 canvasPosition, float canvasWidth,
                                float canvasHeight)
{
    // bounds 已由纯几何 helper 约束到画布局部坐标；这里只叠加窗口
    // 屏幕原点，不修改其大小和命中语义。
    const ImVec2 minimum{ canvasPosition.x + bounds.left,
                          canvasPosition.y + bounds.top };
    const ImVec2 maximum{ canvasPosition.x + bounds.right,
                          canvasPosition.y + bounds.bottom };

    auto& skin        = Config::SkinManager::instance();
    auto  accentColor = skin.getColor("preview.judgeline");
    // SkinManager 的洋红色哨兵表示颜色键缺失；回退 ImGui CheckMark
    // 能随当前皮肤保持高对比，而不是把哨兵色直接显示给用户。
    if ( accentColor.r == 1.0F && accentColor.g == 0.0F &&
         accentColor.b == 1.0F ) {
        const ImVec4 fallback = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        accentColor = { fallback.x, fallback.y, fallback.z, fallback.w };
    }

    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(
        { accentColor.r, accentColor.g, accentColor.b, 1.0F });
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
        { accentColor.r, accentColor.g, accentColor.b, 0.16F });
    constexpr ImU32 SHADOW   = IM_COL32(0, 0, 0, 230);
    constexpr float ROUNDING = 4.0F;

    // 目标提示属于当前画布的前景层，与离屏 Vulkan 内容分离；
    // PushClipRect 防止描边和顶部三角越出内容区。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(
        canvasPosition,
        { canvasPosition.x + canvasWidth, canvasPosition.y + canvasHeight },
        true);
    drawList->AddRectFilled(minimum, maximum, fill, ROUNDING);
    // 六像素暗描边先提供轮廓，二点五像素强调色再表达选中来源。
    drawList->AddRect(minimum, maximum, SHADOW, ROUNDING, 0, 6.0F);
    drawList->AddRect(minimum, maximum, accent, ROUNDING, 0, 2.5F);

    // 标记以目标水平中心为锚点，暗色外三角比内芯各扩一点五像素，
    // 在复杂音符纹理上仍保持边缘可见。
    const float markerX = (minimum.x + maximum.x) * 0.5F;
    drawList->AddTriangleFilled({ markerX - 5.0F, minimum.y - 7.0F },
                                { markerX + 5.0F, minimum.y - 7.0F },
                                { markerX, minimum.y - 1.0F },
                                SHADOW);
    drawList->AddTriangleFilled({ markerX - 3.5F, minimum.y - 5.5F },
                                { markerX + 3.5F, minimum.y - 5.5F },
                                { markerX, minimum.y - 1.5F },
                                accent);
    drawList->PopClipRect();
}

/// @brief 组件布局拖动的基础吸附距离，单位逻辑像素。
/// @details 实际阈值会乘当前 DPI，保持不同显示缩放下相近的物理手感。
constexpr float CANVAS_COMPONENT_SNAP_DISTANCE = 8.0f;

/// @brief 从渲染快照实例取得实际文字内容边界。
/// @param instance 组件实例快照。
/// @return 与 Vulkan 字形几何一致的边界。
/// @details 内容边界用于命中、吸附和选中框，不等于组件允许移动的区域。
/// @warning UI 布局热路径：只复制四个浮点值。
Logic::CanvasComponentBounds canvasComponentContentBounds(
    const Common::Render::CanvasComponentInstanceSnapshot& instance)
{
    return { instance.left, instance.top, instance.right, instance.bottom };
}

/// @brief 从渲染快照实例取得其允许布局的区域。
/// @param instance 组件实例快照。
/// @return 普通组件为整张画布，拍号组件为向下扩展文字半高的拍内区间。
/// @details region 由逻辑渲染阶段根据组件语义预计算，
/// UI 只消费该约束，不自行推测时间文字或轨道组件的可移动范围。
/// @warning UI 布局热路径：只复制四个浮点值。
Logic::CanvasComponentBounds canvasComponentLayoutRegion(
    const Common::Render::CanvasComponentInstanceSnapshot& instance)
{
    return { instance.regionLeft,
             instance.regionTop,
             instance.regionRight,
             instance.regionBottom };
}

/// @brief 将矩形两侧、中心和上下边界追加为二维组件吸附目标。
/// @param bounds 目标对象的像素边界。
/// @param targetsX 可写纵向目标线缓存。
/// @param targetsY 可写横向目标线缓存。
/// @details 每个有效矩形贡献 left/center/right 与 top/center/bottom，
/// 调用方预留容量并在拖动结束后丢弃缓存，不跨帧保存目标指针。
/// @warning UI 布局热路径：每个显示组件或轨道调用一次，只追加固定六个值。
void appendCanvasComponentSnapTargets(
    const Logic::CanvasComponentBounds& bounds, std::vector<float>& targetsX,
    std::vector<float>& targetsY)
{
    // 退化矩形不提供吸附线，防止不可见组件把其它对象吸到零面积位置。
    if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) return;
    // 边缘和中心三类基准覆盖常见的同边、居中和相邻对齐需求。
    targetsX.push_back(bounds.left);
    targetsX.push_back((bounds.left + bounds.right) * 0.5f);
    targetsX.push_back(bounds.right);
    targetsY.push_back(bounds.top);
    targetsY.push_back((bounds.top + bounds.bottom) * 0.5f);
    targetsY.push_back(bounds.bottom);
}

/// @brief 将组件边界按指定像素偏移平移。
/// @param bounds 原始组件边界。
/// @param offsetX 横向偏移。
/// @param offsetY 纵向偏移。
/// @return 平移后的组件边界。
/// @details 宽高保持不变，只用于预览同步 KPS 组成员的最终包围框。
/// @warning UI 布局热路径：纯常量级数值计算。
[[nodiscard]] Logic::CanvasComponentBounds offsetCanvasComponentBounds(
    const Logic::CanvasComponentBounds& bounds, float offsetX, float offsetY)
{
    return {
        bounds.left + offsetX,
        bounds.top + offsetY,
        bounds.right + offsetX,
        bounds.bottom + offsetY,
    };
}

/// @brief 将有效组件边界并入外层包围框。
/// @param bounds 待并入的组件边界。
/// @param aggregate 可写外层包围框。
/// @param hasAggregate 是否已经写入首个有效边界。
/// @details 首个有效矩形直接初始化 aggregate，后续矩形逐边扩展；
/// 退化输入不会改变已有包围框或初始化状态。
/// @warning UI 布局热路径：只进行常量级边界比较。
void mergeCanvasComponentBounds(const Logic::CanvasComponentBounds& bounds,
                                Logic::CanvasComponentBounds&       aggregate,
                                bool& hasAggregate)
{
    // 不把零尺寸字形或隐藏实例纳入组外框。
    if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) return;
    if ( !hasAggregate ) {
        // 单独标志避免使用特殊浮点哨兵初始化 min/max。
        aggregate    = bounds;
        hasAggregate = true;
        return;
    }
    aggregate.left = std::min(aggregate.left, bounds.left);
    // 四条边分别取外扩极值，结果可直接作为一个整体吸附目标。
    aggregate.top    = std::min(aggregate.top, bounds.top);
    aggregate.right  = std::max(aggregate.right, bounds.right);
    aggregate.bottom = std::max(aggregate.bottom, bounds.bottom);
}

/// @brief 将全部已显示自定义组件追加为吸附目标，同步 KPS 按组外框处理。
/// @param snapshot 当前画布渲染快照。
/// @param config 自定义组件布局配置。
/// @param targetsX 可写纵向目标线缓存。
/// @param targetsY 可写横向目标线缓存。
/// @details 普通组件逐个提供自身边缘和中心；启用 KPS 同步时，
/// 同步成员先合并为一个组外框，避免同组内部实例互相吸附。
/// syncAllKpsComponentPositions 包含全局和逐轨 KPS，
/// syncKpsTrackRelativePositions 只将 instanceIndex>=0 的逐轨实例成组。
/// @warning UI 布局热路径：整体轨道移动时每帧遍历已缓存的组件快照一次。
void appendDisplayedCanvasComponentSnapTargets(
    const Common::Render::RenderSnapshot&      snapshot,
    const Config::CanvasComponentLayoutConfig& config,
    std::vector<float>& targetsX, std::vector<float>& targetsY)
{
    // 两种同步模式互斥解释；全量同步优先于仅逐轨相对位置同步。
    const bool groupAllKpsPositions =
        config.kps.visible && config.syncAllKpsComponentPositions;
    const bool groupKpsTrackPositions = config.kps.visible &&
                                        !groupAllKpsPositions &&
                                        config.syncKpsTrackRelativePositions;
    Logic::CanvasComponentBounds synchronizedKpsBounds;
    bool                         hasSynchronizedKpsBounds = false;

    for ( const auto& instance : snapshot.canvasComponentInstances ) {
        // 配置已隐藏组件既不参与吸附，也不能扩大 KPS 组外框。
        if ( !config.placement(instance.type).visible ) continue;

        const bool synchronizedKpsGroupMember =
            instance.type == Config::CanvasComponentType::Kps &&
            (groupAllKpsPositions ||
             (groupKpsTrackPositions && instance.instanceIndex >= 0));
        if ( synchronizedKpsGroupMember ) {
            // 组成员只合并边界，不单独追加目标线，防止整体拖动时
            // 被自身其它轨道 KPS 锁住。
            mergeCanvasComponentBounds(canvasComponentContentBounds(instance),
                                       synchronizedKpsBounds,
                                       hasSynchronizedKpsBounds);
            continue;
        }
        // 非同步组件保持独立吸附基准。
        appendCanvasComponentSnapTargets(
            canvasComponentContentBounds(instance), targetsX, targetsY);
    }

    if ( hasSynchronizedKpsBounds ) {
        // 所有成员处理完后，组外框仅作为一个矩形贡献六条基准线。
        appendCanvasComponentSnapTargets(
            synchronizedKpsBounds, targetsX, targetsY);
    }
}

/// @brief 判断组件任一横向基准是否与指定纵向目标线对齐。
/// @param bounds 组件最终像素边界。
/// @param targetX 纵向目标线横坐标。
/// @return 左边缘、中心或右边缘与目标线重合时返回 true。
/// @details 四分之一像素容差吸收归一化往返与 DPI 换算误差，
/// 不用于决定是否吸附，只用于吸附后的参考线显示。
bool canvasComponentAlignsWithX(const Logic::CanvasComponentBounds& bounds,
                                float                               targetX)
{
    constexpr float epsilon = 0.25f;
    return std::abs(bounds.left - targetX) <= epsilon ||
           std::abs((bounds.left + bounds.right) * 0.5f - targetX) <= epsilon ||
           std::abs(bounds.right - targetX) <= epsilon;
}

/// @brief 判断组件任一纵向基准是否与指定横向目标线对齐。
/// @param bounds 组件最终像素边界。
/// @param targetY 横向目标线纵坐标。
/// @return 上边缘、中心或下边缘与目标线重合时返回 true。
/// @details 与横向检查使用相同容差，确保边缘和中心参考线行为一致。
bool canvasComponentAlignsWithY(const Logic::CanvasComponentBounds& bounds,
                                float                               targetY)
{
    constexpr float epsilon = 0.25f;
    return std::abs(bounds.top - targetY) <= epsilon ||
           std::abs((bounds.top + bounds.bottom) * 0.5f - targetY) <= epsilon ||
           std::abs(bounds.bottom - targetY) <= epsilon;
}

/// @brief 绘制一条半透明虚线吸附参考线。
/// @param drawList 目标 ImGui 前景绘制列表。
/// @param start 参考线起点。
/// @param end 参考线终点。
/// @param color 半透明线条颜色。
/// @param thickness 线宽。
/// @param dashLength 单段虚线长度。
/// @param gapLength 相邻虚线间距。
/// @details 先将任意方向线段归一化，再按 dash+gap 步长插值；
/// 最后一段钳制到终点，保证参考线不越出画布边界。
/// @warning UI 布局热路径：仅吸附生效时调用，按画布单轴长度生成短线段。
void drawCanvasComponentSnapGuide(ImDrawList& drawList, const ImVec2& start,
                                  const ImVec2& end, ImU32 color,
                                  float thickness, float dashLength,
                                  float gapLength)
{
    const float deltaX = end.x - start.x;
    const float deltaY = end.y - start.y;
    const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    // 零长度没有方向向量，直接跳过以避免除零。
    if ( length <= 0.0f ) return;

    // 段长和间隔至少一像素，防止无效主题值造成无限循环。
    dashLength       = std::max(1.0f, dashLength);
    gapLength        = std::max(1.0f, gapLength);
    const float dx   = deltaX / length;
    const float dy   = deltaY / length;
    const float step = dashLength + gapLength;
    for ( float distance = 0.0f; distance < length; distance += step ) {
        // 尾段不足完整 dash 时缩短到剩余长度，不跨越 end。
        const float segmentEnd = std::min(distance + dashLength, length);
        drawList.AddLine(
            { start.x + dx * distance, start.y + dy * distance },
            { start.x + dx * segmentEnd, start.y + dy * segmentEnd },
            color,
            thickness);
    }
}

/// @brief 暗化组件实际可调区域以外的画布并标出区域边界。
/// @param drawList 目标 ImGui 前景绘制列表。
/// @param region 组件实例在画布局部坐标中的实际可调区域。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param canvasWidth 画布宽度。
/// @param canvasHeight 画布高度。
/// @param dpiScale 当前窗口内容缩放。
/// @details 将 region 规范化并钳制到画布后，分别绘制允许区域上、下、左、右
/// 四块半透明遮罩；完整覆盖画布的普通组件不绘制任何遮罩。
/// 最后以强调色描出可调区域边界，帮助用户理解拍号等受限组件的范围。
/// @warning UI 布局热路径：悬停或拖动受限组件时每帧调用，只生成固定四块遮罩
/// 和一个边框。
void drawCanvasComponentEditableRegionMask(
    ImDrawList& drawList, const Logic::CanvasComponentBounds& region,
    float canvasScreenX, float canvasScreenY, float canvasWidth,
    float canvasHeight, float dpiScale)
{
    // min/max 兼容反向边界，clamp 阻止逻辑快照异常值把遮罩扩出画布。
    const float left =
        std::clamp(std::min(region.left, region.right), 0.0f, canvasWidth);
    const float top =
        std::clamp(std::min(region.top, region.bottom), 0.0f, canvasHeight);
    const float right =
        std::clamp(std::max(region.left, region.right), 0.0f, canvasWidth);
    const float bottom =
        std::clamp(std::max(region.top, region.bottom), 0.0f, canvasHeight);
    // 退化允许区无法表达合法编辑范围，不绘制误导性的全屏遮罩。
    if ( right <= left || bottom <= top ) return;

    constexpr float edgeEpsilon  = 0.5f;
    const bool      coversCanvas = left <= edgeEpsilon && top <= edgeEpsilon &&
                              right >= canvasWidth - edgeEpsilon &&
                              bottom >= canvasHeight - edgeEpsilon;
    // 普通组件允许覆盖整张画布，边框与遮罩都没有额外信息价值。
    if ( coversCanvas ) return;

    const ImVec2    canvasMin{ canvasScreenX, canvasScreenY };
    const ImVec2    canvasMax{ canvasScreenX + canvasWidth,
                            canvasScreenY + canvasHeight };
    const ImVec2    allowedMin{ canvasScreenX + left, canvasScreenY + top };
    const ImVec2    allowedMax{ canvasScreenX + right, canvasScreenY + bottom };
    constexpr ImU32 maskColor = IM_COL32(0, 0, 0, 118);
    // 上下遮罩先覆盖全宽，再在允许区垂直范围内补左右两块，
    // 四块互不重叠且完整覆盖禁止编辑区域。
    if ( allowedMin.y > canvasMin.y ) {
        drawList.AddRectFilled(
            canvasMin, { canvasMax.x, allowedMin.y }, maskColor);
    }
    if ( allowedMax.y < canvasMax.y ) {
        drawList.AddRectFilled(
            { canvasMin.x, allowedMax.y }, canvasMax, maskColor);
    }
    if ( allowedMin.x > canvasMin.x ) {
        drawList.AddRectFilled({ canvasMin.x, allowedMin.y },
                               { allowedMin.x, allowedMax.y },
                               maskColor);
    }
    if ( allowedMax.x < canvasMax.x ) {
        drawList.AddRectFilled({ allowedMax.x, allowedMin.y },
                               { canvasMax.x, allowedMax.y },
                               maskColor);
    }

    // 边框线宽随 DPI 增长但保留至少 1.5 像素，浅色和深色皮肤中
    // 都能明确区分允许区与暗化区域。
    drawList.AddRect(allowedMin,
                     allowedMax,
                     IM_COL32(255, 218, 96, 210),
                     0.0f,
                     0,
                     std::max(1.5f, 2.0f * dpiScale));
}

/// @brief 将 ASCII 扩展名转换为小写。
/// @param value 输入扩展名。
/// @return 小写后的扩展名。
/// @details 只用于已由 filesystem 分离出的 ASCII 文件扩展名，
/// 不承担 Unicode 文件名大小写折叠。
/// @warning 拖放低频路径：按值接收并就地转换，不在 UI 热路径调用。
std::string toLowerAscii(std::string value)
{
    // 转为 unsigned char 后调用 std::tolower，避免负 char 触发未定义行为。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 判断拖拽路径是否为 zip 兼容谱面包。
/// @param path 拖拽路径。
/// @return 扩展名匹配临时阅览包格式时返回 true。
/// @details 比较不区分 ASCII 大小写；zip/7z 是通用容器，
/// mcz/osz/mpk 是支持的谱包扩展名，均交给临时只读项目入口处理。
/// 这里只按扩展名分类，不访问文件或判断压缩内容。
bool isTemporaryPackagePath(const std::filesystem::path& path)
{
    // path.extension 保留前导点，统一转小写后与固定白名单比较。
    const auto extension = toLowerAscii(Config::pathToUtf8(path.extension()));
    return extension == ".zip" || extension == ".7z" || extension == ".mcz" ||
           extension == ".osz" || extension == ".mpk";
}

/// @brief 跳转到主画布当前悬浮时间点。
/// @param snapshot 当前渲染快照，时间字段使用视觉时间域。
/// @return 成功发送跳转命令时返回 true。
/// @details 开启分拍吸附时优先使用 snappedTime，否则使用 hoveredTime；
/// 非有限目标或与当前视觉时间相同的点击不会发布冗余 Seek。
/// CmdSeek 接受播放时间，因此发布前扣除当前有效视觉偏移。
/// @warning UI 输入路径：只在点击导航手势触发时调用；不访问 ECS 或文件系统。
bool publishCanvasHoverSeek(const Common::Render::RenderSnapshot& snapshot)
{
    const double targetTime =
        snapshot.isSnapped ? snapshot.snappedTime : snapshot.hoveredTime;
    // 一微秒容差过滤浮点投影往返，避免原地点击重启音频或广播视野。
    if ( !std::isfinite(targetTime) ||
         std::abs(targetTime - snapshot.currentTime) <= 1e-6 ) {
        return false;
    }

    const double visualOffset = Config::AppConfig::instance()
                                    .getVisualConfig()
                                    .getEffectiveVisualOffset();
    // 视觉偏移只改变用户看到的时间位置，不改变谱面内部播放时间。
    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSeek{ targetTime - visualOffset }));
    return true;
}

/// @brief 获取无 ScrollSegment 快照下的默认绝对 Y 速度。
/// @return 默认绝对 Y 速度，单位像素/秒。
/// @details 500 像素/秒为基础投影，乘 timelineZoom 与无 SV 路径保持一致；
/// zoom 至少 0.01，避免反投影除零或产生极端时间跨度。
/// @warning UI 热路径：只读取视觉配置并做常量计算。
double defaultSnapshotAbsYSpeed()
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    return 500.0 * static_cast<double>(std::max(0.01f, visual.timelineZoom));
}

/// @brief 从 UI 快照估算指定显示时间对应的绝对 Y。
/// @param snapshot 当前渲染快照。
/// @param time 显示时间，单位秒。
/// @return 对应的绝对 Y。
/// @details 有分段时以 upper_bound 找到目标之后的首段，
/// 使用前一段起点 absY 与 speed 线性外推；早于首段时使用首段。
/// 该绝对坐标不含判定线与 renderScaleY，供拖动画布的时间反算复用。
/// @warning UI 热路径：Move 工具空白拖动画布时调用；只读取快照中的
/// ScrollSegment，不访问 ECS 或文件系统。
double snapshotAbsYAtTime(const Common::Render::RenderSnapshot& snapshot,
                          double                                time)
{
    if ( snapshot.scrollSegments.empty() ) {
        // 无 SV 快照采用与时间线缩放一致的线性速度。
        return time * defaultSnapshotAbsYSpeed();
    }

    auto it = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        time,
        [](double val, const Common::Render::ScrollSegment& segment) {
            return val < segment.time;
        });

    // 恰好位于段起点时 upper_bound 选择该段后的迭代器，prev 回到该段。
    const auto& segment = it == snapshot.scrollSegments.begin()
                              ? snapshot.scrollSegments.front()
                              : *std::prev(it);
    // 段起点累计距离加局部时间位移，允许负速 SV 自然反向推进。
    return segment.absY + (time - segment.time) * segment.speed;
}

/// @brief 尝试在指定 ScrollSegment 内按绝对 Y 反算显示时间。
/// @param snapshot 当前渲染快照。
/// @param index 目标 ScrollSegment 索引。
/// @param absY 目标绝对 Y。
/// @param outTime 反算出的显示时间。
/// @return 该 segment 覆盖目标绝对 Y 时返回 true。
/// @details 静止段只在 absY 与段锚点重合时返回段起始时间；
/// 非零速度段根据下一段时间计算端点绝对 Y，并以无序 min/max
/// 支持正速和负速。末段按速度方向延伸到正或负无穷。
/// @warning UI 热路径：优先测试当前时间所在 segment，跨段时才由调用方扩展搜索。
bool trySnapshotTimeAtSegmentAbsY(
    const Common::Render::RenderSnapshot& snapshot, size_t index, double absY,
    double& outTime)
{
    constexpr double EPSILON  = 1e-6;
    const auto&      segments = snapshot.scrollSegments;
    if ( index >= segments.size() ) {
        // 调用方扫描边界防御，越界不修改 outTime。
        return false;
    }

    const auto& segment = segments[index];
    if ( std::abs(segment.speed) <= EPSILON ) {
        // 零速段覆盖单一绝对坐标；命中时选择段起点作为稳定代表时间。
        if ( std::abs(absY - segment.absY) <= EPSILON ) {
            outTime = segment.time;
            return true;
        }
        return false;
    }

    const bool hasNext = index + 1 < segments.size();
    // nextTime 同时限定当前段有效时间区间，末段则开放到无穷。
    const double nextTime = hasNext ? segments[index + 1].time
                                    : std::numeric_limits<double>::infinity();
    const double endAbsY =
        hasNext
            ? segment.absY + (nextTime - segment.time) * segment.speed
            : (segment.speed > 0.0 ? std::numeric_limits<double>::infinity()
                                   : -std::numeric_limits<double>::infinity());
    // 速度可为负，因此端点先取 min/max；epsilon 吸收分段边界误差。
    const double minAbsY = std::min(segment.absY, endAbsY) - EPSILON;
    const double maxAbsY = std::max(segment.absY, endAbsY) + EPSILON;
    if ( absY < minAbsY || absY > maxAbsY ) {
        // 目标不落在该段的绝对坐标覆盖区，交由其它段继续尝试。
        return false;
    }

    // 命中后再验证反算时间没有越过当前段时间边界；负速和零附近
    // 浮点误差都由相同 epsilon 容忍。
    outTime = segment.time + (absY - segment.absY) / segment.speed;
    return outTime >= segment.time - EPSILON && outTime <= nextTime + EPSILON;
}

/// @brief 从 UI 快照估算指定绝对 Y 对应的显示时间。
/// @param snapshot 当前渲染快照。
/// @param absY 目标绝对 Y。
/// @return 对应的显示时间，单位秒。
/// @details 无分段时按默认绝对速度直接反算。
/// 有分段时先尝试当前视觉时间所在段，常见小幅拖动可 O(log n)+O(1) 命中；
/// 跨越 SV 边界时再线性扫描其它已缓存段。
/// 若目标落在所有有限覆盖之外，则选择绝对 Y 最近的首/末锚点外推。
/// @warning UI 热路径：Move 工具空白拖动画布时调用；通常命中当前
/// ScrollSegment，跨段拖拽时才扫描快照分段。
double snapshotTimeAtAbsY(const Common::Render::RenderSnapshot& snapshot,
                          double                                absY)
{
    if ( snapshot.scrollSegments.empty() ) {
        const double speed = defaultSnapshotAbsYSpeed();
        // 默认速度正常为正；防御近零配置时保持当前视觉时间。
        return std::abs(speed) > 1e-9 ? absY / speed : snapshot.currentTime;
    }

    auto currentIt = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        snapshot.currentTime,
        [](double val, const Common::Render::ScrollSegment& segment) {
            return val < segment.time;
        });
    // upper_bound 与正向投影使用相同分段选择规则，保证往返一致。
    const size_t currentIndex =
        currentIt == snapshot.scrollSegments.begin()
            ? 0
            : static_cast<size_t>(std::distance(snapshot.scrollSegments.begin(),
                                                std::prev(currentIt)));

    double outTime = snapshot.currentTime;
    // 大多数连续拖动仍处于当前段，优先命中可避免扫描完整 SV 表。
    if ( trySnapshotTimeAtSegmentAbsY(snapshot, currentIndex, absY, outTime) ) {
        return outTime;
    }

    for ( size_t i = 0; i < snapshot.scrollSegments.size(); ++i ) {
        if ( i == currentIndex ) {
            // 当前段已经测试，避免重复反算。
            continue;
        }
        if ( trySnapshotTimeAtSegmentAbsY(snapshot, i, absY, outTime) ) {
            // 首个覆盖该绝对坐标的段按快照顺序作为稳定解。
            return outTime;
        }
    }

    const auto& first = snapshot.scrollSegments.front();
    const auto& last  = snapshot.scrollSegments.back();
    const auto& edge =
        std::abs(absY - first.absY) < std::abs(absY - last.absY) ? first : last;
    // 超出所有段时选择最近端点外推，避免大幅拖动突然钳制在边界。
    if ( std::abs(edge.speed) <= 1e-9 ) {
        // 静止边缘无法按距离反算，只能返回其锚点时间。
        return edge.time;
    }
    return edge.time + (absY - edge.absY) / edge.speed;
}

/// @brief 计算框选拖出画布上下边缘时的自动滚动目标时间。
/// @param snapshot 当前渲染快照，时间字段使用视觉时间域。
/// @param viewportHeight 当前画布高度，单位像素。
/// @param mouseY 当前本地鼠标 Y 坐标，单位像素。
/// @param deltaTime 当前 UI 帧间隔，单位秒。
/// @param scrolled 输出是否需要执行自动滚动。
/// @return 自动滚动后的显示时间，单位秒。
/// @param isAccelerated 是否应用 Shift 加速。
/// @details 鼠标仍在画布内时保持当前时间且不滚动；越界距离按画布高度
/// 归一化后平方加速，Shift 再乘固定倍率。
/// 像素速度经 renderScaleY 还原到绝对 Y，再通过 SV 分段反算目标时间。
/// deltaTime 被限制在 1/240 到 1/15 秒，避免卡顿帧产生巨幅跳跃。
/// @warning UI 热路径：框选拖动时每帧调用；只做数值换算并读取快照。
double marqueeAutoScrollTargetTime(
    const Common::Render::RenderSnapshot& snapshot, float viewportHeight,
    float mouseY, float deltaTime, bool isAccelerated, bool& scrolled)
{
    scrolled = false;
    // 非有限坐标、退化视口或鼠标仍在内容内都不触发自动滚动。
    if ( !std::isfinite(mouseY) || !std::isfinite(viewportHeight) ||
         viewportHeight <= 1.0f ||
         (mouseY >= 0.0f && mouseY <= viewportHeight) ) {
        return snapshot.currentTime;
    }

    const double direction = mouseY < 0.0f ? 1.0 : -1.0;
    // 主画布未来时间向上：越过上边缘增加绝对 Y，越过下边缘减少。
    const float outsidePixels =
        mouseY < 0.0f ? -mouseY : mouseY - viewportHeight;
    if ( outsidePixels <= 0.0f ) {
        return snapshot.currentTime;
    }

    const auto&  visual = Config::AppConfig::instance().getVisualConfig();
    const double sensitivity =
        std::max(0.0f, visual.previewConfig.edgeScrollSensitivity);
    // 用户将灵敏度设为零时明确禁用自动滚动。
    if ( sensitivity <= 1e-6 ) {
        return snapshot.currentTime;
    }

    const double dt = std::clamp(std::isfinite(deltaTime) && deltaTime > 0.0f
                                     ? static_cast<double>(deltaTime)
                                     : 1.0 / 60.0,
                                 1.0 / 240.0,
                                 1.0 / 15.0);
    // 异常或非正帧间隔按 60 FPS 回退，再限制极端帧率对位移的影响。
    const double ramp =
        std::max(0.0,
                 static_cast<double>(outsidePixels) /
                     std::max(1.0, static_cast<double>(viewportHeight) * 0.18));
    // 18% 视口高度作为加速尺度，平方曲线在边缘附近保持精细，
    // 远离画布时快速提升速度。
    const double     acceleratedRamp                = ramp * ramp;
    constexpr double SHIFT_AUTO_SCROLL_ACCELERATION = 3.0;
    const double     acceleration =
        isAccelerated ? SHIFT_AUTO_SCROLL_ACCELERATION : 1.0;
    const double pixelsPerSecond =
        (6000.0 + 24000.0 * acceleratedRamp) * sensitivity * acceleration;
    // 基础速度保证刚越界即开始移动，二次项提供随距离增长的加速。
    const double scale = std::abs(snapshot.renderScaleY) > 1e-6f
                             ? static_cast<double>(snapshot.renderScaleY)
                             : 1.0;
    // renderScaleY 越大，同样屏幕像素对应的绝对 Y 位移越小。
    const double currentAbsY =
        snapshotAbsYAtTime(snapshot, snapshot.currentTime);
    const double targetAbsY =
        currentAbsY + direction * pixelsPerSecond * dt / scale;
    const double targetTime = snapshotTimeAtAbsY(snapshot, targetAbsY);
    // 只有有限且实际变化的时间才声明滚动，调用方据此发布 Seek。
    scrolled = std::isfinite(targetTime) &&
               std::abs(targetTime - snapshot.currentTime) > 1e-6;
    return scrolled ? targetTime : snapshot.currentTime;
}

/// @brief 开始一个固定在当前 ImGui viewport 内的画布悬浮信息窗口。
/// @param mousePos 当前鼠标屏幕坐标。
/// @return `ImGui::Begin` 的返回值；调用方必须始终调用 `ImGui::End`。
/// @details 根据鼠标位于 viewport 的左右/上下半区选择 pivot，
/// 将浮层放到指针朝屏幕中心的一侧，减少越出显示器的概率。
/// 浮层无输入、导航、停靠和持久化状态，只作为当前帧只读提示。
/// @warning UI 热路径：悬浮可见时每帧调用，不分配持久资源。
bool beginCanvasHoverOverlay(const ImVec2& mousePos)
{
    ImGuiViewport* viewport = ImGui::GetWindowViewport();
    // 默认放在鼠标右下方；位于 viewport 后半区时分别翻转轴向。
    ImVec2 pivot{ 0.0f, 0.0f };
    float  xOffset = CANVAS_HOVER_OVERLAY_OFFSET;
    float  yOffset = CANVAS_HOVER_OVERLAY_OFFSET;
    if ( viewport && mousePos.x > viewport->Pos.x + viewport->Size.x * 0.5f ) {
        // 右半区以窗口右上/右下角为锚点，偏移改向左侧。
        pivot.x = 1.0f;
        xOffset = -CANVAS_HOVER_OVERLAY_OFFSET;
    }
    if ( viewport && mousePos.y > viewport->Pos.y + viewport->Size.y * 0.5f ) {
        // 下半区以窗口底边为锚点，提示向上展开。
        pivot.y = 1.0f;
        yOffset = -CANVAS_HOVER_OVERLAY_OFFSET;
    }

    if ( viewport ) {
        // 固定到来源 viewport，避免多窗口平台后端把提示创建到主窗口。
        ImGui::SetNextWindowViewport(viewport->ID);
    }
    ImGui::SetNextWindowPos(ImVec2(mousePos.x + xOffset, mousePos.y + yOffset),
                            ImGuiCond_Always,
                            pivot);
    ImGui::SetNextWindowBgAlpha(CANVAS_HOVER_OVERLAY_BACKGROUND_ALPHA);

    constexpr ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoDocking;
    // NoInputs 使提示不阻挡画布 hover、滚轮或拖动状态。
    return ImGui::Begin("##CanvasHoverInspectOverlay", nullptr, overlayFlags);
}

/// @brief 读取批注 UI 皮肤颜色并在缺失时回退。
/// @param key 颜色键。
/// @param fallback 缺失颜色。
/// @return 可直接交给 ImDrawList 的颜色。
/// @details SkinManager 以不透明洋红色表示键缺失，
/// 此 helper 将哨兵转换为调用方提供的 ImGui 主题回退色。
/// @warning UI 热路径：只查询已加载皮肤颜色，不访问文件系统。
ImU32 annotationUiColor(std::string_view key, const ImVec4& fallback)
{
    const auto color =
        Config::SkinManager::instance().getColor(std::string(key));
    // 必须同时匹配 RGBA，允许皮肤合法使用其它透明度的洋红色。
    const bool missing = color.r == 1.0F && color.g == 0.0F &&
                         color.b == 1.0F && color.a == 1.0F;
    return ImGui::ColorConvertFloat4ToU32(
        missing ? fallback : ImVec4(color.r, color.g, color.b, color.a));
}

/// @brief 批注详情卡片单帧绘制数量上限。
/// @details 固定上限允许使用栈上数组，避免悬浮热路径动态分配；
/// 超出部分仍保留时间戳标记，只不展开详情卡片。
constexpr std::size_t MAX_VISIBLE_ANNOTATION_DETAIL_CARDS = 48U;

/// @brief 单张详情卡片对应的只读批注数据。
/// @details 仅保存当前 RenderSnapshot 内元素的观察指针，
/// 数组只在本函数调用期间存在，不跨帧保留这些地址。
struct AnnotationDetailCardEntry {
    /// @brief 卡片所在的时间戳分组。
    const Common::Render::AnnotationRenderMarker* marker{ nullptr };
    /// @brief 卡片展示的具体批注。
    const Common::Render::AnnotationRenderItem* item{ nullptr };
    /// @brief 批注在时间戳分组内的索引。
    std::size_t itemIndex{ 0U };
    /// @brief Markdown 正文完整排版高度。
    /// @details 用于计算滚动范围，卡片本身只显示有限行高。
    float contentHeight{ 0.0F };
};

/// @brief 批注详情卡片悬浮命中结果。
/// @details 调用方据此阻止底层画布手势、显示目标提示并保留滚动状态。
struct AnnotationDetailCardHit {
    /// @brief 被命中的时间戳分组。
    const Common::Render::AnnotationRenderMarker* marker{ nullptr };
    /// @brief 被命中的分组内批注索引。
    std::size_t itemIndex{ 0U };
    /// @brief 指针是否命中了卡片正文中的链接。
    /// @details 当前卡片禁用交互链接，但仍传播 Markdown 命中语义供后续扩展。
    bool linkHovered{ false };
    /// @brief 卡片是否消费了本帧滚轮输入。
    /// @details 只有可滚动正文实际改变偏移时为 true。
    bool wheelConsumed{ false };
};

/// @brief 获取批注目标类型的翻译键。
/// @param targetKind 批注绑定的目标类别。
/// @return 对应本地化标签键；未知值回退到时间戳。
/// @details PLAYER_OBJECT 与 AUDIO_SAMPLE 分别显示明确目标，
/// Timestamp 也是未来新增枚举无法识别时的安全通用回退。
/// @warning UI 热路径：只做常量枚举比较。
const char* annotationTargetLabelKey(
    ::MMM::BeatmapAnnotationTargetKind targetKind)
{
    if ( targetKind == ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
        // 玩家物件包含普通音符及其折线子节点。
        return "ui.annotation.target.player_object";
    }
    if ( targetKind == ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
        // 音频采样目标通过资源与轨道元数据进一步说明。
        return "ui.annotation.target.audio_sample";
    }
    return "ui.annotation.target.timestamp";
}

/// @brief 绘制当前可见批注的避让卡片与物件连线。
///
/// @details 容量与分配约束：
/// - 每帧最多展开 `MAX_VISIBLE_ANNOTATION_DETAIL_CARDS` 张卡片。
/// - 卡片条目和布局输入使用固定容量栈上数组。
/// - 超出上限的批注仍保留时间戳气泡。
/// - 条目保存快照观察指针，不复制作者和正文。
/// - Markdown 测量复用公共渲染器的只读布局入口。
/// - 本函数不访问文件系统、ECS 或批注表数据源。
/// - 不排序 markers，沿用逻辑快照提供的稳定视觉顺序。
/// - 每张卡片只产生固定数量基础几何和正文绘制。
///
/// @details 侧边选择：
/// - 先计算批注栏左右两侧可用宽度。
/// - 右侧满足最小宽度时优先放在右侧。
/// - 右侧不足时选择可用空间更大的一侧。
/// - 两侧均不足最小宽度时不绘制详情卡。
/// - 所有卡片在同一帧使用同一侧。
/// - 统一侧边避免连接线横穿主轨道并相互交叉。
/// - 卡片宽度不小于可读下限。
/// - 卡片宽度不超过舒适阅读上限。
/// - 正文宽度扣除两侧 padding 和滚动条空间。
/// - 极端布局下正文宽度至少保留一个像素。
///
/// @details 内容测量：
/// - 每个可见 marker 可贡献多条批注卡片。
/// - marker 的 canvasY 作为期望卡片中心来源。
/// - 作者为空时使用未知作者本地化标签。
/// - 目标类型通过稳定翻译键转换为可见标签。
/// - 轨道元数据仅在 track 非负时展示。
/// - 目标缺失状态计入卡片元数据高度。
/// - Markdown 正文先测量完整内容高度。
/// - 卡片正文显示高度受单卡上限约束。
/// - 超出显示高度的内容启用内部滚动。
/// - 标题、元数据、分隔线和正文间距共同计入卡片高度。
/// - 测量和实际绘制使用相同字体宽度。
/// - 空正文仍保留最小正文行高。
///
/// @details 避让布局：
/// - 卡片期望位置跟随对应 marker 的纵坐标。
/// - 公共布局 helper 在 topY/bottomY 内解决重叠。
/// - 相邻卡片之间保留固定间距。
/// - 上下边界钳制不会改变 marker 的真实位置。
/// - 连线从批注栏侧边出发。
/// - 连线经过固定 elbow 后连接卡片边缘。
/// - 左右放置时连接线方向对称。
/// - 卡片背景使用当前 ImGui WindowBg 并降低透明度。
/// - 边框、标题和正文沿用当前主题颜色。
/// - 连接线颜色来自批注皮肤语义键。
///
/// @details 滚动状态：
/// - 只有当前悬停卡片维护正文滚动偏移。
/// - `scrollItemId` 使用具体批注 ID，而不是 marker ID。
/// - 悬停切换到另一条批注时滚动位置归零。
/// - 同一条批注持续悬停时保留上一帧偏移。
/// - 滚轮步长依据当前字体尺寸计算。
/// - 内容未超高时滚轮不被卡片消费。
/// - 到达顶部继续向上时不消费滚轮。
/// - 到达底部继续向下时不消费滚轮。
/// - 实际改变 scrollY 时设置 wheelConsumed。
/// - scrollY 每帧钳制到零和最大偏移之间。
/// - 非悬停卡片始终从正文顶部绘制。
///
/// @details 命中与淡化：
/// - 命中使用画布局部坐标，与 marker 快照坐标一致。
/// - 只有当前画布窗口 hover 时卡片响应输入。
/// - 卡片矩形命中先于正文链接命中结果返回。
/// - 返回值精确包含 marker 指针和分组内 itemIndex。
/// - 绘制每张卡片前记录 ImDrawList 顶点起点。
/// - 绘制后只修改该卡片新增顶点的 alpha。
/// - 指针接近卡片时提高可见度。
/// - 指针远离时降低 alpha，减少谱面遮挡。
/// - 当前悬停卡片保持最高可见度。
/// - 淡化只影响本帧绘制顶点，不污染主题颜色。
/// - 返回后调用方负责目标提示和编辑手势。
/// @param markers 当前主画布已裁剪的批注时间戳分组。
/// @param projection 当前画布横向投影。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param targetWidth 画布宽度。
/// @param topY 轨道区顶部局部坐标。
/// @param bottomY 轨道区底部局部坐标。
/// @param pointerX 指针相对画布左侧的横坐标。
/// @param pointerY 指针相对画布顶部的纵坐标。
/// @param canvasHovered 指针是否位于画布窗口。
/// @param scrollItemId 当前保留滚动状态的批注 ID。
/// @param scrollY 当前批注卡片正文的纵向滚动偏移。
/// @return 指针命中的详情卡片及其分组内索引。
/// @details 函数先选择轨道区空间更充足的一侧，测量最多 48 条可见批注的
/// Markdown 高度，并按标记视觉顺序建立卡片布局输入。
/// layoutAnnotationDetailCards 在上下边界内消解卡片重叠；
/// 每张卡片以折线连接到批注目标横向来源，并显示作者、目标元数据和正文。
/// 只有当前悬浮卡片维护 scrollItemId/scrollY，切换卡片时重置偏移。
/// 绘制结束后根据指针距离统一衰减该卡片新增顶点的 alpha，
/// 避免未悬浮详情持续遮挡谱面物件。
/// @warning UI 热路径：只线性遍历当前快照最多 48 条可见批注，不排序、不访问
/// ECS 或文件系统。
AnnotationDetailCardHit renderConnectedAnnotationDetails(
    const std::vector<Common::Render::AnnotationRenderMarker>& markers,
    const Logic::CanvasLaneProjection& projection, float canvasScreenX,
    float canvasScreenY, float targetWidth, float topY, float bottomY,
    float pointerX, float pointerY, bool canvasHovered,
    std::string& scrollItemId, float& scrollY)
{
    AnnotationDetailCardHit result;
    // 没有标记或轨道垂直范围退化时，不建立任何临时布局。
    if ( markers.empty() || bottomY <= topY ) return result;

    // 卡片尺寸在可读性与画布占用之间取固定逻辑像素范围；
    // SCROLLBAR_SPACE 预留正文右侧滚动条，不挤压 Markdown 文本。
    constexpr float CARD_MARGIN     = 10.0F;
    constexpr float CARD_MIN_WIDTH  = 170.0F;
    constexpr float CARD_MAX_WIDTH  = 340.0F;
    constexpr float CARD_GAP        = 5.0F;
    constexpr float CARD_PADDING    = CANVAS_HOVER_OVERLAY_PADDING;
    constexpr float SCROLLBAR_SPACE = 7.0F;
    constexpr float CONNECTOR_ELBOW = 9.0F;
    const float     rightAvailable =
        targetWidth - projection.annotationRightX - CARD_MARGIN * 2.0F;
    const float leftAvailable = projection.annotationLeftX - CARD_MARGIN * 2.0F;
    // 右侧达到最小宽度时优先右置，否则比较两侧剩余空间；
    // 选择结果对本帧所有卡片一致，连线不会交叉穿越轨道。
    const bool placeRight =
        rightAvailable >= CARD_MIN_WIDTH || rightAvailable >= leftAvailable;
    const float available = placeRight ? rightAvailable : leftAvailable;
    // 两侧均不足最小宽度时只保留批注标记，不绘制不可读窄卡片。
    if ( available < CARD_MIN_WIDTH ) return result;

    // 实际宽度不超过最大值，超宽画布中正文仍保持舒适行长。
    const float cardWidth = std::min(CARD_MAX_WIDTH, available);
    const float cardLeftX =
        placeRight ? projection.annotationRightX + CARD_MARGIN
                   : projection.annotationLeftX - CARD_MARGIN - cardWidth;
    const float cardRightX = cardLeftX + cardWidth;
    const float markerCenterX =
        (projection.annotationLeftX + projection.annotationRightX) * 0.5F;
    // contentWidth 扣除双侧 padding 和滚动条槽，并至少保留一像素，
    // 防止极限布局把负宽度交给 Markdown 测量器。
    const float fontSize = ImGui::GetFontSize();
    const float contentWidth =
        std::max(1.0F, cardWidth - CARD_PADDING * 2.0F - SCROLLBAR_SPACE);

    const ImU32 connectorColor = annotationUiColor(
        "annotations.connector", ImVec4(0.42F, 0.72F, 0.96F, 0.86F));
    const auto& imguiStyle = ImGui::GetStyle();
    // 卡片背景取当前窗口色并降低 alpha，边框和文字继续复用 ImGui 主题。
    ImVec4 background           = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    background.w                = CANVAS_HOVER_OVERLAY_BACKGROUND_ALPHA;
    const ImU32 backgroundColor = ImGui::ColorConvertFloat4ToU32(background);
    const ImU32 borderColor     = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 headerColor     = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 mutedColor      = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 scrollbarBackgroundColor =
        ImGui::GetColorU32(ImGuiCol_ScrollbarBg);
    const ImU32 scrollbarColor = ImGui::GetColorU32(ImGuiCol_ScrollbarGrab);
    const ImU32 scrollbarHoverColor =
        ImGui::GetColorU32(ImGuiCol_ScrollbarGrabHovered);
    const UI::MarkdownStyle         markdownStyle = UI::defaultMarkdownStyle();
    const UI::MarkdownRenderOptions markdownOptions{
        // 测量和绘制必须使用同一 wrapWidth/compact/style，
        // 否则滚动范围与实际换行高度会不一致。
        .wrapWidth        = contentWidth,
        .compact          = true,
        .interactiveLinks = false,
        .style            = &markdownStyle,
    };

    std::array<AnnotationDetailCardEntry, MAX_VISIBLE_ANNOTATION_DETAIL_CARDS>
        entries{};
    // 使用固定栈数组分别保存数据引用和纯布局状态，cardCount 控制有效前缀。
    std::array<AnnotationDetailCardPlacement,
               MAX_VISIBLE_ANNOTATION_DETAIL_CARDS>
                placements{};
    std::size_t cardCount = 0U;

    auto appendMarker =
        [&](const Common::Render::AnnotationRenderMarker& marker) {
            // 标记在轨道区外十像素以上时无需展开详情，边缘容差保留
            // 刚进入视野的卡片过渡。
            if ( marker.canvasY < topY - 10.0F ||
                 marker.canvasY > bottomY + 10.0F ) {
                return;
            }
            for ( std::size_t index = 0U; index < marker.items.size();
                  ++index ) {
                // 达到栈数组上限后停止追加当前及后续标记，禁止越界。
                if ( cardCount >= entries.size() ) return;
                const auto& item = marker.items[index];
                // 先完整测量正文，显示高度再限制为一到五行；完整高度
                // 保留在 entry 中用于滚动条比例和最大偏移。
                const auto contentLayout =
                    UI::measureMarkdown(item.content, markdownOptions);
                const float visibleContentHeight =
                    std::clamp(contentLayout.size.y, fontSize, fontSize * 5.0F);
                entries[cardCount] = {
                    // marker/item 指针均指向当前快照容器，本帧布局完成前稳定。
                    &marker,
                    &item,
                    index,
                    std::max(fontSize, contentLayout.size.y),
                };
                placements[cardCount] = {
                    // preferredY 使用时间戳标记 Y，卡片高度包含标题、元数据、
                    // 两段间距、正文与上下 padding。
                    marker.canvasY,
                    CARD_PADDING * 2.0F +
                        CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y * 2.0F +
                        fontSize * 2.0F + visibleContentHeight,
                    0.0F,
                };
                ++cardCount;
            }
        };

    const bool ascending = markers.size() < 2U ||
                           markers.front().canvasY <= markers.back().canvasY;
    // 布局算法要求输入按视觉 Y 递增；快照可能因时间向上而降序，
    // 通过正向/反向遍历校正，避免额外排序和分配。
    if ( ascending ) {
        for ( const auto& marker : markers ) appendMarker(marker);
    } else {
        for ( auto marker = markers.rbegin(); marker != markers.rend();
              ++marker ) {
            appendMarker(*marker);
        }
    }
    // 所有候选均在视野外或受上限过滤时直接返回空命中。
    if ( cardCount == 0U ) return result;

    // 纯布局 helper 只修改 placement.topY，在轨道上下留四像素边界，
    // 相邻卡片至少保留 CARD_GAP。
    layoutAnnotationDetailCards(
        std::span<AnnotationDetailCardPlacement>(placements.data(), cardCount),
        topY + 4.0F,
        bottomY - 4.0F,
        CARD_GAP);

    auto* drawList = ImGui::GetWindowDrawList();
    // 卡片、连线和滚动条统一裁剪在轨道垂直范围与当前画布宽度内。
    drawList->PushClipRect(
        { canvasScreenX, canvasScreenY + topY },
        { canvasScreenX + targetWidth, canvasScreenY + bottomY },
        true);
    for ( std::size_t index = 0U; index < cardCount; ++index ) {
        const auto& entry     = entries[index];
        const auto& placement = placements[index];
        // 固定数组默认槽为空，防御无效条目但不会访问其指针。
        if ( !entry.marker || !entry.item ) continue;

        const float cardTopY    = placement.topY;
        const float cardBottomY = cardTopY + placement.height;
        const bool  hovered     = canvasHovered && pointerX >= cardLeftX &&
                             pointerX <= cardRightX && pointerY >= cardTopY &&
                             pointerY <= cardBottomY;
        // 多张卡片理论上不重叠；若边界恰好共享，只保留首个命中，
        // 保持滚轮和目标提示归属稳定。
        if ( hovered && !result.marker ) {
            result.marker    = entry.marker;
            result.itemIndex = entry.itemIndex;
        }

        const int firstVertex = drawList->VtxBuffer.Size;
        // 记录本卡片绘制前顶点下标，后续 alpha 衰减只影响其连线、
        // 背景、文字与滚动条，不修改此前卡片或谱面内容。
        const float opacity = annotationDetailOpacity(pointerX,
                                                      pointerY,
                                                      cardLeftX,
                                                      cardTopY,
                                                      cardRightX,
                                                      cardBottomY,
                                                      canvasHovered,
                                                      fontSize * 2.0F);
        const float sourceX =
            annotationConnectorSourceX(*entry.item, projection, markerCenterX);
        // 连接线从目标横向来源到批注时间点中心，再折向卡片边缘，
        // placeRight 决定肘部朝向，避免穿过卡片正文。
        const float cardEdgeX   = placeRight ? cardLeftX : cardRightX;
        const float elbowX      = placeRight ? cardEdgeX - CONNECTOR_ELBOW
                                             : cardEdgeX + CONNECTOR_ELBOW;
        const float cardCenterY = (cardTopY + cardBottomY) * 0.5F;
        const std::array<ImVec2, 4> connector{
            ImVec2{ canvasScreenX + sourceX,
                    canvasScreenY + entry.marker->canvasY },
            ImVec2{ canvasScreenX + markerCenterX,
                    canvasScreenY + entry.marker->canvasY },
            ImVec2{ canvasScreenX + elbowX, canvasScreenY + cardCenterY },
            ImVec2{ canvasScreenX + cardEdgeX, canvasScreenY + cardCenterY },
        };
        drawList->AddPolyline(connector.data(),
                              static_cast<int>(connector.size()),
                              connectorColor,
                              ImDrawFlags_None,
                              hovered ? 2.5F : 1.5F);
        // 悬浮卡片加粗连线和源点，帮助识别密集时间戳中的目标关系。
        drawList->AddCircleFilled(
            connector.front(), hovered ? 4.0F : 3.0F, connectorColor);

        const ImVec2 cardMin{ canvasScreenX + cardLeftX,
                              canvasScreenY + cardTopY };
        const ImVec2 cardMax{ canvasScreenX + cardRightX,
                              canvasScreenY + cardBottomY };
        // 背景与边框沿用窗口圆角/边框尺寸，在所有皮肤中与设置项视觉一致。
        drawList->AddRectFilled(
            cardMin, cardMax, backgroundColor, imguiStyle.WindowRounding);
        if ( imguiStyle.WindowBorderSize > 0.0F ) {
            drawList->AddRect(cardMin,
                              cardMax,
                              borderColor,
                              imguiStyle.WindowRounding,
                              ImDrawFlags_None,
                              imguiStyle.WindowBorderSize);
        }

        const std::string_view author =
            entry.item->author.empty()
                ? TR("ui.annotation.unknown_author").view()
                : std::string_view(entry.item->author);
        // 空作者使用本地化占位；string_view 仅在本次 AddText 调用期间引用。
        drawList->AddText(
            { cardMin.x + CARD_PADDING, cardMin.y + CARD_PADDING },
            headerColor,
            author.data(),
            author.data() + author.size());

        char       metadata[512]{};
        const auto target =
            TR(annotationTargetLabelKey(entry.item->targetKind));
        // metadata 使用固定栈缓冲，避免每张卡片创建临时格式化字符串。
        // 有轨道目标显示一基轨号，缺失目标以感叹号提示但仍保留批注。
        if ( entry.item->track >= 0 ) {
            std::snprintf(metadata,
                          sizeof(metadata),
                          "%.3f s · %s #%d%s",
                          entry.marker->timestamp,
                          target.data(),
                          entry.item->track + 1,
                          entry.item->targetMissing ? " !" : "");
        } else {
            std::snprintf(metadata,
                          sizeof(metadata),
                          "%.3f s · %s%s",
                          entry.marker->timestamp,
                          target.data(),
                          entry.item->targetMissing ? " !" : "");
        }
        const ImVec2 metadataPos{ cardMin.x + CARD_PADDING,
                                  cardMin.y + CARD_PADDING + fontSize +
                                      CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y };
        drawList->AddText(metadataPos, mutedColor, metadata);

        const ImVec2 contentMin{ cardMin.x + CARD_PADDING,
                                 metadataPos.y + fontSize +
                                     CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y };
        const ImVec2 contentMax{ cardMax.x - CARD_PADDING - SCROLLBAR_SPACE,
                                 cardMax.y - CARD_PADDING };
        // 正文区域位于元数据下方，并永久为滚动条留出右侧槽位。
        const float visibleContentHeight =
            std::max(1.0F, contentMax.y - contentMin.y);
        const float maxScrollY =
            std::max(0.0F, entry.contentHeight - visibleContentHeight);
        // 只有 scrollItemId 对应卡片恢复滚动位置，其它卡片从顶部绘制。
        float cardScrollY = entry.item->id == scrollItemId
                                ? std::clamp(scrollY, 0.0F, maxScrollY)
                                : 0.0F;
        if ( hovered ) {
            if ( scrollItemId != entry.item->id ) {
                // 指针切换到新卡片时将滚动所有权和偏移一起重置。
                scrollItemId = entry.item->id;
                scrollY      = 0.0F;
            }
            const auto wheelResult =
                // 每格滚轮移动三行，helper 负责钳制并报告是否真正消费。
                updateAnnotationDetailWheel(ImGui::GetIO().MouseWheel,
                                            scrollY,
                                            maxScrollY,
                                            fontSize * 3.0F);
            scrollY              = wheelResult.scrollY;
            cardScrollY          = scrollY;
            result.wheelConsumed = wheelResult.consumed;
        }

        auto renderOptions = markdownOptions;
        // 绘制阶段补充视口高度与垂直偏移，仍复用测量时的换行样式。
        renderOptions.maxHeight      = visibleContentHeight;
        renderOptions.verticalOffset = cardScrollY;
        const auto markdownResult =
            UI::renderMarkdownToDrawList(*drawList,
                                         contentMin,
                                         contentMax,
                                         entry.item->content,
                                         renderOptions);
        // 链接当前不可点击，但命中结果仍用于阻止底层画布手势。
        if ( hovered && markdownResult.linkHovered ) result.linkHovered = true;

        if ( maxScrollY > 0.01F ) {
            // 只有正文确实溢出时绘制细滚动条，避免短批注产生无意义槽位。
            constexpr float SCROLLBAR_WIDTH = 3.0F;
            const float     trackTop        = contentMin.y;
            const float     trackBottom     = contentMax.y;
            const float trackHeight = std::max(1.0F, trackBottom - trackTop);
            const float thumbHeight = std::clamp(
                // 滑块高度按可见比例计算，同时至少十二像素便于辨认。
                trackHeight * visibleContentHeight / entry.contentHeight,
                12.0F,
                trackHeight);
            const float thumbTravel = std::max(0.0F, trackHeight - thumbHeight);
            const float thumbTop =
                trackTop + thumbTravel * (cardScrollY / maxScrollY);
            // 滑块位置与钳制后的 cardScrollY 成比例，悬浮时使用主题高亮色。
            const float scrollbarX = cardMax.x - CARD_PADDING * 0.5F;
            drawList->AddRectFilled({ scrollbarX - SCROLLBAR_WIDTH, trackTop },
                                    { scrollbarX, trackBottom },
                                    scrollbarBackgroundColor,
                                    2.0F);
            drawList->AddRectFilled(
                { scrollbarX - SCROLLBAR_WIDTH, thumbTop },
                { scrollbarX, thumbTop + thumbHeight },
                hovered ? scrollbarHoverColor : scrollbarColor,
                2.0F);
        }
        // 统一衰减卡片、正文和连线，避免不透明文字继续遮挡音符。
        if ( opacity < 1.0F ) {
            // 只改写本卡片新增顶点的 alpha 通道，保留 RGB 和此前几何；
            // opacity 已由纯 helper 限制在合法范围。
            for ( int vertexIndex = firstVertex;
                  vertexIndex < drawList->VtxBuffer.Size;
                  ++vertexIndex ) {
                auto&      color = drawList->VtxBuffer[vertexIndex].col;
                const auto alpha =
                    static_cast<ImU32>((color >> IM_COL32_A_SHIFT) * opacity);
                color =
                    (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
            }
        }
    }
    // 恢复画布原裁剪栈，调用方随后可绘制其它悬浮提示。
    drawList->PopClipRect();
    return result;
}
}  // namespace

/// @brief 创建绑定到指定画布和逻辑相机的交互控制器。
/// @param canvasName UI 来源名称，用于侧栏事件路由。
/// @param cameraId 逻辑命令目标相机标识。
/// @details 构造时订阅全局 GLFWDropEvent，只把路径和鼠标位置复制到
/// 本实例 pending 队列；实际分类要等所属画布窗口获得处理机会。
/// 订阅回调不访问 ImGui、项目或文件系统，避免平台事件线程越界。
Basic2DCanvasInteraction::Basic2DCanvasInteraction(
    const std::string& canvasName, const std::string& cameraId)
    : m_canvasName(canvasName), m_cameraId(cameraId)
{
    // 订阅 ID 由实例保存，析构时精确取消同一个回调。
    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            // 日志只记录路径数量，不展开用户路径内容；pending 项按
            // 到达顺序保留，下一次 UI 更新统一决定归属。
            XINFO("CanvasInteraction received GLFWDropEvent with {} paths",
                  e.paths.size());
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
}

/// @brief 取消拖放事件订阅并销毁交互状态。
/// @details EventBus 不再持有捕获 this 的回调后，成员容器按值自动释放；
/// 析构不等待逻辑命令完成，也不直接停止共享音频预览池。
Basic2DCanvasInteraction::~Basic2DCanvasInteraction()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
}

/// @brief 判断连续拖动编辑命令是否需要发送，并更新缓存。
///
/// @details 去重键与边界：
/// - 首次调用时缓存无效，命令必须发送。
/// - 鼠标 X 或 Y 超过像素阈值时发送。
/// - 当前视觉时间变化时发送。
/// - 可见时间起点或终点变化时发送。
/// - 纵向渲染缩放变化时发送。
/// - 当前分拍除数变化时发送。
/// - 主修饰键或次修饰键变化时发送。
/// - 播放和自动平移可由调用方显式绕过去重。
/// - 判定发送时立即覆盖完整缓存。
/// - 判定不发送时保留上次已发布基线。
/// - 各连续工具持有彼此独立的缓存实例。
/// - 鼠标释放和工具切换会统一使缓存失效。
/// - 缓存不拥有快照，只复制必要标量。
/// - 本函数不发布事件，也不创建撤销记录。
/// - 固定阈值只过滤设备噪声，不引入时间等待。
/// - 最终释放命令不经过本 helper，不会被抑制。
/// - 时间比较使用远小于正常 UI 帧间隔的微秒级容差。
/// - 缩放比较使用独立浮点容差，不与像素阈值混用。
/// - 离散分拍和修饰键字段要求精确匹配。
/// - 去重只减少跨线程更新数量，不改变本地视觉反馈。
/// - 调用方在返回 true 后应立即发布对应更新命令。
/// @param last 对应手势类型的上一条已发布命令缓存。
/// @param pos 当前画布局部鼠标位置。
/// @param snapshot 当前视觉时间、可见范围、缩放和分拍快照。
/// @param primaryModifier 主修饰键状态。
/// @param secondaryModifier 次修饰键状态。
/// @return 任一影响编辑结果的输入变化超过容差时返回 true。
/// @details 除鼠标位移外，播放滚动、可见区变化、缩放、分拍和修饰键
/// 都可能改变同一屏幕位置对应的谱面结果，因此必须参与去重键。
/// 返回 true 时缓存立即更新为当前状态；调用方随后发布对应命令。
/// @warning UI 拖动热路径：只比较和写入固定字段，不分配或阻塞。
bool Basic2DCanvasInteraction::shouldSendContinuousEditCommand(
    LastContinuousEditCommand& last, glm::vec2 pos,
    const Common::Render::RenderSnapshot& snapshot, bool primaryModifier,
    bool secondaryModifier)
{
    // 时间容差远小于一帧，缩放容差过滤浮点往返，鼠标容差过滤
    // 亚像素噪声；离散字段则要求完全相等。
    constexpr double visualTimeEpsilon  = 1e-6;
    constexpr float  renderScaleEpsilon = 1e-5f;
    const bool       shouldSend =
        // 无有效历史的首帧必须发送，以建立逻辑侧手势起点。
        !last.valid ||
        std::abs(last.pos.x - pos.x) > CONTINUOUS_EDIT_MOUSE_EPSILON ||
        std::abs(last.pos.y - pos.y) > CONTINUOUS_EDIT_MOUSE_EPSILON ||
        std::abs(last.visualTime - snapshot.currentTime) > visualTimeEpsilon ||
        std::abs(last.visibleTimeStart - snapshot.visibleTimeStart) >
            visualTimeEpsilon ||
        std::abs(last.visibleTimeEnd - snapshot.visibleTimeEnd) >
            visualTimeEpsilon ||
        std::abs(last.renderScaleY - snapshot.renderScaleY) >
            renderScaleEpsilon ||
        last.beatDivisor != snapshot.currentBeatDivisor ||
        last.primaryModifier != primaryModifier ||
        last.secondaryModifier != secondaryModifier;
    if ( shouldSend ) {
        // 所有参与比较的字段成组更新，防止下一帧以混合代际判断变化。
        last.valid             = true;
        last.pos               = pos;
        last.visualTime        = snapshot.currentTime;
        last.visibleTimeStart  = snapshot.visibleTimeStart;
        last.visibleTimeEnd    = snapshot.visibleTimeEnd;
        last.renderScaleY      = snapshot.renderScaleY;
        last.beatDivisor       = snapshot.currentBeatDivisor;
        last.primaryModifier   = primaryModifier;
        last.secondaryModifier = secondaryModifier;
    }
    return shouldSend;
}

/// @brief 清空连续拖动编辑命令缓存。
/// @details 同时终止框选、画笔、移动、擦除四类缓存及右键擦除状态；
/// 下一次拖动更新必然发送首条命令，不会与上一手势错误去重。
/// @warning UI 手势边界路径：只写固定状态，不发布命令。
void Basic2DCanvasInteraction::resetContinuousEditCommands()
{
    // 每种工具拥有独立缓存，切换工具或释放按钮时必须全部失效，
    // 避免随后恢复旧工具复用过期位置和时间。
    m_lastMarqueeUpdateCommand.valid = false;
    m_lastBrushUpdateCommand.valid   = false;
    m_lastMoveUpdateCommand.valid    = false;
    m_lastEraseUpdateCommand.valid   = false;
    // 右键擦除的 active 状态不属于快照，需在本地同步清理。
    m_rightEraseActive = false;
}

/// @brief 在移动工具下绘制悬浮物件的项目音频试听按钮。
///
/// @details 显示条件：
/// - 仅 Move 工具显示对象音频预览控件。
/// - 当前项目必须存在并可解析资源 ID。
/// - 画布宽高必须为正。
/// - 左键画布手势开始后立即隐藏控件。
/// - 右键擦除事务活动时立即隐藏控件。
/// - 快照 hoverInspect 必须声明可预览音频。
/// - 目标实体必须有效。
/// - 资源 ID 不能为空。
/// - 目标可以是玩家音符、草稿音符或音频采样对象。
/// - 无法找到对应 hitbox 时不显示悬空控件。
///
/// @details 目标锁定：
/// - 首次显示使用当前 hoverInspect 目标。
/// - 指针进入控件、弹窗或桥接区后锁定上一目标。
/// - 音量编辑弹窗打开期间无条件保持目标。
/// - 锁定期间不被底层新 hover 对象替换。
/// - 锁定实体从快照消失时立即清空状态。
/// - 同一实体的样本子索引参与身份比较。
/// - 对象种类参与身份比较，防止不同 registry 混淆。
/// - 资源 ID 变化会刷新预览绑定。
/// - 新目标从快照初始化音量编辑值。
/// - 锁定目标保留尚在事件往返中的本地音量值。
///
/// @details hitbox 与锚点：
/// - 同一对象可能由头、体、尾等多个 hitbox 构成。
/// - 首次命中优先选择包含当前指针的 hitbox。
/// - 锁定后选择最接近上一锚点中心的 hitbox。
/// - 该策略避免折线或长条控件在节点间跳动。
/// - interactionHitboxScale 不改变控件视觉锚点。
/// - 锚点使用原始渲染 hitbox 尺寸。
/// - 每帧更新锚点位置以跟随时间滚动。
/// - 预览实例键不随锚点位置变化。
/// - 控件优先放在对象上方并水平居中。
/// - 控件矩形最终钳制在画布可见范围。
///
/// @details 控件与输入：
/// - 控件尺寸由当前 ImGui FrameHeight 派生。
/// - 一行固定容纳播放、停止、循环和音量四个操作。
/// - 进度条尺寸限制在可读范围。
/// - 稳定预览池键包含 camera、kind、entity 和 subIndex。
/// - 多谱面相同实体编号不会共享播放实例。
/// - 共享控件管理实际解码、播放池和音量弹窗。
/// - 本层只负责画布位置和逻辑命令桥接。
/// - 音量变化发布 `CmdUpdateObjectSampleVolume`。
/// - UI 不直接写 BeatMap 对象。
/// - 指针位于对象本体时不阻止正常 Move 手势。
/// - 指针位于按钮、弹窗或桥接通道时阻止画布。
/// - 桥接通道覆盖对象和面板之间的空隙。
/// - 保留边距随主题 ItemSpacing 调整且具有最小值。
/// @param currentSnapshot 当前画布快照与悬浮检查结果。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param targetWidth 画布内容宽度。
/// @param targetHeight 画布内容高度。
/// @param pointerX 鼠标画布局部横坐标。
/// @param pointerY 鼠标画布局部纵坐标。
/// @return 控件、音量编辑器或对象到控件桥接区需要阻挡画布手势时返回 true。
/// @details 仅 Move 工具且未进行其它按键手势时显示。
/// 指针离开物件后，只要仍在对象与控件联合保留区或音量编辑器打开，
/// 就锁定上一目标，避免控件在移动鼠标途中消失。
/// 同一实体可能拥有多个 hitbox，首次按指针命中优先，
/// 已锁定目标则选择距离上一锚点中心最近的 hitbox 以保持位置稳定。
/// @warning UI 热路径：只扫描快照 hitbox，不访问 ECS；试听播放由共享控件处理。
bool Basic2DCanvasInteraction::renderObjectAudioPreviewControls(
    const Common::Render::RenderSnapshot& currentSnapshot, float canvasScreenX,
    float canvasScreenY, float targetWidth, float targetHeight, float pointerX,
    float pointerY)
{
    const auto& inspect = currentSnapshot.hoverInspect;
    auto*       project = Logic::EditorEngine::instance().getCurrentProject();
    // 任何编辑手势开始后立即清空悬浮控件，防止按钮覆盖或消费正在
    // 进行的画布拖动；无项目和退化视口同样无法解析音频资源。
    if ( currentSnapshot.currentTool != Logic::EditTool::Move ||
         m_leftPressStartedOnCanvas || m_leftPressStartedOnEntity ||
         m_rightEraseActive || !project || targetWidth <= 0.0F ||
         targetHeight <= 0.0F ) {
        m_audioPreviewOverlay = {};
        return false;
    }

    const auto& style = ImGui::GetStyle();
    // 保留区按主题 ItemSpacing 派生且至少六像素，跨皮肤保持可到达性。
    const float retentionPadding = std::max(6.0F, style.ItemSpacing.x * 0.5F);
    const bool  pointerInsideLockedRetention =
        m_audioPreviewOverlay.valid &&
        // 音量弹窗打开时无条件锁定目标，否则指针必须位于对象和控件
        // 外包矩形的扩展区域内。
        (m_audioPreviewOverlay.volumeEditorOpen ||
         (pointerX >= std::min(m_audioPreviewOverlay.left,
                               m_audioPreviewOverlay.controlsLeft) -
                          retentionPadding &&
          pointerX <= std::max(m_audioPreviewOverlay.right,
                               m_audioPreviewOverlay.controlsRight) +
                          retentionPadding &&
          pointerY >= std::min(m_audioPreviewOverlay.top,
                               m_audioPreviewOverlay.controlsTop) -
                          retentionPadding &&
          pointerY <= std::max(m_audioPreviewOverlay.bottom,
                               m_audioPreviewOverlay.controlsBottom) +
                          retentionPadding));

    entt::entity           targetEntity{ entt::null };
    Logic::ChartObjectKind targetObjectKind{
        Logic::ChartObjectKind::PlayerNote
    };
    std::string_view targetAudioResourceId;
    float            targetVolume{ 1.0F };
    std::int32_t     targetSampleBindingSubIndex{ -1 };
    if ( pointerInsideLockedRetention ) {
        // 锁定期间完全沿用上一目标身份和音量，快照 hover 短暂切换
        // 不会让正在操作的控件跳到其它物件。
        targetEntity          = m_audioPreviewOverlay.entity;
        targetObjectKind      = m_audioPreviewOverlay.objectKind;
        targetAudioResourceId = m_audioPreviewOverlay.audioResourceId;
        targetVolume          = m_audioPreviewOverlay.volume;
        targetSampleBindingSubIndex =
            m_audioPreviewOverlay.sampleBindingSubIndex;
    } else if ( inspect.show && inspect.showAudioPreview &&
                inspect.entity != entt::null &&
                !inspect.audioResourceId.empty() ) {
        // 未锁定时从逻辑线程预计算的 hoverInspect 取得资源绑定，
        // UI 不读取实体组件或重新解析样本索引。
        targetEntity                = inspect.entity;
        targetObjectKind            = inspect.objectKind;
        targetAudioResourceId       = inspect.audioResourceId;
        targetVolume                = inspect.volume;
        targetSampleBindingSubIndex = inspect.sampleBindingSubIndex;
    } else {
        // 没有可试听目标时丢弃全部保留区域和预览池键。
        m_audioPreviewOverlay = {};
        return false;
    }

    const bool sameObject =
        m_audioPreviewOverlay.valid &&
        m_audioPreviewOverlay.entity == targetEntity &&
        m_audioPreviewOverlay.objectKind == targetObjectKind &&
        m_audioPreviewOverlay.audioResourceId == targetAudioResourceId &&
        m_audioPreviewOverlay.sampleBindingSubIndex ==
            targetSampleBindingSubIndex;
    // 样本子索引参与身份，同一折线/物件的不同采样绑定拥有独立控件。
    const float previousCenterX =
        (m_audioPreviewOverlay.left + m_audioPreviewOverlay.right) * 0.5F;
    const float previousCenterY =
        (m_audioPreviewOverlay.top + m_audioPreviewOverlay.bottom) * 0.5F;

    const Common::Render::Hitbox* anchor    = nullptr;
    float                         bestScore = std::numeric_limits<float>::max();
    // 只在当前快照 hitbox 中寻找同实体同种类候选；折线等对象可能
    // 有多个部件矩形，需要选择稳定锚点。
    for ( const auto& hitbox : currentSnapshot.hitboxes ) {
        if ( hitbox.entity != targetEntity || hitbox.kind != targetObjectKind ||
             hitbox.w <= 0.0F || hitbox.h <= 0.0F ) {
            continue;
        }

        const bool pointerInside =
            pointerX >= hitbox.x && pointerX <= hitbox.x + hitbox.w &&
            pointerY >= hitbox.y && pointerY <= hitbox.y + hitbox.h;
        const float centerX = hitbox.x + hitbox.w * 0.5F;
        const float centerY = hitbox.y + hitbox.h * 0.5F;
        const float targetX = sameObject ? previousCenterX : pointerX;
        // 新对象以鼠标为参考，已锁定对象以上一锚点中心为参考，
        // 避免多个相邻 hitbox 随指针小幅移动来回切换。
        const float targetY = sameObject ? previousCenterY : pointerY;
        const float deltaX  = centerX - targetX;
        const float deltaY  = centerY - targetY;
        const float score =
            pointerInside ? -1.0F : deltaX * deltaX + deltaY * deltaY;
        // 直接命中拥有负分，优先于任意距离；否则选择平方距离最小项。
        if ( score < bestScore ) {
            bestScore = score;
            anchor    = &hitbox;
        }
    }
    if ( !anchor ) {
        // 目标已从快照删除或全部 hitbox 退化时结束控件生命周期。
        m_audioPreviewOverlay = {};
        return false;
    }

    m_audioPreviewOverlay.valid      = true;
    m_audioPreviewOverlay.entity     = targetEntity;
    m_audioPreviewOverlay.objectKind = targetObjectKind;
    if ( !pointerInsideLockedRetention ) {
        // 只有从新 hover 取得目标时刷新绑定数据；锁定状态保留用户
        // 正在编辑的本地音量值，等待命令快照往返。
        m_audioPreviewOverlay.audioResourceId = targetAudioResourceId;
        m_audioPreviewOverlay.volume          = targetVolume;
        m_audioPreviewOverlay.sampleBindingSubIndex =
            targetSampleBindingSubIndex;
    }
    if ( !sameObject ) {
        // 预览实例键组合 camera、对象种类、实体和样本子索引，
        // 多谱面或同实体多样本不会共享错误的播放进度。
        const std::string previewInstanceId =
            "canvas/" + m_cameraId + "/" +
            std::to_string(static_cast<std::uint32_t>(targetObjectKind)) + "/" +
            std::to_string(
                static_cast<std::uint32_t>(entt::to_integral(targetEntity))) +
            "/" + std::to_string(targetSampleBindingSubIndex);
        m_audioPreviewOverlay.previewPoolKey =
            UI::makeProjectAudioPreviewPoolKey(previewInstanceId);
    }
    m_audioPreviewOverlay.left = anchor->x;
    // 每帧更新锚点边界，使谱面滚动时控件跟随物件而身份保持不变。
    m_audioPreviewOverlay.top    = anchor->y;
    m_audioPreviewOverlay.right  = anchor->x + anchor->w;
    m_audioPreviewOverlay.bottom = anchor->y + anchor->h;

    const float buttonSize =
        std::ceil(std::max(20.0F, ImGui::GetFrameHeight()));
    // 尺寸主要取主题控件高度，并对间距与进度条设置可读上下限。
    const float spacing =
        std::max(2.0F, std::min(style.ItemInnerSpacing.x, 5.0F));
    const float progressHeight = std::clamp(buttonSize * 0.16F, 4.0F, 7.0F);
    const float progressSpacing =
        std::max(2.0F, std::min(style.ItemInnerSpacing.y, 4.0F));
    const float rowWidth = buttonSize * 4.0F + spacing * 3.0F;
    // 一行四个按钮，下方/上方与进度条共同形成固定面板高度。
    const float panelHeight = progressHeight + progressSpacing + buttonSize;
    const float gap         = std::max(6.0F, style.ItemSpacing.y * 0.5F);

    float controlsX =
        (m_audioPreviewOverlay.left + m_audioPreviewOverlay.right - rowWidth) *
        0.5F;
    // 优先水平居中于物件，再钳制到画布左右边界。
    controlsX =
        std::clamp(controlsX, 0.0F, std::max(0.0F, targetWidth - rowWidth));
    float controlsY = m_audioPreviewOverlay.top - gap - panelHeight;
    // 优先放在物件上方，空间不足时钳制到画布内；桥接区保证鼠标可达。
    controlsY =
        std::clamp(controlsY, 0.0F, std::max(0.0F, targetHeight - panelHeight));

    m_audioPreviewOverlay.controlsLeft   = controlsX;
    m_audioPreviewOverlay.controlsTop    = controlsY;
    m_audioPreviewOverlay.controlsRight  = controlsX + rowWidth;
    m_audioPreviewOverlay.controlsBottom = controlsY + panelHeight;

    const auto result = UI::renderProjectAudioPreviewControls(
        // 共享控件负责播放池、进度和音量弹窗；这里仅提供稳定实例键
        // 与画布屏幕坐标布局。
        "CanvasObjectAudioPreview",
        *project,
        m_audioPreviewOverlay.audioResourceId,
        m_audioPreviewOverlay.previewPoolKey,
        m_audioPreviewOverlay.volume,
        &m_audioPreviewOverlay.volume,
        UI::ProjectAudioPreviewControlsLayout{
            .topLeft = { canvasScreenX + controlsX, canvasScreenY + controlsY },
            .width   = rowWidth,
            .buttonSize      = buttonSize,
            .buttonSpacing   = spacing,
            .progressHeight  = progressHeight,
            .progressSpacing = progressSpacing,
        });
    m_audioPreviewOverlay.volumeEditorOpen = result.volumeEditorOpen;
    if ( result.volumeChanged ) {
        // 音量变化转换为逻辑命令，实体种类和样本子索引精确定位绑定；
        // UI 不直接修改 BeatMap 对象。
        Logic::EditorEngine::instance().pushCommand(
            Logic::LogicCommand(Logic::CmdUpdateObjectSampleVolume{
                .entity   = m_audioPreviewOverlay.entity,
                .kind     = m_audioPreviewOverlay.objectKind,
                .subIndex = m_audioPreviewOverlay.sampleBindingSubIndex,
                .volume   = m_audioPreviewOverlay.volume,
            }));
    }

    const bool pointerInsideObject = pointerX >= m_audioPreviewOverlay.left &&
                                     pointerX <= m_audioPreviewOverlay.right &&
                                     pointerY >= m_audioPreviewOverlay.top &&
                                     pointerY <= m_audioPreviewOverlay.bottom;
    // bridge 合并物件与面板外包框并扩展 padding，允许鼠标穿越两者
    // 之间的 gap 而不让控件消失。
    const float bridgePadding = std::max(retentionPadding, gap);
    const float bridgeLeft =
        std::min(m_audioPreviewOverlay.left, controlsX) - bridgePadding;
    const float bridgeTop =
        std::min(m_audioPreviewOverlay.top, controlsY) - bridgePadding;
    const float bridgeRight =
        std::max(m_audioPreviewOverlay.right, controlsX + rowWidth) +
        bridgePadding;
    const float bridgeBottom =
        std::max(m_audioPreviewOverlay.bottom, controlsY + panelHeight) +
        bridgePadding;
    const bool pointerInsideBridge =
        pointerX >= bridgeLeft && pointerX <= bridgeRight &&
        pointerY >= bridgeTop && pointerY <= bridgeBottom;
    // 指针仍在物件本体但未进入控件时不阻挡正常 Move 手势；只有
    // 控件/弹窗或连接通道需要抢占画布输入。
    return result.hovered || result.volumeEditorOpen ||
           (pointerInsideBridge && !pointerInsideObject);
}

/// @brief 执行活动主画布的一帧交互调度。
///
/// @details 调度约束：
/// - 平台拖放先于当前谱面快照交互处理。
/// - 拖放可能异步请求项目或会话切换。
/// - 当前调用仍只使用传入的不可变快照。
/// - 快照为空时不解释快捷键和鼠标工具。
/// - 快照存在时快捷键先于鼠标状态机执行。
/// - 同帧工具切换可以影响后续交互分支。
/// - 鼠标交互负责常规手势和覆盖层路由。
/// - 瞬态 UI 无论是否有谱面都必须推进。
/// - 项目刚关闭时已有速度提示仍自然结束。
/// - 本函数不等待项目切换、逻辑命令或音频线程。
/// - `sourceManager` 只传给需要切换侧栏的拖放入口。
/// - 画布尺寸原样传给坐标投影和鼠标命令。
/// - 退化尺寸由具体交互函数执行防御处理。
/// - 调度本身不访问 ECS、磁盘或资源解码器。
/// - 所有跨线程操作都转换为事件或逻辑命令。
/// @param sourceManager UI 管理器观察指针，用于拖放后打开侧栏。
/// @param currentSnapshot 当前不可变渲染快照；为空时只处理拖放和瞬态 UI。
/// @param targetWidth 画布逻辑宽度。
/// @param targetHeight 画布逻辑高度。
/// @details 拖放优先处理，因为它可能触发项目切换；有快照时再处理
/// 快捷键和具体手势；最后无条件推进瞬态提示计时器。
/// @warning UI 热路径：每帧调用，不得在此加入同步文件读取或 ECS 遍历。
void Basic2DCanvasInteraction::update(
    UI::UIManager*                        sourceManager,
    const Common::Render::RenderSnapshot* currentSnapshot, float targetWidth,
    float targetHeight)
{
    // pending drop 已在平台回调中收集，本阶段按当前窗口 hover 决定归属。
    handleDrops(sourceManager);

    if ( currentSnapshot ) {
        // 快捷键先于鼠标交互，工具切换可在同帧被后续手势观察到。
        handleHotkeys(currentSnapshot);
        handleInteractions(currentSnapshot, targetWidth, targetHeight);
    }

    // 瞬态 UI 与谱面快照无关，即使项目刚关闭也要自然结束计时。
    updateTransientUi();
}

/// @brief 处理活动主画布上的 Ctrl/Command/Alt 修饰键滚轮。
///
/// @details 输入判定：
/// - 无渲染快照时不能解释工具语义并返回未消费。
/// - 绝对值不超过 0.01 的滚轮残差返回未消费。
/// - Ctrl 与 macOS Command 统一视为 Command 修饰键。
/// - Command 和 Alt 均未按下时留给普通画布滚动。
/// - Shift 只修改当前修饰键分支的步进方式。
/// - 返回 true 表示调用方不得再发送普通 CmdScroll。
/// - 本函数不保存鼠标坐标，也不改变 hover 状态。
///
/// @details 播放反馈协议：
/// - 通常先发送零位移 ModifierAdjustment 滚动意图。
/// - 该意图允许逻辑层按设置停止当前播放。
/// - 是否播放交互音效复用 stopPlaybackOnScroll 设置。
/// - 活动 Marquee 的 Command 滚动不先发送零位移意图。
/// - 框选滚动必须保持连续选择事务。
/// - 实际配置值未变化时不播放反馈音效。
/// - 反馈音效由统一 UI 入口触发。
///
/// @details Command+Alt 播放速度：
/// - 速度只在 0.25、0.50、0.75、1.00 四档间切换。
/// - 当前值不在预设中时先选择距离最近的档位。
/// - 等距时保留数组中更靠前的稳定档位。
/// - 正滚轮向更高速度移动一档。
/// - 负滚轮向更低速度移动一档。
/// - 到达首尾边界时保持原值。
/// - 与当前速度差值小于容差时不发布命令。
/// - 变化后启动非阻塞的两秒速度提示。
/// - 提示计时由 `updateTransientUi` 使用 DeltaTime 推进。
///
/// @details Command 缩放或框选：
/// - Marquee 正在选择且允许选择滚动时发布普通 CmdScroll。
/// - 该滚动保持 Shift 加速语义。
/// - 其它状态下 Command 调整 timelineZoom。
/// - 默认每个滚轮单位调整 0.1。
/// - Shift 将步长乘用户滚动倍率。
/// - timelineZoom 被限制在 0.1 到 10.0。
/// - 缩放未发生实际变化时不发布完整编辑器配置。
/// - 缩放配置通过 EditorEngine 统一更新。
///
/// @details Alt 分拍调整：
/// - 触控板分数滚轮按 cameraId 分别累计。
/// - 累计达到一个完整单位后才形成整数步。
/// - 小数余量保留到后续帧。
/// - 多谱面画布之间不共享余量。
/// - Alt 单独按整数步调整任意分拍除数。
/// - Alt+Shift 在常用音乐分拍预设间跳转。
/// - 常用集合为 1、2、3、4、6、8、12、16。
/// - 正步选择严格更大的下一预设。
/// - 负步选择严格更小的上一预设。
/// - 多个整数步逐档迭代，不跳过边界规则。
/// - 最终 beatDivisor 始终限制在 1 到 64。
/// - 钳制后与原值相同时不更新配置。
/// @param currentSnapshot 当前画布快照。
/// @param allowSelectionScroll 是否允许活动框选消费 Command 滚轮。
/// @return 本帧滚轮属于修饰键语义并已消费时返回 true。
/// @details Command+Alt 在离散预设间调整播放速度；
/// Command 单独在框选中滚动选择范围，否则调整时间线缩放；
/// Alt 单独调整分拍除数，Shift 时按常用预设跳转。
/// 修饰键调整前发布零位移 ModifierAdjustment，使逻辑层按设置停止播放，
/// 但活动框选滚动保留自身连续选择语义。
/// @warning UI 热路径：仅在修饰键滚轮出现时执行，发布常量数量命令。
bool Basic2DCanvasInteraction::handleModifierWheel(
    const Common::Render::RenderSnapshot* currentSnapshot,
    bool                                  allowSelectionScroll)
{
    if ( !currentSnapshot ) {
        // 所有滚轮语义依赖当前工具和选择状态，无快照不能安全解释。
        return false;
    }

    const auto& io               = ImGui::GetIO();
    const float wheel            = io.MouseWheel;
    const bool  isCommandPressed = io.KeyCtrl || io.KeySuper;
    // 小幅触控板残差或没有 Command/Alt 的普通滚轮留给后续滚动路径。
    if ( std::abs(wheel) <= 0.01f || (!isCommandPressed && !io.KeyAlt) ) {
        return false;
    }

    const bool isAltPressed   = io.KeyAlt;
    const bool isShiftPressed = io.KeyShift;
    auto&      engine         = Logic::EditorEngine::instance();
    const bool shouldPlayAdjustmentFeedback =
        engine.getEditorConfig().settings.stopPlaybackOnScroll;
    // 框选活动时 Command 滚轮扩展选择时间范围，不应先发零位移
    // Adjustment 打断其连续选择状态。
    const bool scrollsActiveSelection =
        allowSelectionScroll && isCommandPressed && !isAltPressed &&
        currentSnapshot->currentTool == Logic::EditTool::Marquee &&
        currentSnapshot->isSelecting;
    if ( !scrollsActiveSelection ) {
        // 零位移命令只表达修饰调整意图，由逻辑设置决定是否停止播放。
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdScroll{
                m_cameraId,
                0.0f,
                false,
                Logic::ScrollCommandIntent::ModifierAdjustment }));
    }

    if ( isCommandPressed && isAltPressed ) {
        // 播放速度限制为四个可预期档位，滚轮每次移动到相邻档。
        constexpr std::array<double, 4> presets = { 0.25, 0.50, 0.75, 1.0 };
        double                          currentSpeed =
            Audio::AudioManager::instance().getPlaybackSpeed();

        size_t bestIdx = 0;
        double minDiff = std::abs(currentSpeed - presets[0]);
        // 当前速度可能来自外部配置而不在预设中，先寻找最近档位作为基准。
        for ( size_t i = 1; i < presets.size(); ++i ) {
            double diff = std::abs(currentSpeed - presets[i]);
            if ( diff < minDiff ) {
                minDiff = diff;
                bestIdx = i;
            }
        }

        if ( wheel > 0.01f ) {
            // 边界档保持不变，不发布重复速度命令。
            if ( bestIdx < presets.size() - 1 ) bestIdx++;
        } else if ( wheel < -0.01f ) {
            if ( bestIdx > 0 ) bestIdx--;
        }

        double newSpeed = presets[bestIdx];
        // 浮点容差避免当前值已等于目标档时重复刷新提示和反馈。
        if ( std::abs(newSpeed - currentSpeed) > 1e-4 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetPlaybackSpeed{ newSpeed }));
            m_speedTooltipValue = static_cast<float>(newSpeed);
            // 提示保持两秒，由 updateTransientUi 使用 DeltaTime 非阻塞递减。
            m_speedTooltipTimer = 2.0f;
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
        return true;
    }

    if ( isCommandPressed ) {
        if ( scrollsActiveSelection ) {
            // 符号取反与普通画布滚动方向约定一致，Shift 交给逻辑层加速。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdScroll{ m_cameraId, -wheel, isShiftPressed }));
        } else {
            auto  editorCfg = engine.getEditorConfig();
            float step      = 0.1f;
            // Shift 使用用户配置的滚动倍率加速缩放步长。
            if ( isShiftPressed )
                step *= editorCfg.settings.scrollSpeedMultiplier;
            const float currentZoom = editorCfg.visual.timelineZoom;
            const float newZoom =
                std::clamp(currentZoom + wheel * step, 0.1f, 10.0f);
            // 缩放上下限防止投影退化；只有实际变化才发布完整配置。
            if ( std::abs(newZoom - currentZoom) > 0.0001f ) {
                editorCfg.visual.timelineZoom = newZoom;
                engine.setEditorConfig(editorCfg);
                if ( shouldPlayAdjustmentFeedback ) {
                    ::MMM::UI::PlayInteractionMouseUpFeedback();
                }
            }
        }
        return true;
    }

    auto      editorCfg       = engine.getEditorConfig();
    const int originalDivisor = editorCfg.settings.beatDivisor;

    // 高频触控板滚轮可能产生分数增量；按 cameraId 累积到整步，
    // 多谱面之间不共享残余值。
    static std::unordered_map<std::string, float> wheelAccumulator;
    float& acc = wheelAccumulator[m_cameraId];
    acc += wheel;

    int steps = 0;
    if ( acc >= 1.0f ) {
        // 一帧累计超过一格时保留整数步数，并把小数余量留给后续帧。
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    } else if ( acc <= -1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    }

    if ( steps != 0 ) {
        if ( isShiftPressed ) {
            // Shift 只在常用音乐分拍集合间跳转，避免逐个经过冷门除数。
            constexpr std::array<int, 8> presets = { 1, 2, 3, 4, 6, 8, 12, 16 };
            int current = editorCfg.settings.beatDivisor;

            if ( steps > 0 ) {
                // 每个正步选择严格大于当前值的首个预设，末端保持 16。
                for ( int i = 0; i < steps; ++i ) {
                    auto it = std::upper_bound(
                        presets.begin(), presets.end(), current);
                    current = it != presets.end() ? *it : presets.back();
                }
            } else {
                // 负步选择严格小于当前值的前一预设，首端保持 1。
                for ( int i = 0; i < -steps; ++i ) {
                    auto it = std::lower_bound(
                        presets.begin(), presets.end(), current);
                    current = it != presets.begin() ? *std::prev(it)
                                                    : presets.front();
                }
            }
            editorCfg.settings.beatDivisor = current;
        } else {
            // 无 Shift 时允许 1..64 内的任意整数除数逐步调整。
            editorCfg.settings.beatDivisor += steps;
        }
        editorCfg.settings.beatDivisor =
            std::clamp(editorCfg.settings.beatDivisor, 1, 64);
        // 只有钳制后值确实改变才发布配置与音效反馈。
        if ( editorCfg.settings.beatDivisor != originalDivisor ) {
            engine.setEditorConfig(editorCfg);
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
    }

    return true;
}

/// @brief 推进并绘制交互层的临时 UI。
///
/// @details 速度提示窗口：
/// - 计时器仅在播放速度实际改变后启动。
/// - 每帧按 ImGui DeltaTime 非阻塞递减。
/// - 计时结束后不再创建提示窗口。
/// - 窗口位置始终跟随当前鼠标屏幕坐标。
/// - 鼠标位于工作区右侧时水平锚点翻转。
/// - 鼠标位于工作区下侧时垂直锚点翻转。
/// - 偏移方向与锚点相反，窗口远离鼠标热点。
/// - 使用主 viewport 工作区判断边缘。
/// - 背景透明度固定为可读且不完全遮挡画布的值。
/// - 圆角和内边距只在窗口绘制期间压栈。
/// - 窗口无标题、无调整尺寸、无移动和导航。
/// - `NoInputs` 保证提示不消费滚轮和鼠标释放。
/// - `NoSavedSettings` 防止写入 imgui.ini。
/// - 皮肤 content 字体存在时临时使用该字体。
/// - 字体缺失时沿用当前 ImGui 字体。
/// - Begin 返回 false 时仍成对调用 End 和 PopStyleVar。
/// - 提示值是最近一次已发布的新播放速度。
/// - 本函数不查询音频播放进度或修改播放状态。
/// @details 当前仅维护播放速度提示，计时器按 ImGui DeltaTime 递减；
/// 窗口跟随鼠标并在主 viewport 右/下 30% 区域翻转锚点，避免越界。
/// 提示无输入、导航和持久化状态，不会抢占画布焦点。
/// @warning UI 热路径：每帧最多绘制一个播放速度提示窗口。
void Basic2DCanvasInteraction::updateTransientUi()
{
    if ( m_speedTooltipTimer > 0.0f ) {
        // 非阻塞推进剩余显示时间，计时结束后整个窗口分支自然跳过。
        m_speedTooltipTimer -= ImGui::GetIO().DeltaTime;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2         mousePos = ImGui::GetMousePos();

        // 默认以左上角锚定鼠标右下方；靠近工作区右/下边缘时翻转。
        ImVec2 pivot = ImVec2(0.0f, 0.0f);
        if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
            pivot.x = 1.0f;
        if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
            pivot.y = 1.0f;

        float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
        float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;
        // 偏移方向与 pivot 一致，使窗口始终远离鼠标热点而不遮挡操作。

        ImGui::SetNextWindowPos(
            ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
            ImGuiCond_Always,
            pivot);
        ImGui::SetNextWindowBgAlpha(0.7f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        // NoInputs 保证提示本身不消费后续滚轮或鼠标释放。

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 10));
        if ( ImGui::Begin("##SpeedTooltip", nullptr, flags) ) {
            ImFont* font = Config::SkinManager::instance().getFont("content");
            // 皮肤内容字体可选，缺失时沿用当前 ImGui 字体。
            if ( font ) ImGui::PushFont(font, font->LegacySize);
            ImGui::Text(TR("ui.toolbar.playback_speed_value").data(),
                        m_speedTooltipValue);
            if ( font ) ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

/// @brief 将全局平台拖放事件路由到当前悬浮画布。
///
/// @details 事件所有权：
/// - GLFW 回调只把路径复制到 `m_pendingDrops`。
/// - 实际路由延迟到当前 ImGui 帧判断窗口归属。
/// - 根窗口与子窗口 hover 均视为当前画布范围。
/// - 活动 ImGui 控件不会阻止窗口归属判断。
/// - 一次平台拖放只允许一个画布消费。
/// - pending 队列在本次判断后统一清空。
/// - 未悬停画布不会打开文件或切换侧栏。
/// - 多路径事件当前只把首路径解释为打开动作。
/// - 批量资源导入由专门资源入口负责。
/// - 本函数不解压谱包，也不直接加载谱面。
///
/// @details 路径分类：
/// - UTF-8 平台路径先转换为 `std::filesystem::path`。
/// - zip、7z、mcz、osz、mpk 属于临时谱包。
/// - 临时谱包由应用级主窗口路由，画布跳过。
/// - 目录由打开项目入口处理，画布跳过。
/// - 文件父目录作为待打开项目路径。
/// - 扩展名比较先进行 ASCII 小写规范化。
/// - 仅接受 mmm、osu、mc、imd 谱面文件。
/// - 未知扩展名不会产生项目切换事件。
/// - filesystem 查询使用 error_code，不引入异常机制。
/// - 查询失败按普通文件路径继续扩展名过滤。
///
/// @details 打开流程：
/// - 谱面文件通过 `OpenProjectEvent` 交给项目流程。
/// - 事件携带目标谱面的完整路径和打开来源。
/// - 项目流程先准备父目录资源，再选中目标谱面。
/// - 成功路由后请求显示谱面管理侧栏。
/// - `sourceManager` 仅作为侧栏事件的观察指针透传。
/// - 本函数不等待项目打开完成。
/// - 本函数不修改欢迎页或 Dock 布局状态。
/// - 后续会话切换由事件订阅者完成。
/// - 不受支持路径不阻止其它应用级入口处理。
/// - 路由完成后队列清理防止下一帧重复打开。
/// @param sourceManager UI 管理器观察指针，用于打开谱面管理侧栏。
/// @details 只处理 pending 队列的首路径；谱包由主窗口统一路由，
/// 目录留给打开项目入口，画布仅接受受支持的谱面文件。
/// 谱面文件通过 OpenProjectEvent 携带完整文件路径，
/// 逻辑流程先打开父项目再选择该谱面，随后显示谱面管理器。
/// 无论当前窗口是否悬浮，本次 pending 队列最终都会清空，
/// 保证同一系统拖放事件只由一个窗口消费。
/// @warning UI 低频路径：会查询文件类型，但只在实际拖放后执行。
void Basic2DCanvasInteraction::handleDrops(UI::UIManager* sourceManager)
{
    // 没有平台事件时常规帧立即返回，不触发窗口或文件系统查询。
    if ( m_pendingDrops.empty() ) return;

    // 允许 active 控件阻挡时仍判断根窗口归属，实际谱包/文件只交给
    // 鼠标所在画布，避免多谱面窗口重复打开。
    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if ( isHovered ) {
        for ( const auto& drop : m_pendingDrops ) {
            if ( !drop.paths.empty() ) {
                // 当前协议只使用首路径作为一个打开动作，多文件批量导入
                // 由专门资源入口处理，避免隐式打开多个项目。
                std::filesystem::path p = Config::utf8ToPath(drop.paths[0]);
                if ( isTemporaryPackagePath(p) ) {
                    // 主窗口路由负责谱包解压和临时只读状态，画布不重复处理。
                    continue;
                }

                std::error_code filesystemError;
                const bool      isDirectory =
                    std::filesystem::is_directory(p, filesystemError) &&
                    !filesystemError;
                // 文件夹拖放由应用级打开项目入口消费，当前画布只处理文件。
                if ( isDirectory ) continue;
                std::filesystem::path projectPath =
                    isDirectory ? p : p.parent_path();
                auto ext = Config::pathToUtf8(p.extension());
                // 扩展名按 ASCII 小写匹配，保留 Unicode 父目录原始路径。
                std::transform(
                    ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                const bool isBeatmap = ext == ".osu" || ext == ".imd" ||
                                       ext == ".mc" || ext == ".mmm";
                // 非谱面资源不在现有项目中隐式导入，交给其它拖放入口。
                if ( !isBeatmap ) continue;

                XINFO("File dropped on Canvas: {}, opening project: {}",
                      Config::pathToUtf8(p),
                      Config::pathToUtf8(projectPath));

                // 完整谱面路径让打开流程自行确定父项目并在就绪后选择文件。
                Event::OpenProjectEvent ev;
                ev.m_projectPath = p;
                ev.m_origin      = Event::ProjectOpenOrigin::BeatmapDrop;
                Event::EventBus::instance().publish(ev);

                // 同时请求显示谱面管理器，让用户看到项目加载结果与当前选择。
                Event::UISubViewToggleEvent evt;
                evt.sourceUiName           = m_canvasName;
                evt.uiManager              = sourceManager;
                evt.targetFloatManagerName = "SideBarManager";
                evt.subViewId =
                    UI::TabToSubViewId(UI::SideBarTab::BeatMapExplorer);
                evt.showSubView = true;
                Event::EventBus::instance().publish(evt);
            }
        }
    }
    // 非悬浮窗口也丢弃该轮全局事件，事件归属由同一 UI 帧的窗口顺序决定。
    m_pendingDrops.clear();
}

/// @brief 处理主画布工具切换和编辑快捷键。
///
/// @details 快捷键边界：
/// - Layout 工具拥有专用键鼠语义并直接返回。
/// - 文本输入或弹窗活动时由 ShortcutUtils 阻挡。
/// - 录制新快捷键时原始按键只属于设置界面。
/// - 工具匹配顺序固定为 Move、Marquee、Draw、ColorBrush、ColorEraser。
/// - 同一组合误配给多个工具时只采用首个命中。
/// - 工具切换发布 `CmdChangeTool`，不直接改引擎字段。
/// - 同一帧最多发布一次工具切换。
/// - 删除只在本帧没有工具切换时处理。
/// - 删除发布 `CmdDeleteSelected` 交由逻辑层校验状态。
/// - 全局复制、粘贴、剪切、撤销、重做和播放由菜单层处理。
/// - 本地不重复发布全局快捷键命令。
/// - 同一帧工具切换先于鼠标交互执行。
/// - 本函数不访问 ECS registry。
/// - 本函数不读写系统剪贴板或项目文件。
/// - 未匹配按键不会改变当前手势锁存。
/// @param currentSnapshot 当前渲染快照。
/// @details Layout 工具有专用交互且不响应普通工具快捷键。
/// 文本输入、弹窗和快捷键录制状态由 ShortcutUtils 统一阻挡；
/// 五种可编辑工具按固定顺序匹配，首个命中即发布一次 CmdChangeTool。
/// 删除快捷键只在没有工具切换时处理，全局复制/撤销/播放由主菜单负责。
/// @warning UI 热路径：每帧检查输入状态；禁止加入文件系统访问、ECS
/// 全量遍历或阻塞操作。
void Basic2DCanvasInteraction::handleHotkeys(
    const Common::Render::RenderSnapshot* currentSnapshot)
{
    // Layout 模式的键鼠语义由 handleLayoutEditing 独占，避免快捷键
    // 在拖动配置时切换工具并留下半完成状态。
    if ( Logic::EditorEngine::instance().getCurrentTool() ==
         Logic::EditTool::Layout ) {
        return;
    }

    // 键盘焦点不在主画布时跳过，避免穿透设置页、弹窗或时间线窗口。
    if ( UI::ShortcutUtils::shouldBlockCanvasEditingShortcuts() ) return;
    // 录制新快捷键时原始按键只属于设置界面，不能触发编辑动作。
    if ( UI::ShortcutUtils::isShortcutRecordingActive() ) return;

    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    const std::array<Logic::EditTool, 5> editableTools{
        Logic::EditTool::Move,        Logic::EditTool::Marquee,
        Logic::EditTool::Draw,        Logic::EditTool::ColorBrush,
        Logic::EditTool::ColorEraser,
    };

    bool handledShortcut = false;
    // 同一物理组合即使误配给多个工具，也只采用数组顺序中的首项。
    for ( Logic::EditTool tool : editableTools ) {
        if ( UI::ShortcutUtils::isShortcutPressed(
                 UI::ShortcutUtils::getToolShortcut(settings, tool)) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdChangeTool{ tool }));
            handledShortcut = true;
            // 一帧只切换一次工具，不继续匹配其它配置。
            break;
        }
    }

    if ( !handledShortcut && UI::ShortcutUtils::isShortcutPressed(
                                 settings.shortcutConfig.deleteSelected) ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdDeleteSelected{}));
    }

    // Ctrl+C/V/X/Z/Y 与 Space 已由全局 MainMenuView 处理；本地不重复
    // 发布，避免一次按键在菜单和画布各执行一次。
}

/// @brief 从快照 hitbox 重建布局模式的音符外包矩形缓存。
///
/// @details 缓存规则：
/// - 只读取当前帧已经裁剪的 hitbox 快照。
/// - 只收集 `PlayerNote` 类型。
/// - null entity 不进入布局缓存。
/// - 非有限坐标或尺寸被忽略。
/// - 宽度或高度非正的 hitbox 被忽略。
/// - 同一实体的头、体、尾和节点边界合并。
/// - 合并外框取四条边的最小值与最大值。
/// - 每个实体最终只产生一个音符布局实例。
/// - 结果顺序沿用 hitbox 首次出现顺序。
/// - 后续反向命中据此保持上层实体优先。
/// - scratch 索引保存 entity 到结果槽位的映射。
/// - 两个成员容器每帧 clear 但保留容量。
/// - reserve 依据当前 hitbox 数降低重新分配概率。
/// - 缓存只用于 Layout 模式的四角命中和轮廓绘制。
/// - UI 线程不读取音符 ECS 组件。
/// - 结果只复制 entity 与数值边界，不保留快照指针。
/// @param currentSnapshot 当前画布渲染快照。
/// @details 只收集有效 PlayerNote hitbox，并按 entity 合并多个部件矩形；
/// 结果用于音符缩放/布局命中，不在 UI 线程访问 ECS。
/// 两个 scratch 容器复用容量，重建只处理当前已裁剪 hitbox。
/// @warning UI 布局热路径：Layout 模式每帧调用，线性遍历快照 hitbox。
void Basic2DCanvasInteraction::rebuildNoteLayoutInstances(
    const Common::Render::RenderSnapshot& currentSnapshot)
{
    // clear 保留容量，随后按本帧 hitbox 数预留，降低连续布局编辑分配。
    m_noteLayoutInstances.clear();
    m_noteLayoutIndexScratch.clear();
    m_noteLayoutInstances.reserve(currentSnapshot.hitboxes.size());
    m_noteLayoutIndexScratch.reserve(currentSnapshot.hitboxes.size());

    for ( const auto& hitbox : currentSnapshot.hitboxes ) {
        // 过滤非玩家物件、空实体、非有限和退化矩形，保证后续命中
        // 与缩放算法只接收规范边界。
        if ( hitbox.kind != Logic::ChartObjectKind::PlayerNote ||
             hitbox.entity == entt::null || !std::isfinite(hitbox.x) ||
             !std::isfinite(hitbox.y) || !std::isfinite(hitbox.w) ||
             !std::isfinite(hitbox.h) || hitbox.w <= 0.0f ||
             hitbox.h <= 0.0f ) {
            continue;
        }

        const Logic::CanvasComponentBounds bounds{
            hitbox.x, hitbox.y, hitbox.x + hitbox.w, hitbox.y + hitbox.h
        };
        const auto [it, inserted] = m_noteLayoutIndexScratch.try_emplace(
            hitbox.entity, m_noteLayoutInstances.size());
        if ( inserted ) {
            // 首个部件建立实体条目，索引 map 指向 vector 稳定位置。
            m_noteLayoutInstances.push_back({ hitbox.entity, bounds });
            continue;
        }

        // 同一实体的长条头、身体或折线节点合并为一个整体外包矩形。
        bool hasBounds = true;
        mergeCanvasComponentBounds(
            bounds, m_noteLayoutInstances[it->second].bounds, hasBounds);
    }
}

/// @brief 完成当前组件/轨道/音符布局编辑手势。
///
/// @details 收尾不变量：
/// - 只有本次手势实际修改配置时才持久化。
/// - 连续拖动积累的变化只保存一次。
/// - 保存后只播放一次统一鼠标释放反馈。
/// - 无修改手势也执行全部状态清理。
/// - 主轨道句柄恢复为 None。
/// - 辅助区句柄和区域类型恢复为 None。
/// - 音符缩放目标与角点句柄被清除。
/// - 文字组件目标、句柄和实例索引被清除。
/// - 横纵吸附参考线都被清除。
/// - 吸附目标向量 clear 后保留容量复用。
/// - KPS 同步起始快照不得跨手势保留。
/// - `m_layoutConfigurationChanged` 最后恢复为 false。
/// - 工具切换、中键接管和鼠标释放共用本入口。
/// - 视口退化后的鼠标释放也共用本入口。
/// - 重复调用不会再次保存或播放反馈。
/// - 下一帧布局从 AppConfig 和新快照重新建立。
/// @details 只有配置实际变化时持久化 AppConfig 并播放一次释放反馈；
/// 随后统一清除所有拖动目标、句柄、吸附线、冻结目标和 KPS 起始状态。
/// 该函数可由正常鼠标释放或视口失效路径调用，重复调用安全。
/// @warning UI 手势边界路径：配置保存可能访问磁盘，仅在拖动结束执行。
void Basic2DCanvasInteraction::finishLayoutEditing()
{
    if ( m_layoutConfigurationChanged ) {
        // 连续拖动只修改内存配置，最终释放时合并为一次磁盘保存和反馈。
        Config::AppConfig::instance().save();
        ::MMM::UI::PlayInteractionMouseUpFeedback();
    }
    m_trackLayoutDragHandle = TrackLayoutDragHandle::None;
    // 横向辅助区与主轨道拖动共享同一保存终点和状态清理。
    m_auxiliaryLayoutRegion      = AuxiliaryLayoutRegion::None;
    m_horizontalRegionDragHandle = HorizontalRegionDragHandle::None;
    m_noteScaleDragTarget.reset();
    m_noteScaleDragHandle = Logic::CanvasComponentDragHandle::None;
    m_canvasComponentDragTarget.reset();
    m_canvasComponentDragHandle        = Logic::CanvasComponentDragHandle::None;
    m_canvasComponentDragInstanceIndex = 0;
    m_canvasComponentSnapGuideX.reset();
    m_canvasComponentSnapGuideY.reset();
    m_canvasComponentSnapTargetsX.clear();
    m_canvasComponentSnapTargetsY.clear();
    m_layoutConfigurationChanged = false;
    // KPS 同步组保存的所有实例起点只对一次手势有效。
    m_synchronizedKpsTransformStarts.clear();
}

/// @brief 处理 Layout 工具下的全部轨道、辅助区、音符和文字组件编辑。
///
/// @details 输入与坐标域：
/// - `pointerX`、`pointerY` 是当前画布窗口的局部像素坐标。
/// - `canvasScreenX`、`canvasScreenY` 只用于转换辅助几何的屏幕坐标。
/// - TrackLayout 四条边使用零到一的归一化画布坐标。
/// - 组件快照边界和可编辑 region 使用画布局部像素坐标。
/// - 辅助区横坐标使用相机平移前的归一化世界坐标。
/// - `canvasHorizontalOffsetX` 只作用于横轴，不叠加到纵向投影。
/// - 判定线位置是画布高度比例，与 TrackLayout 上下边独立存储。
/// - 吸附阈值以逻辑像素表示，并按 DPI 缩放为显示阈值。
/// - 像素到配置的换算都在候选完成钳制后进行。
/// - 配置到像素的换算都使用当前帧已规范化布局。
///
/// @details 命中优先级：
/// - 已经开始的拖动始终锁定原目标和原句柄。
/// - 音符四角缩放句柄优先于文字组件。
/// - 文字组件四角和主体优先于主轨道大矩形。
/// - 主轨道边缘、中心和判定线优先于辅助区域。
/// - 辅助区按批注、草稿、BGM 的固定顺序消解重叠命中。
/// - 音符主体不在 Layout 模式移动，只允许四角调整全局渲染比例。
/// - 隐藏文字组件和零面积字形不能成为命中目标。
/// - 退化辅助区域不能成为移动或缩放目标。
/// - 指针不在当前画布时不能开始新手势。
/// - 已开始手势可在指针离开画布后继续并在释放时结束。
/// - 同一左键手势最多只能激活一个布局目标。
///
/// @details 手势起始快照：
/// - 按下时冻结当前 TrackLayout，拖动不累加上一帧候选。
/// - 按下时冻结组件 placement、内容边界和可编辑 region。
/// - 按下时冻结音符包围盒和当前 noteScaleX/noteScaleY。
/// - 按下时记录指针相对组件中心的偏移，防止组件瞬移。
/// - 主轨道和辅助区记录归一化指针起点。
/// - 横向缩放在按下时收集固定吸附目标。
/// - 同步 KPS 手势在按下时捕获所有受影响实例。
/// - KPS 捕获项保存 instanceIndex、resolved placement、边界和 region。
/// - 捕获数据只在本次拖动期间有效，释放后立即清空。
/// - 新快照只提供显示内容，不替换本次手势起始几何。
/// - 冻结起点保证结果不随 UI 帧率变化。
///
/// @details 文字组件语义：
/// - Move 改变归一化锚点，不改变字体比例。
/// - 四角缩放改变字体比例，并按抓取角点修正锚点。
/// - 每个组件都被限制在自身可编辑 region 内。
/// - 普通文字组件按类型只有一个可编辑 placement。
/// - KPS 可由汇总实例和多个逐轨实例组成。
/// - `instanceIndex == -1` 表示汇总或全局实例。
/// - 非负 instanceIndex 表示对应玩家轨道的实例。
/// - `syncAllKpsComponentPositions` 把所有 KPS 作为移动组。
/// - `syncKpsTrackRelativePositions` 只把逐轨 KPS 作为移动组。
/// - 全量位置同步优先于逐轨相对位置同步。
/// - `syncKpsTrackSizes` 只同步逐轨 KPS 的字体大小。
/// - 同步移动从各成员冻结 placement 应用同一像素位移。
/// - 同步缩放从各成员冻结边界应用同一目标字体比例。
/// - 每个同步成员仍按自己的 region 独立钳制。
/// - 同步组吸附使用合并外框，不使用单个成员边界。
/// - 被拖动对象及随动组成员不进入自己的吸附目标集合。
///
/// @details 吸附目标：
/// - 一个有效矩形贡献左、中心、右三条纵线。
/// - 一个有效矩形贡献上、中心、下三条横线。
/// - 可见文字组件可以成为主轨道移动的吸附目标。
/// - 主轨道总框和每条玩家轨道可以成为组件移动目标。
/// - 画布水平中心和垂直中心始终是可用目标。
/// - 画布左右可见边缘可以成为横向缩放目标。
/// - 其它辅助区左右边缘可以成为当前辅助区目标。
/// - 调整辅助区时主轨道左右边缘也参与吸附。
/// - 调整主轨道宽度时只收集其它区域与文字组件。
/// - 当前调整区域和随动对象必须从目标中排除。
/// - 隐藏组件不得提供不可见吸附线。
/// - 同步 KPS 静止组只以合并外框贡献一次目标。
/// - 等距候选依赖固定追加顺序保持结果稳定。
/// - resize helper 的边界钳制可能使候选无法抵达吸附点。
/// - 只有最终几何仍对齐目标时才显示参考线。
/// - 参考线在不适用句柄和鼠标释放时清空。
///
/// @details 主轨道和辅助区语义：
/// - 主轨道 Left/Right 只调整横向边界。
/// - 主轨道 Top/Bottom 只调整纵向边界。
/// - 主轨道 Move 保持宽高并同时移动四条边。
/// - JudgmentLine 只调整判定线纵向比例。
/// - 辅助区只支持水平 Move、Left 和 Right。
/// - 草稿区以右边界为锚，新增轨道向左扩展。
/// - BGM 区以左边界为锚，新增轨道向右扩展。
/// - 批注区以左边界和完整区域宽度存储。
/// - 草稿与 BGM 的配置 width 表示单轨宽度。
/// - 屏幕总宽写回草稿/BGM 前除以对应轨道数量。
/// - 至少按一条轨道换算，防止空轨道数量导致除零。
/// - optional 左右锚点的存在性属于持久化语义。
/// - 切换锚点方向时即使数值相同也必须写回配置。
/// - 所有候选经过公共 sanitize/resize/move helper 规整。
///
/// @details 配置更新与收尾：
/// - 数值变化使用小容差过滤浮点往返噪声。
/// - 变化先写入当前 keyCount 对应的可编辑配置。
/// - 每次有效变化发布 `CmdUpdateEditorConfig` 供逻辑快照回流。
/// - 连续拖动期间不直接写配置文件。
/// - `m_layoutConfigurationChanged` 记录本次手势是否需要持久化。
/// - 鼠标释放后 `finishLayoutEditing` 统一保存并清理状态。
/// - 工具切换和中键平移也调用相同收尾入口。
/// - 释放位置是否仍在画布内不影响手势提交。
/// - 收尾后不保留吸附目标、参考线或同步 KPS 捕获项。
/// - 本帧写回后重新读取规范化配置生成编辑辅助几何。
/// - 辅助几何只写 ImGui 前景绘制列表，不修改 Vulkan 快照。
/// - 绘制裁剪严格限制在当前画布窗口。
/// - 暗色遮罩突出主轨道矩形但保留谱面上下文。
/// - 句柄和边线使用一致的悬停高亮判据。
/// - 同步 KPS 多成员组额外绘制组合外框。
/// - 音符与文字组件分别使用不同默认轮廓色。
/// - 当前拖动对象即使指针离开也保持高亮。
/// - 所有 PushClipRect 在函数退出前成对 PopClipRect。
/// @param pointerX 鼠标画布局部横坐标。
/// @param pointerY 鼠标画布局部纵坐标。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param targetWidth 画布逻辑宽度。
/// @param targetHeight 画布逻辑高度。
/// @param isHovered 鼠标是否位于当前画布内容区。
/// @param currentSnapshot 当前不可变渲染快照。
/// @details 该状态机按优先级处理音符缩放、文字组件、辅助横向区域、
/// 主轨道矩形与判定线；按下时冻结几何起点和吸附目标，拖动中只做
/// 纯数值更新，释放时统一保存配置。
/// 所有像素结果通过专用 sanitize/normalize helper 写回配置，
/// 组件允许区域、KPS 同步组和相机横移均在各自坐标域显式转换。
/// @warning UI 布局热路径：Layout 工具每帧调用；禁止文件系统访问，
/// 唯一配置保存位于 finishLayoutEditing 的鼠标释放分支。
void Basic2DCanvasInteraction::handleLayoutEditing(
    float pointerX, float pointerY, float canvasScreenX, float canvasScreenY,
    float targetWidth, float targetHeight, bool isHovered,
    const Common::Render::RenderSnapshot& currentSnapshot)
{
    // 视口退化时无法继续几何换算；若鼠标已经释放，仍需完成并清理
    // 先前手势，避免拖动状态在窗口恢复后继续生效。
    if ( targetWidth <= 0.0f || targetHeight <= 0.0f ) {
        if ( (m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
              m_horizontalRegionDragHandle !=
                  HorizontalRegionDragHandle::None ||
              m_noteScaleDragTarget.has_value() ||
              m_canvasComponentDragTarget.has_value()) &&
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            finishLayoutEditing();
        }
        return;
    }

    // 快照 hitbox 在 Layout 模式下转换为实体外包矩形，后续音符命中
    // 不需要访问逻辑 ECS。
    rebuildNoteLayoutInstances(currentSnapshot);

    auto&      appConfig = Config::AppConfig::instance();
    const auto keyCount  = currentSnapshot.hasBeatmap
                               ? std::max(currentSnapshot.trackCount, 1)
                               : 0;
    auto       layout    = sanitizeTrackLayout(
        appConfig.getVisualConfig().trackLayoutForKeyCount(keyCount));
    // 无谱面时使用 keyCount=0 的独立布局配置；真实谱面至少按一轨处理。
    const float cameraOffsetX = currentSnapshot.canvasHorizontalOffsetX;
    const float worldPointerX = pointerX - cameraOffsetX;
    // 主轨道随相机横移，配置仍存世界归一化坐标，命中时需先去掉偏移。
    // 辅助区布局使用零相机偏移解析为世界坐标，便于直接写回归一化值。
    const auto auxiliaryProjection =
        Logic::calculateCanvasLaneProjection(targetWidth,
                                             keyCount,
                                             currentSnapshot.bgmTrackCount,
                                             layout,
                                             0.0F,
                                             true,
                                             true,
                                             true,
                                             currentSnapshot.draftTrackCount,
                                             true);
    // 该投影启用全部辅助区域并使用零横移，得到可直接映射到持久化
    // 归一化配置的世界边界；显示用投影另行包含 cameraOffsetX。
    const auto regionBounds = [&](AuxiliaryLayoutRegion region) {
        if ( !auxiliaryProjection.valid || targetWidth <= 0.0F ) {
            // 无有效投影时返回默认退化边界，命中 helper 会拒绝该区域。
            return HorizontalRegionBounds{};
        }
        // 草稿/BGM 句柄覆盖完整轨道组；写回时再折算为单轨宽度。
        switch ( region ) {
        case AuxiliaryLayoutRegion::Draft:
            // 草稿区可能包含多条草稿轨，整体像素宽度作为区域配置处理。
            return HorizontalRegionBounds{
                auxiliaryProjection.draftLeftX / targetWidth,
                (auxiliaryProjection.draftRightX -
                 auxiliaryProjection.draftLeftX) /
                    targetWidth,
            };
        case AuxiliaryLayoutRegion::Annotation:
            // 批注 gutter 是单一辅助区域，不按轨道数折分。
            return HorizontalRegionBounds{
                auxiliaryProjection.annotationLeftX / targetWidth,
                (auxiliaryProjection.annotationRightX -
                 auxiliaryProjection.annotationLeftX) /
                    targetWidth,
            };
        case AuxiliaryLayoutRegion::Bgm:
            // BGM 区同样以完整组边界参与命中，写回 helper 再处理单轨语义。
            return HorizontalRegionBounds{
                auxiliaryProjection.bgmLeftX / targetWidth,
                (auxiliaryProjection.bgmRightX - auxiliaryProjection.bgmLeftX) /
                    targetWidth,
            };
        case AuxiliaryLayoutRegion::None: break;
        }
        return HorizontalRegionBounds{};
    };
    float judgmentLinePosition = sanitizeJudgmentLinePosition(
        appConfig.getVisualConfig().judgmentLinePositionForKeyCount(keyCount));
    // 命中半径随 DPI 放大并保留逻辑像素下限；移动中心句柄范围大于
    // 边缘/角点，使精细缩放仍优先落在边界附近。
    const float dpiScale                 = appConfig.getWindowContentScale();
    const float edgeHitRadius            = std::max(6.0f, 7.0f * dpiScale);
    const float moveHandleRadius         = std::max(12.0f, 15.0f * dpiScale);
    const float componentCornerHitRadius = std::max(6.0f, 7.0f * dpiScale);
    const float componentSnapDistance =
        std::max(CANVAS_COMPONENT_SNAP_DISTANCE,
                 CANVAS_COMPONENT_SNAP_DISTANCE * dpiScale);
    // 宽度手势开始时冻结其他布局区、可见组件与画布两侧边缘，避免目标随缩放抖动。
    // 目标全部采用世界像素，后续主轨道区与辅助区域可以共享一维吸附算法。
    // 此收集器只在鼠标按下首帧调用，连续拖动不会重复遍历组件快照。
    const auto collectHorizontalResizeSnapTargets =
        [&](bool includeTrackLayout, AuxiliaryLayoutRegion excludedRegion) {
            // 新手势不继承上次命中的参考线，避免按下瞬间出现残留视觉反馈。
            m_canvasComponentSnapGuideX.reset();
            m_canvasComponentSnapGuideY.reset();
            // 横向缩放仅保留 X 目标；Y 缓存同时清空以隔离整体移动状态。
            m_canvasComponentSnapTargetsX.clear();
            m_canvasComponentSnapTargetsY.clear();
            // 每个可见组件最多贡献左右两条线，十个固定槽覆盖画布和四个区域。
            // 预留容量把手势开始时的动态分配限制为至多一次。
            const std::size_t targetCount =
                currentSnapshot.canvasComponentInstances.size() * 2U + 10U;
            m_canvasComponentSnapTargetsX.reserve(targetCount);
            const auto appendWorldEdges = [&](float left, float right) {
                // 无效或反向边界不能参与距离比较，否则会抢占正常目标。
                if ( !std::isfinite(left) || !std::isfinite(right) ||
                     right < left ) {
                    return;
                }
                // 缩放只匹配左右边缘，不引入中心线吸附。
                // 重复边缘允许保留，最近目标算法在等距时维持首次出现的优先级。
                m_canvasComponentSnapTargetsX.push_back(left);
                m_canvasComponentSnapTargetsX.push_back(right);
            };

            // 画布边缘换算到相机平移前的世界坐标，与轨道配置采用同一坐标系。
            // 因此相机横移后，屏幕两侧仍是稳定可见的吸附目标。
            appendWorldEdges(-cameraOffsetX, targetWidth - cameraOffsetX);
            if ( includeTrackLayout ) {
                // 调整辅助区域时，玩家主轨道区的两侧都属于其他组件边缘。
                appendWorldEdges(layout.left * targetWidth,
                                 layout.right * targetWidth);
            }
            // 固定顺序同时定义等距情况下 Draft、批注、BGM 的稳定优先级。
            constexpr std::array auxiliaryRegions{
                AuxiliaryLayoutRegion::Draft,
                AuxiliaryLayoutRegion::Annotation,
                AuxiliaryLayoutRegion::Bgm,
            };
            for ( const auto region : auxiliaryRegions ) {
                // 活动区域必须排除，防止句柄重新吸附到自身拖动起点。
                if ( region == excludedRegion ) continue;
                // 推导式旧配置也在按下时物化为固定目标，拖动期间不会跟随主区变化。
                const auto bounds = regionBounds(region);
                appendWorldEdges(bounds.left * targetWidth,
                                 bounds.right() * targetWidth);
            }

            // 使用当前键数配置过滤隐藏组件，避免吸附到用户看不见的文字边界。
            const auto& components =
                appConfig.getVisualConfig().canvasComponentsForKeyCount(
                    keyCount);
            for ( const auto& instance :
                  currentSnapshot.canvasComponentInstances ) {
                // 快照可能仍含上一帧实例，配置可见性是最终筛选依据。
                if ( !components.placement(instance.type).visible ) continue;
                const auto bounds = canvasComponentContentBounds(instance);
                // 自定义组件固定在画布局部坐标，减去相机偏移后参与世界边缘比较。
                // 保存参考线时会重新加回偏移，保证屏幕位置和被吸附组件一致。
                appendWorldEdges(bounds.left - cameraOffsetX,
                                 bounds.right - cameraOffsetX);
            }
        };
    // 整体移动沿用二维边缘与中心吸附，宽度缩放则进入下面两个专用状态。
    const bool movingCanvasComponent =
        m_canvasComponentDragTarget.has_value() &&
        m_canvasComponentDragHandle == Logic::CanvasComponentDragHandle::Move;
    const bool movingTrackLayout =
        !m_noteScaleDragTarget.has_value() &&
        !m_canvasComponentDragTarget.has_value() &&
        m_trackLayoutDragHandle == TrackLayoutDragHandle::Move;
    // 辅助区域只有左右句柄改变宽度，中心 Move 不应复用冻结目标。
    const bool resizingHorizontalRegion =
        m_horizontalRegionDragHandle == HorizontalRegionDragHandle::Left ||
        m_horizontalRegionDragHandle == HorizontalRegionDragHandle::Right;
    // 主轨道上下、判定线和 Move 都不属于横向宽度吸附。
    const bool resizingTrackWidth =
        m_trackLayoutDragHandle == TrackLayoutDragHandle::Left ||
        m_trackLayoutDragHandle == TrackLayoutDragHandle::Right;
    // 仅活动吸附手势可以保留参考线；松开鼠标会在提交配置前立即清理。
    if ( (!movingCanvasComponent && !movingTrackLayout &&
          !resizingHorizontalRegion && !resizingTrackWidth) ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_canvasComponentSnapGuideX.reset();
        m_canvasComponentSnapGuideY.reset();
    }

    std::optional<entt::entity>                 hoveredNote;
    std::optional<Logic::CanvasComponentBounds> hoveredNoteBounds;
    Logic::CanvasComponentDragHandle            hoveredNoteHandle =
        Logic::CanvasComponentDragHandle::None;
    if ( m_noteScaleDragTarget.has_value() ) {
        // 活动拖动期间锁定按下时音符和句柄，不随指针经过其它物件切换。
        hoveredNote       = m_noteScaleDragTarget;
        hoveredNoteHandle = m_noteScaleDragHandle;
    } else if ( !m_canvasComponentDragTarget.has_value() && isHovered ) {
        // 未拖动文字组件时才允许音符角点抢占；反向遍历使后绘制、
        // 视觉上位于上层的实体优先命中。
        for ( auto it = m_noteLayoutInstances.rbegin();
              it != m_noteLayoutInstances.rend();
              ++it ) {
            const auto hit = Logic::hitTestCanvasComponent(
                it->bounds, pointerX, pointerY, componentCornerHitRadius);
            // 音符布局只允许通过角点调整全局渲染缩放，主体 Move
            // 留给普通物件移动工具，避免 Layout 模式误改谱面位置。
            if ( hit == Logic::CanvasComponentDragHandle::None ||
                 hit == Logic::CanvasComponentDragHandle::Move ) {
                continue;
            }
            hoveredNote = it->entity;
            // 保存命中边界供按下首帧冻结，后续快照滚动不改变缩放基准。
            hoveredNoteBounds = it->bounds;
            hoveredNoteHandle = hit;
            break;
        }
    }

    std::optional<Config::CanvasComponentType> hoveredComponent;
    std::optional<Common::Render::CanvasComponentInstanceSnapshot>
                                     hoveredComponentInstance;
    Logic::CanvasComponentDragHandle hoveredComponentHandle =
        Logic::CanvasComponentDragHandle::None;
    if ( m_canvasComponentDragTarget.has_value() ) {
        // 活动文字组件拖动同样锁定目标和句柄。
        hoveredComponent       = m_canvasComponentDragTarget;
        hoveredComponentHandle = m_canvasComponentDragHandle;
    } else if ( !hoveredNote.has_value() && isHovered ) {
        // 音符角点优先于文字组件；文字实例反向遍历保持渲染上层优先。
        for ( auto it = currentSnapshot.canvasComponentInstances.rbegin();
              it != currentSnapshot.canvasComponentInstances.rend();
              ++it ) {
            const auto& placement = appConfig.getVisualConfig()
                                        .canvasComponentsForKeyCount(keyCount)
                                        .placement(it->type);
            // 快照可能仍含上一代实例，当前配置可见性作为最终命中门槛。
            if ( !placement.visible ) continue;

            const auto bounds = canvasComponentContentBounds(*it);
            if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) {
                // 无实际字形面积的组件不能产生移动或缩放句柄。
                continue;
            }
            const auto hit = Logic::hitTestCanvasComponent(
                bounds, pointerX, pointerY, componentCornerHitRadius);
            if ( hit != Logic::CanvasComponentDragHandle::None ) {
                // 保存完整实例快照，按下时需要其 instanceIndex、内容边界
                // 和允许布局区域，而不仅是组件类型。
                hoveredComponent         = it->type;
                hoveredComponentInstance = *it;
                hoveredComponentHandle   = hit;
                break;
            }
        }
    }

    TrackLayoutDragHandle hoveredHandle = TrackLayoutDragHandle::None;
    if ( m_trackLayoutDragHandle != TrackLayoutDragHandle::None ) {
        // 活动主轨道拖动锁定原句柄，指针越过其它边界也不切换方向。
        hoveredHandle = m_trackLayoutDragHandle;
    } else if ( isHovered && !hoveredNote.has_value() &&
                !hoveredComponent.has_value() ) {
        // 主轨道命中低于音符和文字组件，防止重叠处拖动底层大区域。
        hoveredHandle = hitTestTrackLayout(layout,
                                           judgmentLinePosition,
                                           worldPointerX,
                                           pointerY,
                                           targetWidth,
                                           targetHeight,
                                           edgeHitRadius,
                                           moveHandleRadius);
    }

    AuxiliaryLayoutRegion hoveredAuxiliaryRegion = AuxiliaryLayoutRegion::None;
    HorizontalRegionDragHandle hoveredHorizontalHandle =
        HorizontalRegionDragHandle::None;
    if ( m_horizontalRegionDragHandle != HorizontalRegionDragHandle::None ) {
        // 拖动期间锁定最初区域，避免重叠句柄导致目标切换。
        hoveredAuxiliaryRegion  = m_auxiliaryLayoutRegion;
        hoveredHorizontalHandle = m_horizontalRegionDragHandle;
    } else if ( auxiliaryProjection.valid && isHovered &&
                hoveredHandle == TrackLayoutDragHandle::None &&
                !hoveredNote.has_value() && !hoveredComponent.has_value() ) {
        // 辅助区命中只在更高优先级对象均未命中时执行；固定数组顺序
        // 处理区域重叠时的稳定优先级。
        constexpr std::array regions{
            AuxiliaryLayoutRegion::Annotation,
            AuxiliaryLayoutRegion::Draft,
            AuxiliaryLayoutRegion::Bgm,
        };
        for ( const auto region : regions ) {
            // 所有区域共享主轨道上下边界，但使用各自横向归一化边界。
            const auto hit =
                hitTestHorizontalRegion(regionBounds(region),
                                        layout.top * targetHeight,
                                        layout.bottom * targetHeight,
                                        worldPointerX,
                                        pointerY,
                                        targetWidth,
                                        edgeHitRadius,
                                        moveHandleRadius);
            if ( hit == HorizontalRegionDragHandle::None ) continue;
            // 首个命中即停止，避免同一按下同时激活多个区域。
            hoveredAuxiliaryRegion  = region;
            hoveredHorizontalHandle = hit;
            break;
        }
    }

    const Logic::CanvasComponentDragHandle hoveredResizeHandle =
        hoveredNoteHandle != Logic::CanvasComponentDragHandle::None
            ? hoveredNoteHandle
            : hoveredComponentHandle;
    // 光标样式只反映最终优先级命中；角点按对角线方向、水平区域按 X、
    // 主轨道上下边/判定线按 Y，整体移动使用全向光标。
    if ( hoveredResizeHandle == Logic::CanvasComponentDragHandle::TopLeft ||
         hoveredResizeHandle ==
             Logic::CanvasComponentDragHandle::BottomRight ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if ( hoveredResizeHandle ==
                    Logic::CanvasComponentDragHandle::TopRight ||
                hoveredResizeHandle ==
                    Logic::CanvasComponentDragHandle::BottomLeft ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if ( hoveredResizeHandle ==
                Logic::CanvasComponentDragHandle::Move ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if ( hoveredHorizontalHandle == HorizontalRegionDragHandle::Left ||
                hoveredHorizontalHandle == HorizontalRegionDragHandle::Right ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if ( hoveredHorizontalHandle == HorizontalRegionDragHandle::Move ) {
        // 辅助区域严格限制为 X 方向移动。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if ( hoveredHandle == TrackLayoutDragHandle::Left ||
                hoveredHandle == TrackLayoutDragHandle::Right ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if ( hoveredHandle == TrackLayoutDragHandle::Top ||
                hoveredHandle == TrackLayoutDragHandle::Bottom ||
                hoveredHandle == TrackLayoutDragHandle::JudgmentLine ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if ( hoveredHandle == TrackLayoutDragHandle::Move ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) ) {
        // 鼠标按下首帧按同一优先级冻结目标和起始几何；后续分支只读取
        // 成员状态，不再重新解释当前 hover。
        if ( hoveredNote.has_value() && hoveredNoteBounds.has_value() ) {
            // 音符角点调整的是全局 noteScaleX/Y，起始 hitbox 只提供
            // 指针位移到比例变化的参考尺寸。
            m_noteScaleDragTarget      = hoveredNote;
            m_noteScaleDragHandle      = hoveredNoteHandle;
            m_noteScaleDragStartBounds = *hoveredNoteBounds;
            const auto& visual         = appConfig.getVisualConfig();
            m_noteScaleDragStart = { visual.noteScaleX, visual.noteScaleY };
        } else if ( hoveredComponent.has_value() &&
                    hoveredComponentInstance.has_value() ) {
            // resolvedPlacement 将默认、按键数及逐轨覆盖合成为本实例
            // 当前有效配置，后续编辑写回对应可编辑层级。
            m_canvasComponentDragTarget = hoveredComponent;
            m_canvasComponentDragHandle = hoveredComponentHandle;
            const auto placement =
                appConfig.getVisualConfig()
                    .canvasComponentsForKeyCount(keyCount)
                    .resolvedPlacement(*hoveredComponent,
                                       hoveredComponentInstance->instanceIndex,
                                       currentSnapshot.trackCount,
                                       layout.left,
                                       layout.right);
            m_canvasComponentDragStart = placement;
            m_canvasComponentDragStartBounds =
                canvasComponentContentBounds(*hoveredComponentInstance);
            m_canvasComponentDragRegion =
                canvasComponentLayoutRegion(*hoveredComponentInstance);
            // instanceIndex=-1 表示全局组件，非负值表示逐轨 KPS 实例。
            m_canvasComponentDragInstanceIndex =
                hoveredComponentInstance->instanceIndex;
            m_synchronizedKpsTransformStarts.clear();
            const auto& canvasComponents =
                appConfig.getVisualConfig().canvasComponentsForKeyCount(
                    keyCount);
            const bool draggedKpsTrack =
                hoveredComponentInstance->instanceIndex >= 0;
            // 移动同步与尺寸同步分开判定；全量位置同步优先于逐轨相对同步。
            const bool captureAllKpsMove =
                hoveredComponentHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                canvasComponents.syncAllKpsComponentPositions;
            const bool captureKpsTrackMove =
                hoveredComponentHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                draggedKpsTrack &&
                canvasComponents.syncKpsTrackRelativePositions &&
                !captureAllKpsMove;
            const bool captureSynchronizedKpsResize =
                hoveredComponentHandle !=
                    Logic::CanvasComponentDragHandle::Move &&
                draggedKpsTrack && canvasComponents.syncKpsTrackSizes;
            if ( *hoveredComponent == Config::CanvasComponentType::Kps &&
                 (captureAllKpsMove || captureKpsTrackMove ||
                  captureSynchronizedKpsResize) ) {
                // 同步手势在按下时捕获所有受影响实例的配置、内容边界
                // 和允许区域，拖动中不依赖不断变化的新快照。
                m_synchronizedKpsTransformStarts.reserve(
                    currentSnapshot.canvasComponentInstances.size());
                for ( const auto& instance :
                      currentSnapshot.canvasComponentInstances ) {
                    // 仅收集 KPS；全量同步包含全局项，逐轨模式排除 index<0。
                    if ( instance.type != Config::CanvasComponentType::Kps ||
                         (!captureAllKpsMove && instance.instanceIndex < 0) ) {
                        continue;
                    }
                    m_synchronizedKpsTransformStarts.push_back(
                        // 每项保存独立 resolvedPlacement，保持相对间距和尺寸。
                        { instance.instanceIndex,
                          canvasComponents.resolvedPlacement(
                              Config::CanvasComponentType::Kps,
                              instance.instanceIndex,
                              currentSnapshot.trackCount,
                              layout.left,
                              layout.right),
                          canvasComponentContentBounds(instance),
                          canvasComponentLayoutRegion(instance) });
                }
            }
            const float centerX = (m_canvasComponentDragStartBounds.left +
                                   m_canvasComponentDragStartBounds.right) *
                                  0.5f;
            const float centerY = (m_canvasComponentDragStartBounds.top +
                                   m_canvasComponentDragStartBounds.bottom) *
                                  0.5f;
            // 保存指针相对组件中心偏移，拖动开始时不会把组件中心瞬移到鼠标。
            m_canvasComponentPointerOffset = {
                centerX - pointerX,
                centerY - pointerY,
            };
        } else if ( hoveredHorizontalHandle !=
                    HorizontalRegionDragHandle::None ) {
            // 仅在首次按下时物化当前推导边界，保持旧配置迁移兼容。
            m_auxiliaryLayoutRegion      = hoveredAuxiliaryRegion;
            m_horizontalRegionDragHandle = hoveredHorizontalHandle;
            m_horizontalRegionDragStart  = regionBounds(hoveredAuxiliaryRegion);
            // 指针起点按画布宽度归一化，与持久化区域坐标保持同域。
            m_horizontalRegionPointerStart = worldPointerX / targetWidth;
            if ( hoveredHorizontalHandle == HorizontalRegionDragHandle::Left ||
                 hoveredHorizontalHandle ==
                     HorizontalRegionDragHandle::Right ) {
                // Move 不改变宽度，因此无需冻结边缘目标。
                // 活动区域自身不作为吸附目标，另一侧边缘仍由缩放函数固定。
                collectHorizontalResizeSnapTargets(true,
                                                   hoveredAuxiliaryRegion);
            }
        } else if ( hoveredHandle != TrackLayoutDragHandle::None ) {
            // 主轨道手势冻结完整矩形、判定线另由成员当前值读取，
            // 指针起点同时保存归一化 X/Y。
            m_trackLayoutDragHandle   = hoveredHandle;
            m_trackLayoutDragStart    = layout;
            m_trackLayoutPointerStart = {
                worldPointerX / targetWidth,
                pointerY / targetHeight,
            };
            if ( hoveredHandle == TrackLayoutDragHandle::Left ||
                 hoveredHandle == TrackLayoutDragHandle::Right ) {
                // 上下边与判定线不改变宽度，继续保持原始无吸附行为。
                // 主轨道区缩放时只收集其他区域与组件，避免吸附到自身固定边。
                collectHorizontalResizeSnapTargets(false,
                                                   AuxiliaryLayoutRegion::None);
            }
        }
    }

    if ( m_noteScaleDragTarget.has_value() &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 角点拖动使用冻结 hitbox 和起始全局缩放计算候选，避免当前
        // 缩放反馈到新快照后再次叠加造成指数变化。
        constexpr float scaleEpsilon = 1e-6f;
        const auto      candidate =
            Logic::resizeNoteRenderScale(m_noteScaleDragStart,
                                         m_noteScaleDragHandle,
                                         m_noteScaleDragStartBounds,
                                         pointerX,
                                         pointerY);
        const auto& visual = appConfig.getVisualConfig();
        // 只有比例实际变化才写配置并通知逻辑线程，亚像素抖动被容差过滤。
        if ( std::abs(visual.noteScaleX - candidate.x) > scaleEpsilon ||
             std::abs(visual.noteScaleY - candidate.y) > scaleEpsilon ) {
            appConfig.getVisualConfig().noteScaleX = candidate.x;
            appConfig.getVisualConfig().noteScaleY = candidate.y;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
            // 标记仅用于释放时合并持久化和反馈，不在连续拖动中写磁盘。
            m_layoutConfigurationChanged = true;
        }
    }

    if ( m_canvasComponentDragTarget.has_value() &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 文字组件移动/缩放写入当前 keyCount 和 instanceIndex 对应的
        // 可编辑 placement，不覆盖其它轨道数配置。
        constexpr float componentPositionEpsilon = 1e-6f;
        auto&           canvasComponents =
            appConfig.getVisualConfig().editableCanvasComponentsForKeyCount(
                keyCount);
        auto& component = canvasComponents.editablePlacement(
            *m_canvasComponentDragTarget,
            m_canvasComponentDragInstanceIndex,
            currentSnapshot.trackCount,
            layout.left,
            layout.right);
        Config::CanvasComponentPlacement candidate = component;
        if ( m_canvasComponentDragHandle ==
             Logic::CanvasComponentDragHandle::Move ) {
            // move helper 以组件中心目标和冻结宽高在允许 region 内钳制，
            // pointerOffset 保留按下位置相对中心关系。
            candidate = Logic::moveCanvasComponentInRegion(
                component,
                pointerX + m_canvasComponentPointerOffset.x,
                pointerY + m_canvasComponentPointerOffset.y,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());

            m_canvasComponentSnapGuideX.reset();
            m_canvasComponentSnapGuideY.reset();
            m_canvasComponentSnapTargetsX.clear();
            m_canvasComponentSnapTargetsY.clear();
            // 移动吸附每帧根据当前可见对象重建，轨道数负值先规范为零。
            const auto safeTrackCount =
                std::max<std::int32_t>(currentSnapshot.trackCount, 0);
            const std::size_t targetObjectCount =
                currentSnapshot.canvasComponentInstances.size() +
                static_cast<std::size_t>(safeTrackCount) + 2U;
            m_canvasComponentSnapTargetsX.reserve(targetObjectCount * 3U + 1U);
            // 每个目标贡献边缘/中心三条线，额外槽位用于画布或组外框。
            m_canvasComponentSnapTargetsY.reserve(targetObjectCount * 3U + 1U);

            const bool draggedKps = *m_canvasComponentDragTarget ==
                                    Config::CanvasComponentType::Kps;
            // KPS 文本可能按“全部实例”或“仅轨道实例”组成同步移动组。
            // 两种配置互斥：全局同步优先，轨道相对同步只在前者关闭时生效。
            // instanceIndex 小于零的是汇总实例，不属于逐轨相对同步集合。
            // 后续吸附必须把同步组视作一个整体，否则单个成员对齐后会把
            // 其它成员推出目标边界，最终视觉结果与参考线不一致。
            // 仅 KPS 组件应用同步组规则，其它文字组件始终独立移动。
            const bool synchronizeAllKpsComponentPositions =
                draggedKps && canvasComponents.syncAllKpsComponentPositions;
            const bool synchronizeKpsTrackRelativePositions =
                draggedKps && m_canvasComponentDragInstanceIndex >= 0 &&
                !synchronizeAllKpsComponentPositions &&
                canvasComponents.syncKpsTrackRelativePositions;
            const bool groupAllKpsPositions =
                canvasComponents.kps.visible &&
                canvasComponents.syncAllKpsComponentPositions;
            const bool groupKpsTrackPositions =
                canvasComponents.kps.visible && !groupAllKpsPositions &&
                canvasComponents.syncKpsTrackRelativePositions;
            const bool movingSynchronizedKpsGroup =
                synchronizeAllKpsComponentPositions ||
                synchronizeKpsTrackRelativePositions;
            Logic::CanvasComponentBounds synchronizedKpsTargetBounds;
            bool                         hasSynchronizedKpsTargetBounds = false;
            for ( const auto& instance :
                  currentSnapshot.canvasComponentInstances ) {
                // 隐藏组件没有可见几何，不能成为吸附目标。
                if ( !canvasComponents.placement(instance.type).visible ) {
                    continue;
                }

                // 未随当前拖动移动的同步 KPS 集合先合并为一个外框。
                // 这样拖动其它组件时只会吸附到组外缘或组中心，不会落到
                // 组内成员彼此重叠的位置。
                const bool synchronizedKpsGroupMember =
                    instance.type == Config::CanvasComponentType::Kps &&
                    (groupAllKpsPositions ||
                     (groupKpsTrackPositions && instance.instanceIndex >= 0));
                if ( synchronizedKpsGroupMember &&
                     !movingSynchronizedKpsGroup ) {
                    mergeCanvasComponentBounds(
                        canvasComponentContentBounds(instance),
                        synchronizedKpsTargetBounds,
                        hasSynchronizedKpsTargetBounds);
                    continue;
                }

                bool movesWithDraggedComponent = false;
                // 被同步规则带动的实例必须从目标集合中排除，避免对象吸附
                // 到自身或同组成员，造成拖动在阈值内来回抖动。
                if ( instance.type == *m_canvasComponentDragTarget ) {
                    if ( instance.type != Config::CanvasComponentType::Kps ) {
                        movesWithDraggedComponent = true;
                    } else if ( synchronizeAllKpsComponentPositions ) {
                        movesWithDraggedComponent = true;
                    } else if ( synchronizeKpsTrackRelativePositions &&
                                instance.instanceIndex >= 0 ) {
                        movesWithDraggedComponent = true;
                    } else {
                        movesWithDraggedComponent =
                            instance.instanceIndex ==
                            m_canvasComponentDragInstanceIndex;
                    }
                }
                if ( movesWithDraggedComponent ) continue;

                appendCanvasComponentSnapTargets(
                    canvasComponentContentBounds(instance),
                    m_canvasComponentSnapTargetsX,
                    m_canvasComponentSnapTargetsY);
            }
            if ( hasSynchronizedKpsTargetBounds ) {
                // 同步组只贡献一次边缘与中心，保持候选优先级稳定。
                appendCanvasComponentSnapTargets(synchronizedKpsTargetBounds,
                                                 m_canvasComponentSnapTargetsX,
                                                 m_canvasComponentSnapTargetsY);
            }

            const Logic::CanvasComponentBounds trackLayoutBounds{
                layout.left * targetWidth + cameraOffsetX,
                layout.top * targetHeight,
                layout.right * targetWidth + cameraOffsetX,
                layout.bottom * targetHeight,
            };
            // 主轨道总框和每条轨道都是有意义的排版基准。
            // 总框便于贴齐画布内容区，单轨框便于把文字居中到指定轨道。
            appendCanvasComponentSnapTargets(trackLayoutBounds,
                                             m_canvasComponentSnapTargetsX,
                                             m_canvasComponentSnapTargetsY);
            if ( safeTrackCount > 0 ) {
                const float singleTrackWidth =
                    trackLayoutBounds.width() /
                    static_cast<float>(safeTrackCount);
                for ( std::int32_t trackIndex = 0; trackIndex < safeTrackCount;
                      ++trackIndex ) {
                    // 轨道等分基于当前布局宽度，而不是整个视口宽度；横向
                    // 相机偏移已经包含在 trackLayoutBounds 中。
                    const float trackLeft =
                        trackLayoutBounds.left +
                        static_cast<float>(trackIndex) * singleTrackWidth;
                    appendCanvasComponentSnapTargets(
                        { trackLeft,
                          trackLayoutBounds.top,
                          trackLeft + singleTrackWidth,
                          trackLayoutBounds.bottom },
                        m_canvasComponentSnapTargetsX,
                        m_canvasComponentSnapTargetsY);
                }
            }
            m_canvasComponentSnapTargetsX.push_back(targetWidth * 0.5f);
            m_canvasComponentSnapTargetsY.push_back(targetHeight * 0.5f);
            // 视口中心是最后的兜底基准，不依赖任何可见业务组件。

            const auto candidateBounds = Logic::canvasComponentBoundsInRegion(
                candidate,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());
            Logic::CanvasComponentBounds synchronizedKpsGroupStartBounds;
            bool                         hasSynchronizedKpsGroupBounds = false;
            if ( synchronizeAllKpsComponentPositions ||
                 synchronizeKpsTrackRelativePositions ) {
                // 同步成员的起始边界在按下时冻结；拖动期间不从已写回配置的
                // 新快照反推，避免组框逐帧漂移。
                for ( const auto& transformStart :
                      m_synchronizedKpsTransformStarts ) {
                    mergeCanvasComponentBounds(transformStart.bounds,
                                               synchronizedKpsGroupStartBounds,
                                               hasSynchronizedKpsGroupBounds);
                }
            }

            Logic::CanvasComponentBounds snapSourceBounds = candidateBounds;
            if ( hasSynchronizedKpsGroupBounds ) {
                // 主实例候选位移转换成组外框位移，再以组框执行一次吸附。
                // 组内相对间距保持不变，只有整体中心发生变化。
                const float candidateOffsetX =
                    (candidateBounds.left + candidateBounds.right -
                     m_canvasComponentDragStartBounds.left -
                     m_canvasComponentDragStartBounds.right) *
                    0.5f;
                const float candidateOffsetY =
                    (candidateBounds.top + candidateBounds.bottom -
                     m_canvasComponentDragStartBounds.top -
                     m_canvasComponentDragStartBounds.bottom) *
                    0.5f;
                snapSourceBounds =
                    offsetCanvasComponentBounds(synchronizedKpsGroupStartBounds,
                                                candidateOffsetX,
                                                candidateOffsetY);
            }
            const auto snap =
                Logic::snapCanvasComponentBounds(snapSourceBounds,
                                                 m_canvasComponentSnapTargetsX,
                                                 m_canvasComponentSnapTargetsY,
                                                 componentSnapDistance);
            // snap.center 是源外框吸附后的中心。将其与吸附前中心的差值
            // 施加回实际拖动实例，统一处理单对象和同步组两种情况。
            const float snapOffsetX =
                snap.center.x -
                (snapSourceBounds.left + snapSourceBounds.right) * 0.5f;
            const float snapOffsetY =
                snap.center.y -
                (snapSourceBounds.top + snapSourceBounds.bottom) * 0.5f;
            candidate = Logic::moveCanvasComponentInRegion(
                candidate,
                (candidateBounds.left + candidateBounds.right) * 0.5f +
                    snapOffsetX,
                (candidateBounds.top + candidateBounds.bottom) * 0.5f +
                    snapOffsetY,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());
            // move helper 仍可能因可编辑区域边界而钳制候选，因此必须用
            // 最终 placement 再投影一次，而不能直接相信原始 snap 结果。
            const auto snappedBounds = Logic::canvasComponentBoundsInRegion(
                candidate,
                m_canvasComponentDragRegion,
                m_canvasComponentDragStartBounds.width(),
                m_canvasComponentDragStartBounds.height());
            Logic::CanvasComponentBounds finalSnapSourceBounds = snappedBounds;
            if ( hasSynchronizedKpsGroupBounds ) {
                // 对组框复现最终实际位移，以判断钳制后是否仍与目标对齐。
                const float appliedOffsetX =
                    (snappedBounds.left + snappedBounds.right -
                     m_canvasComponentDragStartBounds.left -
                     m_canvasComponentDragStartBounds.right) *
                    0.5f;
                const float appliedOffsetY =
                    (snappedBounds.top + snappedBounds.bottom -
                     m_canvasComponentDragStartBounds.top -
                     m_canvasComponentDragStartBounds.bottom) *
                    0.5f;
                finalSnapSourceBounds =
                    offsetCanvasComponentBounds(synchronizedKpsGroupStartBounds,
                                                appliedOffsetX,
                                                appliedOffsetY);
            }
            if ( snap.snappedX && canvasComponentAlignsWithX(
                                      finalSnapSourceBounds, snap.targetX) ) {
                // 参考线只表达最终成立的对齐关系，不显示无法到达的候选。
                m_canvasComponentSnapGuideX = snap.targetX;
            }
            if ( snap.snappedY && canvasComponentAlignsWithY(
                                      finalSnapSourceBounds, snap.targetY) ) {
                m_canvasComponentSnapGuideY = snap.targetY;
            }
        } else {
            // 缩放只改变字体比例和锚点；同步尺寸在候选写回后统一传播。
            candidate = Logic::resizeCanvasComponentInRegion(
                m_canvasComponentDragStart,
                m_canvasComponentDragHandle,
                m_canvasComponentDragStartBounds,
                pointerX,
                pointerY,
                m_canvasComponentDragRegion);
        }
        if ( std::abs(component.anchorX - candidate.anchorX) >
                 componentPositionEpsilon ||
             std::abs(component.anchorY - candidate.anchorY) >
                 componentPositionEpsilon ||
             std::abs(component.fontSizeRatio - candidate.fontSizeRatio) >
                 componentPositionEpsilon ) {
            // 容差过滤鼠标噪声，避免每帧发布内容相同的配置命令。
            component = candidate;
            const bool synchronizeAllKpsComponentPositions =
                *m_canvasComponentDragTarget ==
                    Config::CanvasComponentType::Kps &&
                m_canvasComponentDragHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                canvasComponents.syncAllKpsComponentPositions;
            const bool synchronizeKpsTrackRelativePositions =
                *m_canvasComponentDragTarget ==
                    Config::CanvasComponentType::Kps &&
                m_canvasComponentDragInstanceIndex >= 0 &&
                m_canvasComponentDragHandle ==
                    Logic::CanvasComponentDragHandle::Move &&
                !synchronizeAllKpsComponentPositions &&
                canvasComponents.syncKpsTrackRelativePositions;
            if ( synchronizeAllKpsComponentPositions ||
                 synchronizeKpsTrackRelativePositions ) {
                // 同步移动以主实例“候选中心减冻结中心”为唯一位移来源。
                // 每个成员都从自己的冻结 placement 重算，不能在上帧结果上
                // 累加，否则帧率会改变最终位置。
                const float candidateCenterX =
                    m_canvasComponentDragRegion.left +
                    candidate.anchorX * m_canvasComponentDragRegion.width();
                const float candidateCenterY =
                    m_canvasComponentDragRegion.top +
                    candidate.anchorY * m_canvasComponentDragRegion.height();
                const float startCenterX =
                    (m_canvasComponentDragStartBounds.left +
                     m_canvasComponentDragStartBounds.right) *
                    0.5f;
                const float startCenterY =
                    (m_canvasComponentDragStartBounds.top +
                     m_canvasComponentDragStartBounds.bottom) *
                    0.5f;
                const float offsetX = candidateCenterX - startCenterX;
                const float offsetY = candidateCenterY - startCenterY;
                for ( const auto& transformStart :
                      m_synchronizedKpsTransformStarts ) {
                    if ( transformStart.instanceIndex ==
                         m_canvasComponentDragInstanceIndex ) {
                        continue;
                    }
                    auto synchronizedPlacement =
                        Logic::moveCanvasComponentByOffsetInRegion(
                            transformStart.placement,
                            transformStart.bounds,
                            offsetX,
                            offsetY,
                            transformStart.region);
                    // 各实例拥有不同的可编辑 region，统一像素位移仍需分别
                    // 钳制并换算回各自的归一化锚点。
                    auto& synchronizedComponent =
                        canvasComponents.editablePlacement(
                            Config::CanvasComponentType::Kps,
                            transformStart.instanceIndex,
                            currentSnapshot.trackCount,
                            layout.left,
                            layout.right);
                    synchronizedComponent = synchronizedPlacement;
                }
            }
            const bool synchronizeKpsTrackSize =
                *m_canvasComponentDragTarget ==
                    Config::CanvasComponentType::Kps &&
                m_canvasComponentDragInstanceIndex >= 0 &&
                m_canvasComponentDragHandle !=
                    Logic::CanvasComponentDragHandle::Move &&
                canvasComponents.syncKpsTrackSizes;
            if ( synchronizeKpsTrackSize ) {
                // 逐轨 KPS 尺寸同步只传播字体比例；每个实例的锚点由其句柄
                // 和冻结边界独立调整，以保持被抓取角点的视觉语义。
                for ( const auto& transformStart :
                      m_synchronizedKpsTransformStarts ) {
                    if ( transformStart.instanceIndex ==
                         m_canvasComponentDragInstanceIndex ) {
                        continue;
                    }
                    auto synchronizedPlacement =
                        Logic::resizeCanvasComponentToFontSizeInRegion(
                            transformStart.placement,
                            m_canvasComponentDragHandle,
                            transformStart.bounds,
                            candidate.fontSizeRatio,
                            transformStart.region);
                    auto& synchronizedComponent =
                        canvasComponents.editablePlacement(
                            Config::CanvasComponentType::Kps,
                            transformStart.instanceIndex,
                            currentSnapshot.trackCount,
                            layout.left,
                            layout.right);
                    synchronizedComponent = synchronizedPlacement;
                }
                canvasComponents.synchronizeKpsTrackFontSize(
                    candidate.fontSizeRatio);
            }
            // 连续预览通过逻辑线程快照回流；持久化留到鼠标释放统一完成。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
            m_layoutConfigurationChanged = true;
        }
    }

    if ( !m_noteScaleDragTarget.has_value() &&
         !m_canvasComponentDragTarget.has_value() &&
         m_horizontalRegionDragHandle != HorizontalRegionDragHandle::None &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 辅助区拖动与文本组件拖动互斥；前面的条件确保同一帧只有一个
        // 布局对象能消费左键手势。
        // 默认结果保留原始指针，Move 句柄因此无需单独分支关闭吸附。
        HorizontalResizeSnapResult edgeSnap{ .position = worldPointerX };
        if ( m_horizontalRegionDragHandle == HorizontalRegionDragHandle::Left ||
             m_horizontalRegionDragHandle ==
                 HorizontalRegionDragHandle::Right ) {
            // 统一使用逻辑像素阈值，DPI 放大时保持相同的可感知吸附范围。
            // 最近目标由纯函数选择，配置写回仍交给原有辅助区缩放与规整逻辑。
            edgeSnap = snapHorizontalResizeEdge(worldPointerX,
                                                m_canvasComponentSnapTargetsX,
                                                componentSnapDistance);
        }
        // 吸附发生在像素空间，写回配置前再恢复归一化横坐标。
        const float            normalizedX = edgeSnap.position / targetWidth;
        HorizontalRegionBounds candidateBounds = m_horizontalRegionDragStart;
        if ( m_horizontalRegionDragHandle ==
             HorizontalRegionDragHandle::Move ) {
            candidateBounds = moveHorizontalRegion(
                m_horizontalRegionDragStart,
                normalizedX - m_horizontalRegionPointerStart);
        } else {
            candidateBounds =
                resizeHorizontalRegion(m_horizontalRegionDragStart,
                                       m_horizontalRegionDragHandle,
                                       normalizedX);
        }
        if ( m_horizontalRegionDragHandle == HorizontalRegionDragHandle::Left ||
             m_horizontalRegionDragHandle ==
                 HorizontalRegionDragHandle::Right ) {
            // 最小宽度规整可能阻止边缘到达目标，仅实际对齐时显示参考线。
            const float resizedEdge = (m_horizontalRegionDragHandle ==
                                               HorizontalRegionDragHandle::Left
                                           ? candidateBounds.left
                                           : candidateBounds.right()) *
                                      targetWidth;
            // 每帧先清空旧命中，移出阈值后参考线会立即消失。
            m_canvasComponentSnapGuideX.reset();
            m_canvasComponentSnapGuideY.reset();
            if ( edgeSnap.snapped &&
                 std::abs(resizedEdge - edgeSnap.target) <= 0.25F ) {
                // 参考线使用画布局部坐标，所以把世界目标重新加回相机偏移。
                m_canvasComponentSnapGuideX = edgeSnap.target + cameraOffsetX;
            }
        }

        Config::TrackLayout             candidate   = layout;
        Config::HorizontalRegionLayout* target      = nullptr;
        float                           storedWidth = candidateBounds.width;
        bool                            anchorRight = false;
        // 草稿与 BGM 配置保存单轨宽度，批注区保存整个区域宽度。
        // 这里把屏幕上看到的总宽度还原为配置模型的存储语义，避免轨道数
        // 改变后辅助区宽度被重复放大。
        switch ( m_auxiliaryLayoutRegion ) {
        case AuxiliaryLayoutRegion::Draft:
            target      = &candidate.draftLanes;
            anchorRight = true;
            storedWidth /= static_cast<float>(
                std::max(auxiliaryProjection.draftLaneCount, 1U));
            break;
        case AuxiliaryLayoutRegion::Annotation:
            target = &candidate.annotation;
            break;
        case AuxiliaryLayoutRegion::Bgm:
            target = &candidate.bgmLanes;
            storedWidth /= static_cast<float>(
                std::max(auxiliaryProjection.bgmLaneCount, 1U));
            break;
        case AuxiliaryLayoutRegion::None: break;
        }
        if ( target ) {
            constexpr float layoutEpsilon = 1e-6F;
            const float     storedAnchor =
                anchorRight ? candidateBounds.right() : candidateBounds.left;
            const auto& currentAnchor =
                anchorRight ? target->right : target->left;
            const bool changed =
                !currentAnchor || !target->width ||
                std::abs(*currentAnchor - storedAnchor) > layoutEpsilon ||
                std::abs(*target->width - storedWidth) > layoutEpsilon ||
                (anchorRight && target->left.has_value());
            if ( changed ) {
                // optional anchor 的存在性也是配置状态的一部分；即使数值相同，
                // 从左锚切到右锚仍必须写回。
                // 草稿区锁定右边界，新增轨只会向左扩展；其余区域锁定左边界。
                if ( anchorRight ) {
                    target->left.reset();
                    target->right = storedAnchor;
                } else {
                    target->left = storedAnchor;
                }
                target->width = storedWidth;
                appConfig.getVisualConfig().editableTrackLayoutForKeyCount(
                    keyCount) = candidate;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                        appConfig.getEditorConfig() }));
                m_layoutConfigurationChanged = true;
                layout                       = candidate;
            }
        }
    }

    if ( !m_noteScaleDragTarget.has_value() &&
         !m_canvasComponentDragTarget.has_value() &&
         m_trackLayoutDragHandle != TrackLayoutDragHandle::None &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 主轨道布局最后处理，优先级低于音符比例、文字组件和辅助区。
        // 非左右句柄保持未吸附的原始世界坐标。
        HorizontalResizeSnapResult widthSnap{ .position = worldPointerX };
        if ( m_trackLayoutDragHandle == TrackLayoutDragHandle::Left ||
             m_trackLayoutDragHandle == TrackLayoutDragHandle::Right ) {
            // 冻结列表已经排除主轨道自身，只会匹配真正的其他组件边缘。
            widthSnap = snapHorizontalResizeEdge(worldPointerX,
                                                 m_canvasComponentSnapTargetsX,
                                                 componentSnapDistance);
        }
        const float         normalizedX = widthSnap.position / targetWidth;
        const float         normalizedY = pointerY / targetHeight;
        Config::TrackLayout candidate   = m_trackLayoutDragStart;
        if ( m_trackLayoutDragHandle == TrackLayoutDragHandle::JudgmentLine ) {
            // 判定线只修改纵向比例，不改变轨道矩形。
            constexpr float positionEpsilon = 1e-6f;
            const float     candidatePosition =
                sanitizeJudgmentLinePosition(normalizedY);
            const float currentPosition =
                appConfig.getVisualConfig().judgmentLinePositionForKeyCount(
                    keyCount);
            if ( !std::isfinite(currentPosition) ||
                 std::abs(currentPosition - candidatePosition) >
                     positionEpsilon ) {
                // 非有限旧值也视为变化，借本次交互把配置恢复到合法范围。
                appConfig.getVisualConfig()
                    .editableJudgmentLinePositionForKeyCount(keyCount) =
                    candidatePosition;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                        appConfig.getEditorConfig() }));
                m_layoutConfigurationChanged = true;
                judgmentLinePosition         = candidatePosition;
            }
        } else {
            // 边缘缩放只改变对应轴；中心句柄同时移动四条边。
            switch ( m_trackLayoutDragHandle ) {
            case TrackLayoutDragHandle::Left:
            case TrackLayoutDragHandle::Right:
                candidate = resizeTrackLayout(m_trackLayoutDragStart,
                                              m_trackLayoutDragHandle,
                                              normalizedX);
                break;
            case TrackLayoutDragHandle::Top:
            case TrackLayoutDragHandle::Bottom:
                candidate = resizeTrackLayout(m_trackLayoutDragStart,
                                              m_trackLayoutDragHandle,
                                              normalizedY);
                break;
            case TrackLayoutDragHandle::Move: {
                // 移动时动态收集所有可见文字组件和画布中心作为对齐基准。
                // 主轨道自身不进入目标集合，避免零位移总是被自身命中。
                candidate =
                    moveTrackLayout(m_trackLayoutDragStart,
                                    normalizedX - m_trackLayoutPointerStart.x,
                                    normalizedY - m_trackLayoutPointerStart.y);
                m_canvasComponentSnapGuideX.reset();
                m_canvasComponentSnapGuideY.reset();
                m_canvasComponentSnapTargetsX.clear();
                m_canvasComponentSnapTargetsY.clear();
                const auto& canvasComponents =
                    appConfig.getVisualConfig().canvasComponentsForKeyCount(
                        keyCount);
                const std::size_t targetObjectCount =
                    currentSnapshot.canvasComponentInstances.size();
                m_canvasComponentSnapTargetsX.reserve(targetObjectCount * 3U +
                                                      1U);
                m_canvasComponentSnapTargetsY.reserve(targetObjectCount * 3U +
                                                      1U);
                appendDisplayedCanvasComponentSnapTargets(
                    currentSnapshot,
                    canvasComponents,
                    m_canvasComponentSnapTargetsX,
                    m_canvasComponentSnapTargetsY);
                m_canvasComponentSnapTargetsX.push_back(targetWidth * 0.5f);
                m_canvasComponentSnapTargetsY.push_back(targetHeight * 0.5f);

                // 配置使用归一化坐标，吸附算法使用画布局部像素；相机偏移
                // 只作用于横轴，构造与回写时必须成对加减。
                const Logic::CanvasComponentBounds candidateBounds{
                    candidate.left * targetWidth + cameraOffsetX,
                    candidate.top * targetHeight,
                    candidate.right * targetWidth + cameraOffsetX,
                    candidate.bottom * targetHeight,
                };
                const auto snap = Logic::snapCanvasComponentBounds(
                    candidateBounds,
                    m_canvasComponentSnapTargetsX,
                    m_canvasComponentSnapTargetsY,
                    componentSnapDistance);
                // 以吸附后的像素中心整体搬移轨道框，宽高保持冻结值。
                candidate =
                    moveTrackLayoutToPixelCenter(candidate,
                                                 snap.center.x - cameraOffsetX,
                                                 snap.center.y,
                                                 targetWidth,
                                                 targetHeight);
                const Logic::CanvasComponentBounds snappedBounds{
                    candidate.left * targetWidth + cameraOffsetX,
                    candidate.top * targetHeight,
                    candidate.right * targetWidth + cameraOffsetX,
                    candidate.bottom * targetHeight,
                };
                // 边界钳制可能使候选未真正到达目标，因此参考线仍需复核。
                if ( snap.snappedX &&
                     canvasComponentAlignsWithX(snappedBounds, snap.targetX) ) {
                    m_canvasComponentSnapGuideX = snap.targetX;
                }
                if ( snap.snappedY &&
                     canvasComponentAlignsWithY(snappedBounds, snap.targetY) ) {
                    m_canvasComponentSnapGuideY = snap.targetY;
                }
                break;
            }
            case TrackLayoutDragHandle::JudgmentLine: break;
            case TrackLayoutDragHandle::None: break;
            }

            if ( m_trackLayoutDragHandle == TrackLayoutDragHandle::Left ||
                 m_trackLayoutDragHandle == TrackLayoutDragHandle::Right ) {
                // 布局最小跨度或视口边界钳制后，仅对真实重合的边缘保留参考线。
                const float resizedEdge =
                    (m_trackLayoutDragHandle == TrackLayoutDragHandle::Left
                         ? candidate.left
                         : candidate.right) *
                    targetWidth;
                // 钳制失败或指针离开阈值时不保留上一帧参考线。
                m_canvasComponentSnapGuideX.reset();
                m_canvasComponentSnapGuideY.reset();
                if ( widthSnap.snapped &&
                     std::abs(resizedEdge - widthSnap.target) <= 0.25F ) {
                    // 配置边缘采用世界坐标，参考线采用画布局部坐标。
                    m_canvasComponentSnapGuideX =
                        widthSnap.target + cameraOffsetX;
                }
            }

            constexpr float layoutEpsilon = 1e-6f;
            const auto&     current =
                appConfig.getVisualConfig().trackLayoutForKeyCount(keyCount);
            const bool changed =
                std::abs(current.left - candidate.left) > layoutEpsilon ||
                std::abs(current.top - candidate.top) > layoutEpsilon ||
                std::abs(current.right - candidate.right) > layoutEpsilon ||
                std::abs(current.bottom - candidate.bottom) > layoutEpsilon;
            if ( changed ) {
                // 拖动中发布配置用于即时预览，但只在释放时保存到磁盘。
                appConfig.getVisualConfig().editableTrackLayoutForKeyCount(
                    keyCount) = candidate;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                        appConfig.getEditorConfig() }));
                m_layoutConfigurationChanged = true;
                layout                       = candidate;
            }
        }
    }

    if ( (m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
          m_horizontalRegionDragHandle != HorizontalRegionDragHandle::None ||
          m_noteScaleDragTarget.has_value() ||
          m_canvasComponentDragTarget.has_value()) &&
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 释放发生在画布外也必须结束手势并落盘，不能依赖 hovered 状态。
        finishLayoutEditing();
    }

    layout = sanitizeTrackLayout(
        appConfig.getVisualConfig().trackLayoutForKeyCount(keyCount));
    judgmentLinePosition = sanitizeJudgmentLinePosition(
        appConfig.getVisualConfig().judgmentLinePositionForKeyCount(keyCount));
    // 写回配置后重新投影，确保本帧句柄紧随拖动结果。
    const auto displayAuxiliaryProjection =
        Logic::calculateCanvasLaneProjection(targetWidth,
                                             keyCount,
                                             currentSnapshot.bgmTrackCount,
                                             layout,
                                             cameraOffsetX,
                                             true,
                                             true,
                                             true,
                                             currentSnapshot.draftTrackCount,
                                             true);
    const ImVec2 canvasMin{ canvasScreenX, canvasScreenY };
    const ImVec2 canvasMax{ canvasScreenX + targetWidth,
                            canvasScreenY + targetHeight };
    const ImVec2 layoutMin{ canvasScreenX + layout.left * targetWidth +
                                cameraOffsetX,
                            canvasScreenY + layout.top * targetHeight };
    const ImVec2 layoutMax{ canvasScreenX + layout.right * targetWidth +
                                cameraOffsetX,
                            canvasScreenY + layout.bottom * targetHeight };
    const ImVec2 layoutCenter{ (layoutMin.x + layoutMax.x) * 0.5f,
                               (layoutMin.y + layoutMax.y) * 0.5f };
    const float  judgmentLineY =
        canvasScreenY + judgmentLinePosition * targetHeight;

    // 编辑遮罩和句柄使用前景绘制列表，确保不会被谱面物件覆盖。
    // clip rect 把相机横移后的几何严格裁剪在当前画布标签页内。
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->PushClipRect(canvasMin, canvasMax, true);
    drawList->AddRectFilled(
        canvasMin, { canvasMax.x, layoutMin.y }, IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled(
        { canvasMin.x, layoutMax.y }, canvasMax, IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled({ canvasMin.x, layoutMin.y },
                            { layoutMin.x, layoutMax.y },
                            IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled({ layoutMax.x, layoutMin.y },
                            { canvasMax.x, layoutMax.y },
                            IM_COL32(0, 0, 0, 72));
    drawList->AddRectFilled(layoutMin, layoutMax, IM_COL32(64, 180, 255, 24));
    // 四块暗色遮罩强调主轨道矩形，半透明填充保留背景参照。

    std::optional<Logic::CanvasComponentBounds> editableComponentRegion;
    if ( m_canvasComponentDragTarget.has_value() ) {
        editableComponentRegion = m_canvasComponentDragRegion;
    } else if ( hoveredComponentInstance.has_value() &&
                hoveredComponentHandle !=
                    Logic::CanvasComponentDragHandle::None ) {
        editableComponentRegion =
            canvasComponentLayoutRegion(*hoveredComponentInstance);
    }
    if ( editableComponentRegion.has_value() ) {
        // 文本组件使用各自的约束区；拖动时优先显示冻结区域，悬停时显示
        // 当前实例区域，帮助用户理解锚点的有效范围。
        drawCanvasComponentEditableRegionMask(*drawList,
                                              *editableComponentRegion,
                                              canvasScreenX,
                                              canvasScreenY,
                                              targetWidth,
                                              targetHeight,
                                              dpiScale);
    }

    const ImU32 edgeColor        = IM_COL32(64, 190, 255, 230);
    const ImU32 highlightedColor = IM_COL32(255, 218, 96, 255);
    const float edgeThickness    = std::max(2.0f, 2.0f * dpiScale);
    auto        handleColor      = [&](TrackLayoutDragHandle handle) {
        // 同一套颜色函数同时驱动边线和抓手，避免悬停反馈不一致。
        return hoveredHandle == handle ? highlightedColor : edgeColor;
    };

    // 三个辅助区使用不同颜色，矩形纵向边界始终绑定主轨道区。
    const auto drawAuxiliaryRegion = [&](AuxiliaryLayoutRegion region,
                                         float                 left,
                                         float                 right,
                                         ImU32                 color) {
        if ( !displayAuxiliaryProjection.valid || right <= left ) return;
        // 投影坐标已经包含横向相机偏移，此处只叠加屏幕原点。
        const ImVec2 minimum{ canvasScreenX + left, layoutMin.y };
        const ImVec2 maximum{ canvasScreenX + right, layoutMax.y };
        const bool   active      = hoveredAuxiliaryRegion == region;
        const ImU32  activeColor = active ? highlightedColor : color;
        drawList->AddRectFilled(
            minimum, maximum, color & IM_COL32(255, 255, 255, 30));
        drawList->AddRect(
            minimum, maximum, activeColor, 0.0F, 0, edgeThickness);
        const ImVec2 center{ (minimum.x + maximum.x) * 0.5F,
                             (minimum.y + maximum.y) * 0.5F };
        drawList->AddCircleFilled(
            center, std::max(4.0F, 5.0F * dpiScale), activeColor);
        // 中心圆是移动句柄，边缘短线是左右缩放句柄。
        // 左右短横线明确表示只能沿 X 方向缩放和移动。
        const float tick = std::max(5.0F, 7.0F * dpiScale);
        drawList->AddLine({ minimum.x, center.y - tick },
                          { minimum.x, center.y + tick },
                          activeColor,
                          edgeThickness);
        drawList->AddLine({ maximum.x, center.y - tick },
                          { maximum.x, center.y + tick },
                          activeColor,
                          edgeThickness);
    };
    drawAuxiliaryRegion(AuxiliaryLayoutRegion::Draft,
                        displayAuxiliaryProjection.draftLeftX,
                        displayAuxiliaryProjection.draftRightX,
                        IM_COL32(96, 220, 255, 220));
    drawAuxiliaryRegion(AuxiliaryLayoutRegion::Annotation,
                        displayAuxiliaryProjection.annotationLeftX,
                        displayAuxiliaryProjection.annotationRightX,
                        IM_COL32(255, 190, 72, 230));
    drawAuxiliaryRegion(AuxiliaryLayoutRegion::Bgm,
                        displayAuxiliaryProjection.bgmLeftX,
                        displayAuxiliaryProjection.bgmRightX,
                        IM_COL32(176, 112, 255, 230));
    // 辅助区先绘制，主轨道边线后绘制；重叠处以主轨道轮廓为准。

    drawList->AddLine({ layoutMin.x, layoutMin.y },
                      { layoutMin.x, layoutMax.y },
                      handleColor(TrackLayoutDragHandle::Left),
                      edgeThickness);
    drawList->AddLine({ layoutMin.x, layoutMin.y },
                      { layoutMax.x, layoutMin.y },
                      handleColor(TrackLayoutDragHandle::Top),
                      edgeThickness);
    drawList->AddLine({ layoutMax.x, layoutMin.y },
                      { layoutMax.x, layoutMax.y },
                      handleColor(TrackLayoutDragHandle::Right),
                      edgeThickness);
    drawList->AddLine({ layoutMin.x, layoutMax.y },
                      { layoutMax.x, layoutMax.y },
                      handleColor(TrackLayoutDragHandle::Bottom),
                      edgeThickness);

    const ImU32 judgmentLineColor =
        hoveredHandle == TrackLayoutDragHandle::JudgmentLine
            ? highlightedColor
            : IM_COL32(255, 112, 190, 235);
    // 判定线颜色独立于轨道边缘，使其句柄在密集布局中仍可识别。
    drawList->AddLine({ layoutMin.x, judgmentLineY },
                      { layoutMax.x, judgmentLineY },
                      judgmentLineColor,
                      edgeThickness);

    const float gripHalfLong  = std::max(12.0f, 15.0f * dpiScale);
    const float gripHalfShort = std::max(3.0f, 4.0f * dpiScale);
    const float gripRounding  = gripHalfShort;
    const float middleX       = layoutCenter.x;
    const float middleY       = layoutCenter.y;
    // 四条边只在中点绘制长条抓手，降低与角落文字组件句柄的冲突。
    drawList->AddRectFilled(
        { layoutMin.x - gripHalfShort, middleY - gripHalfLong },
        { layoutMin.x + gripHalfShort, middleY + gripHalfLong },
        handleColor(TrackLayoutDragHandle::Left),
        gripRounding);
    drawList->AddRectFilled(
        { layoutMax.x - gripHalfShort, middleY - gripHalfLong },
        { layoutMax.x + gripHalfShort, middleY + gripHalfLong },
        handleColor(TrackLayoutDragHandle::Right),
        gripRounding);
    drawList->AddRectFilled(
        { middleX - gripHalfLong, layoutMin.y - gripHalfShort },
        { middleX + gripHalfLong, layoutMin.y + gripHalfShort },
        handleColor(TrackLayoutDragHandle::Top),
        gripRounding);
    drawList->AddRectFilled(
        { middleX - gripHalfLong, layoutMax.y - gripHalfShort },
        { middleX + gripHalfLong, layoutMax.y + gripHalfShort },
        handleColor(TrackLayoutDragHandle::Bottom),
        gripRounding);
    drawList->AddRectFilled(
        { layoutMax.x - gripHalfLong, judgmentLineY - gripHalfShort },
        { layoutMax.x + gripHalfLong, judgmentLineY + gripHalfShort },
        judgmentLineColor,
        gripRounding);

    const ImU32 moveColor = handleColor(TrackLayoutDragHandle::Move);
    // 中心十字圆表示二维移动，深色十字在浅色和深色皮肤上都保持轮廓。
    drawList->AddCircleFilled(layoutCenter, moveHandleRadius, moveColor);
    const float arrowLength    = moveHandleRadius * 0.55f;
    const float arrowThickness = std::max(1.5f, 1.5f * dpiScale);
    drawList->AddLine({ layoutCenter.x - arrowLength, layoutCenter.y },
                      { layoutCenter.x + arrowLength, layoutCenter.y },
                      IM_COL32(20, 30, 40, 255),
                      arrowThickness);
    drawList->AddLine({ layoutCenter.x, layoutCenter.y - arrowLength },
                      { layoutCenter.x, layoutCenter.y + arrowLength },
                      IM_COL32(20, 30, 40, 255),
                      arrowThickness);

    const auto& canvasComponents =
        appConfig.getVisualConfig().canvasComponentsForKeyCount(keyCount);
    const bool groupAllKpsPositions =
        canvasComponents.kps.visible &&
        canvasComponents.syncAllKpsComponentPositions;
    const bool groupKpsTrackPositions =
        canvasComponents.kps.visible && !groupAllKpsPositions &&
        canvasComponents.syncKpsTrackRelativePositions;
    Logic::CanvasComponentBounds synchronizedKpsGroupBounds;
    bool                         hasSynchronizedKpsGroupBounds   = false;
    std::size_t                  synchronizedKpsGroupMemberCount = 0U;
    if ( groupAllKpsPositions || groupKpsTrackPositions ) {
        // 同步 KPS 的虚拟组框不来自快照，需用当前 placement 和内容尺寸
        // 重新计算，才能反映本帧刚写入的布局配置。
        for ( const auto& instance :
              currentSnapshot.canvasComponentInstances ) {
            if ( instance.type != Config::CanvasComponentType::Kps ||
                 (groupKpsTrackPositions && instance.instanceIndex < 0) ) {
                continue;
            }
            const auto placement = canvasComponents.resolvedPlacement(
                Config::CanvasComponentType::Kps,
                instance.instanceIndex,
                currentSnapshot.trackCount,
                layout.left,
                layout.right);
            const auto contentBounds = canvasComponentContentBounds(instance);
            const auto bounds        = Logic::canvasComponentBoundsInRegion(
                placement,
                canvasComponentLayoutRegion(instance),
                contentBounds.width(),
                contentBounds.height());
            // 无有效内容尺寸的实例不会参与组框，也不计入可视成员数。
            if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) continue;
            mergeCanvasComponentBounds(bounds,
                                       synchronizedKpsGroupBounds,
                                       hasSynchronizedKpsGroupBounds);
            ++synchronizedKpsGroupMemberCount;
        }
    }
    if ( hasSynchronizedKpsGroupBounds &&
         synchronizedKpsGroupMemberCount > 1U ) {
        // 单成员无需额外组框；只有组操作能影响多个实例时才提供提示。
        const bool movingSynchronizedKps =
            m_canvasComponentDragTarget == Config::CanvasComponentType::Kps &&
            m_canvasComponentDragHandle ==
                Logic::CanvasComponentDragHandle::Move &&
            (groupAllKpsPositions || (groupKpsTrackPositions &&
                                      m_canvasComponentDragInstanceIndex >= 0));
        const ImU32 groupColor = movingSynchronizedKps
                                     ? highlightedColor
                                     : IM_COL32(90, 220, 255, 180);
        // 正在整体移动时使用高亮色，静止时用低权重青色虚框。
        drawList->AddRect({ canvasScreenX + synchronizedKpsGroupBounds.left,
                            canvasScreenY + synchronizedKpsGroupBounds.top },
                          { canvasScreenX + synchronizedKpsGroupBounds.right,
                            canvasScreenY + synchronizedKpsGroupBounds.bottom },
                          groupColor,
                          0.0f,
                          0,
                          std::max(1.5f, 2.0f * dpiScale));
    }

    const ImU32 snapGuideColor     = IM_COL32(255, 218, 96, 150);
    const float snapGuideThickness = std::max(1.0f, 1.25f * dpiScale);
    const float snapGuideDash      = std::max(4.0f, 6.0f * dpiScale);
    const float snapGuideGap       = std::max(3.0f, 4.0f * dpiScale);
    if ( m_canvasComponentSnapGuideX.has_value() ) {
        // 参考线存储画布局部坐标，绘制时补回标签页屏幕原点。
        const float guideX = canvasScreenX + *m_canvasComponentSnapGuideX;
        drawCanvasComponentSnapGuide(*drawList,
                                     { guideX, canvasMin.y },
                                     { guideX, canvasMax.y },
                                     snapGuideColor,
                                     snapGuideThickness,
                                     snapGuideDash,
                                     snapGuideGap);
    }
    if ( m_canvasComponentSnapGuideY.has_value() ) {
        // 横向参考线覆盖完整画布，便于跨区域判断对齐对象。
        const float guideY = canvasScreenY + *m_canvasComponentSnapGuideY;
        drawCanvasComponentSnapGuide(*drawList,
                                     { canvasMin.x, guideY },
                                     { canvasMax.x, guideY },
                                     snapGuideColor,
                                     snapGuideThickness,
                                     snapGuideDash,
                                     snapGuideGap);
    }

    const ImU32 noteBoundsColor = IM_COL32(255, 150, 96, 220);
    for ( const auto& instance : m_noteLayoutInstances ) {
        // 音符缩放句柄基于按帧重建的音符包围盒；无尺寸对象不可编辑。
        const auto& bounds = instance.bounds;
        if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) continue;
        const bool highlighted =
            (hoveredNote.has_value() && *hoveredNote == instance.entity) ||
            (m_noteScaleDragTarget.has_value() &&
             *m_noteScaleDragTarget == instance.entity);
        // 悬停和正在拖动共享高亮色，指针离开画布后仍保持拖动反馈。
        const ImU32 componentColor =
            highlighted ? highlightedColor : noteBoundsColor;
        const ImVec2 componentMin{ canvasScreenX + bounds.left,
                                   canvasScreenY + bounds.top };
        const ImVec2 componentMax{ canvasScreenX + bounds.right,
                                   canvasScreenY + bounds.bottom };
        drawList->AddRect(componentMin,
                          componentMax,
                          componentColor,
                          0.0f,
                          0,
                          std::max(1.0f, 1.25f * dpiScale));

        const float handleHalf = std::max(2.5f, 3.0f * dpiScale);
        const std::array<ImVec2, 4> corners{
            componentMin,
            ImVec2{ componentMax.x, componentMin.y },
            ImVec2{ componentMin.x, componentMax.y },
            componentMax,
        };
        for ( const auto& corner : corners ) {
            // 四角均可等比或分轴缩放，具体语义由命中的角点枚举决定。
            drawList->AddRectFilled(
                { corner.x - handleHalf, corner.y - handleHalf },
                { corner.x + handleHalf, corner.y + handleHalf },
                componentColor);
        }
    }

    for ( const auto& instance : currentSnapshot.canvasComponentInstances ) {
        // 文本组件沿用渲染快照的内容边界，确保编辑框和实际字形一致。
        const auto& placement = appConfig.getVisualConfig()
                                    .canvasComponentsForKeyCount(keyCount)
                                    .placement(instance.type);
        if ( !placement.visible ) continue;

        const auto bounds = canvasComponentContentBounds(instance);
        if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) continue;
        const bool highlighted =
            hoveredComponent.has_value() &&
            *hoveredComponent == instance.type &&
            ((hoveredComponentInstance.has_value() &&
              hoveredComponentInstance->instanceIndex ==
                  instance.instanceIndex) ||
             (m_canvasComponentDragTarget.has_value() &&
              m_canvasComponentDragInstanceIndex == instance.instanceIndex));
        // 同类型可有多个 KPS 实例，必须同时比较 instanceIndex。
        const ImU32 componentColor =
            highlighted ? highlightedColor : IM_COL32(90, 220, 255, 240);
        const ImVec2 componentMin{ canvasScreenX + bounds.left,
                                   canvasScreenY + bounds.top };
        const ImVec2 componentMax{ canvasScreenX + bounds.right,
                                   canvasScreenY + bounds.bottom };
        drawList->AddRect(componentMin,
                          componentMax,
                          componentColor,
                          0.0f,
                          0,
                          std::max(1.0f, 1.5f * dpiScale));

        const float handleHalf = std::max(3.0f, 3.5f * dpiScale);
        const std::array<ImVec2, 4> corners{
            componentMin,
            ImVec2{ componentMax.x, componentMin.y },
            ImVec2{ componentMin.x, componentMax.y },
            componentMax,
        };
        for ( const auto& corner : corners ) {
            // 组件角点改变字体比例；中心区域由 hit-test 解释为移动。
            drawList->AddRectFilled(
                { corner.x - handleHalf, corner.y - handleHalf },
                { corner.x + handleHalf, corner.y + handleHalf },
                componentColor);
        }
    }
    // 所有布局辅助几何完成后恢复前景绘制列表的裁剪栈。
    drawList->PopClipRect();
}

/// @brief 绘制批注栏、详情提示和编辑弹窗。
///
/// @details 数据与生命周期：
/// - 批注标记完全来自当前不可变 `RenderSnapshot`。
/// - 批注显示不依赖批注表窗口是否打开。
/// - 本函数不触发批注数据预热或表格快照消费。
/// - marker 和 item 观察指针只在本次调用期间有效。
/// - 成员只保存字符串 ID、索引、滚动量和编辑缓冲区。
/// - 快照更新后通过稳定 ID 重新识别时间戳分组。
/// - 条目删除导致索引越界时会钳制到新末尾。
/// - 时间戳相同的多条批注聚合为一个画布标记。
/// - 聚合标记的首条 ID 作为本帧分组稳定键。
/// - 气泡数量只在聚合条目超过一条时显示。
/// - 目标提示边界来自快照 hitbox，不查询 ECS。
/// - 目标已删除时保留批注并显示缺失提示。
///
/// @details 几何与绘制：
/// - 批注栏横向范围来自统一轨道投影。
/// - 批注栏纵向范围跟随主轨道布局上下边界。
/// - 标记中心固定在批注栏横向中心。
/// - 标记纵坐标由逻辑层按当前时间视野投影。
/// - 画布外十像素缓冲用于完整裁剪气泡尾部。
/// - 标记和连线绘制裁剪在轨道纵向范围内。
/// - tooltip 和 modal 在恢复裁剪栈后绘制。
/// - 皮肤缺色时使用稳定的 ImGui 回退色。
/// - 悬停气泡使用独立高亮色，不改变持久数据。
/// - 详情卡可占用批注栏以外的画布空白区域。
/// - 目标提示裁剪在整个画布，不覆盖相邻 Dock。
/// - 轨道号以一基形式展示，内部仍保持零基值。
/// - Markdown 正文通过公共渲染器显示。
/// - 内容摘要只引用首行，不复制完整正文。
///
/// @details 命中与优先级：
/// - 详情卡命中优先于同位置的批注气泡。
/// - 批注栏之外不会通过气泡纵坐标扩大命中范围。
/// - 详情卡链接命中优先于批注编辑手势。
/// - modal 打开时批注层始终阻止底层画布输入。
/// - 批注栏悬停会阻止新画笔、选择和对象拖动。
/// - 已从画布开始的框选允许跨过批注栏继续。
/// - 已有手势在批注栏释放时仍会收到结束命令。
/// - 详情卡正文实际滚动时消费滚轮。
/// - 详情到达滚动边界时允许外层判断是否透传。
/// - 气泡 tooltip 可滚动时优先消费滚轮。
/// - 编辑弹窗打开时任何滚轮都不传给画布。
/// - 空白批注栏仅在可编辑静止会话中响应右键。
/// - 播放中与只读会话仍允许阅读，但禁止修改。
///
/// @details 条目浏览：
/// - 详情卡命中具体条目时同步当前详情索引。
/// - 从气泡进入 tooltip 时默认显示当前或第一条。
/// - 上/左方向键选择前一条。
/// - 下/右方向键选择后一条。
/// - 条目切换在首尾循环。
/// - 文本输入期间不解释方向键。
/// - 切换时间戳分组后重置 tooltip 滚动位置。
/// - 切换同组条目后同样重置完整正文滚动位置。
/// - 列表当前项使用强调色。
/// - 非当前项使用低权重文字色。
/// - 多条时显示方向键或滚轮提示。
/// - 单条长内容只显示滚动提示。
/// - 标题同时说明时间戳与聚合数量。
/// - 详情元数据先于 Markdown 正文展示。
///
/// @details 编辑手势：
/// - 气泡左键打开已有批注编辑器。
/// - 气泡右键在同一时间戳新增批注。
/// - 详情卡使用 Shift+右键打开已有批注编辑器。
/// - 详情卡普通右键保留给卡片和链接上下文。
/// - 空白批注栏右键在当前悬浮时间新建。
/// - 开启节奏吸附时新建使用 `snappedTime`。
/// - 未吸附时新建使用连续 `hoveredTime`。
/// - 负悬浮时间在新建前钳制为零。
/// - 编辑已有条目时保存原 annotationId。
/// - 新建条目保持 annotationId 为空。
/// - 固定正文缓冲区复制时预留 NUL 结尾。
/// - 可见 modal 标题允许翻译，`###` 后使用固定内部 ID。
/// - OpenPopup 请求延迟到统一绘制位置执行。
///
/// @details 保存与删除：
/// - annotationId 是否为空是编辑和新增的唯一判据。
/// - 新增批注要求规范化后的默认创作者非空。
/// - 历史批注允许展示未知作者。
/// - 正文为空时保存按钮禁用。
/// - 新增和编辑统一发布 `CmdUpsertBeatmapAnnotation`。
/// - 空 ID 的生成职责属于逻辑层。
/// - 时间点新建使用 TIMESTAMP 目标类型。
/// - 编辑器不直接修改 BeatMap 或批注容器。
/// - 删除只在已有 annotationId 时显示。
/// - 删除发布 `CmdRemoveBeatmapAnnotation`。
/// - 命令发布后关闭弹窗，等待快照异步回流。
/// - 取消只关闭 UI，不发布数据命令。
/// - 下次打开入口会完整重置编辑缓冲和元数据。
///
/// @details 返回协议：
/// - `blocksCanvas` 表示当前批注层是否阻止画布点击。
/// - `passesWheelToCanvas` 独立表示滚轮是否允许继续路由。
/// - `wheelConsumed` 表示详情内容本帧已实际滚动。
/// - 三个标志不能合并为单一 hovered 状态。
/// - 调用方依据这些标志完成被遮挡手势的收尾。
/// - 调用方只在允许时继续修饰键和普通滚轮处理。
/// @param currentSnapshot 当前主画布渲染快照。
/// @param canvasScreenX 画布左上角屏幕横坐标。
/// @param canvasScreenY 画布左上角屏幕纵坐标。
/// @param targetWidth 画布宽度。
/// @param targetHeight 画布高度。
/// @param pointerX 指针相对画布左侧的横坐标。
/// @param pointerY 指针相对画布顶部的纵坐标。
/// @param canvasHovered 指针是否位于当前画布。
/// @return 批注交互层对指针和滚轮输入的处理结果。
/// @warning UI 热路径：每帧只遍历当前快照可见批注；全量表格数据由 Timeline
/// 窗口在批注版本变化时低频刷新。
Basic2DCanvasInteraction::AnnotationGutterInteractionResult
Basic2DCanvasInteraction::renderAnnotationGutter(
    const Common::Render::RenderSnapshot& currentSnapshot, float canvasScreenX,
    float canvasScreenY, float targetWidth, float targetHeight, float pointerX,
    float pointerY, bool canvasHovered)
{
    // 批注栏位置由当前轨道布局和辅助轨道投影共同决定。
    // 这里不读取批注表窗口状态，画布批注是独立数据的只读快照视图。
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout =
        visual.trackLayoutForKeyCount(currentSnapshot.trackCount);
    const auto projection = Logic::calculateCanvasLaneProjection(
        targetWidth,
        currentSnapshot.trackCount,
        currentSnapshot.bgmTrackCount,
        layout,
        currentSnapshot.canvasHorizontalOffsetX,
        true,
        currentSnapshot.bmsEditingEnabled,
        currentSnapshot.draftLanesEnabled,
        currentSnapshot.draftTrackCount,
        true);
    // 纵向范围严格跟随主轨道上下边界，防止批注标记侵入工具栏区域。
    const float topY          = layout.top * targetHeight;
    const float bottomY       = layout.bottom * targetHeight;
    const bool  gutterHovered = projection.valid && canvasHovered &&
                               pointerX >= projection.annotationLeftX &&
                               pointerX <= projection.annotationRightX &&
                               pointerY >= topY && pointerY <= bottomY;

    // hoveredMarker 指向当前快照，生命周期仅限本帧，不保存为成员。
    // detail index 只有在详情卡命中具体条目时才存在；普通气泡命中默认
    // 沿用当前选择或回到第一条。
    const Common::Render::AnnotationRenderMarker* hoveredMarker = nullptr;
    std::optional<std::size_t>                    hoveredDetailIndex;
    bool                                          detailCardHovered   = false;
    bool                                          detailLinkHovered   = false;
    bool                                          detailWheelConsumed = false;
    if ( projection.valid && currentSnapshot.hasBeatmap ) {
        // 标记与详情连线裁剪在轨道纵向范围内，横向允许详情卡利用画布空间。
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(
            { canvasScreenX, canvasScreenY + topY },
            { canvasScreenX + targetWidth, canvasScreenY + bottomY },
            true);
        const float centerX =
            (projection.annotationLeftX + projection.annotationRightX) * 0.5F;
        // 颜色从皮肤语义键读取；缺失时使用保证对比度的默认色。
        const ImU32 markerColor = annotationUiColor(
            "annotations.marker", ImVec4(0.42F, 0.72F, 0.96F, 0.98F));
        const ImU32 hoverColor = annotationUiColor(
            "annotations.marker_hover", ImVec4(0.68F, 0.86F, 1.0F, 1.0F));
        const ImU32 textColor = annotationUiColor(
            "annotations.marker_text", ImVec4(0.04F, 0.08F, 0.12F, 1.0F));

        if ( Config::AppConfig::instance()
                 .getEditorSettings()
                 .showAnnotationDetails ) {
            // 详情卡布局函数负责同时间戳多条批注的堆叠、内部滚动和链接
            // 命中，本层只消费其交互结果并维护编辑选择。
            const auto detailHit = renderConnectedAnnotationDetails(
                currentSnapshot.annotationMarkers,
                projection,
                canvasScreenX,
                canvasScreenY,
                targetWidth,
                topY,
                bottomY,
                pointerX,
                pointerY,
                canvasHovered,
                m_annotationDetailScrollItemId,
                m_annotationDetailScrollY);
            if ( detailHit.marker ) {
                // 详情卡命中优先于同位置的气泡，避免两个入口争用点击。
                hoveredMarker       = detailHit.marker;
                hoveredDetailIndex  = detailHit.itemIndex;
                detailCardHovered   = true;
                detailLinkHovered   = detailHit.linkHovered;
                detailWheelConsumed = detailHit.wheelConsumed;
                if ( detailHit.itemIndex < detailHit.marker->items.size() ) {
                    // 目标仍存在时在谱面对象周围绘制提示；缺失目标只在详情
                    // 文本中显示状态，不伪造包围盒。
                    const auto targetBounds = findAnnotationTargetHintBounds(
                        detailHit.marker->items[detailHit.itemIndex],
                        currentSnapshot.hitboxes);
                    if ( targetBounds ) {
                        renderAnnotationTargetHint(
                            *targetBounds,
                            { canvasScreenX, canvasScreenY },
                            targetWidth,
                            targetHeight);
                    }
                }
            }
        }

        for ( const auto& marker : currentSnapshot.annotationMarkers ) {
            const float markerY = marker.canvasY;
            // 给气泡尾部保留十像素缓冲，边缘附近不会突然截断主体。
            if ( markerY < topY - 10.0F || markerY > bottomY + 10.0F ) {
                continue;
            }
            // 标记只在批注栏横向范围内响应，避免覆盖主轨道物件交互。
            const bool markerHovered =
                gutterHovered && std::abs(pointerY - markerY) <= 10.0F;
            // 详情卡已命中时不得被后续气泡覆盖。
            if ( markerHovered && !hoveredMarker ) hoveredMarker = &marker;

            // 气泡中心固定在批注栏中央，纵向位置来自逻辑快照投影。
            const ImVec2 bubbleMin{ canvasScreenX + centerX - 8.0F,
                                    canvasScreenY + markerY - 6.5F };
            const ImVec2 bubbleMax{ canvasScreenX + centerX + 8.0F,
                                    canvasScreenY + markerY + 6.5F };
            drawList->AddRectFilled(bubbleMin,
                                    bubbleMax,
                                    markerHovered ? hoverColor : markerColor,
                                    3.0F);
            // 向下的小三角用于区分批注标记与普通矩形选框。
            const std::array<ImVec2, 3> tail{
                ImVec2{ canvasScreenX + centerX - 3.0F,
                        canvasScreenY + markerY + 6.0F },
                ImVec2{ canvasScreenX + centerX + 1.0F,
                        canvasScreenY + markerY + 10.0F },
                ImVec2{ canvasScreenX + centerX + 3.0F,
                        canvasScreenY + markerY + 6.0F },
            };
            drawList->AddTriangleFilled(
                tail[0],
                tail[1],
                tail[2],
                markerHovered ? hoverColor : markerColor);
            if ( marker.items.size() > 1U ) {
                // 同一时间戳聚合为一个气泡，仅在多条时显示数量。
                const std::string count = std::to_string(marker.items.size());
                const ImVec2      textSize = ImGui::CalcTextSize(count.c_str());
                drawList->AddText(
                    { canvasScreenX + centerX - textSize.x * 0.5F,
                      canvasScreenY + markerY - textSize.y * 0.5F },
                    textColor,
                    count.c_str());
            }
        }
        // 与上方 PushClipRect 成对，后续 tooltip 和 modal 不受画布裁剪。
        drawList->PopClipRect();
    }

    if ( hoveredMarker && !hoveredMarker->items.empty() ) {
        // 聚合标记使用首条批注 ID 作为稳定分组键；同一标记内切换详情
        // 不会错误触发整组重置。
        const std::string& markerId = hoveredMarker->items.front().id;
        bool               detailSelectionChanged = false;
        if ( markerId != m_annotationHoverMarkerId ) {
            // 进入新时间戳时优先采用详情卡精确命中的条目，否则从首条开始。
            m_annotationHoverMarkerId    = markerId;
            m_annotationHoverDetailIndex = hoveredDetailIndex.value_or(0U);
            detailSelectionChanged       = true;
        } else if ( hoveredDetailIndex ) {
            // 同一聚合组内移动到另一张卡片时同步当前详情索引。
            detailSelectionChanged =
                m_annotationHoverDetailIndex != *hoveredDetailIndex;
            m_annotationHoverDetailIndex = *hoveredDetailIndex;
        }
        m_annotationHoverDetailIndex = std::min(
            m_annotationHoverDetailIndex, hoveredMarker->items.size() - 1U);
        // 快照更新可能删掉末尾条目，钳制索引避免下一帧越界。

        if ( detailCardHovered ) {
            // 详情内容已直接显示在画布上，悬停时只提示编辑手势，避免再
            // 叠加一份完整 tooltip 遮挡相邻卡片。
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(TR("ui.annotation.detail_edit_hint").data());
            ImGui::EndTooltip();
        } else {
            // 气泡 tooltip 允许纵向滚动；宽度限制避免长 Markdown 占满画布。
            ImGui::SetNextWindowSizeConstraints(ImVec2(360.0F, 0.0F),
                                                ImVec2(620.0F, 720.0F));
            ImGui::BeginTooltip();
            // 切换条目后从顶部展示，不能继承上一条长文本的滚动位置。
            if ( detailSelectionChanged ) ImGui::SetScrollY(0.0F);
            const float tooltipMaxScrollY = ImGui::GetScrollMaxY();
            const float wheel             = ImGui::GetIO().MouseWheel;
            if ( !detailWheelConsumed && std::abs(wheel) > 0.01F ) {
                // 详情卡未消费滚轮时，tooltip 先尝试内部滚动；只有到达
                // 边界且未消费时，外层才可能把滚轮传给画布。
                const auto wheelResult =
                    updateAnnotationDetailWheel(wheel,
                                                ImGui::GetScrollY(),
                                                tooltipMaxScrollY,
                                                ImGui::GetFontSize() * 4.0F);
                if ( wheelResult.consumed ) {
                    ImGui::SetScrollY(wheelResult.scrollY);
                    detailWheelConsumed = true;
                }
            }
            if ( !detailCardHovered && hoveredMarker->items.size() > 1U &&
                 !ImGui::GetIO().WantTextInput ) {
                // 方向键只在无文本输入焦点时切换同时间戳条目，避免干扰
                // 输入法和批注编辑弹窗。
                int direction = 0;
                if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ) {
                    direction = -1;
                } else if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ) {
                    direction = 1;
                }
                if ( direction != 0 ) {
                    // step helper 在首尾循环，用户无需把鼠标移到其它气泡。
                    m_annotationHoverDetailIndex =
                        stepAnnotationDetailItem(hoveredMarker->items.size(),
                                                 m_annotationHoverDetailIndex,
                                                 direction);
                    ImGui::SetScrollY(0.0F);
                }
            }
            const auto timeText = MMM::UI::Utils::formatCanvasTime(
                hoveredMarker->timestamp, &currentSnapshot);
            // 标题同时展示格式化时间和聚合数量，先建立当前上下文。
            ImGui::Text("%s · %s · %zu",
                        TR("ui.annotation.marker_title").data(),
                        timeText.c_str(),
                        hoveredMarker->items.size());
            ImGui::Separator();
            for ( std::size_t index = 0U; index < hoveredMarker->items.size();
                  ++index ) {
                // 列表只显示作者和内容首行，完整 Markdown 在分隔线后渲染。
                const auto&            item = hoveredMarker->items[index];
                const std::string_view author =
                    item.author.empty()
                        ? TR("ui.annotation.unknown_author").view()
                        : std::string_view(item.author);
                // 当前选择使用高亮色，其余条目降低对比度但保持可读。
                const ImVec4 color = index == m_annotationHoverDetailIndex
                                         ? ImVec4(0.45F, 0.78F, 1.0F, 1.0F)
                                         : ImVec4(0.72F, 0.74F, 0.78F, 1.0F);
                ImGui::TextColored(color,
                                   "%zu. %.*s",
                                   index + 1U,
                                   static_cast<int>(author.size()),
                                   author.data());
                const auto firstLineEnd = item.content.find('\n');
                // string_view 直接引用快照字符串，不在每帧复制摘要文本。
                const std::string_view firstLine(
                    item.content.data(),
                    firstLineEnd == std::string::npos ? item.content.size()
                                                      : firstLineEnd);
                ImGui::SameLine();
                ImGui::TextWrapped("— %.*s",
                                   static_cast<int>(firstLine.size()),
                                   firstLine.data());
            }
            if ( hoveredMarker->items.size() > 1U ) {
                // 多条聚合优先提示切换方式；单条长文本才提示滚动。
                ImGui::TextDisabled("%s",
                                    TR("ui.annotation.wheel_hint").data());
            } else if ( tooltipMaxScrollY > 0.01F ) {
                ImGui::TextDisabled("%s",
                                    TR("ui.annotation.scroll_hint").data());
            }
            ImGui::Separator();
            const auto& detail =
                hoveredMarker->items[m_annotationHoverDetailIndex];
            // 目标种类通过翻译键显示，内部枚举不会泄漏到用户界面。
            const char* targetLabel =
                annotationTargetLabelKey(detail.targetKind);
            ImGui::Text("%s: %s",
                        TR("ui.annotation.target").data(),
                        TR(targetLabel).data());
            if ( detail.track >= 0 ) {
                // 轨道编号对用户从一开始，内部数据仍保持零基索引。
                ImGui::SameLine();
                ImGui::TextDisabled("#%d", detail.track + 1);
            }
            if ( detail.targetMissing ) {
                // 删除谱面对象不会删除历史批注，缺失状态需要显式警告。
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.32F, 1.0F),
                                   "%s",
                                   TR("ui.annotation.target_missing").data());
            }
            ImGui::Text("%s: %s",
                        TR("ui.annotation.author").data(),
                        detail.author.empty()
                            ? TR("ui.annotation.unknown_author").data()
                            : detail.author.c_str());
            ImGui::Separator();
            // Markdown 渲染器处理链接和富文本；其链接命中会在上层阻止
            // 编辑手势，避免点击链接同时打开编辑器。
            UI::renderMarkdown(detail.content);
            ImGui::EndTooltip();
        }

        if ( !currentSnapshot.isPlaying &&
             currentSnapshot.acceptsInteraction ) {
            // 播放中或只读会话禁止修改批注，但悬停和阅读仍然可用。
            if ( !detailLinkHovered &&
                 (detailCardHovered
                      ? ImGui::GetIO().KeyShift &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Right, false)
                      : ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) ) {
                // 详情卡用 Shift+右键编辑，气泡用左键编辑；两者区分是为了
                // 给卡片内部链接和普通右键新建手势留出空间。
                const auto& selected =
                    hoveredMarker->items[m_annotationHoverDetailIndex];
                m_annotationEditor.annotationId = selected.id;
                m_annotationEditor.author       = selected.author;
                m_annotationEditor.timestamp    = hoveredMarker->timestamp;
                m_annotationEditor.content.fill('\0');
                const auto& content = selected.content;
                // 编辑缓冲区固定容量，复制时始终为结尾 NUL 留出一个字节。
                std::copy_n(content.data(),
                            std::min(content.size(),
                                     m_annotationEditor.content.size() - 1U),
                            m_annotationEditor.content.data());
                m_annotationEditor.requestOpen = true;
            } else if ( !detailCardHovered && !detailLinkHovered &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right, false) ) {
                // 在已有时间戳气泡上右键表示新增同时间批注，不携带旧 ID。
                m_annotationEditor.annotationId.clear();
                m_annotationEditor.author.clear();
                m_annotationEditor.timestamp = hoveredMarker->timestamp;
                m_annotationEditor.content.fill('\0');
                m_annotationEditor.requestOpen = true;
            }
        }
    } else if ( gutterHovered && !currentSnapshot.isPlaying &&
                currentSnapshot.acceptsInteraction &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right, false) ) {
        // 空白批注栏右键以当前指针时间新建；启用吸附时使用吸附时间，
        // 从而与画布画笔的节奏定位一致。
        m_annotationEditor.annotationId.clear();
        m_annotationEditor.author.clear();
        const double hoveredTime     = currentSnapshot.isSnapped
                                           ? currentSnapshot.snappedTime
                                           : currentSnapshot.hoveredTime;
        m_annotationEditor.timestamp = std::max(0.0, hoveredTime);
        // 时间轴左侧的负时间只用于视觉预卷，批注持久化时间不得为负。
        m_annotationEditor.content.fill('\0');
        m_annotationEditor.requestOpen = true;
    } else if ( gutterHovered && !hoveredMarker ) {
        // 空白区域仅给出新建提示，不创建不可见的 ImGui 交互控件。
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(TR("ui.annotation.gutter_hint").data());
        ImGui::EndTooltip();
    }

    std::string popupLabel(TR("ui.annotation.editor_title").view());
    popupLabel += "###BeatmapAnnotationEditor";
    // 可见标题允许翻译变化，### 后固定 ID 保持 modal 状态稳定。
    if ( m_annotationEditor.requestOpen ) {
        // OpenPopup 必须在绘制线程执行，交互分支只设置延迟打开标志。
        ImGui::OpenPopup(popupLabel.c_str());
        m_annotationEditor.requestOpen = false;
    }
    if ( ImGui::BeginPopupModal(
             popupLabel.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        // annotationId 是编辑与新增的唯一判据；作者为空可能是历史数据，
        // 不能据此误判为新增。
        const bool editingExisting = !m_annotationEditor.annotationId.empty();
        const auto creator         = Config::normalizeCreatorIdentity(
            Config::AppConfig::instance().getEditorSettings().defaultCreator);
        // 新建使用当前默认创作者，编辑时界面展示原作者但提交仍由命令层
        // 按既有更新语义处理身份。
        const auto timeText = MMM::UI::Utils::formatCanvasTime(
            m_annotationEditor.timestamp, &currentSnapshot);
        ImGui::Text(
            "%s: %s", TR("ui.annotation.timestamp").data(), timeText.c_str());
        ImGui::Text(
            "%s: %s",
            TR("ui.annotation.author").data(),
            editingExisting
                ? (m_annotationEditor.author.empty()
                       ? TR("ui.annotation.unknown_author").data()
                       : m_annotationEditor.author.c_str())
                : (creator.empty() ? TR("ui.annotation.unknown_author").data()
                                   : creator.c_str()));
        if ( !editingExisting && creator.empty() ) {
            // 新批注必须具备可追踪作者；旧批注允许保留历史空作者。
            ImGui::TextColored(ImVec4(1.0F, 0.34F, 0.25F, 1.0F),
                               "%s",
                               TR("ui.annotation.creator_required").data());
        }
        ImGui::TextDisabled("%s", TR("ui.annotation.markdown_hint").data());
        ImGui::InputTextMultiline("##BeatmapAnnotationMarkdown",
                                  m_annotationEditor.content.data(),
                                  m_annotationEditor.content.size(),
                                  ImVec2(520.0F, 220.0F));

        // 空正文不能保存；新增还要求规范化后的创作者非空。
        const bool canSave = m_annotationEditor.content.front() != '\0' &&
                             (editingExisting || !creator.empty());
        ImGui::BeginDisabled(!canSave);
        if ( ::MMM::UI::FeedbackButton(editingExisting
                                           ? TR("ui.annotation.save").data()
                                           : TR("ui.annotation.add").data()) ) {
            // 新增和编辑统一走 upsert 命令；空 ID 由逻辑层生成新标识。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpsertBeatmapAnnotation{
                    .annotationId = m_annotationEditor.annotationId,
                    .targetKind = ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
                    .timestamp  = m_annotationEditor.timestamp,
                    .author     = creator,
                    .content    = m_annotationEditor.content.data(),
                }));
            // 命令进入事件队列后即可关闭弹窗，快照更新由逻辑线程回流。
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if ( editingExisting ) {
            // 删除入口只对已有 ID 显示，避免发送无目标删除命令。
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.annotation.delete").data()) ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdRemoveBeatmapAnnotation{
                        m_annotationEditor.annotationId }));
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        // 取消只关闭 UI，不修改编辑缓冲；下次打开会由入口重新初始化。
        if ( ::MMM::UI::FeedbackButton(TR("ui.annotation.cancel").data()) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    const bool annotationHovered = gutterHovered || detailCardHovered;
    const bool editorPopupOpen   = ImGui::IsPopupOpen(popupLabel.c_str());
    // 最终返回值把“阻止画布点击”和“滚轮是否透传”分开表达。
    // 弹窗始终阻止画布；悬停详情若已内部滚动，也不能再滚动画布。
    return {
        .blocksCanvas = annotationBlocksCanvas(
            gutterHovered, detailCardHovered, editorPopupOpen),
        .passesWheelToCanvas = shouldPassAnnotationWheelToCanvas(
            annotationHovered, editorPopupOpen, detailWheelConsumed),
        .wheelConsumed = detailWheelConsumed,
    };
}

/// @brief 仅同步后台主画布的鼠标悬停位置。
/// @param targetWidth 画布逻辑宽度。
/// @param targetHeight 画布逻辑高度。
/// @warning UI 热路径：后台可见画布每帧调用；只在指针状态变化时发布一条
/// 鼠标位置命令，禁止扩展为绘制、拾取或拖拽处理。
void Basic2DCanvasInteraction::updateHoverState(float targetWidth,
                                                float targetHeight)
{
    // ImGui 在窗口失焦或平台后端尚未提供坐标时会返回无效鼠标位置。
    // 这种情况下沿用最近一次局部坐标，只把悬停状态切为 false，避免把
    // FLT_MAX 一类哨兵值发送给逻辑线程。
    ImVec2     mousePos         = ImGui::GetMousePos();
    ImVec2     windowPos        = ImGui::GetCursorScreenPos();
    const bool hasValidMousePos = ImGui::IsMousePosValid(&mousePos) &&
                                  std::isfinite(mousePos.x) &&
                                  std::isfinite(mousePos.y);
    ImVec2 localMousePos{ 0.0F, 0.0F };
    if ( hasValidMousePos ) {
        // 逻辑命令使用当前画布窗口的局部坐标，而非桌面屏幕坐标。
        localMousePos = { mousePos.x - windowPos.x, mousePos.y - windowPos.y };
    } else if ( m_lastMouseCommand.valid ) {
        // 保留最后位置可让悬停退出只产生状态变化，不制造坐标跳跃。
        localMousePos = { m_lastMouseCommand.pos.x, m_lastMouseCommand.pos.y };
    }

    // 尺寸无效时画布必定不悬停，防止除零和零面积窗口误命中。
    const bool isInsideCanvas =
        hasValidMousePos && targetWidth > 0.0F && targetHeight > 0.0F &&
        localMousePos.x >= 0.0F && localMousePos.x <= targetWidth &&
        localMousePos.y >= 0.0F && localMousePos.y <= targetHeight;
    const bool isHovered = isInsideCanvas && ImGui::IsWindowHovered();

    // 后台标签页不处理实体拾取，但仍需把鼠标退出事件同步一次。
    // 坐标和尺寸变化小于阈值时省略命令，减少 UI/逻辑线程队列压力。
    constexpr float mouseEpsilon = 0.1F;
    const bool      shouldSendMouse =
        !m_lastMouseCommand.valid ||
        std::abs(m_lastMouseCommand.pos.x - localMousePos.x) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.pos.y - localMousePos.y) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportWidth - targetWidth) >
            mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportHeight - targetHeight) >
            mouseEpsilon ||
        m_lastMouseCommand.isHovering != isHovered ||
        m_lastMouseCommand.isDragging;
    if ( !shouldSendMouse ) return;

    // 后台同步永远不报告拖拽；活动画布的完整手势由 handleInteractions
    // 负责，两个入口不会争用逻辑层拖拽状态。
    Event::EventBus::instance().publish(Event::LogicCommandEvent(
        Logic::CmdSetMousePosition{ .cameraId       = m_cameraId,
                                    .mouseX         = localMousePos.x,
                                    .mouseY         = localMousePos.y,
                                    .viewportWidth  = targetWidth,
                                    .viewportHeight = targetHeight,
                                    .isHovering     = isHovered,
                                    .isDragging     = false }));
    // 缓存必须与实际发送内容一致，作为下一帧的去重基线。
    m_lastMouseCommand.valid          = true;
    m_lastMouseCommand.pos            = { localMousePos.x, localMousePos.y };
    m_lastMouseCommand.viewportWidth  = targetWidth;
    m_lastMouseCommand.viewportHeight = targetHeight;
    m_lastMouseCommand.isHovering     = isHovered;
    m_lastMouseCommand.isDragging     = false;
}

/// @brief 处理主画布鼠标悬停、点击、拖拽和滚轮交互。
///
/// @details 路由层次：
/// - 本函数只处理当前活动主画布的一帧输入。
/// - 后台画布只调用轻量 `updateHoverState`。
/// - 平台文件拖放和全局快捷键已由 `update` 提前处理。
/// - 中键平移拥有最高鼠标手势优先级。
/// - Layout 工具随后独占布局编辑输入。
/// - 批注栏可以阻止底层谱面输入。
/// - 对象音频预览控件可以阻止实体选择。
/// - 常规谱面工具只在以上层均未消费后运行。
/// - 滚轮最后处理，以便覆盖层先声明消费结果。
/// - 任一独占层取得手势所有权后立即返回。
/// - 同一物理输入不得发布两种互斥工具命令。
///
/// @details 鼠标同步：
/// - ImGui 屏幕坐标先转换为画布窗口局部坐标。
/// - 无效平台鼠标位置不会直接进入逻辑命令。
/// - 无效位置时沿用最近局部坐标并退出 hover。
/// - 几何范围和 ImGui 窗口 hover 分开判断。
/// - 窗口尺寸必须为正才可能命中画布。
/// - 鼠标位置、视口尺寸、hover 和 dragging 共同组成去重键。
/// - 小于 0.1 像素的纯坐标噪声不会重复发送。
/// - 状态布尔变化不受坐标阈值抑制。
/// - 缓存只在命令发布后更新。
/// - 中键平移期间逻辑层 dragging 强制为 false。
/// - UI 线程不把 ImGui 引用或指针发送给逻辑线程。
///
/// @details 中键平移：
/// - 中键可从被活动控件覆盖的画布区域开始。
/// - 按下时保存局部鼠标位置作为增量基线。
/// - 开始平移会结束已有框选事务。
/// - 开始平移会结束已有画笔事务。
/// - 开始平移会结束已有对象拖拽事务。
/// - 开始平移会结束已有右键擦除事务。
/// - 开始平移会保存并结束已有布局事务。
/// - 所有左键锁存和连续命令缓存随后清空。
/// - 平移每帧发送相邻鼠标位置差值。
/// - 小于阈值的静止噪声不会产生平移命令。
/// - CmdPanCanvas 同时携带视口尺寸和纵向渲染比例。
/// - 释放位置可以在画布外。
/// - 中键活动期间不执行其它交互分支。
///
/// @details 拖放与轨道范围：
/// - 项目资源面板的音频载荷可拖到 BGM 辅助轨。
/// - 只有已打开谱面且停止播放时注册投放目标。
/// - BGM 投影矩形先裁剪到当前画布范围。
/// - 退化或不可见的 BGM 区不注册目标。
/// - 自定义 ImGui 拖放矩形使用屏幕坐标。
/// - 创建命令仍使用画布局部鼠标坐标。
/// - 载荷类型、数据指针和结构大小都必须匹配。
/// - 只在拖放 Delivery 阶段发布创建命令。
/// - 资源 ID 复制后才进入异步事件队列。
/// - 空资源 ID 不发布命令。
/// - 当前 Ctrl 状态作为创建修饰语义一并发送。
/// - contentBounds 覆盖启用的玩家轨与辅助轨。
/// - seek 和工具按下还要求纵向位于 TrackLayout。
///
/// @details 批注与覆盖层：
/// - 批注详情卡、批注栏和编辑 modal 共享阻挡协议。
/// - 已开始的 Marquee 可以穿过批注栏继续。
/// - 其它新手势不能透过批注区域开始。
/// - 在批注区域释放的旧手势仍会正常结束。
/// - 批注阻挡时实体 hover 被清空。
/// - 批注内部滚轮消费优先于画布时间滚动。
/// - 批注允许透传时仍先尝试修饰键滚轮。
/// - 音频预览控件位于谱面对象之上。
/// - 控件和对象之间的桥接区维持控件可达性。
/// - 音频覆盖层阻挡时可以显示 tooltip，但不继续拾取。
/// - 覆盖层不直接修改谱面音频绑定。
///
/// @details 实体拾取：
/// - hitbox 按渲染逆序扫描，上层对象优先。
/// - interactionHitboxScale 只放大命中区域。
/// - 同一实体部件的多个矩形去重为一个候选。
/// - 候选键包含 entity、kind、part 和 subIndex。
/// - 候选签名变化时层索引重置为零。
/// - 候选减少时已有层索引钳制到有效范围。
/// - 方向键可循环选择重叠候选。
/// - 文本输入期间不抢占方向键。
/// - 只有当前层候选发送到逻辑 hover 状态。
/// - hover 命令按完整复合键去重。
/// - 空命中用 null entity 清除逻辑高亮。
/// - kind 在 null entity 时只是协议占位值。
///
/// @details 左键工具状态：
/// - 按下时锁存起点是否在画布、轨道和实体上。
/// - 锁存状态保证手势可在窗口外正确释放。
/// - 双击 seek 优先于普通左键工具开始。
/// - Draw 工具排除双击 seek，避免误跳转。
/// - Marquee 在实体上按下时先选择并转为对象拖拽。
/// - Marquee 在空白处按下时创建选择矩形。
/// - Move 在实体上按下时开始对象拖拽。
/// - Move 不隐式改变已有选择集合。
/// - Draw 在静止状态开始画笔事务。
/// - ColorBrush 和 ColorEraser 只作用于音符种类。
/// - 一次颜色笔划内同一实体只处理一次。
/// - 播放中仍可选择查看，但禁止开始修改事务。
///
/// @details 连续拖动：
/// - 每个工具使用独立连续命令去重缓存。
/// - 坐标、视觉时间、缩放和修饰键共同决定是否重发。
/// - Marquee 越过上下边缘时按 deltaTime 自动滚动。
/// - 自动滚动发布去除视觉偏移后的 Seek。
/// - 相机滚动后即使鼠标静止也要更新框选终点。
/// - 活动画笔在播放或滚动改变投影时重发落点。
/// - 对象拖拽在四周边缘触发二维自动平移。
/// - 横向自动平移受当前轨道投影范围约束。
/// - 先发布相机平移，再发布同帧对象位置更新。
/// - 播放位置变化会绕过纯坐标去重。
/// - Shift/Ctrl 改变也会触发连续命令更新。
/// - 连续更新不会在每帧单独创建撤销事务。
///
/// @details 释放与右键：
/// - 左键释放依据按下时锁存的事务种类发送结束命令。
/// - 框选、画笔和对象拖拽拥有不同结束命令。
/// - Move 空白单击可以清空当前选择。
/// - Move 的 Shift 单击跳转要求全程未拖动。
/// - seek 在编辑结束命令之后发布。
/// - 左键释放统一清除颜色集合和命令缓存。
/// - Draw 工具普通右键开启擦除事务。
/// - Ctrl+右键优先移除框选，不同时擦除。
/// - 擦除更新使用独立坐标与修饰键去重缓存。
/// - 右键在画布外释放仍结束擦除事务。
/// - 移除框选命令携带画布局部坐标。
///
/// @details 滚轮语义：
/// - 批注内容已消费的滚轮在此归零。
/// - Ctrl/Command/Alt 修饰键语义先于普通滚动。
/// - 修饰键滚轮可穿过活动 ImGui 控件获取窗口归属。
/// - 鼠标按键活动时不启用这种穿透。
/// - 未被修饰键处理的滚轮发布 CmdScroll。
/// - Shift 状态随普通滚动命令发送。
/// - 滚动改变活动画笔投影时立即补发 UpdateBrush。
/// - 本函数不使用固定等待或延迟最终输入。
/// @param currentSnapshot 当前渲染快照。
/// @param targetWidth 画布宽度。
/// @param targetHeight 画布高度。
/// @warning UI 热路径约束如下。
/// 热路径：每帧执行并可能推送逻辑命令；禁止加入文件系统访问、完整排序或阻塞操作。
void Basic2DCanvasInteraction::handleInteractions(
    const Common::Render::RenderSnapshot* currentSnapshot, float targetWidth,
    float targetHeight)
{
    // 本函数是活动主画布的统一输入路由器。处理顺序体现手势优先级：
    // 中键平移、布局工具、批注栏、音频预览覆盖层、常规谱面工具。
    // 前一层取得手势所有权后立即返回，后续层不得重复消费同一输入。
    ImVec2     mousePos         = ImGui::GetMousePos();
    ImVec2     windowPos        = ImGui::GetCursorScreenPos();
    const bool hasValidMousePos = ImGui::IsMousePosValid(&mousePos) &&
                                  std::isfinite(mousePos.x) &&
                                  std::isfinite(mousePos.y);
    ImVec2 localMousePos{ 0.0f, 0.0f };
    if ( hasValidMousePos ) {
        // 所有逻辑工具共享相对于画布内容原点的坐标系。
        localMousePos = { mousePos.x - windowPos.x, mousePos.y - windowPos.y };
    } else if ( m_lastMouseCommand.valid ) {
        // 平台光标暂时无效时沿用已发送坐标，仅改变悬停/拖拽状态。
        localMousePos = { m_lastMouseCommand.pos.x, m_lastMouseCommand.pos.y };
    }

    // isInsideCanvas 只检查几何范围，isHovered 还要求 ImGui 窗口可接收输入。
    const bool isInsideCanvas =
        hasValidMousePos && targetWidth > 0.0f && targetHeight > 0.0f &&
        localMousePos.x >= 0.0f && localMousePos.x <= targetWidth &&
        localMousePos.y >= 0.0f && localMousePos.y <= targetHeight;
    bool isHovered = isInsideCanvas && ImGui::IsWindowHovered();
    // 中键平移允许从被活动控件覆盖的画布区域开始，提供全局导航手势。
    const bool middlePanStartHovered =
        isInsideCanvas &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool middleClicked =
        middlePanStartHovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle, false);
    if ( middleClicked ) {
        // 记录局部起点，后续每帧发布增量而不是累计总位移。
        m_isMiddleCanvasPanning      = true;
        m_lastMiddlePanMousePosition = { localMousePos.x, localMousePos.y };
    }
    const bool isDragging = !m_isMiddleCanvasPanning && hasValidMousePos &&
                            ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    // 中键占用期间不得把左键拖拽状态继续报告给逻辑层。

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto& layout =
        visual.trackLayoutForKeyCount(currentSnapshot->trackCount);
    if ( currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying &&
         targetWidth > 0.0F && targetHeight > 0.0F ) {
        // 音频资源只能投放到 BGM 辅助轨道。播放中禁用投放，保证创建时间
        // 与当前静态指针投影一致。
        const auto projection = Logic::calculateCanvasLaneProjection(
            targetWidth,
            currentSnapshot->trackCount,
            currentSnapshot->bgmTrackCount,
            layout,
            currentSnapshot->canvasHorizontalOffsetX,
            true,
            currentSnapshot->bmsEditingEnabled,
            currentSnapshot->draftLanesEnabled,
            currentSnapshot->draftTrackCount,
            true);
        const float dropLeft =
            std::clamp(projection.bgmLeftX, 0.0F, targetWidth);
        const float dropRight =
            std::clamp(projection.bgmRightX, 0.0F, targetWidth);
        const float dropTop =
            std::clamp(layout.top * targetHeight, 0.0F, targetHeight);
        const float dropBottom =
            std::clamp(layout.bottom * targetHeight, 0.0F, targetHeight);
        // 将投影裁剪到当前视口；完全移出视口或退化的区域不注册目标。
        if ( projection.valid && dropRight > dropLeft &&
             dropBottom > dropTop ) {
            // BeginDragDropTargetCustom 需要屏幕坐标矩形，业务命令仍接收局部
            // 坐标以便逻辑层完成时间与轨道换算。
            const ImRect dropRect{
                { windowPos.x + dropLeft, windowPos.y + dropTop },
                { windowPos.x + dropRight, windowPos.y + dropBottom },
            };
            const ImGuiID dropTargetId =
                ImGui::GetID("##Basic2DCanvasAudioResourceDropTarget");
            // 固定内部 ID 避免 BGM 区域尺寸变化时丢失拖放状态。
            if ( ImGui::BeginDragDropTargetCustom(dropRect, dropTargetId) ) {
                if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                         Common::AUDIO_RESOURCE_DRAG_PAYLOAD_TYPE);
                     payload && payload->IsDelivery() && payload->Data &&
                     payload->DataSize ==
                         static_cast<int>(
                             sizeof(Common::AudioResourceDragPayload)) ) {
                    // 只在 Delivery 阶段创建对象；预览阶段仅由 ImGui 绘制反馈。
                    // DataSize 精确校验阻止把其它拖放载荷误解释为资源结构。
                    const auto& resourcePayload =
                        *static_cast<const Common::AudioResourceDragPayload*>(
                            payload->Data);
                    const std::string audioResourceId{
                        Common::audioResourceIdView(resourcePayload),
                    };
                    if ( !audioResourceId.empty() ) {
                        // 资源 ID 复制出 payload 生命周期后再进入异步事件队列。
                        // Ctrl 状态随命令快照发送，由逻辑层决定创建变体语义。
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdCreateAudioSample{
                                    .audioResourceId =
                                        std::move(audioResourceId),
                                    .cameraId   = m_cameraId,
                                    .mouseX     = localMousePos.x,
                                    .mouseY     = localMousePos.y,
                                    .isCtrlDown = ImGui::GetIO().KeyCtrl,
                                }));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    const auto laneProjection = Logic::calculateCanvasLaneProjection(
        targetWidth,
        currentSnapshot->trackCount,
        currentSnapshot->bgmTrackCount,
        layout,
        currentSnapshot->canvasHorizontalOffsetX,
        true,
        currentSnapshot->bmsEditingEnabled,
        currentSnapshot->draftLanesEnabled,
        currentSnapshot->draftTrackCount,
        true);
    // contentBounds 同时覆盖主轨道和已启用辅助区，用于限制横向交互范围。
    const auto  contentBounds = laneProjection.contentBounds();
    const float trackLeftX    = contentBounds.leftX;
    const float trackRightX   = contentBounds.rightX;
    const float normY =
        targetHeight > 0.0f ? localMousePos.y / targetHeight : 0.0f;
    // “轨道布局内”用于 seek 和工具操作，比单纯窗口悬停范围更严格。
    const bool isMouseInTrackLayout =
        isHovered && localMousePos.x >= trackLeftX &&
        localMousePos.x <= trackRightX && normY >= layout.top &&
        normY <= layout.bottom;
    const bool isLayoutEditing =
        Logic::EditorEngine::instance().getCurrentTool() ==
        Logic::EditTool::Layout;

    if ( !isLayoutEditing &&
         (m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
          m_horizontalRegionDragHandle != HorizontalRegionDragHandle::None ||
          m_noteScaleDragTarget.has_value() ||
          m_canvasComponentDragTarget.has_value() ||
          m_layoutConfigurationChanged) ) {
        // 通过快捷键切换工具时可能没有收到正常鼠标释放；退出布局模式必须
        // 主动收尾并保存已发生的配置修改。
        finishLayoutEditing();
    }

    // 鼠标命令是逻辑层拾取与画笔投影的公共输入，按坐标、视口和状态
    // 一起去重。拖拽布尔变化本身也必须触发一次发送。
    constexpr float mouseEpsilon = 0.1f;
    bool            shouldSendMouse =
        !m_lastMouseCommand.valid ||
        std::abs(m_lastMouseCommand.pos.x - localMousePos.x) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.pos.y - localMousePos.y) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportWidth - targetWidth) >
            mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportHeight - targetHeight) >
            mouseEpsilon ||
        m_lastMouseCommand.isHovering != isHovered ||
        m_lastMouseCommand.isDragging != isDragging;

    if ( shouldSendMouse ) {
        // 事件队列按 UI 帧产生最新输入；逻辑层不直接访问 ImGui 状态。
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ .cameraId       = m_cameraId,
                                        .mouseX         = localMousePos.x,
                                        .mouseY         = localMousePos.y,
                                        .viewportWidth  = targetWidth,
                                        .viewportHeight = targetHeight,
                                        .isHovering     = isHovered,
                                        .isDragging     = isDragging }));
        // 只有成功发布后更新基线，保持缓存代表逻辑线程可见的最后状态。
        m_lastMouseCommand.valid         = true;
        m_lastMouseCommand.pos           = { localMousePos.x, localMousePos.y };
        m_lastMouseCommand.viewportWidth = targetWidth;
        m_lastMouseCommand.viewportHeight = targetHeight;
        m_lastMouseCommand.isHovering     = isHovered;
        m_lastMouseCommand.isDragging     = isDragging;
    }

    if ( m_isMiddleCanvasPanning ) {
        // 中键平移拥有最高优先级。一旦开始，当前左键绘制、框选、拖拽和
        // 右键擦除都必须先结束，避免平移后留下未提交的编辑事务。
        if ( middleClicked ) {
            // 中键取得当前手势所有权前先结束已存在的左/右键编辑，避免工具状态悬空。
            if ( m_leftPressStartedOnCanvas && !m_leftPressStartedObjectDrag &&
                 currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                // 框选、画笔和对象拖拽分别有独立的结束命令。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
            } else if ( m_leftPressStartedOnCanvas &&
                        currentSnapshot->currentTool ==
                            Logic::EditTool::Draw ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
            } else if ( m_leftPressStartedObjectDrag ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
            }
            if ( m_rightEraseActive ) {
                // 擦除状态可能与左键工具并存，也需要单独终止。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
            }
            if ( m_trackLayoutDragHandle != TrackLayoutDragHandle::None ||
                 m_horizontalRegionDragHandle !=
                     HorizontalRegionDragHandle::None ||
                 m_noteScaleDragTarget.has_value() ||
                 m_canvasComponentDragTarget.has_value() ||
                 m_layoutConfigurationChanged ) {
                // 中键开始同样会中断布局拖动并保存最终配置。
                finishLayoutEditing();
            }

            // 清空本地手势锁存，后续中键帧不能触发任何左键更新。
            m_leftPressStartedOnCanvas      = false;
            m_leftPressStartedInTrackLayout = false;
            m_leftPressStartedOnEntity      = false;
            m_leftPressStartedObjectDrag    = false;
            m_leftPressDragged              = false;
            m_colorStrokeEntities.clear();
            resetContinuousEditCommands();
        }

        // ResizeAll 光标明确表示二维平移，不依赖当前编辑工具图标。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Middle) && hasValidMousePos ) {
            const glm::vec2 currentMousePosition{
                localMousePos.x,
                localMousePos.y,
            };
            const glm::vec2 delta =
                currentMousePosition - m_lastMiddlePanMousePosition;
            // 每次发送后推进基线，delta 始终表示单帧相对位移。
            m_lastMiddlePanMousePosition = currentMousePosition;
            if ( std::abs(delta.x) > 0.001F || std::abs(delta.y) > 0.001F ) {
                // 微小阈值过滤触摸板或高 DPI 鼠标静止噪声。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdPanCanvas{
                        .cameraId       = m_cameraId,
                        .deltaX         = delta.x,
                        .deltaY         = delta.y,
                        .viewportWidth  = targetWidth,
                        .viewportHeight = targetHeight,
                        .renderScaleY   = currentSnapshot->renderScaleY,
                    }));
            }
        }

        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Middle) ) {
            // 即使在画布外释放也结束平移；状态不依赖窗口 hover。
            m_isMiddleCanvasPanning = false;
        }
        // 中键手势期间不再进入布局、批注或谱面对象交互。
        return;
    }

    if ( isLayoutEditing ) {
        // 布局工具编辑视觉配置而非谱面实体，先清除逻辑层实体悬停状态。
        if ( !m_hasLastHovered || m_lastHoveredEntity != entt::null ||
             m_lastHoveredPart != 0 || m_lastHoveredSubIndex != -1 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetHoveredEntity{ entt::null, 0, -1 }));
            m_hasLastHovered        = true;
            m_lastHoveredEntity     = entt::null;
            m_lastHoveredObjectKind = Logic::ChartObjectKind::PlayerNote;
            m_lastHoveredPart       = 0;
            m_lastHoveredSubIndex   = -1;
        }

        // 切入布局模式时清除普通工具手势状态，避免切回后继续旧拖拽。
        m_leftPressStartedOnCanvas      = false;
        m_leftPressStartedInTrackLayout = false;
        m_leftPressStartedOnEntity      = false;
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
        // 布局处理器独占当前帧输入并负责绘制编辑遮罩、句柄与参考线。
        handleLayoutEditing(localMousePos.x,
                            localMousePos.y,
                            windowPos.x,
                            windowPos.y,
                            targetWidth,
                            targetHeight,
                            isHovered,
                            *currentSnapshot);
        return;
    }

    const auto annotationGutterInteraction =
        renderAnnotationGutter(*currentSnapshot,
                               windowPos.x,
                               windowPos.y,
                               targetWidth,
                               targetHeight,
                               localMousePos.x,
                               localMousePos.y,
                               isHovered);
    // 已经开始的框选允许穿过批注栏继续更新，否则用户从轨道拖到辅助区
    // 会被中途截断；其它手势仍遵守批注栏阻挡规则。
    const bool continueMarqueeAcrossAnnotation =
        shouldContinueMarqueeAcrossBlockedArea(
            currentSnapshot->currentTool,
            ImGui::IsMouseDragging(ImGuiMouseButton_Left),
            m_leftPressStartedOnCanvas,
            m_leftPressStartedOnEntity,
            m_leftPressStartedObjectDrag);
    if ( annotationGutterInteraction.blocksCanvas &&
         !continueMarqueeAcrossAnnotation ) {
        // 批注层接管输入时清除谱面实体 hover，避免底层对象残留高亮。
        if ( !m_hasLastHovered || m_lastHoveredEntity != entt::null ||
             m_lastHoveredPart != 0 || m_lastHoveredSubIndex != -1 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetHoveredEntity{ entt::null, 0, -1 }));
            m_hasLastHovered        = true;
            m_lastHoveredEntity     = entt::null;
            m_lastHoveredObjectKind = Logic::ChartObjectKind::PlayerNote;
            m_lastHoveredPart       = 0;
            m_lastHoveredSubIndex   = -1;
        }

        // 手势可能从画布开始、在批注栏上释放。即使批注层阻挡新输入，
        // 也必须把已有事务的结束命令送到逻辑线程。
        const auto completion = resolveBlockedCanvasGestureCompletion(
            currentSnapshot->currentTool,
            ImGui::IsMouseReleased(ImGuiMouseButton_Left),
            ImGui::IsMouseReleased(ImGuiMouseButton_Right),
            m_leftPressStartedOnCanvas,
            m_leftPressStartedObjectDrag,
            m_rightEraseActive);
        switch ( completion.leftEnd ) {
        case BlockedCanvasLeftGestureEnd::Marquee:
            // 完成框选会提交当前矩形选择结果。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
            break;
        case BlockedCanvasLeftGestureEnd::Brush:
            // 完成画笔会结束当前连续创建事务。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
            break;
        case BlockedCanvasLeftGestureEnd::ObjectDrag:
            // 完成对象拖拽会提交最终位置并形成撤销记录。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
            break;
        case BlockedCanvasLeftGestureEnd::None: break;
        }
        if ( completion.endErase ) {
            // 右键也可能在批注区释放，必须清理逻辑和本地擦除状态。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
            m_lastEraseUpdateCommand.valid = false;
            m_rightEraseActive             = false;
        }
        if ( completion.clearLeftState ) {
            // 本地锁存与连续命令缓存同步清空，下一次按下重新建立基线。
            m_leftPressStartedOnCanvas      = false;
            m_leftPressStartedInTrackLayout = false;
            m_leftPressStartedOnEntity      = false;
            m_leftPressStartedObjectDrag    = false;
            m_leftPressDragged              = false;
            m_colorStrokeEntities.clear();
            m_lastMarqueeUpdateCommand.valid = false;
            m_lastBrushUpdateCommand.valid   = false;
            m_lastMoveUpdateCommand.valid    = false;
        }
        if ( annotationGutterInteraction.passesWheelToCanvas ) {
            // 批注详情没有消费滚轮且弹窗未打开时，才允许画布滚动。
            const auto& io = ImGui::GetIO();
            if ( std::abs(io.MouseWheel) > 0.01F &&
                 !handleModifierWheel(currentSnapshot) ) {
                // 修饰键滚轮优先用于属性调整，普通滚轮再映射时间滚动。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdScroll{
                        m_cameraId, -io.MouseWheel, io.KeyShift }));
            }
        }
        // 批注层阻挡时不执行音频覆盖层、拾取或常规工具处理。
        return;
    }

    const bool audioPreviewOverlayBlocksCanvas =
        renderObjectAudioPreviewControls(*currentSnapshot,
                                         windowPos.x,
                                         windowPos.y,
                                         targetWidth,
                                         targetHeight,
                                         localMousePos.x,
                                         localMousePos.y);
    // 音频预览按钮位于谱面对象上方，按钮命中时必须阻止底层选择和拖动。

    // 精确时间提示只在静止、可交互且未被音频按钮覆盖时显示。
    if ( MMM::UI::Utils::shouldShowCanvasHoverInspection(
             currentSnapshot->hasBeatmap,
             audioPreviewOverlayBlocksCanvas,
             isHovered,
             currentSnapshot->isHoveringCanvas,
             currentSnapshot->isPlaying) ) {
        if ( isMouseInTrackLayout ) {
            // Move/Marquee 只在存在吸附或对象检查内容时显示；绘制类工具
            // 始终需要时间与拍点反馈。
            bool isEditTool =
                (currentSnapshot->currentTool != Logic::EditTool::Move &&
                 currentSnapshot->currentTool != Logic::EditTool::Marquee);

            if ( currentSnapshot->isSnapped || isEditTool ||
                 currentSnapshot->hoverInspect.show ) {
                // 使用紧凑 padding 和 spacing，避免详细对象信息遮挡过多轨道。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(CANVAS_HOVER_OVERLAY_PADDING,
                                           CANVAS_HOVER_OVERLAY_PADDING));
                ImGui::PushStyleVar(
                    ImGuiStyleVar_ItemSpacing,
                    ImVec2(CANVAS_HOVER_OVERLAY_ITEM_SPACING_X,
                           CANVAS_HOVER_OVERLAY_ITEM_SPACING_Y));

                const bool showHoverOverlay = beginCanvasHoverOverlay(mousePos);
                if ( showHoverOverlay ) {
                    if ( currentSnapshot->hoverInspect.show ) {
                        // hoverInspect 由逻辑快照提供，UI 只负责格式化，不在此
                        // 重新解释谱面实体组件。
                        const auto& inspect = currentSnapshot->hoverInspect;
                        auto        drawPoint =
                            [currentSnapshot](
                                const char*                           labelKey,
                                const Common::Render::HoverBeatPoint& point,
                                bool showTrack) {
                                // 一个检查对象可能只有头、体或尾中的部分点有效。
                                if ( !point.show ) return;
                                // 拍号分数保留逻辑层给出的既约表示，与吸附算法
                                // 使用的节奏位置保持一致。
                                const auto label = TR(labelKey);
                                ImGui::TextColored(
                                    ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                    "%s %s: %d + %d/%d",
                                    label.data(),
                                    TR("ui.canvas.note_fraction").data(),
                                    point.beatIndex,
                                    point.numerator,
                                    point.denominator);
                                const auto timeText =
                                    MMM::UI::Utils::formatCanvasTime(
                                        point.time, currentSnapshot);
                                // 时间格式沿用当前谱面显示精度和偏移配置。
                                ImGui::TextColored(
                                    ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                    "%s %s: %s",
                                    label.data(),
                                    TR("ui.canvas.note_time").data(),
                                    timeText.c_str());
                                if ( showTrack ) {
                                    // 轨道标签同时理解主轨、草稿轨和越界状态。
                                    const ImVec4 color(0.5f, 1.0f, 0.5f, 1.0f);
                                    renderHoverTrack(
                                        label.data(),
                                        point.track,
                                        currentSnapshot->trackCount,
                                        currentSnapshot->draftTrackCount,
                                        &color);
                                }
                            };

                        switch ( inspect.kind ) {
                        // 普通音符只有一个头点。
                        case Common::Render::HoverInspectKind::Note:
                            drawPoint("ui.canvas.hover.note",
                                      inspect.head,
                                      inspect.showTrack);
                            break;
                        // 长条头尾分别使用独立标签，便于核对持续时间。
                        case Common::Render::HoverInspectKind::HoldHead:
                            drawPoint(
                                "ui.canvas.hover.head", inspect.head, true);
                            break;
                        case Common::Render::HoverInspectKind::HoldEnd:
                        case Common::Render::HoverInspectKind::PolylineHoldEnd:
                            drawPoint(
                                "ui.canvas.hover.hold_end", inspect.end, true);
                            break;
                        // 滑键的头、体、尾可能位于不同轨道；体节点不重复
                        // 展示轨道，除非快照明确要求。
                        case Common::Render::HoverInspectKind::FlickHead:
                            drawPoint("ui.canvas.hover.flick_head",
                                      inspect.head,
                                      true);
                            break;
                        case Common::Render::HoverInspectKind::FlickBody:
                        case Common::Render::HoverInspectKind::
                            PolylineFlickBody:
                            drawPoint("ui.canvas.hover.flick_body",
                                      inspect.body,
                                      false);
                            break;
                        case Common::Render::HoverInspectKind::FlickEnd:
                        case Common::Render::HoverInspectKind::PolylineFlickEnd:
                            drawPoint(
                                "ui.canvas.hover.flick_end", inspect.end, true);
                            break;
                        case Common::Render::HoverInspectKind::PolylineHead:
                            // 折线头与折线节点都存放在 body 槽位，由 kind
                            // 区分。
                            drawPoint("ui.canvas.hover.polyline_head",
                                      inspect.body,
                                      inspect.showTrack);
                            break;
                        case Common::Render::HoverInspectKind::PolylineNode:
                            drawPoint("ui.canvas.hover.polyline_node",
                                      inspect.body,
                                      inspect.showTrack);
                            break;
                        case Common::Render::HoverInspectKind::
                            AudioSampleAnchor:
                        case Common::Render::HoverInspectKind::
                            AudioSampleTrigger:
                            drawPoint("ui.canvas.hover.sample_anchor",
                                      inspect.head,
                                      inspect.showTrack);
                            drawPoint("ui.canvas.hover.sample_trigger",
                                      inspect.end,
                                      inspect.showTrack);
                            break;
                        // HoldBody 只在下方补充轨道，不重复绘制拍点。
                        case Common::Render::HoverInspectKind::HoldBody:
                        case Common::Render::HoverInspectKind::PolylineHoldBody:
                        case Common::Render::HoverInspectKind::None: break;
                        }

                        if ( inspect.showDuration ) {
                            // 持续时长使用相对时间格式，不叠加谱面视觉偏移。
                            const auto durationText =
                                MMM::UI::Utils::formatCanvasDuration(
                                    inspect.duration);
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %s",
                                TR("ui.canvas.hover.duration").data(),
                                durationText.c_str());
                        }
                        if ( inspect.showDtrack ) {
                            // dtrack 是格式原始属性，保留带符号整数语义。
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %d",
                                TR("ui.canvas.hover.dtrack").data(),
                                inspect.dtrack);
                        }
                        if ( inspect.showTrack &&
                             (inspect.kind ==
                                  Common::Render::HoverInspectKind::HoldBody ||
                              inspect.kind == Common::Render::HoverInspectKind::
                                                  PolylineHoldBody) ) {
                            // 长条主体没有独立拍点，但仍需要展示所在轨道。
                            const ImVec4 color(0.5f, 1.0f, 0.5f, 1.0f);
                            renderHoverTrack(nullptr,
                                             inspect.track,
                                             currentSnapshot->trackCount,
                                             currentSnapshot->draftTrackCount,
                                             &color);
                        }
                        if ( inspect.showAudioPreview ) {
                            // 音频资源 ID 可能较长，使用 TextWrapped
                            // 保持浮层宽度。
                            ImGui::TextWrapped(
                                "%s: %s",
                                TR("ui.canvas.hover.audio_resource").data(),
                                inspect.audioResourceId.c_str());
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %.1f%%",
                                TR("ui.canvas.hover.volume").data(),
                                inspect.volume * 100.0F);
                        }
                        if ( inspect.showAudioSample ) {
                            // 采样偏移带正负号，明确表示相对触发点的方向。
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %+lld ms",
                                TR("ui.canvas.hover.offset").data(),
                                static_cast<long long>(inspect.offsetMs));
                        }

                        // 重叠数量沿用渲染遮罩的检测结果，避免回退到旧的包围盒计数。
                        // inspect.overlapCount
                        // 是当前候选的基础计数，遮罩可能包含
                        // 更完整的几何重叠集合，二者取最大值。
                        int overlappingCount =
                            std::max(1, inspect.overlapCount);
                        for ( const auto& mask :
                              currentSnapshot->overlapMasks ) {
                            // 遮罩命中使用画布局部坐标，与渲染生成的矩形一致。
                            if ( localMousePos.x >= mask.x &&
                                 localMousePos.x <= mask.x + mask.w &&
                                 localMousePos.y >= mask.y &&
                                 localMousePos.y <= mask.y + mask.h ) {
                                overlappingCount = std::max(overlappingCount,
                                                            mask.objectCount);
                            }
                        }

                        if ( overlappingCount > 1 ) {
                            // 多个对象重叠时突出警告，并提示可切换 hover 层。
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                                "%s: %d%s",
                                TR("ui.canvas.overlapping_hitboxes").data(),
                                overlappingCount,
                                TR("ui.canvas.overlapping_warning").data());
                        } else {
                            // 单对象仍显示计数，便于确认拾取与视觉一致。
                            ImGui::TextColored(
                                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                "%s: %d",
                                TR("ui.canvas.overlapping_hitboxes").data(),
                                overlappingCount);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                    }

                    if ( currentSnapshot->isSnapped ) {
                        // 吸附时间优先于连续
                        // hoveredTime，颜色用于提示将要落点。
                        const auto timeText = MMM::UI::Utils::formatCanvasTime(
                            currentSnapshot->snappedTime, currentSnapshot);
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                           "%s: %s",
                                           TR("ui.canvas.snap").data(),
                                           timeText.c_str());

                        if ( currentSnapshot->snappedNumerator == 1 &&
                             currentSnapshot->snappedDenominator == 1 ) {
                            // 整拍单独使用高对比色，快速区分 1/1 与细分拍。
                            ImGui::TextColored(
                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                "%s (1/1)",
                                TR("ui.canvas.beat_fraction").data());
                        } else {
                            ImGui::TextColored(
                                ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                                "%s (%d/%d)",
                                TR("ui.canvas.beat_fraction").data(),
                                currentSnapshot->snappedNumerator,
                                currentSnapshot->snappedDenominator);
                        }
                    } else {
                        // 未吸附时显示连续时间，不伪造拍号分数。
                        const auto timeText = MMM::UI::Utils::formatCanvasTime(
                            currentSnapshot->hoveredTime, currentSnapshot);
                        ImGui::Text("%s: %s",
                                    TR("ui.canvas.time").data(),
                                    timeText.c_str());
                    }

                    if ( currentSnapshot->hoveredBeatIndex > 0 ) {
                        // 无有效节拍时索引为非正值，此时隐藏字段。
                        ImGui::Text("%s: %d",
                                    TR("ui.canvas.beat_index").data(),
                                    currentSnapshot->hoveredBeatIndex);
                    }

                    renderHoverTrack(nullptr,
                                     currentSnapshot->hoveredTrack,
                                     currentSnapshot->trackCount,
                                     currentSnapshot->draftTrackCount);
                    // 当前轨道展示与对象详情中的轨道格式完全一致。

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                       "%s: %d",
                                       TR("ui.canvas.beat_divisor").data(),
                                       currentSnapshot->currentBeatDivisor);
                    // 分拍数说明当前画笔/吸附粒度，不代表谱面的拍号分母。
                    if ( m_hoverLayerCount > 1 ) {
                        // 只有实际存在重叠候选时显示层序和切换提示。
                        ImGui::TextColored(
                            ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                            "%s: %d/%d  %s",
                            TR("ui.canvas.hover.layer").data(),
                            m_hoverLayerIndex + 1,
                            m_hoverLayerCount,
                            TR("ui.canvas.hover.layer_hint").data());
                    }
                }

                ImGui::End();
                // 无论窗口是否被 Begin 折叠，都必须恢复两项样式。
                ImGui::PopStyleVar(2);
            }
        }
    }

    if ( audioPreviewOverlayBlocksCanvas ) {
        // 覆盖层已经消费输入；tooltip 可以显示，但不得继续实体拾取。
        return;
    }

    // 默认无实体命中。kind 保留 PlayerNote 只是命令协议的占位值，entity
    // 为 null 时逻辑层不会据此访问对象。
    entt::entity           hoveredEntity = entt::null;
    Logic::ChartObjectKind hoveredObjectKind{
        Logic::ChartObjectKind::PlayerNote
    };
    uint8_t hoveredPart     = 0;
    int     hoveredSubIndex = -1;

    std::vector<HoverLayerCandidate> candidates;
    std::string                      layerSignature;
    if ( isHovered ) {
        // hitbox 按渲染逆序扫描，使后绘制对象优先成为第一候选。
        for ( auto it = currentSnapshot->hitboxes.rbegin();
              it != currentSnapshot->hitboxes.rend();
              ++it ) {
            const auto hitbox = Common::Render::scaleInteractionHitbox(
                *it,
                currentSnapshot->interactionHitboxScaleX,
                currentSnapshot->interactionHitboxScaleY);
            // 交互缩放只扩展命中区域，不改变渲染几何和候选标识。
            if ( localMousePos.x >= hitbox.x &&
                 localMousePos.x <= hitbox.x + hitbox.w &&
                 localMousePos.y >= hitbox.y &&
                 localMousePos.y <= hitbox.y + hitbox.h ) {
                const bool appended = appendHoverLayerCandidate(
                    candidates,
                    { hitbox.entity,
                      hitbox.kind,
                      static_cast<std::uint8_t>(hitbox.part),
                      hitbox.subIndex });
                // 同一实体部件可能贡献多个相交矩形，只保留一个层候选。
                if ( !appended ) continue;
                // 签名包含实体、类型、部件和子索引；集合变化时重置层选择。
                layerSignature +=
                    std::to_string(static_cast<uint32_t>(
                        entt::to_integral(hitbox.entity))) +
                    ":" + std::to_string(static_cast<uint32_t>(hitbox.kind)) +
                    ":" + std::to_string(static_cast<uint32_t>(hitbox.part)) +
                    ":" + std::to_string(hitbox.subIndex) + ";";
            }
        }
    }

    if ( layerSignature != m_hoverLayerSignature ) {
        // 候选集合或顺序变化后回到最上层，避免旧索引指向另一对象。
        m_hoverLayerSignature = layerSignature;
        m_hoverLayerIndex     = 0;
    }

    m_hoverLayerCount = static_cast<int>(candidates.size());
    if ( candidates.empty() ) {
        // 空集合归零索引，保持 tooltip 展示条件简单。
        m_hoverLayerIndex = 0;
    } else {
        if ( m_hoverLayerIndex >= m_hoverLayerCount ) {
            // 快照更新减少候选时钳制已有索引。
            m_hoverLayerIndex = m_hoverLayerCount - 1;
        }

        if ( m_hoverLayerCount > 1 && isHovered &&
             !ImGui::GetIO().WantTextInput ) {
            // 方向键循环选择重叠层；文本输入期间不抢占光标移动按键。
            if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ) {
                m_hoverLayerIndex = (m_hoverLayerIndex + 1) % m_hoverLayerCount;
            } else if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ) {
                m_hoverLayerIndex =
                    (m_hoverLayerIndex + m_hoverLayerCount - 1) %
                    m_hoverLayerCount;
            }
        }

        const auto& candidate = candidates[m_hoverLayerIndex];
        // 只有选定候选进入逻辑 hover 状态，其余候选仅用于层计数。
        hoveredEntity     = candidate.entity;
        hoveredObjectKind = candidate.kind;
        hoveredPart       = candidate.part;
        hoveredSubIndex   = candidate.subIndex;
    }

    bool shouldSendHover = !m_hasLastHovered ||
                           m_lastHoveredEntity != hoveredEntity ||
                           m_lastHoveredObjectKind != hoveredObjectKind ||
                           m_lastHoveredPart != hoveredPart ||
                           m_lastHoveredSubIndex != hoveredSubIndex;
    if ( shouldSendHover ) {
        // 完整复合键去重，子节点切换也会产生 hover 更新。
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetHoveredEntity{ hoveredEntity,
                                        hoveredPart,
                                        hoveredSubIndex,
                                        hoveredObjectKind }));
        m_hasLastHovered        = true;
        m_lastHoveredEntity     = hoveredEntity;
        m_lastHoveredObjectKind = hoveredObjectKind;
        m_lastHoveredPart       = hoveredPart;
        m_lastHoveredSubIndex   = hoveredSubIndex;
    }

    auto processColorToolTarget = [&](Logic::EditTool tool) {
        // 调色工具只接受玩家音符和草稿音符；播放中禁止修改，空命中直接
        // 返回，避免发送无目标命令。
        if ( currentSnapshot->isPlaying || hoveredEntity == entt::null ||
             (hoveredObjectKind != Logic::ChartObjectKind::PlayerNote &&
              hoveredObjectKind != Logic::ChartObjectKind::DraftNote) ) {
            return;
        }
        // 一次按下拖动期间每个实体只处理一次，重叠 hitbox 或鼠标抖动
        // 不会对同一对象反复建立撤销记录。
        if ( !m_colorStrokeEntities.insert(hoveredEntity).second ) return;

        if ( tool == Logic::EditTool::ColorBrush ) {
            // 颜色画笔把当前调色板覆盖应用到整个实体。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdApplyBrushPaletteToEntity{ hoveredEntity }));
        } else if ( tool == Logic::EditTool::ColorEraser ) {
            // 颜色橡皮只清除颜色覆盖，不删除谱面对象。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdClearNoteColorOverrides{ hoveredEntity }));
        }
    };

    const bool leftClicked =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
    const bool leftDoubleClicked =
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool leftSeekClicked =
        isMouseInTrackLayout && currentSnapshot->isHoveringCanvas &&
        currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying &&
        currentSnapshot->currentTool != Logic::EditTool::Draw &&
        leftDoubleClicked;
    // 双击 seek 排除 Draw 工具，防止双击创建物件时意外跳转播放头。

    if ( leftSeekClicked ) {
        // seek 不属于连续编辑手势，先清空可能由第一次点击建立的锁存状态。
        m_leftPressStartedOnCanvas      = false;
        m_leftPressStartedInTrackLayout = false;
        m_leftPressStartedOnEntity      = false;
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
        // 逻辑快照已经计算好指针时间和视觉偏移，统一由 helper 发布。
        publishCanvasHoverSeek(*currentSnapshot);
    } else if ( leftClicked ) {
        // 按下时冻结“起点是否在画布/轨道/实体上”，后续即使指针移出
        // 窗口也依据这些锁存完成手势。
        m_leftPressStartedOnCanvas      = isHovered;
        m_leftPressStartedInTrackLayout = isMouseInTrackLayout;
        m_leftPressStartedOnEntity      = hoveredEntity != entt::null;
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();

        if ( isHovered ) {
            if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                // 框选工具在实体上按下时先选择并转为对象拖拽；空白处才开始
                // 新框选矩形。
                if ( hoveredEntity != entt::null ) {
                    // 未按 Ctrl 时替换选择，按 Ctrl 时保留既有选择集合。
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdSelectEntity{ hoveredEntity,
                                                    !ImGui::GetIO().KeyCtrl,
                                                    hoveredObjectKind }));
                    if ( !currentSnapshot->isPlaying ) {
                        // 播放时仍允许选择查看，但不允许改变对象位置。
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdStartDrag{ hoveredEntity,
                                                     m_cameraId,
                                                     ImGui::GetIO().KeyCtrl,
                                                     hoveredObjectKind }));
                        m_leftPressStartedObjectDrag = true;
                    }
                } else {
                    // 框选起点使用当前画布局部坐标，逻辑层维护矩形终点。
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartMarquee{ m_cameraId,
                                                    localMousePos.x,
                                                    localMousePos.y,
                                                    ImGui::GetIO().KeyCtrl }));
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::Move ) {
                // Move 工具只在实体命中且停止播放时开启拖拽事务。
                if ( !currentSnapshot->isPlaying &&
                     hoveredEntity != entt::null ) {
                    // 抓取工具不负责选中，保留用户已有的多选集合。
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartDrag{ hoveredEntity,
                                                 m_cameraId,
                                                 ImGui::GetIO().KeyCtrl,
                                                 hoveredObjectKind }));
                    m_leftPressStartedObjectDrag = true;
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::Draw ) {
                if ( !currentSnapshot->isPlaying ) {
                    // 画笔起点连同 Shift/Ctrl 状态一次性发送，逻辑层决定
                    // 物件种类、吸附与复合绘制模式。
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartBrush{ m_cameraId,
                                                  localMousePos.x,
                                                  localMousePos.y,
                                                  ImGui::GetIO().KeyShift,
                                                  ImGui::GetIO().KeyCtrl }));
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::ColorBrush ) {
                // 调色工具在按下首帧立即处理命中对象。
                processColorToolTarget(Logic::EditTool::ColorBrush);
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::ColorEraser ) {
                processColorToolTarget(Logic::EditTool::ColorEraser);
            }
        }
    }

    if ( ImGui::IsMouseDragging(0) ) {
        // 记录本次按下是否形成过拖动，用于释放时区分单击和拖拽。
        m_leftPressDragged = true;

        if ( m_leftPressStartedOnCanvas && !m_leftPressStartedObjectDrag &&
             currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            // 框选可以越过视口上下边缘自动滚动；逻辑矩形仍以当前指针
            // 局部坐标更新，保证选择范围连续。
            bool autoScrolled = false;
            if ( currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying ) {
                const double autoScrollTargetTime =
                    marqueeAutoScrollTargetTime(*currentSnapshot,
                                                targetHeight,
                                                localMousePos.y,
                                                ImGui::GetIO().DeltaTime,
                                                ImGui::GetIO().KeyShift,
                                                autoScrolled);
                if ( autoScrolled ) {
                    // CmdSeek 接收音频时间，需移除仅用于视觉显示的偏移量。
                    const double visualOffset = Config::AppConfig::instance()
                                                    .getVisualConfig()
                                                    .getEffectiveVisualOffset();
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdSeek{
                            autoScrollTargetTime - visualOffset }));
                }
            }

            const bool playbackScrolled = currentSnapshot->hasBeatmap &&
                                          currentSnapshot->isPlaying &&
                                          currentSnapshot->isSelecting;
            // 正常静止输入按复合状态去重；自动滚动或播放会改变世界投影，
            // 即使鼠标未动也必须重发框选终点。
            const bool shouldUpdateMarquee =
                shouldSendContinuousEditCommand(
                    m_lastMarqueeUpdateCommand,
                    { localMousePos.x, localMousePos.y },
                    *currentSnapshot,
                    ImGui::GetIO().KeyCtrl,
                    false) ||
                autoScrolled || playbackScrolled;
            if ( shouldUpdateMarquee ) {
                // UpdateMarquee 只更新当前事务，不在每帧创建撤销节点。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUpdateMarquee{
                        localMousePos.x, localMousePos.y }));
            }
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool == Logic::EditTool::Draw &&
                    (!currentSnapshot->isPlaying ||
                     currentSnapshot->brush.isActive) ) {
            // 已经启动的画笔事务在播放状态变化后仍允许完成，避免半途丢失。
            const bool playbackScrolled = currentSnapshot->hasBeatmap &&
                                          currentSnapshot->isPlaying &&
                                          currentSnapshot->brush.isActive;
            if ( shouldSendContinuousEditCommand(
                     m_lastBrushUpdateCommand,
                     { localMousePos.x, localMousePos.y },
                     *currentSnapshot,
                     ImGui::GetIO().KeyShift,
                     ImGui::GetIO().KeyCtrl) ||
                 playbackScrolled ) {
                // Shift/Ctrl 属于连续命令去重键，拖动中切换修饰键会立即更新。
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateBrush{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           ImGui::GetIO().KeyShift,
                                           ImGui::GetIO().KeyCtrl }));
            }
        } else if ( m_leftPressStartedObjectDrag &&
                    (currentSnapshot->currentTool == Logic::EditTool::Move ||
                     currentSnapshot->currentTool ==
                         Logic::EditTool::Marquee) ) {
            // 对象拖拽在边缘触发二维自动平移，灵敏度来自预览交互配置。
            glm::vec2 autoPanDelta =
                currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying
                    ? objectDragAutoPanDelta(
                          { localMousePos.x, localMousePos.y },
                          targetWidth,
                          targetHeight,
                          ImGui::GetIO().DeltaTime,
                          std::max(0.0F,
                                   visual.previewConfig.edgeScrollSensitivity))
                    : glm::vec2{ 0.0F, 0.0F };
            // 横向自动平移还要受当前辅助轨道内容范围约束，避免把谱面
            // 完全拖出可交互区域。
            autoPanDelta.x = clampObjectDragHorizontalAutoPanDelta(
                autoPanDelta.x, targetWidth, laneProjection);
            const bool autoPanned = std::abs(autoPanDelta.x) > 0.001F ||
                                    std::abs(autoPanDelta.y) > 0.001F;
            if ( autoPanned ) {
                // 先发布相机平移，再以同一指针位置更新对象拖拽；事件顺序
                // 让逻辑层用新的视口映射计算最终目标。
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdPanCanvas{
                        .cameraId       = m_cameraId,
                        .deltaX         = autoPanDelta.x,
                        .deltaY         = autoPanDelta.y,
                        .viewportWidth  = targetWidth,
                        .viewportHeight = targetHeight,
                        .renderScaleY   = currentSnapshot->renderScaleY,
                    }));
            }
            const bool playbackScrolled =
                currentSnapshot->hasBeatmap && currentSnapshot->isPlaying;
            // 相机或播放位置变化会改变指针对应时间，必须绕过去重阈值。
            const bool shouldUpdateMove =
                shouldSendContinuousEditCommand(
                    m_lastMoveUpdateCommand,
                    { localMousePos.x, localMousePos.y },
                    *currentSnapshot,
                    ImGui::GetIO().KeyCtrl,
                    false) ||
                autoPanned || playbackScrolled;
            if ( shouldUpdateMove ) {
                // Ctrl 状态允许逻辑层在拖动期间切换复制/约束语义。
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateDrag{ m_cameraId,
                                          localMousePos.x,
                                          localMousePos.y,
                                          ImGui::GetIO().KeyCtrl }));
            }
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool ==
                        Logic::EditTool::ColorBrush ) {
            // 颜色笔沿经过的当前 hover 实体逐个应用，集合负责单次去重。
            processColorToolTarget(Logic::EditTool::ColorBrush);
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool ==
                        Logic::EditTool::ColorEraser ) {
            processColorToolTarget(Logic::EditTool::ColorEraser);
        }
    }

    if ( ImGui::IsMouseReleased(0) ) {
        // Shift+单击 seek 只属于 Move 工具，要求按下与释放都在轨道区、
        // 全程未拖动且未从实体开始。
        const bool shiftReleaseSeek =
            m_leftPressStartedOnCanvas && m_leftPressStartedInTrackLayout &&
            isMouseInTrackLayout && !m_leftPressDragged &&
            ImGui::GetIO().KeyShift && currentSnapshot->isHoveringCanvas &&
            currentSnapshot->hasBeatmap && !currentSnapshot->isPlaying &&
            currentSnapshot->currentTool == Logic::EditTool::Move;

        if ( m_leftPressStartedOnCanvas && !m_leftPressStartedObjectDrag &&
             currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            // 释放命令依据按下时锁存的手势种类，而非释放位置的 hover。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
        } else if ( m_leftPressStartedOnCanvas &&
                    currentSnapshot->currentTool == Logic::EditTool::Draw ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
        } else if ( m_leftPressStartedObjectDrag ) {
            // 对象拖拽结束会固化本次连续位移。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
        }

        if ( currentSnapshot->currentTool == Logic::EditTool::Move ) {
            // Move 工具空白单击清空选择；Ctrl 保留/切换选择语义由命令处理。
            if ( m_leftPressStartedOnCanvas && !m_leftPressStartedOnEntity &&
                 !m_leftPressDragged && !shiftReleaseSeek ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSelectEntity{
                        entt::null, !ImGui::GetIO().KeyCtrl }));
            }
        }

        if ( shiftReleaseSeek ) {
            // seek 放在所有编辑结束命令之后，避免逻辑状态仍处于拖拽事务。
            publishCanvasHoverSeek(*currentSnapshot);
        }

        // 每次左键释放统一清理锁存和连续命令缓存，下一手势从干净状态开始。
        m_leftPressStartedOnCanvas      = false;
        m_leftPressStartedInTrackLayout = false;
        m_leftPressStartedOnEntity      = false;
        m_leftPressStartedObjectDrag    = false;
        m_leftPressDragged              = false;
        m_colorStrokeEntities.clear();
        resetContinuousEditCommands();
    }

    const bool ctrlRightRemoveMarquee =
        isHovered && ImGui::IsMouseClicked(1) && ImGui::GetIO().KeyCtrl;
    // Ctrl+右键移除框选具有全局优先级，不与 Draw 工具擦除同时触发。

    // Draw 工具下的普通右键是一段独立擦除事务。
    if ( currentSnapshot->currentTool == Logic::EditTool::Draw ) {
        if ( !currentSnapshot->isPlaying && ImGui::IsMouseClicked(1) &&
             isHovered && !ctrlRightRemoveMarquee ) {
            // 新事务先清除去重基线，保证第一次拖动更新一定发送。
            m_lastEraseUpdateCommand.valid = false;
            m_rightEraseActive             = true;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdStartErase{ m_cameraId, ImGui::GetIO().KeyShift }));
        }
        if ( m_rightEraseActive && !currentSnapshot->isPlaying &&
             ImGui::IsMouseDragging(1) ) {
            // 擦除拖动按坐标、快照和 Shift 状态去重，降低事件队列压力。
            if ( shouldSendContinuousEditCommand(
                     m_lastEraseUpdateCommand,
                     { localMousePos.x, localMousePos.y },
                     *currentSnapshot,
                     ImGui::GetIO().KeyShift,
                     false) ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateErase{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           ImGui::GetIO().KeyShift }));
            }
        }
        if ( m_rightEraseActive && ImGui::IsMouseReleased(1) ) {
            // 即使释放发生在画布外也结束事务并清理本地状态。
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
            m_lastEraseUpdateCommand.valid = false;
            m_rightEraseActive             = false;
        }
    }

    // --- Ctrl+右键：移除框选框（全局可用） ---
    if ( ctrlRightRemoveMarquee ) {
        // 命令携带当前局部坐标，由逻辑层查找命中的框选区域。
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdRemoveMarqueeAt{
                m_cameraId, localMousePos.x, localMousePos.y }));
    }

    // 滚轮最后处理：批注、修饰键属性工具和普通时间滚动按优先级消费。
    const auto& io = ImGui::GetIO();
    float       wheel =
        annotationGutterInteraction.wheelConsumed ? 0.0F : io.MouseWheel;
    const bool isModifierWheelHovered =
        isInsideCanvas && (io.KeyCtrl || io.KeySuper || io.KeyAlt) &&
        !ImGui::IsAnyMouseDown() &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // 修饰键滚轮允许在被活动控件遮挡的画布上生效，但鼠标按键按下时
    // 禁用，避免与拖拽过程中的修饰键切换冲突。
    if ( (isHovered || isModifierWheelHovered) && std::abs(wheel) > 0.01f ) {
        if ( !handleModifierWheel(currentSnapshot) ) {
            // 未被属性调整消费的滚轮映射为时间滚动；Shift 改变滚动模式。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdScroll{ m_cameraId, -wheel, io.KeyShift }));

            const bool canUpdateActiveBrush =
                currentSnapshot->currentTool == Logic::EditTool::Draw &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                (!currentSnapshot->isPlaying ||
                 currentSnapshot->brush.isActive);
            if ( canUpdateActiveBrush ) {
                // 滚动改变指针对应时间，活动画笔即使鼠标未移动也需重算落点。
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateBrush{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           io.KeyShift,
                                           io.KeyCtrl }));
            }
        }
    }
}

}  // namespace MMM::Canvas
