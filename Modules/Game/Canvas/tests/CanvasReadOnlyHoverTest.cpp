#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/PlanarObservationCamera.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"

#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace
{
/// @brief 运行左右窗口实际 ImGui 帧，强制左侧聚焦并检视右侧。
/// @param interaction 绑定右侧相机的生产交互控制器。
/// @param snapshot 右侧物件与分拍快照，不借用左侧数据。
/// @param pointer 鼠标屏幕位置。
/// @note 每帧显式聚焦左侧，不能靠右窗意外获得焦点通过测试。
/// 鼠标事件在帧前排队，窗口悬停使用生产 ImGui 命中结果。
/// 右窗使用独立位置和尺寸，不能被左窗遮挡。
/// 浮层不得接收输入，结束绘制后导航焦点仍应在左窗。
/// 不创建 GPU 后端，字体顶点用于验证文本实际进入绘制列表。
/// 返回内容区屏幕原点，使合成的鼠标事件与 ImGui 实际布局一致。
/// 所有拾取和坐标发布仍由生产控制器完成，测试只负责驱动窗口帧。
/// activeUpdate 为真时走完整 update 调度，后台帧继续走只读悬停入口。
/// leftButtonDown 仅在显式给值时注入边沿，空值用于保持上一帧按钮状态。
/// 固定右窗几何让用例不依赖平台窗口管理器或 GPU 后端。
ImVec2 frame(MMM::Canvas::Basic2DCanvasInteraction&     interaction,
             const MMM::Common::Render::RenderSnapshot& snapshot,
             ImVec2 pointer, bool activeUpdate = false,
             std::optional<bool> leftButtonDown = std::nullopt)
{
    // 鼠标事件在帧开始前排队，与 GLFW 后端时序一致。
    ImGui::GetIO().AddMousePosEvent(pointer.x, pointer.y);
    // 显式提交按钮边沿，让真实 ImGui 状态机产生 click、drag 和 release。
    // 空值不重放按钮事件，移动帧沿用 ImGui 当前的物理按下状态。
    if ( leftButtonDown )
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,
                                           *leftButtonDown);
    // 输入事件必须在 NewFrame 前排队，确保 IsMouseClicked 等读取本帧边沿。
    ImGui::NewFrame();
    constexpr auto flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;
    // 两个窗口完全分离，右窗口 Begin 不应自动抢走左窗口焦点。
    // 尺寸故意大于 280x380 的交互视口，保留边距但不遮挡目标区域。
    ImGui::SetNextWindowPos({ 0.0F, 0.0F });
    ImGui::SetNextWindowSize({ 300.0F, 400.0F });
    ImGui::Begin("Left", nullptr, flags);
    // 左窗聚焦后 End 只结束绘制，不改变导航所属窗口。
    ImGui::SetWindowFocus();
    ImGui::End();
    ImGui::SetNextWindowPos({ 350.0F, 0.0F });
    ImGui::SetNextWindowSize({ 300.0F, 400.0F });
    // 右窗 Begin 只切换绘制目标，不能显式聚焦。
    ImGui::Begin("Right", nullptr, flags);
    // 返回真实内容原点，让透视样本以生产窗口局部坐标生成。
    const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
    if ( activeUpdate ) {
        // 活动路径仍经过生产 update 调度，UI 管理器可为空且没有待处理拖放。
        // 本用例没有投递文件拖放，handleDrops 会在解引用管理器前立即返回。
        interaction.update(nullptr, &snapshot, 280.0F, 380.0F);
    } else {
        // 后台只读入口同步鼠标、拾取物件和绘制提示，不执行完整编辑流程。
        interaction.updateReadOnlyHover(&snapshot, 280.0F, 380.0F);
    }
    ImGui::End();
    // 固化当前帧窗口状态和绘制数据，随后检查真实显示结果。
    // 返回 origin 的值副本可供下一次事件排队，不保留 ImGui 临时引用。
    ImGui::Render();
    return contentOrigin;
}

/// @brief 保存生产只读入口发布的鼠标与悬停命令。
/// @details 记录整条命令而非仅实体，便于核对相机路由和逆投影坐标。
/// 每个阶段可以清空记录器，但控制器内部仍按复合候选身份去重。
/// 因此场景阶段交界处显式离开画布，保证新阶段观察到全新的命令。
/// 活动拖动命令单独保存，避免把它们误算为只读流程的编辑副作用。
/// 未显式归类的命令仍会触发 editing 标记，覆盖意外扩展的事件种类。
struct CommandCapture {
    std::vector<MMM::Logic::CmdSetHoveredEntity> hovered;
    std::vector<MMM::Logic::CmdSetMousePosition> mousePositions;
    std::vector<MMM::Logic::CmdStartDrag>        startDrags;
    std::vector<MMM::Logic::CmdUpdateDrag>       updateDrags;
    std::vector<MMM::Logic::CmdEndDrag>          endDrags;
    std::vector<MMM::Logic::LogicCommand>        orderedCommands;
    bool                                         editing{ false };
};

