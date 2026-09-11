/// @file PreviewCanvas.cpp
/// @brief 实现谱面预览窗口、全谱密度导航及对应 Vulkan 命令录制。
///
/// PreviewCanvas 只消费逻辑线程已经发布的不可变渲染快照，
/// 不在 UI 或录制阶段重新遍历 ECS，也不自行推导谱面物件。
/// UI 主线程负责窗口布局、鼠标命中与低频资源重载；
/// 并行准备阶段负责从 RenderSnapshotBuffer 取得下一帧快照；
/// Vulkan 录制阶段只读取已经交换完成的快照和纹理图集。
///
/// 右侧密度栏与预览画布共享同一个全谱时间域，
/// 但密度栏是 ImGui 覆盖层，不进入离屏顶点和索引缓冲。
/// 连续拖动采用预览 Seek，释放时再提交最终 Seek，
/// 从而保持本地反馈即时且不以固定延迟阻塞逻辑线程。
/// 协作者标记只读取房间的稳定视野缓存，跳过本地参与者，
/// 其绘制不会改变预览快照或协作同步状态。
#include "canvas/PreviewCanvas.h"
#include "canvas/CollaborationPeerColor.h"
#include "canvas/PreviewDensityColor.h"
#include "canvas/PreviewDensityInteraction.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include "ui/utils/TimeFormatUtils.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <system_error>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 预览密度栏期望宽度，单位为 DPI 缩放前逻辑像素。
constexpr float PREVIEW_DENSITY_WIDTH = 36.0f;

/// @brief 预览画布与密度栏之间的间隔，单位为 DPI 缩放前逻辑像素。
constexpr float PREVIEW_DENSITY_GAP = 4.0f;

/// @brief 窄窗口中仍需保留的最小预览画布宽度。
constexpr float PREVIEW_MIN_CANVAS_WIDTH = 48.0f;

/// @brief 密度栏外框与内部时间轴的屏幕布局。
struct PreviewDensityRailLayout {
    /// @brief 密度栏外框左上角。
    ImVec2 railMin{ 0.0f, 0.0f };

    /// @brief 密度栏外框右下角。
    ImVec2 railMax{ 0.0f, 0.0f };

    /// @brief 密度时间轴绘制区域左上角。
    ImVec2 innerMin{ 0.0f, 0.0f };

    /// @brief 密度时间轴绘制区域右下角。
    ImVec2 innerMax{ 0.0f, 0.0f };

    /// @brief 当前布局是否足够容纳绘制和交互。
    bool valid{ false };
};

/// @brief 计算预览画布右侧密度栏布局。
/// @param canvasPos 预览画布左上角屏幕坐标。
/// @param canvasSize 预览画布尺寸。
/// @param reservedWidth 为密度栏预留的总宽度。
/// @param dpiScale 当前窗口 DPI 缩放。
/// @return 可供绘制和命中测试共用的密度栏布局。
/// @details 返回的外框覆盖 RenderContext 预留区，内部区域再扣除描边留白。
/// 当窗口过窄、预留宽度不足或内部尺寸退化时返回 invalid，
/// 调用方必须同时停止绘制和结束可能尚未提交的拖动。
/// gap 受预留宽度约束，避免高 DPI 下间隔挤占整个密度栏；
/// innerPadding 同时受逻辑像素和外框比例约束，
/// 保证极窄栏位仍至少留出可命中的正面积时间轴。
/// @warning UI 热路径纯计算：每帧调用，不得引入分配或阻塞操作。
PreviewDensityRailLayout calculatePreviewDensityRailLayout(
    const ImVec2& canvasPos, const ImVec2& canvasSize, float reservedWidth,
    float dpiScale)
{
    PreviewDensityRailLayout layout;
    // 无法形成二维时间轴时保留默认 invalid，避免后续坐标除法产生
    // 无穷值，也让绘制和命中测试共享完全一致的退化判断。
    if ( reservedWidth <= 1.0f || canvasSize.y <= 1.0f ) {
        return layout;
    }

    // 密度栏起点位于预览画布右侧；间隔最多占预留区四分之一，
    // 因此即使实际预留宽度低于主题期望值也能留下主体区域。
    const float gap =
        std::min(reservedWidth * 0.25f,
                 std::max(1.0f, std::floor(PREVIEW_DENSITY_GAP * dpiScale)));
    layout.railMin = { canvasPos.x + canvasSize.x + gap, canvasPos.y };
    layout.railMax = { canvasPos.x + canvasSize.x + reservedWidth,
                       canvasPos.y + canvasSize.y };
    if ( layout.railMax.x - layout.railMin.x <= 1.0f ) {
        return layout;
    }

    // 内边距不能超过当前外框宽度的 20%，否则窄窗口中左右留白
    // 会相交；向下取整则使像素边界在常见 DPI 下保持稳定。
    const float innerPadding =
        std::min(std::floor(2.0f * dpiScale),
                 std::max(0.0f, (layout.railMax.x - layout.railMin.x) * 0.2f));
    layout.innerMin = { layout.railMin.x + innerPadding,
                        layout.railMin.y + innerPadding };
    layout.innerMax = { layout.railMax.x - innerPadding,
                        layout.railMax.y - innerPadding };
    // 最终以内部区域而不是外框判定有效，后续时间/坐标映射才能
    // 安全地使用 innerMin 与 innerMax 作为严格递增边界。
    layout.valid = layout.innerMax.x - layout.innerMin.x > 0.0f &&
                   layout.innerMax.y - layout.innerMin.y > 0.0f;
    return layout;
}
}  // namespace

/// @brief 创建与指定逻辑画布共享快照缓冲的预览视图。
/// @param name 皮肤 Canvas 配置名，同时参与 shader 名称隔离。
/// @param w 初始离屏目标宽度。
/// @param h 初始离屏目标高度。
/// @param syncBuffer 逻辑线程向 UI 发布不可变渲染快照的共享缓冲。
/// @details 构造阶段只保存轻量配置，不创建 Vulkan 资源；
/// 纹理图集由 reloadTextures 在图形设备可用后统一建立。
/// 快照缓冲允许为空，此时预览窗口仍可显示空内容并等待后续绑定。
PreviewCanvas::PreviewCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_syncBuffer(std::move(syncBuffer))
{
    // RenderContext 首帧以目标尺寸建立离屏附件；实际窗口尺寸变化
    // 会继续经 resizeCall 通知逻辑相机更新投影。
    m_targetWidth  = w;
    m_targetHeight = h;
}

