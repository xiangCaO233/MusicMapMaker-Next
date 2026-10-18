#include "canvas/TimelineCanvas.h"

#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace MMM::Canvas
{
/// @brief 仅暴露测试所需的快照绑定、交互入口和警告状态。
/// @note 不复制专业模式泳道映射，遗漏第五列时真实交互必须失败。
/// @note 本访问器不修改当前创建类型或伪造弹窗打开标志。
/// @note 测试使用与 UI 相同的事件发布路径，不跳过格式警告判断。
/// @note 快照由调用方拥有，访问器不延长其生命周期。
struct TimelineStopCreationTestAccess {
    /// @brief 绑定测试拥有的快照并驱动正式画笔交互。
    /// @param canvas 被测时间线视图，无 GPU 和逻辑会话。
    /// @param snapshot 测试快照，整个用例期间保持存活。
    /// @param origin 当前 ImGui 窗口内容区原点。
    static void interact(TimelineCanvas&                 canvas,
                         Common::Render::RenderSnapshot& snapshot,
                         ImVec2                          origin)
    {
        // 画布悬浮与聚焦为真，鼠标按钮边沿仍完全由 ImGui 的输入队列生成。
        canvas.m_currentSnapshot = &snapshot;
        canvas.handleTimingCanvasInteraction(
            origin, { 500.0F, 500.0F }, true, true);
    }
    /// @brief 使用正式删除入口提交同一选区，不复制批量命令的组装逻辑。
    /// @param canvas 绑定测试快照的时间线。
    /// @param entities 本次用户操作选中的多个实体。
    /// @details 仅建立选择前置状态，命令组装、清理与发布全部由生产入口执行。
    /// 两种删除共享同一选区，不把右键命中的一项当成整次用户意图。
    /// @param rightErase 为真时从已选物件发起右键擦除，否则模拟 Delete
    /// 共用入口。
    static bool eraseSelection(TimelineCanvas&                  canvas,
                               const std::vector<entt::entity>& entities,
                               bool                             rightErase)
    {
        // 选区使用稳定实体集合，允许生产实现按任意顺序组装批量载荷。
        // 断言比较成员身份与数量，不依赖 unordered_set 的遍历顺序。
        canvas.m_selectedTimingEntities.insert(entities.begin(),
                                               entities.end());
        if ( rightErase ) {
            // 右键仅命中第一项，生产入口应扩展为删除整个选区。
            canvas.m_timingEraseTargetEntities.insert(entities.front());
            canvas.commitTimingEraseTargets();
            canvas.m_timingEraseTargetEntities.clear();
        } else {
            canvas.deleteSelectedTimingEvents();
        }
        // 本地高亮必须随删除立即清空，不能等异步逻辑快照返回后才消失。
        return canvas.m_selectedTimingEntities.empty();
    }
    /// @brief 检查松开画笔后确实打开 STOP 创建草稿。
    /// @return 弹窗、类型及默认停顿时长全部正确时返回 true。
    static bool stopPopup(const TimelineCanvas& canvas)
    {
        // 只打开窗口还不够，必须排除沿用上一类型或默认 BPM 的情况。
        return canvas.m_isCreatePopupOpen && canvas.m_createType == 4 &&
               canvas.m_createValue == 0.5;
    }
    /// @brief 通过实际命令入口验证格式警告，同时保留正常事件发布。
    /// @param canvas 被测时间线。
    /// @param command 用户提交的创建、更新、删除或批量更新命令。
    /// @return 本次提交是否触发原格式保存风险提示。
    static bool publish(TimelineCanvas&                 canvas,
                        const Event::LogicCommandEvent& command)
    {
        // 逐次清除待提示状态，不能让前一个不支持格式掩盖后续漏报。
        canvas.m_timingFormatWarningPending = false;
        canvas.publishTimelineCommand(command);
        return canvas.m_timingFormatWarningPending;
    }
};
}  // namespace MMM::Canvas