/// @brief 将平面样本正向投影为真实屏幕指针位置。
/// @param projection 当前真实相机矩阵构造的输入映射。
/// @param scenePosition 期望逆投影恢复的平面坐标。
/// @param contentOrigin ImGui 返回的画布内容区屏幕原点。
/// @return 指针屏幕坐标；生产投影无法显示样本时返回空值。
/// @note 该辅助只将几何样本变成平台屏幕事件，不实现逆变换或命中判断。
/// @note 屏幕投影失败时保留空值，让用例明确失败而不是退回猜测坐标。
std::optional<ImVec2> pointerForScenePosition(
    const MMM::Canvas::PlanarObservationInputProjection& projection,
    glm::vec2 scenePosition, ImVec2 contentOrigin)
{
    const auto screen =
        MMM::Canvas::planarObservationProject(projection, scenePosition);
    if ( !screen ) return std::nullopt;
    return ImVec2{ contentOrigin.x + screen->x, contentOrigin.y + screen->y };
}

/// @brief 计算 ImGui 像素量化后实际指针位置对应的平面坐标。
/// @param projection 与交互控制器相同的输入映射。
/// @param pointer 正向投影生成的绝对屏幕指针位置。
/// @param contentOrigin ImGui 返回的画布内容区屏幕原点。
/// @return 对真实整数像素逆投影的结果；逆映射无效时返回空值。
/// @details ImGui 在 NewFrame 将 MousePos 向下取整，测试期望也必须使用该像素。
/// 对屏幕绝对坐标取整后再减内容区原点，与生产入口的处理顺序一致。
/// 不能先取整局部浮点值，因为内容原点也参与屏幕像素量化。
std::optional<glm::vec2> scenePositionForImGuiPointer(
    const MMM::Canvas::PlanarObservationInputProjection& projection,
    ImVec2 pointer, ImVec2 contentOrigin)
{
    // 生产输入消费的是 NewFrame 取整后的屏幕点，而不是注入的浮点值。
    const glm::vec2 screenLocal{ std::floor(pointer.x) - contentOrigin.x,
                                 std::floor(pointer.y) - contentOrigin.y };
    return MMM::Canvas::planarObservationUnproject(projection, screenLocal);
}
}  // namespace