/// @brief 绘制预览窗口右侧独立的全谱物件密度栏。
/// @param canvasPos 预览画布内容左上角屏幕坐标。
/// @param canvasSize 扣除密度栏后的预览画布逻辑尺寸。
/// @param reservedWidth 右侧为密度栏实际预留的逻辑宽度。
/// @param dpiScale 当前窗口 DPI 缩放。
/// @param seekPreviewTime 当前拖动预览时间；无交互时使用快照播放时间。
/// @param sourceManager UI 管理器观察指针，用于读取应用级协作房间。
/// @details 密度快照由逻辑线程预聚合为固定数量的时间 bin。
/// 当栏位像素高度小于 bin 数量时，本函数按可见行分组并取组内最大值，
/// 防止高密度峰值因简单抽样消失；时间升序数据按预览方向自底向上绘制。
/// 当前时间所在行先绘制低透明度底色，再覆盖对应密度条和双层游标线。
/// 双层线使用主题边框作外沿，使亮色与暗色皮肤中都能保持可见。
///
/// 协作者视野标记在密度条之后绘制并限制在 rail 外框内。
/// 本地参与者不重复标记；缺少参与者元数据或非法时间的缓存会被跳过。
/// 函数不保留任何 ImGui 指针，也不修改房间或快照中的状态。
/// @warning UI 热路径：每帧最多聚合并绘制 512 个缓存样本和 8 个协作者
/// 标记；禁止 ECS 遍历、排序、文件访问或共享指针复制。
void PreviewCanvas::drawDensityOverview(const ImVec2& canvasPos,
                                        const ImVec2& canvasSize,
                                        float reservedWidth, float dpiScale,
                                        std::optional<double> seekPreviewTime,
                                        UI::UIManager* sourceManager) const
{
    // 绘制与交互必须使用同一套布局计算；无效布局直接退出，
    // 避免密度条看得见但 InvisibleButton 命中区域位于别处。
    const auto layout = calculatePreviewDensityRailLayout(
        canvasPos, canvasSize, reservedWidth, dpiScale);
    if ( !layout.valid ) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // 圆角不能超过外框短边的一半，否则 ImGui 会在极窄栏位产生
    // 相互覆盖的圆角；主题 FrameRounding 只作为上限偏好。
    const float rounding =
        std::clamp(ImGui::GetStyle().FrameRounding,
                   0.0f,
                   std::min(layout.railMax.x - layout.railMin.x,
                            layout.railMax.y - layout.railMin.y) *
                       0.5f);
    drawList->AddRectFilled(layout.railMin,
                            layout.railMax,
                            ImGui::GetColorU32(ImGuiCol_FrameBg),
                            rounding);
    drawList->AddRect(layout.railMin,
                      layout.railMax,
                      ImGui::GetColorU32(ImGuiCol_Border),
                      rounding);

    if ( !m_currentSnapshot ) {
        return;
    }
    // duration 是所有 bin、当前游标和协作者标记共享的时间基准。
    // 非有限或非正时长无法建立单调映射，因此整条时间轴不绘制。
    const auto& density = m_currentSnapshot->previewDensity;
    if ( !std::isfinite(density.duration) || density.duration <= 0.0 ) {
        return;
    }

    const float innerWidth  = layout.innerMax.x - layout.innerMin.x;
    const float innerHeight = layout.innerMax.y - layout.innerMin.y;
    // 拖动期间优先采用即时目标时间，使游标跟随鼠标而无需等待
    // 逻辑快照往返；其它帧回退到快照播放时间并过滤非有限值。
    const double currentTime =
        seekPreviewTime && std::isfinite(*seekPreviewTime)
            ? *seekPreviewTime
            : (std::isfinite(m_currentSnapshot->currentTime)
                   ? m_currentSnapshot->currentTime
                   : 0.0);
    const double progress =
        std::clamp(currentTime / density.duration, 0.0, 1.0);
    std::size_t currentBin        = 0;
    float       currentNormalized = 0.0F;
    // progress==1 时乘积会落在 size 位置，必须钳制到最后一个 bin；
    // maxCount 为零表示全谱无物件，此时颜色使用最低密度端点。
    if ( !density.counts.empty() && density.maxCount > 0 ) {
        currentBin = std::min(
            density.counts.size() - 1,
            static_cast<std::size_t>(
                progress * static_cast<double>(density.counts.size())));
        currentNormalized = static_cast<float>(density.counts[currentBin]) /
                            static_cast<float>(density.maxCount);
    }
    const auto  currentDensityColor = previewDensityColorAt(currentNormalized);
    ImVec4      activeBackground{ currentDensityColor.r,
                             currentDensityColor.g,
                             currentDensityColor.b,
                             0.20f };
    const ImU32 activeBackgroundColor = ImGui::GetColorU32(activeBackground);

    // 每个屏幕像素行至多生成一个密度矩形，限制窄而高的预览窗口
    // 不会因谱面 bin 数增加而无限增长 ImDrawList 命令数量。
    const std::size_t displayRowCount =
        density.maxCount > 0
            ? std::min<std::size_t>(density.counts.size(),
                                    static_cast<std::size_t>(std::max(
                                        1.0f, std::floor(innerHeight))))
            : 0;
    for ( std::size_t row = 0; row < displayRowCount; ++row ) {
        // 整数分区覆盖全部 bin 且不重叠；最后一行自然吸收除法余数。
        // 组内取最大值保留短促密集段，不让平均值削弱警示颜色。
        const std::size_t binBegin =
            row * density.counts.size() / displayRowCount;
        const std::size_t binEnd = std::max(
            binBegin + 1, (row + 1) * density.counts.size() / displayRowCount);
        std::uint32_t rowCount = 0;
        for ( std::size_t bin = binBegin; bin < binEnd; ++bin ) {
            rowCount = std::max(rowCount, density.counts[bin]);
        }

        // 预览画布的未来时间向上，因此时间升序样本需自底向上排列。
        const std::size_t visualRow = displayRowCount - 1 - row;
        const float       rowY0 =
            layout.innerMin.y + innerHeight * static_cast<float>(visualRow) /
                                    static_cast<float>(displayRowCount);
        const float rowY1 = layout.innerMin.y +
                            innerHeight * static_cast<float>(visualRow + 1) /
                                static_cast<float>(displayRowCount);
        const bool isCurrent = currentBin >= binBegin && currentBin < binEnd;
        // 当前行背景即使 rowCount 为零也要保留，保证播放游标在
        // 空白小节仍能通过整行底色快速定位。
        if ( isCurrent ) {
            drawList->AddRectFilled(ImVec2(layout.innerMin.x, rowY0),
                                    ImVec2(layout.innerMax.x, rowY1),
                                    activeBackgroundColor);
        }
        if ( rowCount == 0 ) {
            continue;
        }

        const float normalized =
            static_cast<float>(rowCount) / static_cast<float>(density.maxCount);
        const auto densityColor = previewDensityColorAt(normalized);
        // 当前行使用完全不透明色，其余行略微降低透明度，
        // 让密度峰值和当前播放位置表达为两个彼此独立的视觉维度。
        const ImU32 barColor =
            ImGui::GetColorU32(ImVec4(densityColor.r,
                                      densityColor.g,
                                      densityColor.b,
                                      isCurrent ? 1.0f : 0.82f));
        const float barWidth      = std::max(1.0f, innerWidth * normalized);
        const float verticalInset = rowY1 - rowY0 >= 2.0f
                                        ? std::min(0.5f, (rowY1 - rowY0) * 0.2f)
                                        : 0.0f;
        drawList->AddRectFilled(
            ImVec2(layout.innerMin.x, rowY0 + verticalInset),
            ImVec2(layout.innerMin.x + barWidth, rowY1 - verticalInset),
            barColor);
    }

    // 时间到 Y 的换算集中在共享 helper，维持“未来向上”的方向约定，
    // 并由 optional 表达所有非法尺寸和时间输入。
    const auto currentY = previewDensityYAtTime(
        currentTime, layout.innerMin.y, layout.innerMax.y, density.duration);
    if ( !currentY ) {
        return;
    }
    const ImU32 currentLineColor =
        ImGui::GetColorU32(ImVec4(currentDensityColor.r,
                                  currentDensityColor.g,
                                  currentDensityColor.b,
                                  1.0f));
    // 先画较粗的主题边框，再画密度色内芯；这样高亮色不会在
    // 相近背景上消失，也无需为不同皮肤维护额外固定对比色。
    drawList->AddLine(ImVec2(layout.innerMin.x, static_cast<float>(*currentY)),
                      ImVec2(layout.innerMax.x, static_cast<float>(*currentY)),
                      ImGui::GetColorU32(ImGuiCol_Border),
                      std::max(3.0f, std::floor(3.0f * dpiScale)));
    drawList->AddLine(ImVec2(layout.innerMin.x, static_cast<float>(*currentY)),
                      ImVec2(layout.innerMax.x, static_cast<float>(*currentY)),
                      currentLineColor,
                      std::max(1.0f, std::floor(2.0f * dpiScale)));

    // UIManager 与 CollaborationRoom 都是观察访问，生命周期由应用层
    // 管理；密度栏不复制房间 shared_ptr，避免每帧原子引用计数。
    auto* room =
        sourceManager ? sourceManager->getCollaborationRoom() : nullptr;
    if ( !room || !room->isActive() || room->localPeerId() == 0 ) {
        return;
    }

    const auto& viewports    = room->participantViewports();
    const auto& participants = room->participants();
    const auto  localId      = room->localPeerId();
    // 三角标记宽度受栏位宽度约束，高 DPI 偏好不能挤出密度区；
    // 描边比内芯各多一像素，为相邻协作者颜色保留清晰轮廓。
    const float markerMaximum = std::max(1.0F, innerWidth * 0.35F);
    const float markerSize =
        std::clamp(std::floor(5.0F * dpiScale), 1.0F, markerMaximum);
    const float markerHalfHeight = std::max(2.0F, markerSize * 0.65F);
    const float lineThickness    = std::max(1.0F, std::floor(2.0F * dpiScale));
    const ImU32 markerOutlineColor = ImGui::GetColorU32(ImGuiCol_Border);
    drawList->PushClipRect(layout.railMin, layout.railMax, true);
    for ( const auto& [peerId, viewport] : viewports ) {
        // 视野可能先于参与者信息到达或在离房过程中短暂残留；
        // 缺少身份颜色时跳过比使用不稳定的默认颜色更易辨认。
        const auto participant = participants.find(peerId);
        if ( peerId == localId || participant == participants.end() ) {
            continue;
        }
        const auto markerY = previewDensityYAtTime(viewport.visualTime,
                                                   layout.innerMin.y,
                                                   layout.innerMax.y,
                                                   density.duration);
        if ( !markerY ) {
            continue;
        }

        // 水平线指向右侧三角，颜色由稳定 participantId 派生，
        // 因而参与者列表重排不会导致同一用户的颜色跳变。
        const float y = static_cast<float>(*markerY);
        const ImU32 color =
            collaborationPeerColor(participant->second.participantId, 255);
        const float tipX = layout.innerMax.x - markerSize;
        drawList->AddLine({ layout.innerMin.x, y },
                          { tipX, y },
                          markerOutlineColor,
                          lineThickness + 2.0F);
        drawList->AddLine(
            { layout.innerMin.x, y }, { tipX, y }, color, lineThickness);
        drawList->AddTriangleFilled(
            { tipX - 1.0F, y },
            { layout.innerMax.x, y - markerHalfHeight - 1.0F },
            { layout.innerMax.x, y + markerHalfHeight + 1.0F },
            markerOutlineColor);
        drawList->AddTriangleFilled({ tipX, y },
                                    { layout.innerMax.x, y - markerHalfHeight },
                                    { layout.innerMax.x, y + markerHalfHeight },
                                    color);
    }
    drawList->PopClipRect();
}