namespace
{
/// @brief 在真实 ImGui 帧中把鼠标放入第五列并提交按钮边沿。
/// @param canvas 正式 TimelineCanvas 实例。
/// @param snapshot 当前谱面快照，不初始化音频或图形设备。
/// @param pointer 按逻辑像素指定的鼠标位置。
/// @param down 当前左键状态，必须经历按下和松开两个帧。
/// @return 窗口内容原点，供下一帧计算精确的泳道坐标。
ImVec2 frame(MMM::Canvas::TimelineCanvas&         canvas,
             MMM::Common::Render::RenderSnapshot& snapshot, ImVec2 pointer,
             bool down)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    // 固定窗口几何，避免依赖桌面显示器和用户保存的停靠布局。
    ImGui::SetNextWindowPos({ 0.0F, 0.0F });
    ImGui::SetNextWindowSize({ 600.0F, 600.0F });
    ImGui::Begin("StopCreationTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const auto origin = ImGui::GetCursorScreenPos();
    MMM::Canvas::TimelineStopCreationTestAccess::interact(
        canvas, snapshot, origin);
    ImGui::End();
    ImGui::EndFrame();
    return origin;
}

/// @brief 验证画笔第五列和不同格式的 STOP 创建/修改警告。
/// @return 画笔创建入口、支持格式与风险提示均符合约定时返回 true。
/// @details 文件路径仅用于能力判断，不创建文件或修改用户项目。
/// @details 所有事件仅观察发布结果，测试不建立正在编辑的会话。
/// @details 既有 STOP 用固定实体身份标记，覆盖表格更新和删除的识别。
/// @pre 专业模式开启，播放停止且没有活动模态窗口。
/// @post 总线订阅在本函数返回之前解除，外部配置由入口函数恢复。
/// @note 空快照不包含已有物件，保证鼠标点击命中新建而非修改入口。
/// @note 首个空闲帧建立窗口，后续按下和松开不会被首帧出现状态吞掉。
/// @note 创建弹窗尚未提交时不应该产生 Timing 创建事件。
/// @note 格式循环在同一个画布实例中进行，覆盖活动文件路径变化。
/// @note 每次检查都重置 pending 标志，防止上一个格式的提醒假通过。
/// @note 警告只验证排队状态，正式模态展示由既有延后显示逻辑负责。
/// @note Entity 只用于身份比较，不访问 ECS 或异步创建逻辑。
/// @note 提交事件计数用于证明不支持格式的提醒没有吞掉用户编辑。
bool checkStopCreation()
{
    using namespace MMM;
    Common::Render::RenderSnapshot snapshot;
    snapshot.hasBeatmap     = true;
    snapshot.currentTool    = Logic::EditTool::Draw;
    snapshot.currentTime    = 1.0;
    snapshot.trackCount     = 4;
    snapshot.beatmapPathKey = "test.mmm";
    Canvas::TimelineCanvas canvas("StopCreationTestCanvas", 500, 500, nullptr);
    // 第五列中心为 x=450；y=150 避开顶部的工具菜单按钮区。
    // 先建立窗口，再在两个后续帧产生真实 click/release。
    const auto   origin = frame(canvas, snapshot, { 450.0F, 150.0F }, false);
    const ImVec2 pointer{ origin.x + 450.0F, origin.y + 150.0F };
    frame(canvas, snapshot, pointer, true);
    frame(canvas, snapshot, pointer, false);
    if ( !Canvas::TimelineStopCreationTestAccess::stopPopup(canvas) ) {
        XERROR("STOP brush release did not open the STOP creation popup");
        return false;
    }

    // 观察正式总线，警告既不能吞命令，也不能重复发布一个命令。
    std::size_t published    = 0;
    auto&       bus          = Event::EventBus::instance();
    const auto  subscription = bus.subscribe<Event::LogicCommandEvent>(
        [&](const auto&) { ++published; });
    Common::Render::TimelineInteractiveElement existing{};
    existing.stopEntity = static_cast<entt::entity>(42);
    existing.effects    = Common::Render::SCROLL_EFFECT_STOP;
    snapshot.timelineElements.push_back(existing);
    bool passed = true;
    // MMM 和全部 BMS 后缀都支持 STOP；MC/OSU/IMD 仍允许编辑但必须提醒。
    // 大写扩展名不能进入另一条能力分支。
    const std::array paths{
        "chart.mmm", "chart.bms", "chart.BME", "chart.bml",
        "chart.PMS", "chart.mc",  "chart.osu", "chart.imd"
    };
    for ( std::size_t i = 0; i < paths.size(); ++i ) {
        snapshot.beatmapPathKey    = paths[i];
        const bool expectedWarning = i >= 5;
        const auto before          = published;
        passed &= Canvas::TimelineStopCreationTestAccess::publish(
                      canvas,
                      Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                          2.0, TimingEffect::STOP, 0.5 })) == expectedWarning;
        // 创建携带类型，更新与删除携带实体；三条路径需要分别判断。
        passed &= Canvas::TimelineStopCreationTestAccess::publish(
                      canvas,
                      Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                          existing.stopEntity, 2.0, 0.75 })) == expectedWarning;
        passed &= Canvas::TimelineStopCreationTestAccess::publish(
                      canvas,
                      Event::LogicCommandEvent(Logic::CmdDeleteTimelineEvent{
                          existing.stopEntity })) == expectedWarning;
        Logic::CmdUpdateTimelineEvents batch;
        batch.events.push_back({ existing.stopEntity, 2.0, 1.0 });
        // 多选编辑也必须识别 STOP，而非只在新建时提示一次。
        passed &=
            Canvas::TimelineStopCreationTestAccess::publish(
                canvas, Event::LogicCommandEvent(batch)) == expectedWarning;
        // 合并删除后仍需按完整选区检查 STOP 的原格式保存能力。
        passed &= Canvas::TimelineStopCreationTestAccess::publish(
                      canvas,
                      Event::LogicCommandEvent(Logic::CmdDeleteTimelineEvents{
                          { existing.stopEntity } })) == expectedWarning;
        passed &= published == before + 5;
    }
    // Delete 与右键擦除都只能发布一条批量命令，不允许继续散发单项删除。
    // 订阅正式事件总线，避免用直接构造正确 DTO 的测试掩盖 UI 仍逐项发布。
    // 本轮单项计数从零开始，前面格式能力检查中的单项删除不计入批量回归。
    // 新订阅在本轮返回前解除，不能让引用本地变量的闭包泄漏到后续测试。
    std::size_t                     batches = 0, singles = 0;
    const std::vector<entt::entity> selected{ static_cast<entt::entity>(42),
                                              static_cast<entt::entity>(43) };
    const auto                      batchSubscription =
        bus.subscribe<Event::LogicCommandEvent>([&](const auto& event) {
            if ( const auto* cmd = std::get_if<Logic::CmdDeleteTimelineEvents>(
                     &event.command) ) {
                // 每条批量命令必须覆盖完整选择，两条各含一项也应被判失败。
                ++batches;
                passed &= cmd->entities.size() == selected.size();
                for ( const auto entity : selected )
                    passed &= std::find(cmd->entities.begin(),
                                        cmd->entities.end(),
                                        entity) != cmd->entities.end();
            }
            singles += std::holds_alternative<Logic::CmdDeleteTimelineEvent>(
                           event.command)
                           ? 1
                           : 0;
        });
    // 共用同一个画布实例，上一操作必须真实完成选择清理后再开始下一操作。
    // 右键只命中其中一项，能够检出擦除分支没有携带其他已选目标的回归。
    for ( const bool rightErase : { false, true } ) {
        const auto before = batches;
        // 快照保持同一实例，选择清理必须由正式入口完成，不能通过重建视图掩盖。
        passed &= Canvas::TimelineStopCreationTestAccess::eraseSelection(
            canvas, selected, rightErase);
        passed &= batches == before + 1 && singles == 0;
    }
    bus.unsubscribe<Event::LogicCommandEvent>(batchSubscription);
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    return passed;
}
}  // namespace