/// @brief 验证非聚焦窗口检视实际显示且视觉命令不会误投活动窗口。
/// @details 运行真实交互控制器，不能以复制一份显示条件函数代替生产入口。
/// 事件监听核对物件的目标相机，后台实体 ID 不能投递给活动谱面。
/// 窗口检查核对提示活跃状态与文字顶点，避免仅找到旧窗口就宣称显示。
/// 鼠标离开后核对空实体与浮层隐藏，防止详情残留。
/// 测试不初始化 EditorEngine 会话，视觉事件只记录不修改项目。
/// 所有控制器和订阅先于 ImGui 上下文释放，避免跨用例状态泄漏。
/// 初始二维阶段保留整幅视口 hitbox，维持既有提示、焦点和目标相机断言。
/// 后续阶段缩小 hitbox，使透视屏幕点明确落在未变换逻辑框之外。
/// 每项扩展回归独立切换输入状态，分别检查命中、命令坐标和清理行为。
/// 测试使用当前矩阵将已知场景坐标投影成 ImGui 屏幕事件，并检查逆向结果。
/// 屏幕离窗和逆变换无解各自验证清理，避免只覆盖单一 hover 退出原因。
/// 活动阶段注入真实 ImGui 按钮边沿，再由 update() 推进 Move 工具状态。
/// 同一手势跨出窗口释放，随后以关闭的投影缓存检查活动二维兼容路径。
/// 释放帧的最后位置由另一个场景坐标正向投影生成，不能重用上一帧指针。
/// 捕获命令按 EventBus 实际发布顺序保存，用于检查最终更新和结束的先后关系。
/// 有序副本仅用于本地断言，不改变发布事件，也不依赖异步逻辑线程完成时机。
/// 按下时验证逆投影拾取静态 hitbox 的实体，并检查 StartDrag 锁存目标。
/// 移动时分别检查逻辑拖拽坐标和鼠标状态坐标，二者必须保持一致。
/// 释放后额外推进一帧，确保事务结束命令仅由 release 边沿产生一次。
/// 用例只有一个本地实体与两处 hitbox 样本，不启动 EditorEngine 项目会话。
/// update() 首次访问的引擎实例只准备配置与事件路由，不启动逻辑线程。
/// 当前路径也不创建会话；快照提供活动 Move 入口所需的完整最小状态。
/// @return 默认窗口聚焦、物件拾取、退出清理及无编辑副作用全部成立时返回零。
int main()
{
    ImGui::CreateContext();
    // 独立上下文避免继承别的用例的悬停窗口和导航焦点。
    auto& io = ImGui::GetIO();
    // 测试不读写用户布局，也不创建平台或 GPU 资源。
    io.IniFilename        = nullptr;
    io.DisplaySize        = { 800.0F, 600.0F };
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 图集数据由 ImGui 持有，不上传 GPU，也不由测试释放。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool passed = false;
    // 套件 runner 只把非零返回码显示为失败，不会展开函数内部的断言。
    // 每个检查阶段独立记录标签，避免只有退出状态却隐藏实际失败位置。
    // 检查逐项执行并累积结果，尽量一次运行覆盖多个独立坐标契约。
    // 日志只在阶段失败时输出，正常 CTest 输出保持安静。
    const auto requireStage = [](bool condition, const char* stage) {
        if ( !condition ) XERROR("CanvasReadOnlyHoverTest 阶段失败：{}", stage);
        return condition;
    };
    {
        MMM::Canvas::Basic2DCanvasInteraction interaction(
            "Right", "Basic2DCanvas_Right");
        MMM::Common::Render::RenderSnapshot snapshot;
        snapshot.hasBeatmap       = true;
        snapshot.isHoveringCanvas = true;
        snapshot.isSnapped        = true;
        snapshot.trackCount       = 4;
        // Move 模式只有分拍或物件详情时显示，避免绘制工具常显分支掩盖问题。
        snapshot.currentTool = MMM::Logic::EditTool::Move;
        // 完整物件详情与分拍来源都属于右侧快照，避免用空提示通过测试。
        snapshot.hoverInspect.show = true;
        snapshot.hoverInspect.kind =
            MMM::Common::Render::HoverInspectKind::Note;
        snapshot.hoverInspect.head.show      = true;
        snapshot.hoverInspect.head.beatIndex = 12;
        snapshot.hoverInspect.head.numerator = 6;
        // 使用非平凡分拍，不依靠空的悬浮信息通过测试。
        snapshot.hoverInspect.head.denominator = 11;
        const auto entity                      = static_cast<entt::entity>(77);
        // 包围盒使用右窗局部坐标，实体只在所属谱面有意义。
        snapshot.hitboxes.push_back({ .entity = entity,
                                      .x      = 0.0F,
                                      .y      = 0.0F,
                                      .w      = 280.0F,
                                      .h      = 380.0F });

        // 分别记录目标命中、退出清空和编辑副作用，不用单一成功标志替代。
        CommandCapture capture;
        auto&          bus = MMM::Event::EventBus::instance();
        // 监听周期仅覆盖当前控制器，不给下一测试留下悬浮状态。
        // 鼠标位置和实体悬停分开保存，因为两者在播放插值时有不同坐标契约。
        // 未预期的其它逻辑命令意味着只读查看路径进入了编辑处理。
        const auto subscription = bus.subscribe<MMM::Event::LogicCommandEvent>(
            [&](const MMM::Event::LogicCommandEvent& event) {
                capture.orderedCommands.push_back(event.command);
                if ( const auto* hover =
                         std::get_if<MMM::Logic::CmdSetHoveredEntity>(
                             &event.command) ) {
                    capture.hovered.push_back(*hover);
                } else if ( const auto* mouse =
                                std::get_if<MMM::Logic::CmdSetMousePosition>(
                                    &event.command) ) {
                    capture.mousePositions.push_back(*mouse);
                } else if ( const auto* start =
                                std::get_if<MMM::Logic::CmdStartDrag>(
                                    &event.command) ) {
                    capture.startDrags.push_back(*start);
                } else if ( const auto* update =
                                std::get_if<MMM::Logic::CmdUpdateDrag>(
                                    &event.command) ) {
                    capture.updateDrags.push_back(*update);
                } else if ( const auto* end =
                                std::get_if<MMM::Logic::CmdEndDrag>(
                                    &event.command) ) {
                    capture.endDrags.push_back(*end);
                } else {
                    // 只读入口没有点击、选中、拖动、播放或项目切换命令。
                    capture.editing = true;
                }
            });
        // 首帧建立窗口；鼠标命令仍用画布内点让既有详情绘制断言保持有效。
        // 多帧允许无平台后端的 ImGui 状态建立稳定窗口和悬停结果。
        // 辅助函数每帧都重新聚焦左窗，检测后台窗没有接管导航焦点。
        ImVec2 contentOrigin{};
        for ( int index = 0; index < 3; ++index )
            contentOrigin = frame(interaction, snapshot, { 500.0F, 200.0F });
        const auto* overlay =
            ImGui::FindWindowByName("##CanvasHoverInspectOverlay");
        const bool baselineTargeted =
            std::any_of(capture.hovered.begin(),
                        capture.hovered.end(),
                        [entity](const auto& hover) {
                            return hover.entity == entity &&
                                   hover.cameraId == "Basic2DCanvas_Right";
                        });
        const auto baselineMouse =
            std::find_if(capture.mousePositions.begin(),
                         capture.mousePositions.end(),
                         [](const auto& command) {
                             return command.cameraId == "Basic2DCanvas_Right";
                         });
        // 基线仍要求非聚焦窗口收到物件详情，且绘制列表包含真实文字顶点。
        // 新的窄 hitbox 只在基线断言完成后设置，避免改变原回归覆盖面。
        // 默认二维输入还需维持原局部像素值，并保持非逆投影命令标志。
        // 以真实内容原点计算期望值，不假设窗口装饰或 padding 的固定高度。
        // 提示、绘制顶点和左窗焦点必须同时成立。
        // 原二维窗口是兼容基线，失败应与新投影路径分开定位。
        passed = requireStage(
            baselineTargeted && !capture.editing && overlay &&
                overlay->Active && overlay->DrawList->VtxBuffer.Size > 0 &&
                GImGui->NavWindow == ImGui::FindWindowByName("Left") &&
                baselineMouse != capture.mousePositions.end() &&
                !baselineMouse->isUnprojected &&
                std::abs(baselineMouse->mouseX - (500.0F - contentOrigin.x)) <
                    0.2F &&
                std::abs(baselineMouse->mouseY - (200.0F - contentOrigin.y)) <
                    0.2F,
            "原二维悬停与绘制基线");

        // 斜视矩阵同时启用倾角、方位和距离，避免屏幕点意外仍落在原 hitbox。
        snapshot.hitboxes.front().x = 100.0F;
        snapshot.hitboxes.front().y = 145.0F;
        snapshot.hitboxes.front().w = 16.0F;
        snapshot.hitboxes.front().h = 20.0F;
        const auto cameraMatrix     = MMM::Canvas::planarObservationProjection(
            280.0F, 380.0F, 0.0F, 32.0F, 27.0F, 1.6F);
        // 相机矩阵与画布逻辑视口同尺寸，使用生产相机工厂构造场景到裁剪变换。
        // 非零倾角改变竖直尺度，yaw 改变平面方向，距离产生透视比例差异。
        const auto inputProjection =
            MMM::Canvas::makePlanarObservationInputProjection(
                cameraMatrix, 280.0F, 380.0F, true);
        // 该缓存与生成鼠标像素的矩阵相同，往返测试不会有意引入坐标偏差。
        // 悬停函数仍须从 ImGui 的绝对屏幕位置减去真实内容区原点再做逆变换。
        interaction.setInputProjection(inputProjection);
        // 先退出旧的二维 hover，避免相同实体的候选去重让新映射缺少独立事件。
        frame(interaction, snapshot, { 100.0F, 200.0F });
        const glm::vec2 hitScene{ 108.0F, 155.0F };
        const auto      projectedPointer =
            pointerForScenePosition(inputProjection, hitScene, contentOrigin);
        // 目标位置取窄框中心，正投影生成屏幕事件，生产路径必须反向恢复位置。
        // 如果点不在相机前方或映射不可用，测试应直接失败，不改用固定像素点。
        // 正投影失败时显式留下阶段日志，后续跳过不能误作成功。
        passed &=
            requireStage(projectedPointer.has_value(), "透视命中点正投影");
        if ( projectedPointer ) {
            // 先从生成的绝对屏幕位置还原局部坐标，只用它验证夹具区分度。
            const glm::vec2 projectedLocal{
                projectedPointer->x - contentOrigin.x,
                projectedPointer->y - contentOrigin.y
            };
            // 该约束让直接使用屏幕像素查询 hitbox 的实现必定失去目标。
            // 坐标按实际内容原点换算，不用窗口左上角近似画布局部位置。
            // 若旧实现直接用屏幕像素查逻辑 hitbox，此样本必须明确落在框外。
            passed &= requireStage(
                projectedLocal.x < snapshot.hitboxes.front().x ||
                    projectedLocal.x > snapshot.hitboxes.front().x +
                                           snapshot.hitboxes.front().w ||
                    projectedLocal.y < snapshot.hitboxes.front().y ||
                    projectedLocal.y > snapshot.hitboxes.front().y +
                                           snapshot.hitboxes.front().h,
                "透视屏幕点与原始 hitbox 可区分");
            // 非零相机角度让目标屏幕像素与 scene 坐标有明显差异。
            // 这个额外断言保护夹具本身，避免未来参数修改削弱回归辨别力。
            capture       = {};
            contentOrigin = frame(interaction, snapshot, *projectedPointer);
            passed &= requireStage(
                std::any_of(capture.hovered.begin(),
                            capture.hovered.end(),
                            [entity](const auto& hover) {
                                return hover.entity == entity &&
                                       hover.cameraId == "Basic2DCanvas_Right";
                            }),
                "透视逆映射命中目标实体与相机");
            passed &= requireStage(
                !capture.editing &&
                    GImGui->NavWindow == ImGui::FindWindowByName("Left"),
                "只读透视悬停不编辑且不抢焦点");
            // 再次检查导航焦点，确保只读拾取没有改变活动输入所属窗口。
        }

        // 动态顶点比静态 hitbox 下移 34
        // 像素：命中补偿只用于拾取，逻辑鼠标保留显示 Y。
        constexpr float playbackYOffset = 34.0F;
        interaction.setInputProjection(inputProjection, playbackYOffset);
        const glm::vec2 dynamicScene{ 108.0F, 155.0F + playbackYOffset };
        // hitbox 保持静态位置，屏幕事件则对应下移后的动态顶点显示位置。
        // 此时只有物件命中坐标可以撤销插值，鼠标逻辑坐标必须保留显示 Y。
        // 动态样本独立检查播放插值补偿，不复用静态投影点。
        const auto dynamicPointer = pointerForScenePosition(
            inputProjection, dynamicScene, contentOrigin);
        passed &= requireStage(dynamicPointer.has_value(), "播放偏移点正投影");
        const auto dynamicEventScene =
            dynamicPointer
                ? scenePositionForImGuiPointer(
                      inputProjection, *dynamicPointer, contentOrigin)
                : std::nullopt;
        // 坐标断言以量化后的真实输入为基准，而非投影前的浮点场景样本。
        // 静态 hitbox 仍由未量化的中心场景样本定义，保持命中覆盖稳定。
        passed &= requireStage(dynamicEventScene.has_value(),
                               "播放偏移事件像素逆投影");
        // 若此时逆投影无效，仍保留阶段诊断并跳过依赖该点的局部断言。
        // 后续独立清理与二维回退会继续执行，减少一次运行中的信息损失。
        if ( dynamicPointer ) {
            // 清空静态命中候选，确保动态样本必须重新经过插值补偿分支。
            // 两阶段采用相同矩阵，只改变动态顶点位置与快照插值量。
            frame(interaction, snapshot, { 100.0F, 200.0F });
            const glm::vec2 dynamicLocal{ dynamicPointer->x - contentOrigin.x,
                                          dynamicPointer->y - contentOrigin.y };
            // 动态位置也与静态 hitbox 分离，否则缺失插值补偿仍会偶然命中。
            passed &= requireStage(
                dynamicLocal.y < snapshot.hitboxes.front().y ||
                    dynamicLocal.y > snapshot.hitboxes.front().y +
                                         snapshot.hitboxes.front().h,
                "动态屏幕点与原始 hitbox 可区分");
            // 两个位置都在真实 ImGui 窗口内，命中差异只能来自逆投影和插值处理。
            capture = {};
            frame(interaction, snapshot, *dynamicPointer);
            const auto hit = std::find_if(
                capture.hovered.begin(),
                capture.hovered.end(),
                [entity](const auto& hover) { return hover.entity == entity; });
            passed &= requireStage(hit != capture.hovered.end() &&
                                       hit->cameraId == "Basic2DCanvas_Right",
                                   "播放插值偏移后命中目标");
            const auto mouse = std::find_if(capture.mousePositions.begin(),
                                            capture.mousePositions.end(),
                                            [](const auto& command) {
                                                return command.cameraId ==
                                                       "Basic2DCanvas_Right";
                                            });
            // 逆投影结果应包含显示平面偏移；若又减一次会回到静态 hitbox Y。
            // 详细值可区分重复减偏移与屏幕像素量化造成的细小误差。
            if ( mouse != capture.mousePositions.end() && dynamicEventScene &&
                 (std::abs(mouse->mouseX - dynamicEventScene->x) >= 0.2F ||
                  std::abs(mouse->mouseY - dynamicEventScene->y) >= 0.2F) )
                XERROR("播放悬停坐标不符：期望 ({}, {}), 实际 ({}, {})",
                       dynamicEventScene->x,
                       dynamicEventScene->y,
                       mouse->mouseX,
                       mouse->mouseY);
            passed &= requireStage(
                mouse != capture.mousePositions.end() && dynamicEventScene &&
                    mouse->isUnprojected &&
                    std::abs(mouse->mouseX - dynamicEventScene->x) < 0.2F &&
                    std::abs(mouse->mouseY - dynamicEventScene->y) < 0.2F &&
                    std::abs(mouse->mouseY -
                             (dynamicEventScene->y - playbackYOffset)) >
                        playbackYOffset / 2.0F,
                "播放偏移不改变逆投影鼠标事件坐标");
            // 二次减偏移会让鼠标 Y 回到 155，错误分拍仍可能伴随实体命中。
            // 命令标志同时证明坐标来自透视逆映射，而非默认二维像素值。
        }

        // 鼠标回到左侧后，右侧悬浮应清空，旧提示不能残留。
        // 该阶段同时检查命令和绘制提示，便于区分事件清理与视觉清理。
        // 重新记录退出动作，不能用初始空候选的清理掩盖离开窗口后未清空。
        capture = {};
        // 左侧屏幕点实际位于右窗之外，检查 ImGui 窗口状态参与清理判断。
        frame(interaction, snapshot, { 100.0F, 200.0F });
        // 初次提示创建失败可能为空，应返回失败而非解引用空指针。
        passed &= requireStage(
            std::any_of(capture.hovered.begin(),
                        capture.hovered.end(),
                        [](const auto& hover) {
                            return hover.entity == entt::null &&
                                   hover.cameraId == "Basic2DCanvas_Right";
                        }) &&
                overlay && !overlay->Active && !capture.editing,
            "离开窗口后清空悬停与提示");

        // 先重新命中，再将真实指针移到窗口外；hover 必须在本帧清空。
        // 这时逆映射仍有效，清理原因仅应是 ImGui 的实际窗口边界。
        if ( dynamicPointer ) {
            // 先恢复物件 hover，再离开右窗，排除先前清理帧的事件干扰。
            // 当前插值偏移仍为 34，前态必须使用动态顶点对应的显示位置。
            frame(interaction, snapshot, *dynamicPointer);
            capture = {};
            // x=700 超过右窗右边界，窗口悬停应为 false，即使场景逆映射仍有效。
            frame(interaction, snapshot, { 700.0F, 200.0F });
            passed &= requireStage(
                std::any_of(capture.hovered.begin(),
                            capture.hovered.end(),
                            [](const auto& hover) {
                                return hover.entity == entt::null &&
                                       hover.cameraId == "Basic2DCanvas_Right";
                            }),
                "真实屏幕外清空悬停");
        }

        // 鼠标留在窗口内但逆映射退化时，同样清除旧 hover，不能沿用前一帧实体。
        // 窗口命中仍有效，因此阶段失败表示无效投影没有清理旧状态。
        if ( dynamicPointer ) {
            // 保持指针在右窗内，只让逆映射无效，覆盖另一种独立的失效原因。
            // 重新命中建立清晰前态，下一帧必须观察到明确的空实体事件。
            // 使用动态场景样本，避免有效缓存下因播放偏移导致前态未命中。
            frame(interaction, snapshot, *dynamicPointer);
            const auto invalidProjection =
                MMM::Canvas::makePlanarObservationInputProjection(
                    glm::mat4(0.0F), 280.0F, 380.0F, true);
            // 零矩阵使构造器发布无效缓存，但鼠标仍处于可见窗口范围内。
            // 失败应清空 hover，不能沿用上一帧成功命中的实体。
            interaction.setInputProjection(invalidProjection);
            capture = {};
            frame(interaction, snapshot, *dynamicPointer);
            passed &= requireStage(
                std::any_of(capture.hovered.begin(),
                            capture.hovered.end(),
                            [](const auto& hover) {
                                return hover.entity == entt::null &&
                                       hover.cameraId == "Basic2DCanvas_Right";
                            }),
                "逆投影无效时清空悬停");
        }

        // 默认二维状态忽略 snapshotYOffset，屏幕局部点仍按原 hitbox 命中。
        // 同一控制器经历投影和失效状态后，关闭映射应恢复旧二维语义。
        interaction.setInputProjection(
            MMM::Canvas::PlanarObservationInputProjection{}, 500.0F);
        // 关闭态收到很大的新 offset 也必须忽略，二维命中继续按原逻辑像素解释。
        // 该用例既检查恒等映射仍可使用，也防止新增相机状态要求改变旧调用方。
        capture = {};
        frame(interaction,
              snapshot,
              { contentOrigin.x + hitScene.x, contentOrigin.y + hitScene.y });
        passed &= requireStage(std::any_of(capture.hovered.begin(),
                                           capture.hovered.end(),
                                           [entity](const auto& hover) {
                                               return hover.entity == entity &&
                                                      hover.cameraId ==
                                                          "Basic2DCanvas_Right";
                                           }),
                               "默认二维只读映射忽略播放偏移");
        // 最后再次验证相机路由，避免无效观察状态污染默认输入模式。

        // 活动入口以真实鼠标边沿驱动 Move 拖拽，覆盖 update() 的完整输入路由。
        // 开始、移动、释放分别记录阶段名，能定位完整输入事务的具体断点。
        // 插值偏移只补偿动态顶点 hitbox，开始、更新与鼠标命令保留显示平面坐标。
        interaction.setInputProjection(inputProjection, playbackYOffset);
        // active handler 使用相同播放补偿契约，但事件通过完整编辑调度器进入。
        const glm::vec2 dragStartScene{ 108.0F, 155.0F + playbackYOffset };
        const glm::vec2 dragMoveScene{ 140.0F, 220.0F };
        const glm::vec2 dragReleaseScene{ 900.0F, 190.0F };
        const auto      dragStartPointer = pointerForScenePosition(
            inputProjection, dragStartScene, contentOrigin);
        const auto dragMovePointer = pointerForScenePosition(
            inputProjection, dragMoveScene, contentOrigin);
        const auto dragReleasePointer = pointerForScenePosition(
            inputProjection, dragReleaseScene, contentOrigin);
        const auto dragStartEventScene =
            dragStartPointer
                ? scenePositionForImGuiPointer(
                      inputProjection, *dragStartPointer, contentOrigin)
                : std::nullopt;
        const auto dragMoveEventScene =
            dragMovePointer
                ? scenePositionForImGuiPointer(
                      inputProjection, *dragMovePointer, contentOrigin)
                : std::nullopt;
        const auto dragReleaseEventScene =
            dragReleasePointer
                ? scenePositionForImGuiPointer(
                      inputProjection, *dragReleasePointer, contentOrigin)
                : std::nullopt;
        // 分别计算点击、移动和释放的实际整数屏幕事件对应场景坐标。
        // 这些期望来自生产逆映射，不通过增大坐标容差吸收像素量化。
        // 释放目标位于同一相机可见的画布右侧之外，仍保留有限的逆映射坐标。
        // 两个平面点都通过同一缓存投影，鼠标移动不是手工屏幕像素差值。
        passed &= requireStage(dragStartPointer && dragMovePointer &&
                                   dragReleasePointer && dragStartEventScene &&
                                   dragMoveEventScene && dragReleaseEventScene,
                               "活动拖拽场景点正投影");
        if ( dragStartPointer && dragMovePointer && dragReleasePointer &&
             dragStartEventScene && dragMoveEventScene &&
             dragReleaseEventScene ) {
            // 所有前置点均有效才执行事务操作，失败路径仍能完成上下文清理。
            // 测试订阅监听期覆盖拖动事务，但不把命令送入异步逻辑线程。
            // 先清除上阶段的 hover，再让按下帧建立独立的 active 命中事件。
            // 这样同一实体的候选去重不会隐藏开始拖拽帧的 hover 命令。
            frame(interaction, snapshot, { 100.0F, 200.0F }, true, false);
            capture = {};
            // 只收集按下这一帧的事件，避免此前清理 hover 混入实体判断。
            // 真实鼠标 down 与正投影场景点同帧提交，产生 ImGui click 状态。
            // StartDrag 只在当前工具为 Move、快照停止播放且命中对象时发出。
            frame(interaction, snapshot, *dragStartPointer, true, true);
            const auto activeHover = std::find_if(
                capture.hovered.begin(),
                capture.hovered.end(),
                [entity](const auto& hover) { return hover.entity == entity; });
            // 事件捕获发生在同一帧 update() 内，订阅不依赖异步逻辑线程调度。
            // 若拾取失败，后续 StartDrag 也必须缺失，两个结果都分别显式验证。
            const auto dragStart = std::find_if(
                capture.startDrags.begin(),
                capture.startDrags.end(),
                [entity](const auto& command) {
                    return command.entity == entity &&
                           command.cameraId == "Basic2DCanvas_Right";
                });
            const auto startMouse = std::find_if(
                capture.mousePositions.begin(),
                capture.mousePositions.end(),
                [](const auto& command) {
                    return command.cameraId == "Basic2DCanvas_Right";
                });
            // 鼠标落在动态顶点位置，只有命中比较撤销插值后才能找到静态框。
            // 像素取整后的点击仍应映射回播放顶点对应的静态 hitbox 区域。
            // 开始命令与 hover 都必须指向按下瞬间锁定的同一实体。
            passed &= requireStage(
                activeHover != capture.hovered.end() &&
                    dragStart != capture.startDrags.end() &&
                    startMouse != capture.mousePositions.end() &&
                    startMouse->isUnprojected &&
                    std::abs(startMouse->mouseX - dragStartEventScene->x) <
                        0.2F &&
                    std::abs(startMouse->mouseY - dragStartEventScene->y) <
                        0.2F,
                "透视点击开始拖拽并保留显示平面坐标");
            // 此断言防止仅靠既有 m_lastHoveredEntity 缓存让错误鼠标映射通过。

            // 左键保持按下，鼠标移至第二个已知场景点，应发布匹配逆投影的拖动更新。
            capture = {};
            // 重置本帧捕获器后，UpdateDrag 必须由仍有效的拖拽锁存独立发出。
            frame(interaction, snapshot, *dragMovePointer, true, true);
            const auto dragUpdate = std::find_if(
                capture.updateDrags.begin(),
                capture.updateDrags.end(),
                [](const auto& command) {
                    return command.cameraId == "Basic2DCanvas_Right";
                });
            // 更新帧内寻找目标相机命令，避免别的画布偶然满足坐标断言。
            const auto moveMouse = std::find_if(
                capture.mousePositions.begin(),
                capture.mousePositions.end(),
                [](const auto& command) {
                    return command.cameraId == "Basic2DCanvas_Right";
                });
            // 目标移动超过 ImGui 拖动阈值，避免仅验证按钮维持按下的状态。
            // UpdateDrag 与 MousePosition 共用同一量化输入对应的场景坐标。
            // 更新命令与鼠标位置命令必须使用同一个逆投影后的显示平面点。
            passed &= requireStage(
                capture.updateDrags.size() == 1 &&
                    dragUpdate != capture.updateDrags.end() &&
                    std::abs(dragUpdate->mouseX - dragMoveEventScene->x) <
                        0.2F &&
                    std::abs(dragUpdate->mouseY - dragMoveEventScene->y) <
                        0.2F &&
                    moveMouse != capture.mousePositions.end() &&
                    moveMouse->isUnprojected &&
                    std::abs(moveMouse->mouseX - dragMoveEventScene->x) <
                        0.2F &&
                    std::abs(moveMouse->mouseY - dragMoveEventScene->y) < 0.2F,
                "活动拖拽移动使用逆投影场景坐标");
            // 单次移动帧只产生一个 UpdateDrag，重复事件说明连续命令去重被破坏。

            // release 帧移动到新场景点；该点投影在画布外，仍需先发布最终位置。
            const glm::vec2 releaseLocal{
                dragReleasePointer->x - contentOrigin.x,
                dragReleasePointer->y - contentOrigin.y
            };
            // 释放仍使用新投影点，必须与前一移动位置不同并越出逻辑画布。
            // 屏幕点仍可逆投影，但超出 280 像素逻辑宽度，不应依赖窗口 hover。
            // 该屏幕位置由明确场景值投影而来，不使用视口外的任意固定像素点。
            passed &= requireStage(releaseLocal.x > 300.0F,
                                   "释放点位于画布窗口边界之外");
            frame(interaction, snapshot, *dragReleasePointer, true, false);
            // 释放后的静止帧不得重复结束或覆盖最后一次更新。
            frame(interaction, snapshot, *dragReleasePointer, true);
            const auto endCount = std::count_if(
                capture.endDrags.begin(),
                capture.endDrags.end(),
                [](const auto& command) {
                    return command.cameraId == "Basic2DCanvas_Right";
                });
            // 计数只统计目标相机，避免其它测试环境事件影响此事务结果。
            // 额外静止帧证明 release 锁存已清除，不会重复结束事务。
            std::optional<std::size_t> lastUpdateIndex;
            std::optional<std::size_t> endIndex;
            bool                       releasePointUpdated = false;
            // 扫描完整命令序列，后续同类命令覆盖前值以确定真正的最后一次更新。
            for ( std::size_t index = 0; index < capture.orderedCommands.size();
                  ++index ) {
                if ( const auto* update =
                         std::get_if<MMM::Logic::CmdUpdateDrag>(
                             &capture.orderedCommands[index]);
                     update && update->cameraId == "Basic2DCanvas_Right" ) {
                    lastUpdateIndex = index;
                    releasePointUpdated =
                        dragReleaseEventScene &&
                        std::abs(update->mouseX - dragReleaseEventScene->x) <
                            0.2F &&
                        std::abs(update->mouseY - dragReleaseEventScene->y) <
                            0.2F;
                    // 最后一条位置更新必须对应释放边沿这一帧的新屏幕输入。
                    // 最终位置是否对应量化输入由释放阶段标签报告。
                } else if ( const auto* end =
                                std::get_if<MMM::Logic::CmdEndDrag>(
                                    &capture.orderedCommands[index]);
                            end && end->cameraId == "Basic2DCanvas_Right" ) {
                    endIndex = index;
                }
            }
            // 命令总序列确认最终位置属于最后一次更新，且先于唯一 EndDrag。
            // 若释放只结束事务而漏掉最终坐标，新场景点的逆投影值就不会出现。
            // EndDrag
            // 之后不接受同手势的更新，因此这里只比较相机内同一条事务序列。
            passed &= requireStage(endCount == 1 && releasePointUpdated &&
                                       lastUpdateIndex && endIndex &&
                                       *lastUpdateIndex < *endIndex,
                                   "释放帧先更新最终坐标且仅结束一次");
            // 唯一结束命令必须排在最终坐标更新后，避免释放瞬间丢失最后位置。
            // 后续 idle 帧继续留在录制区间内，验证结束状态不会重复发出。
            // EndDrag 的相机标识与 StartDrag 一致，事务不依赖释放时 hover。
        }

        // 活动二维回退也忽略快照偏移，直接以窗口局部像素命中并报告逻辑坐标。
        interaction.setInputProjection(
            MMM::Canvas::PlanarObservationInputProjection{}, 500.0F);
        frame(interaction, snapshot, { 100.0F, 200.0F }, true, false);
        const ImVec2 fallbackPointer{ contentOrigin.x + hitScene.x,
                                      contentOrigin.y + hitScene.y };
        // 当前输入映射关闭，添加原点后形成实际屏幕位置，再由生产入口转回局部值。
        capture = {};
        frame(interaction, snapshot, fallbackPointer, true, true);
        // 关闭观察模式后，屏幕点直接取 hitbox 中心的逻辑像素坐标。
        const auto fallbackStart =
            std::find_if(capture.startDrags.begin(),
                         capture.startDrags.end(),
                         [entity](const auto& command) {
                             return command.entity == entity &&
                                    command.cameraId == "Basic2DCanvas_Right";
                         });
        // 关闭态 start 命令同样按实体和相机双重核对，防止坐标断言孤立通过。
        const auto fallbackMouse =
            std::find_if(capture.mousePositions.begin(),
                         capture.mousePositions.end(),
                         [](const auto& command) {
                             return command.cameraId == "Basic2DCanvas_Right";
                         });
        // 二维模式直接采用画布局部像素，不设置逆投影标志，也不处理偏移参数。
        // 这条活动入口断言与上面的只读回退一起保护旧调用方语义。
        passed &= requireStage(
            fallbackStart != capture.startDrags.end() &&
                fallbackMouse != capture.mousePositions.end() &&
                !fallbackMouse->isUnprojected &&
                std::abs(fallbackMouse->mouseX - hitScene.x) < 0.2F &&
                std::abs(fallbackMouse->mouseY - hitScene.y) < 0.2F,
            "活动二维回退保持原始坐标行为");
        // 二维回退独立检查活动入口，避免只读入口正确掩盖编辑分发回归。
        // 这里故意保留很大的 snapshot offset，验证关闭态忽略新补偿参数。
        // 有效 StartDrag 与原始鼠标坐标同时成立，覆盖活动工具的二维回退行为。
        frame(interaction, snapshot, { 700.0F, 200.0F }, true, false);
        // 主动释放按钮后再撤销监听，避免输入状态或栈回调跨出用例范围。
        // 这里不依赖交互控制器析构去修复测试仍按下的 ImGui 鼠标状态。

        // 清空、有效映射和默认回退都在同一个订阅周期内逐项检查。
        // 订阅在交互析构前撤销，使后续用例看不到此处的栈引用。
        // 移除监听后才析构控制器，避免回调访问已退出的栈变量。
        bus.unsubscribe<MMM::Event::LogicCommandEvent>(subscription);
    }
    // 控制器先取消事件订阅，再销毁 ImGui 上下文，避免测试泄漏跨场景状态。
    ImGui::DestroyContext();
    // 所有失败仍经过上下文清理，不能早退跳过生命周期收尾。
    return passed ? 0 : 1;
}