/// @brief 提交密度栏最近一次连续 Seek。
/// @details 连续拖动期间逻辑线程收到 isScrubbing=true 的预览命令；
/// 结束时必须用相同目标时间发布 isScrubbing=false，
/// 让音频、网络同步和历史状态只在明确的交互边界完成最终提交。
/// 重复调用是幂等的，未处于拖动状态时不会产生多余事件。
/// @warning UI 热路径：仅在拖动结束或窗口中断交互时发布一条命令。
void PreviewCanvas::commitDensitySeekScrub()
{
    // 状态位同时承担重复提交保护；窗口关闭、布局失效与正常松手
    // 都可安全调用此函数而不会重复发布最终 Seek。
    if ( !m_wasDensitySeekActive ) {
        return;
    }
    // 使用最近一次已经发送给逻辑线程的命令时间，而不是重新读取
    // 鼠标或视觉偏移，确保一次拖动的预览和提交处于同一时间域。
    Event::EventBus::instance().publish(Event::LogicCommandEvent(Logic::CmdSeek{
        .time        = m_lastDensitySeekCommandTime,
        .isScrubbing = false,
    }));
    m_wasDensitySeekActive = false;
}

/// @brief 处理密度栏按下、拖动和松开时的连续时间跳转。
/// @param canvasPos 预览画布内容左上角屏幕坐标。
/// @param canvasSize 扣除密度栏后的预览画布逻辑尺寸。
/// @param reservedWidth 右侧为密度栏实际预留的逻辑宽度。
/// @param dpiScale 当前窗口 DPI 缩放。
/// @return 当前交互帧需要即时绘制的目标时间；未拖动时返回空。
/// @details InvisibleButton 只占用密度栏，不改变预览画布的光标布局。
/// helper 根据 active、deactivated、上一帧状态和目标变化决定三态分发：
/// None 不发布事件，Preview 发布连续预览，Commit 结束已有拖动。
/// 返回时间仍是视觉时间，供密度游标即时绘制；
/// 发布给逻辑线程前会扣除全局视觉偏移，转换为播放命令时间。
/// 任一前置条件失效都先结束旧拖动，再返回空值，
/// 防止鼠标移出窗口或项目切换后 scrubbing 状态悬挂。
/// @warning UI 热路径：每帧仅处理常量级命中测试与坐标换算；
/// 拖动变化时发布本地预览 Seek，松手时固定发布一次最终提交。
std::optional<double> PreviewCanvas::handleDensitySeekInteraction(
    const ImVec2& canvasPos, const ImVec2& canvasSize, float reservedWidth,
    float dpiScale)
{
    // 布局失效意味着上一帧的 InvisibleButton 已不再存在；此时必须
    // 主动提交已有拖动，不能依赖 ImGui 再产生 deactivated 状态。
    const auto layout = calculatePreviewDensityRailLayout(
        canvasPos, canvasSize, reservedWidth, dpiScale);
    if ( !layout.valid ) {
        commitDensitySeekScrub();
        return std::nullopt;
    }

    // 临时移动 ImGui 光标仅用于建立命中区域，随后立即恢复，
    // 避免密度栏占位改变 RenderContext 后续控件的自动布局。
    const ImVec2 previousCursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(layout.railMin);
    ImGui::InvisibleButton("##PreviewDensitySeek",
                           ImVec2(layout.railMax.x - layout.railMin.x,
                                  layout.railMax.y - layout.railMin.y),
                           ImGuiButtonFlags_MouseButtonLeft);
    const bool isHovered   = ImGui::IsItemHovered();
    const bool isActive    = ImGui::IsItemActive();
    const bool deactivated = ImGui::IsItemDeactivated();
    ImGui::SetCursorScreenPos(previousCursor);

    // 快照为空时没有可靠的 duration；结束旧交互后等待并行准备阶段
    // 发布新项目快照，不能沿用上一个项目的目标时间。
    if ( !m_currentSnapshot ) {
        commitDensitySeekScrub();
        return std::nullopt;
    }
    const double duration = m_currentSnapshot->previewDensity.duration;
    const ImVec2 mousePos = ImGui::GetMousePos();
    // 鼠标坐标在窗口失焦或平台后端切换时可能无效；所有异常输入
    // 都走同一收尾路径，避免向逻辑线程发送 NaN 或无穷时间。
    if ( !std::isfinite(duration) || duration <= 0.0 ||
         !ImGui::IsMousePosValid(&mousePos) || !std::isfinite(mousePos.y) ) {
        commitDensitySeekScrub();
        return std::nullopt;
    }

    // 映射 helper 负责钳制上下边界，并将自底向上的视觉方向还原
    // 为从零开始递增的谱面时间。
    const auto targetTime = previewDensityTimeAtY(
        mousePos.y, layout.innerMin.y, layout.innerMax.y, duration);
    if ( !targetTime ) {
        commitDensitySeekScrub();
        return std::nullopt;
    }

    // 小于浮点容差的鼠标抖动不重复发送预览命令；正常松手仍由
    // dispatch 独立判定为 Commit，不会被变化检测吞掉。
    const bool targetChanged =
        std::abs(*targetTime - m_lastDensitySeekTime) > 1e-6;
    const auto dispatch = resolvePreviewDensitySeekDispatch(
        isActive, deactivated, m_wasDensitySeekActive, targetChanged);
    const bool interactionFrame =
        dispatch != PreviewDensitySeekDispatch::None || isActive;
    // 密度栏展示的是应用视觉时间，CmdSeek 接受逻辑播放时间；
    // 只在本帧得到合法目标后读取偏移，项目切换时不会复用旧换算值。
    const double visualOffset = Config::AppConfig::instance()
                                    .getVisualConfig()
                                    .getEffectiveVisualOffset();
    const double commandTime = *targetTime - visualOffset;
    if ( dispatch == PreviewDensitySeekDispatch::Preview ) {
        // 预览命令允许音频和协作层采用轻量拖动策略；同时缓存视觉
        // 与命令时间，分别服务抖动判断、提示绘制和最终提交。
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSeek{ .time = commandTime, .isScrubbing = true }));
        m_lastDensitySeekTime        = *targetTime;
        m_lastDensitySeekCommandTime = commandTime;
    } else if ( dispatch == PreviewDensitySeekDispatch::Commit ) {
        // 松手位置可能与最后一次 Preview 不同，先更新两个时间缓存，
        // 再通过统一收尾函数发布最终值。
        m_lastDensitySeekTime        = *targetTime;
        m_lastDensitySeekCommandTime = commandTime;
        commitDensitySeekScrub();
    }
    if ( dispatch != PreviewDensitySeekDispatch::Commit ) {
        // Commit 已由 commitDensitySeekScrub 清除状态；其它分支记录
        // 当前 ImGui active 值，为窗口丢失控件时的主动收尾提供依据。
        m_wasDensitySeekActive = isActive;
    }

    if ( isHovered || interactionFrame ) {
        // 拖动中即使鼠标越出外框也保留纵向调整光标和时间提示，
        // 让用户清楚当前捕获仍属于密度栏交互。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const auto timeText = MMM::UI::Utils::formatCanvasTimePair(
            *targetTime, duration, m_currentSnapshot);
        ImGui::SetTooltip("%s", timeText.c_str());
    }
    if ( interactionFrame ) {
        // 仅真实交互帧覆盖密度游标；普通悬浮不应冻结快照播放位置。
        return targetTime;
    }
    return std::nullopt;
}