/// @brief 使用最小 ImGui 上下文执行 STOP 画笔与警告回归。
/// @return 正式创建路径可达且警告不会吞掉命令时返回零。
/// @note 不读写布局文件，不调用 Vulkan，也不修改配置文件。
/// @pre 测试独占 ImGui 上下文，不使用桌面应用的窗口或输入队列。
/// @post 临时配置完整恢复，不把专业模式或警告偏好保存到个人设置。
/// @details 固定帧间隔仅提供 ImGui 时间步长，不执行 sleep 或等待。
/// @details 字体图集只用于 CPU 控件状态机，不需要显示器或 GPU。
/// @details 本用例验证用户输入到提交路由，音频等待另由 STOP 测试覆盖。
/// @details 创建真实谱面与写盘由现有格式测试覆盖，不在此生成资源文件。
/// @warning 只在独立测试进程初始化最小上下文，不能嵌入应用热路径。
int main()
{
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    const auto original                = settings;
    settings.professionalMode          = true;
    settings.hideTimelineFormatWarning = false;
    ImGui::CreateContext();
    auto& io              = ImGui::GetIO();
    io.IniFilename        = nullptr;
    io.DisplaySize        = { 800.0F, 700.0F };
    io.DeltaTime          = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 仅构建 CPU 字体图集，支持真实 UI 帧，无需纹理上传。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const bool passed = checkStopCreation();
    ImGui::DestroyContext();
    settings = original;
    return passed ? 0 : 1;
}