/// @brief 更新预览画布 ImGui 窗口和鼠标交互。
/// @param sourceManager UI 管理器观察指针，用于读取协作房间。
/// @details 更新顺序固定为窗口生命周期、内容区鼠标状态、密度栏交互、
/// 鼠标命令去重、拖动提示、点击跳转和滚轮倍率调整。
/// 密度栏占用 RenderContext 的右侧保留区，不属于主预览内容；
/// 因此主画布拖动和点击命中只使用 getRenderSize 返回的内容尺寸。
/// 鼠标命令缓存最后一次位置、视口和按钮派生状态，
/// 只有任一字段发生可感知变化时才跨线程发布。
/// @warning
/// 热路径：主渲染线程每帧执行；只发送变化后的鼠标命令，避免每帧重复事件。
void PreviewCanvas::update(UI::UIManager* sourceManager)
{
    auto& appConfig      = Config::AppConfig::instance();
    auto& editorSettings = appConfig.getEditorSettings();
    // 菜单隐藏窗口时本帧不会创建密度栏控件，必须显式结束可能仍被
    // ImGui 捕获的拖动，避免逻辑层持续保留 scrubbing 状态。
    if ( !editorSettings.showPreviewWindow ) {
        commitDensitySeekScrub();
        return;
    }

    // 可见标题允许翻译动态变化，### 后的固定 ID 保证停靠节点和
    // imgui.ini 状态仍归属于同一个预览窗口。
    std::string windowName =
        fmt::format("{}###PreviewWindow", TR("canvas.preview"));
    bool windowOpen = editorSettings.showPreviewWindow;

    UI::LayoutContext lctx(m_layoutCtx, windowName, true, 0, &windowOpen);
    if ( !windowOpen ) {
        // 标题栏关闭与菜单隐藏走同一持久化状态；先结束拖动，
        // 再保存设置，下一帧便不会重新创建已关闭窗口。
        commitDensitySeekScrub();
        editorSettings.showPreviewWindow = false;
        appConfig.save();
        return;
    }
    const float dpiScale = std::max(1.0f, lctx.m_dpiScale);
    const float requestedDensityReserve =
        std::floor((PREVIEW_DENSITY_WIDTH + PREVIEW_DENSITY_GAP) * dpiScale);
    // RenderContext 在窗口过窄时优先保证 PREVIEW_MIN_CANVAS_WIDTH，
    // 实际可用的密度栏宽度通过 getReservedRightWidth 反馈给后续步骤。
    RenderContext rctx(this,
                       windowName.c_str(),
                       m_targetWidth,
                       m_targetHeight,
                       nullptr,
                       requestedDensityReserve,
                       PREVIEW_MIN_CANVAS_WIDTH * dpiScale);

    // 平台后端可能在窗口切换期间报告无效鼠标坐标；这时沿用最后一次
    // 有效位置，只把 hover/drag 状态变化发送给逻辑线程。
    ImVec2     mousePos         = ImGui::GetMousePos();
    ImVec2     windowPos        = ImGui::GetCursorScreenPos();
    ImVec2     contentSize      = rctx.getRenderSize();
    const bool hasValidMousePos = ImGui::IsMousePosValid(&mousePos) &&
                                  std::isfinite(mousePos.x) &&
                                  std::isfinite(mousePos.y);
    ImVec2 localMousePos{ 0.0f, 0.0f };
    if ( hasValidMousePos ) {
        // 逻辑相机使用以预览内容区左上角为原点的局部坐标，
        // 不能包含窗口标题、边框或右侧密度栏偏移。
        localMousePos = { mousePos.x - windowPos.x, mousePos.y - windowPos.y };
    } else if ( m_lastMouseCommand.valid ) {
        // 保留最后坐标能让失焦帧只表达离开状态，避免突然跳到原点
        // 触发逻辑侧无意义的悬浮重算。
        localMousePos = { m_lastMouseCommand.pos.x, m_lastMouseCommand.pos.y };
    }

    bool isHoveringContent = hasValidMousePos && mousePos.x >= windowPos.x &&
                             mousePos.x <= windowPos.x + contentSize.x &&
                             mousePos.y >= windowPos.y &&
                             mousePos.y <= windowPos.y + contentSize.y;

    // ImGui 窗口悬浮还会覆盖密度栏和装饰区域，必须再与实际渲染
    // 内容矩形相交，才能向预览相机报告悬浮。
    bool isHovered = ImGui::IsWindowHovered() && isHoveringContent;

    ImVec2 clickPos = ImGui::GetIO().MouseClickedPos[0];
    // 起点单独记录而不是只看当前鼠标位置，阻止从外部窗口拖入后
    // 在预览区释放时误触发跳转。
    bool clickStartedInContent =
        hasValidMousePos && clickPos.x >= windowPos.x &&
        clickPos.x <= windowPos.x + contentSize.x &&
        clickPos.y >= windowPos.y && clickPos.y <= windowPos.y + contentSize.y;

    // 拖动还要求当前窗口持有焦点，避免停靠区域中的其它控件抢占
    // 鼠标后仍向逻辑相机发送预览拖动状态。
    bool isDragging = hasValidMousePos && ImGui::IsMouseDragging(0) &&
                      clickStartedInContent && ImGui::IsWindowFocused();

    const auto densitySeekPreview = handleDensitySeekInteraction(
        windowPos, contentSize, rctx.getReservedRightWidth(), dpiScale);
    // 先处理交互再绘制，当前帧密度游标即可直接采用鼠标目标时间，
    // 无需等待 CmdSeek 经过逻辑线程并产生下一份渲染快照。
    drawDensityOverview(windowPos,
                        contentSize,
                        rctx.getReservedRightWidth(),
                        dpiScale,
                        densitySeekPreview,
                        sourceManager);

    float viewportWidth  = contentSize.x;
    float viewportHeight = contentSize.y;

    constexpr float mouseEpsilon = 0.1f;
    // 位置和视口使用亚像素容差，抑制 DPI 换算与停靠布局带来的
    // 微小抖动；hover/drag 是离散状态，任何变化都立即发布。
    bool shouldSendMouse =
        !m_lastMouseCommand.valid ||
        std::abs(m_lastMouseCommand.pos.x - localMousePos.x) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.pos.y - localMousePos.y) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportWidth - viewportWidth) >
            mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportHeight - viewportHeight) >
            mouseEpsilon ||
        m_lastMouseCommand.isHovering != isHovered ||
        m_lastMouseCommand.isDragging != isDragging;

    if ( shouldSendMouse ) {
        // CmdSetMousePosition 按 cameraId 路由到预览相机，逻辑线程
        // 可独立更新 hover 时间而不读取 ImGui 或窗口对象。
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ .cameraId       = m_cameraId,
                                        .mouseX         = localMousePos.x,
                                        .mouseY         = localMousePos.y,
                                        .viewportWidth  = viewportWidth,
                                        .viewportHeight = viewportHeight,
                                        .isHovering     = isHovered,
                                        .isDragging     = isDragging }));
        m_lastMouseCommand.valid         = true;
        m_lastMouseCommand.pos           = { localMousePos.x, localMousePos.y };
        m_lastMouseCommand.viewportWidth = viewportWidth;
        m_lastMouseCommand.viewportHeight = viewportHeight;
        m_lastMouseCommand.isHovering     = isHovered;
        m_lastMouseCommand.isDragging     = isDragging;
    }

    // 逻辑快照确认正在预览拖动后再显示提示，避免只按下但尚未形成
    // 有效时间映射时展示旧的 previewHoverTime。
    if ( isDragging && m_currentSnapshot &&
         m_currentSnapshot->isPreviewDragging ) {
        const auto timeText = MMM::UI::Utils::formatCanvasTime(
            m_currentSnapshot->previewHoverTime, m_currentSnapshot);
        ImGui::SetTooltip("%s",
                          TR_FMT("canvas.preview.jump_to", timeText).c_str());
    }

    // 点击跳转和滚轮缩放仅在主预览内容悬浮时生效；密度栏拥有
    // 自己的 InvisibleButton 与 Seek 流程，不会进入这里。
    if ( m_currentSnapshot && isHovered ) {
        // 仅在鼠标松开、按下起点位于内容区且窗口仍有焦点时提交。
        // 这同时覆盖普通点击与在预览内开始的拖动，并拒绝外部拖入。
        if ( ImGui::IsMouseReleased(0) && clickStartedInContent &&
             ImGui::IsWindowFocused() ) {
            // hoveredTime 属于视觉时间；与密度栏一样，发布 CmdSeek 前
            // 扣除有效视觉偏移，维持两种导航入口的时间语义一致。
            float visualOffset = Config::AppConfig::instance()
                                     .getVisualConfig()
                                     .getEffectiveVisualOffset();
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSeek{
                    m_currentSnapshot->hoveredTime - visualOffset }));
        }

        // Ctrl/Super/Alt 保留给应用快捷键，只有无这些修饰键的滚轮
        // 才调整预览倍率；Shift 将每格步长加倍以便快速浏览全谱。
        float wheel = ImGui::GetIO().MouseWheel;
        if ( std::abs(wheel) > 0.01f && !ImGui::GetIO().KeyCtrl &&
             !ImGui::GetIO().KeySuper && !ImGui::GetIO().KeyAlt ) {
            auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            float step      = 0.5f;
            if ( ImGui::GetIO().KeyShift ) {
                step *= 2.0f;
            }

            // areaRatio 越大表示一次展示的谱面时间越多，即视觉缩小；
            // 因而向上滚动减小倍率，符合常见的“滚轮向上放大”习惯。
            // 上下限避免零/负投影以及过度压缩造成的不可读预览。
            editorCfg.visual.previewConfig.areaRatio -= wheel * step;
            editorCfg.visual.previewConfig.areaRatio = std::clamp(
                editorCfg.visual.previewConfig.areaRatio, 1.0f, 50.0f);

            // 通过 EditorEngine 统一发布配置，逻辑相机和持久化流程
            // 可以在自己的线程边界观察变化，UI 不直接修改会话对象。
            Logic::EditorEngine::instance().setEditorConfig(editorCfg);
        }
    }
}

/// @brief 判断预览视图是否需要参与本帧 UI 更新。
/// @return 用户设置要求显示预览窗口时返回 true。
/// @details 该状态只反映窗口可见偏好，不表示谱面数据或 GPU 资源发生修改；
/// IUIView 调度器借此跳过已隐藏窗口的常规更新。
bool PreviewCanvas::isDirty() const
{
    return Config::AppConfig::instance().getEditorSettings().showPreviewWindow;
}

/// @brief 判断当前帧是否需要准备预览快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
/// @details 同时要求快照缓冲存在、视图处于打开状态且设置允许显示。
/// UiFrameSnapshot 当前不参与判定，但保留参数以遵循并行 UI 准备接口。
/// 隐藏窗口不消费 RenderSnapshotBuffer，降低无用的跨线程快照访问。
/// @warning UI 调度热路径：每帧调用，只读取稳定标志与设置值。
bool PreviewCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    return m_syncBuffer && m_isOpen &&
           Config::AppConfig::instance().getEditorSettings().showPreviewWindow;
}

/// @brief 在线程池中拉取并准备预览画布快照。
/// @param snapshot 当前帧 UI 快照。
/// @details prepareCanvasSnapshot 会复用上一份偏移快照与已应用 Y 偏移，
/// 将逻辑线程发布的数据转换为可在主线程一次性交换的准备结果。
/// 本函数只写 prepared 槽位，不修改当前渲染所读的 m_currentSnapshot。
/// @warning 并行 UI 准备路径：不得访问 ImGui 或 Vulkan 命令缓冲。
void PreviewCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    // true 表示按预览相机语义准备快照，偏移复用规则由共享 helper
    // 统一维护，避免主画布与预览各自实现不同的滚动补偿。
    m_preparedSnapshot = prepareCanvasSnapshot(
        m_syncBuffer.get(), m_lastOffsetSnapshot, m_lastAppliedYOffset, true);
    m_hasPreparedSnapshot = true;
}

/// @brief 将准备好的预览快照切换到主线程可见状态。
/// @details 交换只在主线程帧边界发生，命令录制因此始终看到完整快照。
/// 活动会话为空或为 Logo 占位页时立即清除所有偏移引用，
/// 防止关闭项目后继续展示上一张谱面的顶点或滚动位置。
/// 准备结果本身为空时也同步清理偏移基线，下一次有效快照将重新对齐。
/// @warning UI 帧边界热路径：只移动共享快照引用，不得等待逻辑线程。
void PreviewCanvas::swapPreparedUiFrameData()
{
    // 调度器可能在无需准备的帧仍调用交换；没有新结果时保持当前
    // 快照不变，避免窗口间歇隐藏造成内容闪烁。
    if ( !m_hasPreparedSnapshot ) {
        return;
    }

    auto&         engine      = Logic::EditorEngine::instance();
    const int32_t activeIndex = engine.getActiveSessionIndex();
    const auto*   activeEntry = engine.getSessionEntry(activeIndex);
    if ( !activeEntry || activeEntry->isLogoPlaceholder ) {
        // 项目级占位页不拥有可预览谱面，清除当前与偏移快照，
        // 同时消费本次 prepared 标志，防止同一空结果重复交换。
        m_currentSnapshot     = nullptr;
        m_lastOffsetSnapshot  = nullptr;
        m_lastAppliedYOffset  = 0.0f;
        m_hasPreparedSnapshot = false;
        return;
    }

    // 三项状态来自同一次 prepareCanvasSnapshot 调用，必须成组交换，
    // 否则顶点快照与 Y 偏移基线不匹配会造成预览瞬时跳动。
    m_currentSnapshot     = m_preparedSnapshot.snapshot;
    m_lastOffsetSnapshot  = m_preparedSnapshot.offsetSnapshot;
    m_lastAppliedYOffset  = m_preparedSnapshot.appliedYOffset;
    m_hasPreparedSnapshot = false;

    if ( !m_currentSnapshot ) {
        // 空快照不能作为下一帧的增量偏移基线；显式归零使恢复时
        // 从逻辑线程的绝对位置重新开始。
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }
}

/// @brief 将预览离屏目标尺寸变化通知逻辑相机。
/// @param oldW 变化前物理宽度。
/// @param oldH 变化前物理高度。
/// @param w 变化后物理宽度。
/// @param h 变化后物理高度。
/// @details 事件以 cameraId 路由，逻辑线程据此更新预览投影；
/// 此回调不直接修改渲染快照或相机对象。
/// @warning 低频资源尺寸变化路径：只发布事件，不等待重建完成。
void PreviewCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

/// @brief 读取皮肤为预览画布指定的 SPIR-V shader 模块。
/// @param shader_name Canvas 配置中的 shader 模块键。
/// @return 按顶点、可选几何、片段阶段排列的二进制内容；失败时为空。
/// @details 预览沿用 m_canvasName 对应的 BasicCanvas 配置，
/// 但通过独立 shader 名称空间建立自己的 Vulkan pipeline 缓存。
/// 首次读取会检查目录并加载必需的顶点与片段模块；
/// GeometryShader.spv 存在且检查无错误时插入中间阶段。
/// 成功结果缓存在实例内，直到皮肤重载显式使缓存失效。
/// 文件系统错误通过返回空结果表达，不使用异常控制流。
/// @warning 低频 shader 创建路径：包含文件系统查询和文件读取，
/// 严禁从每帧 UI 更新或 Vulkan 命令录制路径调用。
std::vector<std::string> PreviewCanvas::getShaderSources(
    const std::string& shader_name)
{
    // 缓存命中直接返回已加载字节，避免多个 pipeline 查询重复访问磁盘。
    if ( m_shaderSourceCache.count(shader_name) ) {
        return m_shaderSourceCache[shader_name];
    }

    // 预览窗口复用对应 BasicCanvas 的皮肤 shader 配置；m_canvasName
    // 而非固定键允许不同画布主题拥有各自的模块集合。
    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);

    if ( canvas_config.canvas_name == "" ) {
        // 缺失 Canvas 配置属于可诊断的皮肤问题，返回空集合交由
        // 上层 pipeline 创建流程采用其既有失败处理。
        XERROR("PreviewCanvas: 无法获取 {} 的着色器配置", m_canvasName);
        return {};
    }

    if ( auto it = canvas_config.canvas_shader_modules.find(shader_name);
         it != canvas_config.canvas_shader_modules.end() ) {

        auto            path = it->second;
        std::error_code shaderPathError;
        // 使用 error_code 重载避免文件系统异常越过项目的无异常边界。
        if ( !std::filesystem::exists(path, shaderPathError) ||
             shaderPathError ) {
            return {};
        }

        std::string vs = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "VertexShader.spv"));
        std::string fs = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "FragmentShader.spv"));

        std::vector<std::string> result;
        auto                     gsPath = path / "GeometryShader.spv";
        std::error_code          geometryShaderPathError;
        // 几何阶段是可选项，但顶点和片段阶段顺序始终固定，
        // 以匹配 IRenderableView 的 pipeline 阶段装配契约。
        if ( std::filesystem::exists(gsPath, geometryShaderPathError) &&
             !geometryShaderPathError ) {
            result = { vs,
                       Graphic::VKShader::readFile(Config::pathToUtf8(gsPath)),
                       fs };
        } else {
            result = { vs, fs };
        }

        // 只缓存成功找到的模块键；未知键继续返回空，皮肤重载后仍可
        // 在同一实例中新增该键而不受负缓存影响。
        m_shaderSourceCache[shader_name] = result;
        return result;
    }

    return {};
}

/// @brief 生成预览视图专用的 shader pipeline 缓存名。
/// @param shader_module_name 皮肤中的模块名称。
/// @return 带 PreviewCanvas 前缀的稳定名称。
/// @details 前缀避免与主画布使用相同模块名时错误复用 pipeline。
std::string PreviewCanvas::getShaderName(const std::string& shader_module_name)
{
    return "PreviewCanvas:" + shader_module_name;
}

/// @brief 清空缓存的 shader 源码。
/// @details 皮肤切换后旧 SPIR-V 字节不能继续参与 pipeline 重建；
/// 图形管理器会在后续按需调用 getShaderSources 重新读取。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void PreviewCanvas::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

/// @brief 查询预览纹理图集是否仍等待重载。
/// @return reloadTextures 尚未完成当前资源代际时返回 true。
/// @warning 渲染调度热路径：只读取布尔状态，不触发文件或 GPU 操作。
bool PreviewCanvas::needReload()
{
    return m_needReload;
}

/// @brief 使用当前皮肤资源重建预览画布纹理图集。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 上传纹理使用的命令池。
/// @param queue 执行纹理上传的队列。
/// @details 固定纹理 ID 与主画布保持一致，使逻辑快照中的 DrawCmd
/// 可以被两个视图直接消费；序列帧继续采用 SkinManager 分配的区间。
/// 纯白占位纹理为无贴图几何提供稳定采样，Background 则由独立路径处理。
/// 图集完成后将全部有效 UV 一次性发布给预览 cameraId，
/// 逻辑线程随后生成与新图集坐标一致的顶点数据。
/// @warning 低频资源重载路径：包含磁盘读取、GPU 上传和图集构建，
/// 只能在渲染器安排的安全资源重建阶段调用。
void PreviewCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 保存设备句柄供 IRenderableView 后续资源查询使用；这里只借用
    // Vulkan 对象，不改变其所有权或销毁顺序。
    m_physicalDevice = physicalDevice;
    m_logicalDevice  = logicalDevice;
    m_cmdPool        = cmdPool;
    m_queue          = queue;

    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);

    // None 纹理为 4x4 纯白 RGBA，颜色型几何可通过顶点色调制，
    // 无需为每条线或纯色矩形创建独立图像。
    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::None), white, 4, 4);

    auto& skin   = Config::SkinManager::instance();
    auto  addTex = [&](Common::Render::TextureID id, const std::string& key) {
        // 空路径表示皮肤未提供可选资源；保持该 ID 缺失，逻辑与
        // 渲染层会按各自兼容规则回退，不把空路径交给加载器。
        auto p = skin.getAssetPath(key);
        if ( !p.empty() )
            m_textureAtlas->addTexture(static_cast<uint32_t>(id), p);
    };

    addTex(Common::Render::TextureID::Note, "note.note");
    // 与主画布使用同一可选长条头；缺失时由渲染器兼容旧皮肤。
    if ( const auto it = skin.getData().assetPaths.find("note.holdhead");
         it != skin.getData().assetPaths.end() && !it->second.empty() ) {
        addTex(Common::Render::TextureID::HoldHead, "note.holdhead");
    }
    addTex(Common::Render::TextureID::Node, "note.node");
    addTex(Common::Render::TextureID::HoldEnd, "note.holdend");
    addTex(Common::Render::TextureID::HoldBodyVertical,
           "note.holdbodyvertical");
    addTex(Common::Render::TextureID::HoldBodyHorizontal,
           "note.holdbodyhorizontal");
    addTex(Common::Render::TextureID::FlickArrowLeft, "note.arrowleft");
    addTex(Common::Render::TextureID::FlickArrowRight, "note.arrowright");
    addTex(Common::Render::TextureID::Track, "panel.track.background");
    addTex(Common::Render::TextureID::JudgeArea, "panel.track.judgearea");
    addTex(Common::Render::TextureID::Logo, "logo");

    // 序列帧 ID 区间由 SkinManager 统一分配；按声明顺序连续装入，
    // 使逻辑快照只需携带当前帧的整数纹理 ID。
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            m_textureAtlas->addTexture(currentId++, frame);
        }
    }

    // 单张 4096 图集减少预览命令录制中的 descriptor 切换；build
    // 完成前不能发布 UV，因为打包器可能重新排列每张纹理的位置。
    m_textureAtlas->build(4096);

    m_atlasUVs.clear();
    // 固定枚举区间与快照协议一致；Background 由画布背景通道绑定，
    // 不能错误指向当前图集中的默认区域。
    for ( uint32_t i = static_cast<uint32_t>(Common::Render::TextureID::None);
          i <= static_cast<uint32_t>(Common::Render::TextureID::Logo);
          ++i ) {
        if ( i == static_cast<uint32_t>(Common::Render::TextureID::Background) )
            continue;
        m_atlasUVs[i] = m_textureAtlas->getUV(i);
    }

    // 可选头部位于既有连续纹理区之后，必须显式发布给逻辑线程。
    // 只发布已加载且尺寸有效的区域，旧皮肤继续通过缺失项回退到 Note。
    if ( const auto it = skin.getData().assetPaths.find("note.holdhead");
         it != skin.getData().assetPaths.end() && !it->second.empty() ) {
        const auto id =
            static_cast<uint32_t>(Common::Render::TextureID::HoldHead);
        const auto uv = m_textureAtlas->getUV(id);
        if ( uv.z > 0.0F && uv.w > 0.0F ) m_atlasUVs[id] = uv;
    }


    // 固定纹理之后补充动态序列帧 UV；每个 ID 必须与上面的加载
    // 顺序一致，效果系统才能通过 startId + frameIndex 直接寻址。
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        for ( uint32_t i = 0; i < seq.frames.size(); ++i ) {
            uint32_t id    = seq.startId + i;
            m_atlasUVs[id] = m_textureAtlas->getUV(id);
        }
    }

    // UV 映射按预览 cameraId 发布，避免主画布的皮肤切换状态与
    // 预览快照生成相互覆盖。
    Logic::EditorEngine::instance().setAtlasUVMap(m_cameraId, m_atlasUVs);

    m_needReload = false;
}

/// @brief 取得当前预览快照的顶点数组。
/// @return 快照顶点的只读引用；无快照时返回进程期空数组。
/// @warning Vulkan 上传热路径：不得在此复制容器或触发快照准备。
const std::vector<Graphic::Vertex::VKBasicVertex>&
PreviewCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

/// @brief 取得当前预览快照的索引数组。
/// @return 快照索引的只读引用；无快照时返回进程期空数组。
/// @warning Vulkan 上传热路径：返回引用的生命周期依赖当前帧快照。
const std::vector<uint32_t>& PreviewCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

/// @brief 录制预览画布离屏绘制命令。
/// @warning 热路径：每帧命令录制时执行；只遍历快照命令列表并复用 descriptor。
void PreviewCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBound = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;

    bool additiveBlend = false;
    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        // 图集纹理可共用描述符，但混合模式改变时必须切换兼容管线。
        if ( additiveBlend != cmd.additiveBlend ) {
            bindMainBlendPipeline(cmdBuf, cmd.additiveBlend);
            additiveBlend = cmd.additiveBlend;
        }
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;

        if ( tex != lastBound ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &tex,
                                      0,
                                      nullptr);
            lastBound = tex;
        }

        if ( cmd.scissor != lastScissor ) {
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
    // 恢复普通绘制，避免后续覆盖层继承发光状态。
    if ( additiveBlend ) bindMainBlendPipeline(cmdBuf, false);
}

/// @brief 记录预览画布最终覆盖层离屏绘制命令。
/// @warning 热路径：每帧命令录制末尾执行；仅遍历 overlay 命令并复用已有描述符。
void PreviewCanvas::onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                                        vk::PipelineLayout      pipelineLayout,
                                        vk::DescriptorSetLayout setLayout,
                                        vk::DescriptorSet defaultDescriptor,
                                        uint32_t          frameIndex)
{
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBound = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;

    for ( const auto& cmd : m_currentSnapshot->overlayCmds ) {
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;

        if ( tex != lastBound ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &tex,
                                      0,
                                      nullptr);
            lastBound = tex;
        }

        if ( cmd.scissor != lastScissor ) {
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

/// @brief 判断当前预览快照是否包含最终覆盖层绘制命令。
/// @return 当前快照存在覆盖层命令时返回 true。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取快照命令数量。
bool PreviewCanvas::hasOverlayDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->overlayCmds.empty();
}

}  // namespace MMM::Canvas
