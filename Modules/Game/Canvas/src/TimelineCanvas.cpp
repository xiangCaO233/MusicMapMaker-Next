/// @file TimelineCanvas.cpp
/// @brief 实现总时间线窗口、时间点交互装饰与 Vulkan 命令录制。
///
/// Timeline 窗口消费独立准备的 RenderSnapshot，负责时间滚动、
/// Timing 齿轮入口、专业分轨覆盖层和时间点表格窗口的 UI 调度。
/// 时间点创建、编辑、拖拽与擦除的具体状态机位于交互拆分文件，
/// 本文件只协调窗口焦点、画布纹理和交互结果的视觉装饰。
///
/// 时间线主窗口可以隐藏，但时间点表格保持独立生命周期；
/// 快照准备条件同时考虑两个窗口，不能把表格可用性绑定到主窗口显示。
/// 交互装饰临时追加到当前可写快照，并在下一次准备前完整恢复，
/// 不把悬停、选中、拖拽预览或擦除颜色写入逻辑层持久数据。
///
/// Vulkan 录制仅消费预生成顶点、索引和命令列表，按纹理与裁剪状态
/// 避免重复绑定；皮肤资源和 shader 文件只在低频重载路径访问。
#include "canvas/TimelineCanvas.h"
#include "audio/AudioManager.h"
#include "canvas/TimelineTableWindowState.h"
#include "canvas/TimelineTimingTooltip.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/render/Batcher.h"
#include "ui/Icons.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace MMM::Canvas
{
namespace
{
/// @brief 判断 ImGui 窗口是否为左侧工具栏窗口。
/// @param window 待判断的 ImGui 窗口。
/// @return 指向工具栏窗口时返回 true。
bool isToolbarWindow(const ImGuiWindow* window)
{
    // 工具栏标题使用固定内部名称；空指针表示当前没有对应窗口。
    return window && std::strcmp(window->Name, " ###Toolbar") == 0;
}

/// @brief 判断当前 ImGui 焦点或悬浮窗口是否为工具栏。
/// @return 工具栏正处理鼠标或键盘焦点时返回 true。
bool isToolbarFocusedOrHovered()
{
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if ( !context ) {
        // ImGui 尚未建立上下文时不能查询内部窗口指针。
        return false;
    }
    // 键盘导航焦点和鼠标悬停任一落在工具栏，都不应清除时间线焦点锁存。
    return isToolbarWindow(context->NavWindow) ||
           isToolbarWindow(context->HoveredWindow);
}

/// @brief 在鼠标附近绘制播放速度临时提示窗口。
/// @param speedValue 当前播放速度倍率。
/// @warning UI 热路径：仅在速度提示计时器生效时绘制一个轻量 ImGui 窗口。
void renderPlaybackSpeedTooltip(float speedValue)
{
    // 提示使用主 viewport 的工作区判断边缘，避免覆盖系统面板区域。
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2         mousePos = ImGui::GetMousePos();

    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    // 靠近右侧或下侧三成区域时翻转对应锚点，使窗口朝中心展开。
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f ) {
        pivot.x = 1.0f;
    }
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f ) {
        pivot.y = 1.0f;
    }

    const float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
    const float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;

    ImGui::SetNextWindowPos(ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
                            ImGuiCond_Always,
                            pivot);
    ImGui::SetNextWindowBgAlpha(0.7f);
    // 半透明背景保留时间线上下文，但仍为倍率文字提供稳定对比。

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // NoInputs 与 NoFocusOnAppearing 保证提示不会打断后续滚轮调整。

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f));
    if ( ImGui::Begin("##TimelineSpeedTooltip", nullptr, flags) ) {
        // 皮肤内容字体可选；缺失时沿用当前 ImGui 字体。
        ImFont* font = Config::SkinManager::instance().getFont("content");
        if ( font ) ImGui::PushFont(font, font->LegacySize);
        ImGui::Text(TR("ui.toolbar.playback_speed_value").data(), speedValue);
        if ( font ) ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// @brief 处理 Timeline 窗格上的修饰键滚轮操作。
/// @param timelineId Timeline 对应的画布 ID，用于隔离 Alt 滚轮累积量。
/// @param wheel 当前帧滚轮增量。
/// @param isCommandPressed Ctrl 或 Command 是否按下。
/// @param isAltPressed Alt 是否按下。
/// @param isShiftPressed Shift 是否按下。
/// @param speedTooltipValue 输出速度提示窗口需要显示的速度倍率。
/// @param speedTooltipTimer 输出速度提示窗口剩余显示时间。
/// @return 已处理修饰滚轮时返回 true。
/// @warning UI 热路径：Timeline 悬停滚轮时调用；会按播放配置短暂锁定
/// SessionRegistry，并发布轻量命令。
bool handleTimelineModifierWheel(const std::string& timelineId, float wheel,
                                 bool isCommandPressed, bool isAltPressed,
                                 bool isShiftPressed, float& speedTooltipValue,
                                 float& speedTooltipTimer)
{
    // 普通滚轮和触控板残余不在此消费，交由时间滚动分支处理。
    if ( std::abs(wheel) <= 0.01f || (!isCommandPressed && !isAltPressed) ) {
        return false;
    }

    const bool shouldPlayAdjustmentFeedback =
        Logic::EditorEngine::instance()
            .getEditorConfig()
            .settings.stopPlaybackOnScroll;
    // 零位移命令表达修饰键调整意图，让逻辑层按用户设置停止播放。
    Event::EventBus::instance().publish(Event::LogicCommandEvent(
        Logic::CmdScroll{ timelineId,
                          0.0f,
                          false,
                          Logic::ScrollCommandIntent::ModifierAdjustment }));

    if ( isCommandPressed && isAltPressed ) {
        // Command+Alt 在固定速度档位间移动，避免产生难以复现的小数倍率。
        constexpr std::array<double, 4> presets = { 0.25, 0.50, 0.75, 1.0 };
        double                          currentSpeed =
            Audio::AudioManager::instance().getPlaybackSpeed();

        std::size_t bestIdx = 0;
        double      minDiff = std::abs(currentSpeed - presets[0]);
        for ( std::size_t i = 1; i < presets.size(); ++i ) {
            // 外部配置值可能不在预设中，先选取最近档作为滚动基线。
            const double diff = std::abs(currentSpeed - presets[i]);
            if ( diff < minDiff ) {
                minDiff = diff;
                bestIdx = i;
            }
        }

        if ( wheel > 0.01f ) {
            // 到达数组边界时保持当前档，不发布重复命令。
            if ( bestIdx < presets.size() - 1U ) bestIdx++;
        } else if ( wheel < -0.01f ) {
            if ( bestIdx > 0U ) bestIdx--;
        }

        const double newSpeed = presets[bestIdx];
        if ( std::abs(newSpeed - currentSpeed) > 1e-4 ) {
            // 只有倍率实际变化才更新音频线程并启动两秒非阻塞提示。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetPlaybackSpeed{ newSpeed }));
            speedTooltipValue = static_cast<float>(newSpeed);
            speedTooltipTimer = 2.0f;
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
        return true;
    }

    if ( isCommandPressed ) {
        // Command 单独调整时间线缩放，Shift 使用用户滚动倍率加速步长。
        auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
        float step      = 0.1f;
        if ( isShiftPressed ) {
            step *= editorCfg.settings.scrollSpeedMultiplier;
        }
        const float currentZoom = editorCfg.visual.timelineZoom;
        const float newZoom =
            std::clamp(currentZoom + wheel * step, 0.1f, 10.0f);
        // 缩放上下限防止时间投影退化或产生不可操作的极端范围。
        if ( std::abs(newZoom - currentZoom) > 0.0001f ) {
            editorCfg.visual.timelineZoom = newZoom;
            Logic::EditorEngine::instance().setEditorConfig(editorCfg);
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
        return true;
    }

    auto      editorCfg = Logic::EditorEngine::instance().getEditorConfig();
    const int originalDivisor = editorCfg.settings.beatDivisor;
    static std::unordered_map<std::string, float> wheelAccumulator;
    // 分数滚轮按 timelineId 隔离累计，多谱面不会共享触控板余量。
    float& acc = wheelAccumulator[timelineId];
    acc += wheel;

    int steps = 0;
    if ( acc >= 1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    } else if ( acc <= -1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    }

    if ( steps == 0 ) {
        // 已识别为 Alt 语义但不足一个整数步，保留余量并声明已消费。
        return true;
    }

    if ( isShiftPressed ) {
        // Shift 只在常见音乐分拍集合间移动，保持常用值易于到达。
        constexpr std::array<int, 8> presets = { 1, 2, 3, 4, 6, 8, 12, 16 };
        int                          current = editorCfg.settings.beatDivisor;
        if ( steps > 0 ) {
            // 多步输入逐档执行，末端稳定钳制到最后一个预设。
            for ( int i = 0; i < steps; ++i ) {
                auto it =
                    std::upper_bound(presets.begin(), presets.end(), current);
                current = it != presets.end() ? *it : presets.back();
            }
        } else {
            // 负向使用 lower_bound 找到严格小于当前值的前一档。
            for ( int i = 0; i < -steps; ++i ) {
                auto it =
                    std::lower_bound(presets.begin(), presets.end(), current);
                current = it != presets.begin() ? *(--it) : presets.front();
            }
        }
        editorCfg.settings.beatDivisor = current;
    } else {
        editorCfg.settings.beatDivisor += steps;
    }

    editorCfg.settings.beatDivisor =
        std::clamp(editorCfg.settings.beatDivisor, 1, 64);
    // 非预设模式也遵守逻辑层支持的完整 1..64 范围。
    if ( editorCfg.settings.beatDivisor != originalDivisor ) {
        Logic::EditorEngine::instance().setEditorConfig(editorCfg);
        if ( shouldPlayAdjustmentFeedback ) {
            ::MMM::UI::PlayInteractionMouseUpFeedback();
        }
    }
    return true;
}

/// @brief 根据齿轮颜色选择具有稳定对比度的中性底板颜色。
/// @param gearColor 齿轮文字颜色。
/// @param hovered 齿轮当前是否悬浮。
/// @return 对比色底板的 ImGui 打包颜色。
/// @warning UI 热路径：每个可见齿轮每帧调用，只执行常量算术。
ImU32 timelineGearBackgroundColor(const ImVec4& gearColor, bool hovered)
{
    // 使用平方 RGB 近似线性亮度，降低 gamma 空间直接加权的偏差。
    const float luminance = 0.2126f * gearColor.x * gearColor.x +
                            0.7152f * gearColor.y * gearColor.y +
                            0.0722f * gearColor.z * gearColor.z;
    const int alpha = hovered ? 242 : 218;
    if ( luminance < 0.18f ) {
        // 深色齿轮配浅底，浅色齿轮配近黑底；悬停只提高底板 alpha。
        return IM_COL32(248, 250, 255, alpha);
    }
    return IM_COL32(8, 11, 18, alpha);
}

/// @brief Timeline 画布齿轮按钮的类型信息
/// @details 将一个 TimingEffect 的快照字段、交互标签、颜色和左右布局
/// 聚合为表驱动描述，命中检测与实际绘制必须复用同一数组顺序。
struct TimelineGearInfo {
    /// @brief 对应 TimelineInteractiveElement 的效果掩码。
    uint32_t mask;

    /// @brief 对应 Timing 类型。
    ::MMM::TimingEffect effect;

    /// @brief 对应 TimelineInteractiveElement 的实体字段。
    entt::entity Common::Render::TimelineInteractiveElement::* entity;

    /// @brief 对应 TimelineInteractiveElement 的参数值字段。
    double Common::Render::TimelineInteractiveElement::* value;

    /// @brief 显示标签。
    const char* label;

    /// @brief 编辑弹窗类型。
    const char* editType;

    /// @brief 齿轮文字颜色。
    ImVec4 color;

    /// @brief 是否显示在 Timeline 右侧。
    bool rightSide;
};

/// @brief 将创建弹窗索引转换为 Timeline Timing 类型。
::MMM::TimingEffect timelineEffectFromCreateType(int createType)
{
    // createType 来自弹窗单选索引；未知值安全回退到最常用的 Scroll。
    switch ( createType ) {
    case 0: return ::MMM::TimingEffect::BPM;
    case 2: return ::MMM::TimingEffect::JUMP;
    case 3: return ::MMM::TimingEffect::HS;
    case 1:
    default: return ::MMM::TimingEffect::SCROLL;
    }
}

/// @brief 获取 Timeline Timing 类型的渲染颜色。
glm::vec4 timelineEffectColor(::MMM::TimingEffect effect, float alpha)
{
    // RGB 与时间线四类 Timing 的既有视觉语义一致，alpha 由调用方控制。
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return { 1.0f, 0.28f, 0.28f, alpha };
    case ::MMM::TimingEffect::SCROLL: return { 0.28f, 1.0f, 0.34f, alpha };
    case ::MMM::TimingEffect::JUMP: return { 0.32f, 0.53f, 1.0f, alpha };
    case ::MMM::TimingEffect::HS: return { 1.0f, 0.87f, 0.28f, alpha };
    }
    return { 1.0f, 1.0f, 1.0f, alpha };
}

/// @brief 获取专业模式中指定 Timing 类型所属的轨道索引。
int professionalTimingLane(::MMM::TimingEffect effect)
{
    // 专业模式固定使用 BPM、Scroll、Jump、HS 四个从左到右的泳道。
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 0;
    case ::MMM::TimingEffect::SCROLL: return 1;
    case ::MMM::TimingEffect::JUMP: return 2;
    case ::MMM::TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 生成用于去重同一个 marker glow 命令的键。
uint64_t timelineMarkerKey(uint32_t indexOffset, uint32_t indexCount)
{
    // 两个 32 位字段无损拼入 64 位键，避免同一 marker 重复追加 glow。
    return (static_cast<uint64_t>(indexOffset) << 32U) |
           static_cast<uint64_t>(indexCount);
}

}  // namespace

TimelineCanvas::TimelineCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer)
    : UI::IUIView(name)
    , UI::IRenderableView(name)
    , m_canvasName(name)
    , m_syncBuffer(std::move(syncBuffer))
{
    // 逻辑尺寸用于首次注册视口，物理尺寸由后续 framebuffer scale 换算。
    m_targetWidth   = w;
    m_targetHeight  = h;
    m_logicalWidth  = w;
    m_logicalHeight = h;

    // Timeline 启动时可能保持隐藏；仍需先注册视口，保证时间点表格有快照来源。
    resizeCall(0U, 0U, w, h);
}

/// @brief 提交总时间轴最近一次连续 Seek。
/// @warning UI 热路径：仅在拖动结束或窗口中断交互时发布一条命令。
void TimelineCanvas::commitAudioTimeSliderScrub()
{
    // 未处于连续拖动时不发布多余的最终 Seek。
    if ( !m_isAudioTimeSliderScrubbing ) return;
    // 最终命令携带最近一次候选并把 isScrubbing 复位，逻辑层据此提交。
    Event::EventBus::instance().publish(Event::LogicCommandEvent(Logic::CmdSeek{
        .time        = m_audioTimeSliderScrubTarget,
        .isScrubbing = false,
    }));
    m_isAudioTimeSliderScrubbing = false;
}

/// @brief 更新 Timeline 窗口、画布交互与叠加控件。
///
/// @details 窗口生命周期：
/// - `showTimelineWindow` 只控制主时间线窗口。
/// - 时间点表格窗口具有独立打开状态。
/// - 主窗口隐藏时仍绘制和维护表格窗口。
/// - 主窗口隐藏前提交尚未结束的时间滑块拖动。
/// - 关闭按钮把设置写回 AppConfig 并低频保存。
/// - 动态可见标题使用 `###` 后固定内部 ID。
/// - 下一帧聚焦请求只消费一次。
/// - 每帧记录当前 Dock 节点供表格窗口恢复使用。
/// - 窗口或子窗口获得焦点时锁存 Timing 交互焦点。
/// - 点击其它普通窗口会释放该锁存。
/// - 点击工具栏不释放锁存，允许切换工具后继续操作时间线。
/// - 主窗口关闭时清除所有焦点锁存。
///
/// @details 时间滑块：
/// - 只有有效谱面且总时长为正时显示。
/// - 滑块范围是零到谱面总时长。
/// - 滑块显示时间属于视觉时间域。
/// - 命令时间在发布前扣除有效视觉偏移。
/// - Shift 拖动先吸附到最近分拍线。
/// - 吸附结果再次钳制在总时长内。
/// - 活动拖动发送 `isScrubbing=true` 的连续 Seek。
/// - 每次候选保存为最终提交目标。
/// - ImGui 报告编辑结束后发送一次 `isScrubbing=false`。
/// - 快照失效或窗口关闭时也提交最近目标。
/// - 滑块悬停和活动时显示当前/总时长提示。
/// - 时间格式复用画布公共格式化配置。
///
/// @details Vulkan 画布嵌入：
/// - 滑块占用空间后重新读取剩余内容尺寸。
/// - 逻辑宽高向下取整，避免亚像素渲染目标抖动。
/// - 正尺寸才请求更新离屏目标大小。
/// - framebuffer scale 单独传给物理尺寸换算。
/// - descriptor 未准备好时不绘制 ImGui Image。
/// - Image 矩形成为后续交互的几何来源。
/// - item hover 与窗口几何回退共同判断画布归属。
/// - 回退允许活动自定义按钮覆盖时仍保持时间线手势。
/// - 所有局部鼠标坐标相对 Image 左上角计算。
/// - 纵向命令坐标扣除快照准备阶段应用的 Y 偏移。
///
/// @details 滚轮与焦点：
/// - 画布 hover 时才处理滚轮。
/// - Command/Alt 修饰语义先于普通时间滚动。
/// - 未被修饰键消费的滚轮发布 `CmdScroll`。
/// - Shift 状态随普通滚动命令发送。
/// - 左、右、中任一按键在时间线上点击都会取得交互焦点。
/// - 点击时间线时同时请求 ImGui 当前帧和下一帧聚焦。
/// - 点击画布外且不在工具栏时释放 Timing 交互焦点。
/// - 锁存焦点允许从工具栏返回后继续 Timing 手势。
/// - 菜单按钮区域不参与时间吸附 hover。
/// - 鼠标位置每帧发送给对应 timeline camera。
/// - dragging 标志汇总三个鼠标按钮的拖动状态。
///
/// @details Timing 齿轮：
/// - BPM 与 Jump 默认放在左侧。
/// - Scroll 与 HS 默认放在右侧。
/// - 专业模式改为四类各占一个水平泳道。
/// - 齿轮尺寸覆盖字体图标的最大边长。
/// - 同侧同时间的多个齿轮沿纵向对称错开。
/// - 专业模式齿轮居中到对应泳道。
/// - 所有位置钳制在当前 Image 可见矩形。
/// - hoveredTime 可由节奏吸附或像素邻近判定。
/// - 快照 effect mask 决定一个时间点需要哪些齿轮。
/// - null Timing entity 不生成齿轮。
/// - 命中预扫描与实际按钮绘制复用同一位置算法。
/// - 预扫描使不可见按钮也能在本帧及时打开编辑器。
/// - 实际 hit zone 使用 InvisibleButton，视觉由 DrawList 自绘。
/// - 反馈通过统一 `FeedbackLastItem` 注入。
/// - 高对比底板隔离底层同色 glow。
/// - 悬停边框加粗并显示时间、参数和单位。
///
/// @details 编辑入口与叠加层：
/// - 只有停止播放且 Draw 工具激活时齿轮可打开编辑器。
/// - 手势、弹窗或创建预览活动时隐藏齿轮编辑入口。
/// - 打开编辑器会清除拖动、擦除和创建预览状态。
/// - 编辑实体、时间、数值和类型从命中快照复制。
/// - 具体交互状态机在 `handleTimingCanvasInteraction` 中推进。
/// - 交互状态完成后刷新快照视觉装饰。
/// - 专业模式覆盖层在交互装饰之后绘制标签。
/// - Timing 交互提示和框选覆盖层最后绘制。
/// - 编辑弹窗和创建弹窗在所有画布按钮之后绘制。
/// - 时间点表格窗口在主时间线 LayoutContext 之外绘制。
/// - 播放速度提示按 DeltaTime 非阻塞递减。
/// @param sourceManager UI 管理器。
/// @warning UI 热路径：每帧调用；主窗口隐藏时仅维护独立表格窗口。
/// 禁止引入文件系统访问、阻塞操作或全量排序。
void TimelineCanvas::update(UI::UIManager* sourceManager)
{
    // sourceManager 当前由拆分的表格/弹窗实现消费，本层保持接口一致。
    auto& appConfig      = Config::AppConfig::instance();
    auto& editorSettings = appConfig.getEditorSettings();
    if ( !editorSettings.showTimelineWindow ) {
        // 主窗口隐藏不能阻断独立表格快照和交互生命周期。
        commitAudioTimeSliderScrub();
        renderTimingPointsTableWindow();
        return;
    }

    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    // 物理缩放用于离屏纹理尺寸，ImGui 内容尺寸仍保持逻辑像素。

    std::string windowName =
        fmt::format("{}###{}", TR("canvas.timeline"), m_name);
    bool windowOpen = editorSettings.showTimelineWindow;

    if ( m_shouldFocusNextFrame ) {
        // 聚焦请求在 Begin 前设置，并在本帧立即清除避免持续抢焦点。
        ImGui::SetNextWindowFocus();
        m_shouldFocusNextFrame = false;
    }
    UI::LayoutContext lctx(m_layoutCtx,
                           windowName,
                           true,
                           ImGuiWindowFlags_NoScrollbar,
                           &windowOpen);
    m_lastDockId = ImGui::IsWindowDocked() ? ImGui::GetWindowDockID() : 0;
    // 独立表格需要最近 Dock ID 恢复到时间线所属区域。
    if ( ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ) {
        m_hasTimingInteractionFocus = true;
    }
    m_wasFocusedLastFrame = m_hasTimingInteractionFocus;
    if ( !windowOpen ) {
        // 标题栏关闭是持久设置变更，同时终止所有主窗口局部状态。
        commitAudioTimeSliderScrub();
        m_wasFocusedLastFrame             = false;
        m_hasTimingInteractionFocus       = false;
        editorSettings.showTimelineWindow = false;
        appConfig.save();
        renderTimingPointsTableWindow();
        return;
    }

    ImVec2 size = ImGui::GetContentRegionAvail();
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ||
         m_currentSnapshot->totalTime <= 0.0 ) {
        // 快照在拖动期间失效时仍提交最后有效候选，防止逻辑层保持 scrub。
        commitAudioTimeSliderScrub();
    }

    if ( m_currentSnapshot ) {
        // 1. 绘制垂直音频时间滚动条及时间点表格按钮
        if ( m_currentSnapshot->hasBeatmap &&
             m_currentSnapshot->totalTime > 0.0 ) {
            float time  = static_cast<float>(m_currentSnapshot->currentTime);
            float total = static_cast<float>(m_currentSnapshot->totalTime);

            float sliderWidth  = 24.0f;
            float sliderHeight = size.y;

            ImGui::BeginGroup();

            ImVec2     sliderSize(sliderWidth, sliderHeight);
            const bool sliderChanged = ::MMM::UI::FeedbackVSliderFloat(
                "##AudioTimeSlider", sliderSize, &time, 0.0f, total, "");
            const bool sliderActive = ImGui::IsItemActive();
            const bool sliderDeactivatedAfterEdit =
                ImGui::IsItemDeactivatedAfterEdit();
            if ( sliderChanged ) {
                // UI 滑块编辑视觉时间，逻辑 Seek 使用扣除视觉偏移的音频时间。
                float visualOffset = Config::AppConfig::instance()
                                         .getVisualConfig()
                                         .getEffectiveVisualOffset();
                double targetTime = static_cast<double>(time);
                if ( ImGui::GetIO().KeyShift ) {
                    // Shift 吸附仅影响本次候选，不永久改变 beat divisor。
                    targetTime = std::clamp(snapTimeToBeatLine(targetTime),
                                            0.0,
                                            static_cast<double>(total));
                    time       = static_cast<float>(targetTime);
                }
                const double commandTime =
                    targetTime - static_cast<double>(visualOffset);
                m_isAudioTimeSliderScrubbing = sliderActive;
                // 保存最近目标，窗口中断时由统一提交入口补发最终命令。
                m_audioTimeSliderScrubTarget = commandTime;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSeek{
                        .time        = commandTime,
                        .isScrubbing = sliderActive,
                    }));
            }
            if ( sliderDeactivatedAfterEdit && m_isAudioTimeSliderScrubbing ) {
                // 只在曾进入活动拖动时提交，普通键盘改变不会重复结束。
                commitAudioTimeSliderScrub();
            }

            if ( sliderActive || ImGui::IsItemHovered() ) {
                const auto timeText = MMM::UI::Utils::formatCanvasTimePair(
                    static_cast<double>(time),
                    static_cast<double>(total),
                    m_currentSnapshot);
                ImGui::SetTooltip("%s", timeText.c_str());
            }

            ImGui::EndGroup();
            ImGui::SameLine();
        }

        // 2. 扣除 slider 空间后剩下的空间绘制画布
        size = ImGui::GetContentRegionAvail();
        // Vulkan 目标只接受整数逻辑尺寸，统一向下取整保持稳定。
        size.x = std::floor(size.x);
        size.y = std::floor(size.y);

        if ( size.x > 0 && size.y > 0 ) {
            setTargetSize(static_cast<uint32_t>(size.x),
                          static_cast<uint32_t>(size.y),
                          framebufferScale.x,
                          framebufferScale.y);
        }

        vk::DescriptorSet texID = getDescriptorSet();
        if ( texID != VK_NULL_HANDLE ) {
            // descriptor 有效后把离屏结果作为一个 ImGui Image 嵌入窗口。
            ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);

            ImVec2 canvasPos = ImGui::GetItemRectMin();
            ImVec2 mousePos  = ImGui::GetMousePos();
            bool   isHovered =
                ImGui::IsItemHovered(
                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                (ImGui::IsWindowHovered(
                     ImGuiHoveredFlags_RootAndChildWindows |
                     ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                 mousePos.x >= canvasPos.x &&
                 mousePos.x <= canvasPos.x + size.x &&
                 mousePos.y >= canvasPos.y &&
                 mousePos.y <= canvasPos.y + size.y);
            // 几何回退覆盖 InvisibleButton 等 active item 阻挡的情况。
            const ImGuiIO& io    = ImGui::GetIO();
            float          wheel = io.MouseWheel;
            if ( isHovered && std::abs(wheel) > 0.01f ) {
                // 修饰键 helper 返回 false 时才执行普通纵向时间滚动。
                if ( !handleTimelineModifierWheel(m_name,
                                                  wheel,
                                                  io.KeyCtrl || io.KeySuper,
                                                  io.KeyAlt,
                                                  io.KeyShift,
                                                  m_speedTooltipValue,
                                                  m_speedTooltipTimer) ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdScroll{ m_name, -wheel, io.KeyShift }));
                }
            }

            // 3. 处理 Timeline Timing 的工具交互和反馈
            bool windowFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            const bool toolbarFocusedOrHovered = isToolbarFocusedOrHovered();
            const bool timelineMouseClicked =
                isHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                              ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                              ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
            const bool outsideMouseClicked =
                !isHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
            if ( windowFocused || timelineMouseClicked ) {
                // 鼠标点击可以恢复此前因其它窗口点击而释放的焦点锁存。
                m_hasTimingInteractionFocus = true;
                if ( timelineMouseClicked ) {
                    ImGui::SetWindowFocus();
                    m_shouldFocusNextFrame = true;
                }
            } else if ( outsideMouseClicked && !toolbarFocusedOrHovered ) {
                // 工具栏是时间线工作流的一部分，不把其点击解释为离开时间线。
                m_hasTimingInteractionFocus = false;
            }
            m_wasFocusedLastFrame = m_hasTimingInteractionFocus;
            const bool hasTimingInteractionFocus =
                windowFocused || m_hasTimingInteractionFocus;
            ImVec2 gearGlyphSize = ImGui::CalcTextSize(UI::ICON_MMM_COG);
            float  iconSize      = std::ceil(
                std::max({ 20.0f, gearGlyphSize.x, gearGlyphSize.y }) + 4.0f);
            float padding = 5.0f;

            const auto& visual =
                Config::AppConfig::instance().getVisualConfig();
            float proximity   = visual.snapThreshold;
            float localMouseX = mousePos.x - canvasPos.x;
            float localMouseY = mousePos.y - canvasPos.y;
            bool  overMenuButton =
                localMouseX >= size.x - 56.0f && localMouseY <= 56.0f;
            // 右上菜单按钮有自己的点击语义，不能同时产生 Timing 吸附。
            const bool timelineHoveringForSnap = isHovered && !overMenuButton;
            const bool timelineDragging =
                ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSetMousePosition{
                    .cameraId       = m_name,
                    .mouseX         = localMouseX,
                    .mouseY         = localMouseY - m_lastAppliedYOffset,
                    .viewportWidth  = size.x,
                    .viewportHeight = size.y,
                    .isHovering     = timelineHoveringForSnap,
                    .isDragging     = timelineDragging }));
            // 逻辑层用修正后的本地 Y 计算 hover 时间和渲染反馈。
            bool   hoveredSnapped = false;
            double hoveredTime    = 0.0;
            if ( timelineHoveringForSnap ) {
                // 先按连续投影求时间，再依据拍线和像素阈值选择吸附结果。
                double rawHoveredTime = canvasTimeAtLocalY(size, localMouseY);
                hoveredTime           = snapTimingTime(
                    size, rawHoveredTime, localMouseY, hoveredSnapped);
            }

            const TimelineGearInfo gears[] = {
                { Common::Render::SCROLL_EFFECT_BPM,
                  ::MMM::TimingEffect::BPM,
                  &Common::Render::TimelineInteractiveElement::bpmEntity,
                  &Common::Render::TimelineInteractiveElement::bpmValue,
                  "BPM",
                  "BPM",
                  ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                  false },
                { Common::Render::SCROLL_EFFECT_SCROLL,
                  ::MMM::TimingEffect::SCROLL,
                  &Common::Render::TimelineInteractiveElement::scrollEntity,
                  &Common::Render::TimelineInteractiveElement::scrollValue,
                  "Scroll",
                  "Scroll",
                  ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                  true },
                { Common::Render::SCROLL_EFFECT_JUMP,
                  ::MMM::TimingEffect::JUMP,
                  &Common::Render::TimelineInteractiveElement::jumpEntity,
                  &Common::Render::TimelineInteractiveElement::jumpValue,
                  "Jump",
                  "Jump",
                  ImVec4(0.2f, 0.45f, 1.0f, 1.0f),
                  false },
                { Common::Render::SCROLL_EFFECT_HS,
                  ::MMM::TimingEffect::HS,
                  &Common::Render::TimelineInteractiveElement::hsEntity,
                  &Common::Render::TimelineInteractiveElement::hsValue,
                  "HS",
                  "HS",
                  ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                  true },
            };

            auto isNearInlineGearTime =
                [&](const Common::Render::TimelineInteractiveElement& el) {
                    bool isNearTime = hoveredSnapped &&
                                      std::abs(el.time - hoveredTime) < 1e-5;
                    bool isNearPixel = std::abs(localMouseY - el.y) < proximity;
                    // 精确吸附时间或视觉像素接近任一成立即可展示齿轮。
                    return isNearTime || isNearPixel;
                };

            auto countInlineGears =
                [&](const Common::Render::TimelineInteractiveElement& el,
                    bool rightSide) {
                    int count = 0;
                    for ( const auto& gear : gears ) {
                        // 先计数同侧有效实体，供位置函数对称排列。
                        if ( gear.rightSide != rightSide ) continue;
                        if ( (el.effects & gear.mask) == 0 ) continue;
                        auto entity = el.*(gear.entity);
                        if ( entity == entt::null ) continue;
                        ++count;
                    }
                    return count;
                };

            auto inlineGearPos =
                [&](const Common::Render::TimelineInteractiveElement& el,
                    const TimelineGearInfo&                           gear,
                    int                                               index,
                    int                                               count) {
                    if ( editorSettings.professionalMode ) {
                        // 专业模式忽略左右侧配置，按 TimingEffect 固定泳道。
                        constexpr float laneCount = 4.0f;
                        const int   lane = professionalTimingLane(gear.effect);
                        const float centerX =
                            canvasPos.x +
                            size.x * (static_cast<float>(lane) + 0.5f) /
                                laneCount;
                        const float minX = canvasPos.x + padding;
                        const float maxX = std::max(
                            minX, canvasPos.x + size.x - iconSize - padding);
                        const float minY = canvasPos.y;
                        const float maxY =
                            std::max(minY, canvasPos.y + size.y - iconSize);
                        return ImVec2(
                            std::clamp(centerX - iconSize * 0.5f, minX, maxX),
                            std::clamp(canvasPos.y + el.y - iconSize * 0.5f,
                                       minY,
                                       maxY));
                    }

                    float yOffset = 0.0f;
                    if ( count > 1 ) {
                        // 多齿轮围绕 marker 中心对称展开，间隔包含四像素空隙。
                        yOffset = (static_cast<float>(index) -
                                   (static_cast<float>(count) - 1.0f) * 0.5f) *
                                  (iconSize + 4.0f);
                    }

                    float x    = gear.rightSide
                                     ? canvasPos.x + size.x - iconSize - padding
                                     : canvasPos.x + padding;
                    float minY = canvasPos.y;
                    float maxY =
                        std::max(minY, canvasPos.y + size.y - iconSize);
                    float y = std::clamp(
                        canvasPos.y + el.y + yOffset - iconSize * 0.5f,
                        minY,
                        maxY);
                    return ImVec2(x, y);
                };

            /// @brief 当前鼠标命中的 Timeline 齿轮按钮。
            struct InlineGearHit {
                /// @brief Timing 实体。
                entt::entity entity{ entt::null };

                /// @brief Timing 时间戳，单位秒。
                double time{ 0.0 };

                /// @brief Timing 参数值。
                double value{ 0.0 };

                /// @brief 编辑类型。
                const char* editType{ "" };

                /// @brief 显示标签。
                const char* label{ "" };
            };

            auto openInlineGearEditor = [&](const InlineGearHit& hit) {
                // null entity 是无命中哨兵，不能打开没有目标的编辑器。
                if ( hit.entity == entt::null ) return;
                XINFO("{} gear clicked at time: {}", hit.label, hit.time);
                m_editingEntity          = hit.entity;
                m_editTime               = hit.time;
                m_editValue              = hit.value;
                m_editType               = hit.editType;
                m_keepSpeedOnBpmEdit     = false;
                m_isPopupOpen            = true;
                m_isCreatePopupOpen      = false;
                m_isTimingDrawPreviewing = false;
                m_isTimingDragging       = false;
                m_isTimingErasing        = false;
                m_shouldFocusNextFrame   = false;
                m_timingEraseTargetEntities.clear();
                // 弹窗接管后清除所有互斥手势，保证单一编辑事务。
                ::MMM::UI::FeedbackOpenPopup("TimelineEventEditor");
            };

            const bool inlineGearCanOpenEditor =
                !m_currentSnapshot->isPlaying &&
                Logic::EditorEngine::instance().getCurrentTool() ==
                    Logic::EditTool::Draw;
            const bool showInlineTimingEditors =
                inlineGearCanOpenEditor && isHovered && !overMenuButton &&
                m_currentSnapshot->hasBeatmap && !m_isTimingDragging &&
                !m_isTimingErasing && !m_isTimingDrawPreviewing &&
                !m_isPopupOpen && !m_isCreatePopupOpen;
            std::optional<InlineGearHit> inlineGearHit;
            bool                         inlineGearEditorOpened = false;
            if ( showInlineTimingEditors ) {
                // 预扫描只寻找首个命中，顺序与后续绘制完全一致。
                for ( const auto& el : m_currentSnapshot->timelineElements ) {
                    if ( !isNearInlineGearTime(el) ) continue;

                    const int leftGearCount  = countInlineGears(el, false);
                    const int rightGearCount = countInlineGears(el, true);
                    int       leftGearIndex  = 0;
                    int       rightGearIndex = 0;
                    for ( const auto& gear : gears ) {
                        if ( (el.effects & gear.mask) == 0 ) continue;
                        auto entity = el.*(gear.entity);
                        if ( entity == entt::null ) continue;

                        int count =
                            gear.rightSide ? rightGearCount : leftGearCount;
                        int index =
                            gear.rightSide ? rightGearIndex++ : leftGearIndex++;
                        ImVec2 pos = inlineGearPos(el, gear, index, count);
                        if ( mousePos.x >= pos.x &&
                             mousePos.x <= pos.x + iconSize &&
                             mousePos.y >= pos.y &&
                             mousePos.y <= pos.y + iconSize ) {
                            inlineGearHit = InlineGearHit{ entity,
                                                           el.time,
                                                           el.*(gear.value),
                                                           gear.editType,
                                                           gear.label };
                            break;
                        }
                    }
                    if ( inlineGearHit ) break;
                }
            }
            if ( inlineGearCanOpenEditor && inlineGearHit &&
                 ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
                // 在 InvisibleButton 建立前也能于按下首帧打开编辑器。
                openInlineGearEditor(*inlineGearHit);
                inlineGearEditorOpened = true;
            }

            handleTimingCanvasInteraction(
                canvasPos, size, isHovered, hasTimingInteractionFocus);
            refreshTimelineInteractionDecoration(size);
            // 交互状态先推进，再基于最终状态生成本帧临时几何。
            if ( editorSettings.professionalMode ) {
                renderProfessionalTimelineOverlay(canvasPos, size);
            }
            renderTimingInteractionOverlay(canvasPos, size);

            // 4. 绘制交互层元件 (齿轮按钮)
            for ( const auto& el : m_currentSnapshot->timelineElements ) {
                // 只有接近当前 hover 时间的 marker 展示内联齿轮，降低遮挡。
                if ( showInlineTimingEditors && isNearInlineGearTime(el) ) {
                    int leftGearCount  = countInlineGears(el, false);
                    int rightGearCount = countInlineGears(el, true);

                    int leftGearIndex  = 0;
                    int rightGearIndex = 0;
                    for ( const auto& gear : gears ) {
                        if ( (el.effects & gear.mask) == 0 ) continue;
                        auto entity = el.*(gear.entity);
                        if ( entity == entt::null ) continue;

                        int count =
                            gear.rightSide ? rightGearCount : leftGearCount;
                        int index =
                            gear.rightSide ? rightGearIndex++ : leftGearIndex++;
                        ImVec2 pos = inlineGearPos(el, gear, index, count);
                        ImGui::SetCursorScreenPos(pos);

                        std::string id =
                            fmt::format("{}_{}_{}",
                                        gear.label,
                                        el.time,
                                        static_cast<uint32_t>(entity));
                        const std::string buttonId =
                            "##TimelineInlineGear_" + id;
                        // ID 包含类型、时间与实体，多个同时间效果互不冲突。
                        ImGui::SetNextItemAllowOverlap();
                        const bool gearClicked = ImGui::InvisibleButton(
                            buttonId.c_str(), ImVec2(iconSize, iconSize));
                        ::MMM::UI::FeedbackLastItem(
                            ImGui::GetID(buttonId.c_str()), gearClicked);
                        // InvisibleButton 没有默认外观，但仍复用统一交互音效。
                        if ( inlineGearCanOpenEditor && gearClicked &&
                             !inlineGearEditorOpened ) {
                            openInlineGearEditor(
                                InlineGearHit{ entity,
                                               el.time,
                                               el.*(gear.value),
                                               gear.editType,
                                               gear.label });
                            inlineGearEditorOpened = true;
                        }

                        const bool   gearHovered  = ImGui::IsItemHovered();
                        const ImVec2 buttonMin    = ImGui::GetItemRectMin();
                        const ImVec2 buttonMax    = ImGui::GetItemRectMax();
                        ImDrawList*  drawList     = ImGui::GetWindowDrawList();
                        const float  gearRounding = iconSize * 0.25f;
                        const ImU32  gearBackground =
                            timelineGearBackgroundColor(gear.color,
                                                        gearHovered);
                        ImVec4 gearBorderColor = gear.color;
                        // hover 只提高边框不透明度，语义色保持不变。
                        gearBorderColor.w = gearHovered ? 1.0f : 0.82f;

                        // 对比色底板隔离底层同色 glow，确保齿轮轮廓始终清晰。
                        drawList->AddRectFilled(
                            buttonMin, buttonMax, gearBackground, gearRounding);
                        drawList->AddRect(
                            buttonMin,
                            buttonMax,
                            ImGui::ColorConvertFloat4ToU32(gearBorderColor),
                            gearRounding,
                            0,
                            gearHovered ? 2.0f : 1.0f);
                        drawList->AddText(
                            ImVec2(buttonMin.x +
                                       (iconSize - gearGlyphSize.x) * 0.5f,
                                   buttonMin.y +
                                       (iconSize - gearGlyphSize.y) * 0.5f),
                            ImGui::ColorConvertFloat4ToU32(gear.color),
                            UI::ICON_MMM_COG);

                        if ( gearHovered ) {
                            // tooltip 的单位和标签由 TimingEffect
                            // 描述器统一提供。
                            const auto timeText =
                                MMM::UI::Utils::formatCanvasTime(
                                    el.time, m_currentSnapshot);
                            const auto descriptor =
                                timelineTimingTooltipDescriptor(gear.effect);
                            ImGui::SetTooltip(
                                "%s Event: %s\n%.*s: %.6g%.*s",
                                gear.label,
                                timeText.c_str(),
                                static_cast<int>(descriptor.label.size()),
                                descriptor.label.data(),
                                el.*(gear.value),
                                static_cast<int>(descriptor.valueSuffix.size()),
                                descriptor.valueSuffix.data());
                        }
                    }
                }
            }
            // 5. 渲染弹窗

            renderEventEditorPopup();
            renderEventCreationPopup();
        }
    }

    renderTimingPointsTableWindow();
    // 表格窗口始终独立绘制，不依赖主时间线是否有有效 descriptor。

    if ( m_speedTooltipTimer > 0.0f ) {
        // 非阻塞计时结束后自然停止绘制，不使用 sleep 或定时线程。
        m_speedTooltipTimer -= ImGui::GetIO().DeltaTime;
        renderPlaybackSpeedTooltip(m_speedTooltipValue);
    }
}

/// @brief 绘制 Timeline 专业模式分轨覆盖层。
/// @details 四个等宽泳道只追加底部文字标签，不修改 Timing marker 几何；
/// 每个标签单独裁剪在所属泳道，窄窗口下不会覆盖相邻类型。
/// 覆盖层使用当前 ImGui DrawList，位于 Vulkan Image 之上但不接收输入。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
void TimelineCanvas::renderProfessionalTimelineOverlay(const ImVec2& canvasPos,
                                                       const ImVec2& size)
{
    if ( !m_currentSnapshot || size.x <= 1.0f || size.y <= 1.0f ) {
        // 无快照或退化视口无法建立四泳道，直接跳过覆盖层。
        return;
    }

    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    const ImVec2 clipMax(canvasPos.x + size.x, canvasPos.y + size.y);
    drawList->PushClipRect(canvasPos, clipMax, true);
    // 外层裁剪保证标签不会溢出当前时间线 Dock 窗口。

    constexpr int laneCount = 4;
    const float   laneWidth = size.x / static_cast<float>(laneCount);
    const char*   laneLabels[laneCount] = { "BPM", "Scroll", "Jump", "HS" };
    const ImU32   laneTextColor         = IM_COL32(240, 235, 225, 210);

    for ( int lane = 0; lane < laneCount; ++lane ) {
        // 最后一泳道直接使用 clipMax，吸收浮点等分后的尾部误差。
        const float laneX0 = canvasPos.x + laneWidth * lane;
        const float laneX1 =
            lane == laneCount - 1 ? clipMax.x : laneX0 + laneWidth;
        const ImVec2 textSize = ImGui::CalcTextSize(laneLabels[lane]);
        const float  labelX =
            laneX0 + std::max(4.0f, (laneX1 - laneX0 - textSize.x) * 0.5f);
        const float labelY = clipMax.y - textSize.y - 8.0f;
        drawList->PushClipRect(ImVec2(laneX0 + 3.0f, clipMax.y - 32.0f),
                               ImVec2(laneX1 - 3.0f, clipMax.y),
                               true);
        // 内层裁剪把长标签限制在本泳道底部三十二像素区域。
        drawList->AddText(
            ImVec2(labelX, labelY), laneTextColor, laneLabels[lane]);
        drawList->PopClipRect();
    }

    drawList->PopClipRect();
}

/// @brief 请求下一帧将时间线窗口聚焦到前台。
void TimelineCanvas::requestFocus()
{
    // 同时锁存交互焦点，下一帧 Begin 前再请求 ImGui 窗口聚焦。
    m_shouldFocusNextFrame      = true;
    m_hasTimingInteractionFocus = true;
    m_wasFocusedLastFrame       = true;
}

/// @brief 设置时间点批量编辑表格窗口打开状态。
/// @param open 是否打开表格窗口。
void TimelineCanvas::setTimingPointsTableOpen(bool open)
{
    // 外部显式设置打开状态时，打开需要恢复位置但不强制抢焦点。
    m_auxiliaryWindowState.timingPointsTableOpen = open;
    m_shouldRecoverTableWindow                   = open;
    m_shouldFocusTableWindow                     = false;
    m_isTableWindowFocusedAndReachable           = false;
}

/// @brief 激活时间点批量编辑表格；已聚焦可见时关闭，否则恢复并聚焦。
void TimelineCanvas::activateTimingPointsTable()
{
    // 统一纯函数区分“当前可达则关闭”和“隐藏/脱离则恢复并聚焦”。
    const auto activation = resolveTimelineTableWindowActivation(
        m_auxiliaryWindowState.timingPointsTableOpen,
        m_isTableWindowFocusedAndReachable);
    m_auxiliaryWindowState.timingPointsTableOpen = activation.open;
    m_shouldFocusTableWindow                     = activation.requestFocus;
    m_shouldRecoverTableWindow                   = activation.requestRecovery;
    if ( !activation.open ) {
        // 关闭后清除可达性缓存，下一次激活按恢复路径处理。
        m_isTableWindowFocusedAndReachable = false;
    }
}

/// @brief 获取时间线窗口当前所在的 ImGui Dock 节点。
/// @return 当前窗口停靠节点 ID；未停靠时返回 0。
ImGuiID TimelineCanvas::getDockId() const
{
    return m_lastDockId;
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
TimelineCanvas::getVertices() const
{
    // 渲染器只观察当前交换完成的快照；无快照返回稳定空容器引用。
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

const std::vector<uint32_t>& TimelineCanvas::getIndices() const
{
    // 顶点和索引必须来自同一 currentSnapshot 代际。
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

bool TimelineCanvas::isDirty() const
{
    // 主时间线可见时持续渲染；隐藏状态由独立表格准备条件另行处理。
    return Config::AppConfig::instance().getEditorSettings().showTimelineWindow;
}

/// @brief 判断当前帧是否需要准备时间线快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
bool TimelineCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    // 主窗口或辅助表格任一需要数据时都准备快照，避免隐藏主窗口使表格失效。
    return m_syncBuffer && m_isOpen &&
           shouldPrepareTimelineSnapshot(Config::AppConfig::instance()
                                             .getEditorSettings()
                                             .showTimelineWindow,
                                         m_auxiliaryWindowState);
}

/// @brief 在线程池中拉取并准备时间线快照。
/// @param snapshot 当前帧 UI 快照。
void TimelineCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    // 临时交互装饰写在上一份可写快照上，拉取新代际前必须先恢复。
    resetTimelineInteractionDecoration();
    // prepareCanvasSnapshot 在工作线程选择最新快照并计算视觉 Y 偏移。
    m_preparedSnapshot = prepareCanvasSnapshot(
        m_syncBuffer.get(), m_lastOffsetSnapshot, m_lastAppliedYOffset, false);
    m_hasPreparedSnapshot = true;
}

/// @brief 将准备好的时间线快照切换到主线程可见状态。
void TimelineCanvas::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedSnapshot ) {
        // 没有工作线程产物时保留当前可见快照。
        return;
    }

    auto&         engine      = Logic::EditorEngine::instance();
    const int32_t activeIndex = engine.getActiveSessionIndex();
    const auto*   activeEntry = engine.getSessionEntry(activeIndex);
    if ( !activeEntry || activeEntry->isLogoPlaceholder ) {
        // Logo 占位会话不应显示上一个谱面的时间线数据。
        m_currentSnapshot     = nullptr;
        m_lastOffsetSnapshot  = nullptr;
        m_lastAppliedYOffset  = 0.0f;
        m_hasPreparedSnapshot = false;
        return;
    }

    m_currentSnapshot = m_preparedSnapshot.snapshot;
    // 三个字段作为一个准备结果原子式切换到 UI 可见状态。
    m_lastOffsetSnapshot  = m_preparedSnapshot.offsetSnapshot;
    m_lastAppliedYOffset  = m_preparedSnapshot.appliedYOffset;
    m_hasPreparedSnapshot = false;

    if ( !m_currentSnapshot ) {
        // 空结果不能继续沿用旧偏移快照，否则鼠标 Y 会跨代际修正。
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }
}

/// @brief 清除上一帧追加到 Timeline 快照中的交互修饰。
/// @details 恢复被改写顶点颜色，并把顶点、索引、普通命令和 glow 命令
/// 截断到装饰前记录的长度；该过程保证同一快照重复使用时不会累积预览几何。
/// @warning UI 准备路径：准备新帧前调用；只修改当前 UI 专用快照副本。
void TimelineCanvas::resetTimelineInteractionDecoration()
{
    if ( !m_decoratedTimelineSnapshot ) {
        // 没有装饰快照时仍清空残留恢复表，维持状态不变量。
        m_timelineColorRestore.clear();
        return;
    }

    auto* snapshot = m_decoratedTimelineSnapshot;
    for ( const auto& restore : m_timelineColorRestore ) {
        // 顶点数量若被外部缩短则跳过失效索引，避免越界。
        if ( restore.vertexIndex < snapshot->vertices.size() ) {
            snapshot->vertices[restore.vertexIndex].color = restore.color;
        }
    }

    if ( snapshot->vertices.size() >= m_decoratedTimelineVertexCount ) {
        // 只在当前容器至少达到基线时截断，防御其它阶段已替换数据。
        snapshot->vertices.resize(m_decoratedTimelineVertexCount);
    }
    if ( snapshot->indices.size() >= m_decoratedTimelineIndexCount ) {
        snapshot->indices.resize(m_decoratedTimelineIndexCount);
    }
    if ( snapshot->cmds.size() >= m_decoratedTimelineCmdCount ) {
        snapshot->cmds.resize(m_decoratedTimelineCmdCount);
    }
    if ( snapshot->glowCmds.size() >= m_decoratedTimelineGlowCmdCount ) {
        snapshot->glowCmds.resize(m_decoratedTimelineGlowCmdCount);
    }

    m_decoratedTimelineSnapshot = nullptr;
    // 所有基线计数与恢复列表成组清零，重复 reset 保持幂等。
    m_decoratedTimelineVertexCount  = 0;
    m_decoratedTimelineIndexCount   = 0;
    m_decoratedTimelineCmdCount     = 0;
    m_decoratedTimelineGlowCmdCount = 0;
    m_timelineColorRestore.clear();
}

/// @brief 根据当前 Timeline 交互状态刷新快照半透明与发光命令。
///
/// @details 装饰基线：
/// - 函数入口先恢复上一帧对快照做出的全部临时改写。
/// - 无有效谱面时不建立新的装饰基线。
/// - 记录原始 vertices 长度。
/// - 记录原始 indices 长度。
/// - 记录原始普通 draw command 长度。
/// - 记录原始 glow command 长度。
/// - 原 marker 顶点颜色改写前逐项保存。
/// - 预览几何始终追加在原容器末尾。
/// - 下一次 reset 通过基线长度截断追加内容。
/// - 无装饰产生时清除 decorated snapshot 指针。
/// - decorated snapshot 只指向 UI 可写快照代际。
/// - 逻辑线程发布的原始快照内容不承担交互状态持久化。
///
/// @details 状态到视觉的映射：
/// - hover marker 追加发光命令。
/// - 已选择但未拖动的 marker 追加发光命令。
/// - 弹窗正在编辑的 marker 追加发光命令。
/// - 擦除候选 marker 追加发光命令。
/// - 正在拖动的原 marker 降低透明度。
/// - 弹窗编辑的原 marker 降低透明度。
/// - 擦除候选原 marker 改为红色并降低透明度。
/// - 拖动候选在新时间位置绘制半透明 marker。
/// - 创建候选在当前吸附时间绘制半透明 marker。
/// - 候选 marker 同时进入普通层和 glow 层。
/// - 多种状态作用于同一 marker 时 glow 只追加一次。
/// - 多种编辑状态作用于同一 marker 时颜色只改写一次。
///
/// @details marker 几何：
/// - 普通模式 marker 使用扣除左右 padding 后的宽度。
/// - 专业模式 marker 只占对应 TimingEffect 泳道。
/// - marker 宽度至少为一个逻辑像素。
/// - 默认高度按宽度的固定比例推导。
/// - Note 纹理 UV 有效时按其宽高比推导高度。
/// - UV 宽度非正时保留默认高度。
/// - BPM 使用专业模式第零泳道。
/// - Scroll 使用第一泳道。
/// - Jump 使用第二泳道。
/// - HS 使用第三泳道。
/// - 颜色由 TimingEffect 选择，透明度由预览状态提供。
/// - Batcher 使用 Note 纹理与当前 noteFillMode。
/// - preview index range 从追加前后索引长度之差获得。
/// - preview glow 键由其唯一追加范围生成。
///
/// @details 可见目标处理：
/// - 只遍历 `collectVisibleTimingTargets` 返回的当前可见目标。
/// - selected 通过实体 ID 查询本地选择集合。
/// - hovered 与单一 hover 实体比较。
/// - erasing 查询当前擦除候选集合。
/// - dragging 要求全局拖动状态和目标已选中。
/// - popupEditing 要求弹窗打开且实体等于编辑目标。
/// - editing 是拖动、弹窗编辑和擦除状态的并集。
/// - 没有 marker geometry 的目标仍可参与逻辑选择。
/// - glow 只对有效 index range 生效。
/// - 颜色改写只对有效 vertex range 生效。
/// - 容器边界在循环内再次防御检查。
///
/// @details 拖动预览：
/// - 所有选中目标共享 `m_timingDragPreviewDelta`。
/// - 候选时间等于原时间加统一增量。
/// - 候选时间最小钳制为零。
/// - 时间通过当前 Timeline 投影转换为局部 Y。
/// - marker 完全超出视口和一个自身高度后跳过。
/// - 边缘部分可见时仍追加完整候选几何。
/// - 原 marker 保持半透明作为移动来源参考。
/// - 候选 marker 使用 0.42 的统一透明度。
/// - 预览不写回实体时间。
/// - 最终命令提交由交互状态机负责。
///
/// @details 创建预览：
/// - 只在 `m_isTimingDrawPreviewing` 为真时生成。
/// - 退化画布尺寸不生成预览。
/// - createType 通过稳定映射转换为 TimingEffect。
/// - 预览时间已经由交互层完成吸附。
/// - 预览 Y 缓存供 ImGui 覆盖层绘制辅助提示。
/// - 创建预览同样使用 0.42 透明度。
/// - 创建预览不生成真实 Timing entity。
/// - 弹窗打开或手势取消后状态机会关闭预览。
///
/// @details 恢复安全：
/// - 顶点恢复项包含原索引和完整原色。
/// - 索引越出当前容器时安全跳过。
/// - 截断只在容器长度仍不小于基线时执行。
/// - 这可防御准备阶段替换或清空同一快照容器。
/// - reset 后所有基线计数归零。
/// - reset 后颜色恢复列表 clear 但可复用容量。
/// - 重复 reset 保持幂等。
/// - 本函数不提交 Vulkan 工作。
/// - 本函数不修改 Timing 数据模型。
/// - 所有视觉变化都能在下一帧无损撤销。
/// @details 本函数记录原始容器长度后临时改写 UI 专用快照：悬停、选择、
/// 弹窗编辑和擦除目标追加 glow，正在编辑的原 marker 降低透明度，
/// 拖动与创建则追加半透明候选 marker。下一次刷新前由 reset 完整恢复。
/// @warning UI 热路径：交互时间线每帧调用；只遍历可见 Timing target，
/// 不访问 ECS、不加载纹理，也不提交 Vulkan 命令。
/// @param size 当前 Timeline 画布尺寸。
void TimelineCanvas::refreshTimelineInteractionDecoration(const ImVec2& size)
{
    resetTimelineInteractionDecoration();
    // 每次都从无装饰基线开始，防止同一快照连续帧重复追加几何。
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) {
        return;
    }

    m_decoratedTimelineSnapshot = m_currentSnapshot;
    // 四类容器长度共同定义恢复基线，任何预览追加都位于其后。
    m_decoratedTimelineVertexCount  = m_currentSnapshot->vertices.size();
    m_decoratedTimelineIndexCount   = m_currentSnapshot->indices.size();
    m_decoratedTimelineCmdCount     = m_currentSnapshot->cmds.size();
    m_decoratedTimelineGlowCmdCount = m_currentSnapshot->glowCmds.size();

    bool                         hasDecoration = false;
    std::unordered_set<uint64_t> glowMarkers;
    std::unordered_set<uint64_t> dimMarkers;
    // 同一 marker 可能被 hover、选择和编辑多种状态命中，集合负责去重。
    const bool professionalMode =
        Config::AppConfig::instance().getEditorSettings().professionalMode;
    float paddingX = 30.0f;

    /// @brief Timeline marker 的绘制矩形参数。
    struct MarkerDrawRect {
        /// @brief 左侧 X 坐标。
        float x{ 0.0f };
        /// @brief 宽度。
        float w{ 0.0f };
        /// @brief 高度。
        float h{ 0.0f };
    };

    auto markerDrawRect = [&](::MMM::TimingEffect effect) {
        // 普通模式 marker 横跨时间线主体；专业模式只占所属泳道。
        float noteW = std::max(1.0f, size.x - paddingX * 2.0f);
        float noteX = paddingX;
        if ( professionalMode ) {
            constexpr float laneCount = 4.0f;
            const float     laneWidth = size.x / laneCount;
            const int       lane      = professionalTimingLane(effect);
            noteW                     = std::max(1.0f, laneWidth - 2.0f);
            noteX                     = laneWidth * static_cast<float>(lane) +
                    (laneWidth - noteW) * 0.5f;
        }

        float noteH = noteW * 0.36f;
        if ( auto uvIt = m_currentSnapshot->uvMap.find(
                 static_cast<uint32_t>(Common::Render::TextureID::Note));
             uvIt != m_currentSnapshot->uvMap.end() && uvIt->second.w > 0.0f ) {
            // 纹理 UV 的宽高比例决定 marker 高度，避免皮肤资源被拉伸。
            noteH = noteW * (uvIt->second.w / uvIt->second.z);
        }
        return MarkerDrawRect{ noteX, noteW, noteH };
    };

    auto appendGlowRange =
        [&](uint32_t indexOffset, uint32_t indexCount, uint64_t key) {
            if ( indexCount == 0U || !glowMarkers.insert(key).second ) {
                // 空范围或已处理 marker 不追加重复 glow draw command。
                return;
            }

            Common::Render::CanvasDrawCmd cmd;
            cmd.indexOffset  = indexOffset;
            cmd.indexCount   = indexCount;
            cmd.vertexOffset = 0;
            cmd.customTextureId =
                static_cast<uint32_t>(Common::Render::TextureID::Note);
            cmd.scissor = {
                0,
                0,
                static_cast<uint32_t>(std::max(1.0f, std::ceil(size.x))),
                static_cast<uint32_t>(std::max(1.0f, std::ceil(size.y)))
            };
            // 发光层裁剪覆盖完整时间线物理逻辑区域，实际物理换算在录制阶段。
            m_currentSnapshot->glowCmds.push_back(cmd);
            hasDecoration = true;
        };

    auto appendPreviewMarker =
        [&](float y, ::MMM::TimingEffect effect, float alpha) {
            const MarkerDrawRect rect = markerDrawRect(effect);
            const uint32_t       previewIndexOffset =
                static_cast<uint32_t>(m_currentSnapshot->indices.size());
            Logic::System::Batcher previewBatcher(m_currentSnapshot,
                                                  &m_currentSnapshot->cmds);
            // 临时 Batcher 直接追加到快照尾部，基线计数保证下一帧可截断。
            previewBatcher.setTexture(Common::Render::TextureID::Note);
            previewBatcher.pushFilledQuad(
                rect.x,
                y + rect.h * 0.5f,
                rect.w,
                rect.h,
                { 1.0f, 1.0f },
                Config::AppConfig::instance().getVisualConfig().noteFillMode,
                timelineEffectColor(effect, alpha));
            previewBatcher.flush();

            const uint32_t previewIndexCount =
                static_cast<uint32_t>(m_currentSnapshot->indices.size()) -
                previewIndexOffset;
            const uint64_t previewKey =
                timelineMarkerKey(previewIndexOffset, previewIndexCount);
            appendGlowRange(previewIndexOffset, previewIndexCount, previewKey);
            // 预览 marker 同时进入普通层和 glow 层，维持与实体 marker
            // 一致外观。
        };

    auto transformMarkerColor =
        [&](uint32_t                                     vertexOffset,
            uint32_t                                     vertexCount,
            float                                        multiplier,
            const std::optional<Graphic::Vertex::Color>& overrideColor) {
            const uint32_t endVertex = vertexOffset + vertexCount;
            for ( uint32_t i = vertexOffset;
                  i < endVertex && i < m_currentSnapshot->vertices.size();
                  ++i ) {
                // 每个顶点改写前保存原色，同一 marker 由 dimMarkers
                // 保证只处理一次。
                m_timelineColorRestore.push_back(
                    { i, m_currentSnapshot->vertices[i].color });
                if ( overrideColor ) {
                    // 擦除预览先覆盖红色，再统一乘透明度强调待删除状态。
                    m_currentSnapshot->vertices[i].color = *overrideColor;
                }
                m_currentSnapshot->vertices[i].color.a *= multiplier;
                hasDecoration = true;
            }
        };

    auto appendGlow = [&](const TimelineHitTarget& target) {
        // 只有快照提供实际 marker 索引范围时才能复用原几何发光。
        if ( !target.hasMarkerGeometry || target.markerIndexCount == 0U ) {
            return;
        }
        const uint64_t key = timelineMarkerKey(target.markerIndexOffset,
                                               target.markerIndexCount);
        appendGlowRange(target.markerIndexOffset, target.markerIndexCount, key);
    };

    for ( const auto& target : collectVisibleTimingTargets() ) {
        // 所有状态都以实体 ID 与当前本地交互集合比较，不查询 registry。
        const bool selected = m_selectedTimingEntities.find(target.entity) !=
                              m_selectedTimingEntities.end();
        const bool hovered = target.entity == m_hoveredTimingEntity;
        const bool erasing = m_timingEraseTargetEntities.find(target.entity) !=
                             m_timingEraseTargetEntities.end();
        const bool dragging = m_isTimingDragging && selected;
        const bool popupEditing =
            m_isPopupOpen && target.entity == m_editingEntity;
        const bool editing = dragging || popupEditing || erasing;

        if ( hovered || (selected && !dragging) || popupEditing || erasing ) {
            // 拖动选中项使用候选 marker，不再给原位置单独保留 selected glow。
            appendGlow(target);
        }
        if ( editing && target.hasMarkerGeometry ) {
            // 正在编辑的原 marker 半透明，明确区分原位置与候选位置。
            const uint64_t key = timelineMarkerKey(target.markerIndexOffset,
                                                   target.markerIndexCount);
            if ( dimMarkers.insert(key).second ) {
                std::optional<Graphic::Vertex::Color> overrideColor;
                if ( erasing ) {
                    overrideColor =
                        Graphic::Vertex::Color{ 1.0f, 0.2f, 0.2f, 1.0f };
                }
                transformMarkerColor(target.markerVertexOffset,
                                     target.markerVertexCount,
                                     0.5f,
                                     overrideColor);
            }
        }
        if ( dragging ) {
            // 所有选中 Timing 使用相同时间增量，保持多选相对间距。
            const double previewTime =
                std::max(0.0, target.time + m_timingDragPreviewDelta);
            const float previewY =
                static_cast<float>(canvasYAtTime(size, previewTime));
            const MarkerDrawRect rect = markerDrawRect(target.effect);
            if ( previewY >= -rect.h && previewY <= size.y + rect.h ) {
                // 给 marker 高度留缓冲，边缘部分可见时仍绘制完整候选。
                appendPreviewMarker(previewY, target.effect, 0.42f);
            }
        }
    }

    if ( m_isTimingDrawPreviewing && size.x > 1.0f && size.y > 1.0f ) {
        // 创建预览类型来自当前弹窗/工具状态，时间已经由交互层吸附。
        const auto  previewEffect = timelineEffectFromCreateType(m_createType);
        const float previewY =
            static_cast<float>(canvasYAtTime(size, m_timingDrawPreviewTime));
        m_timingDrawPreviewY = previewY;
        appendPreviewMarker(previewY, previewEffect, 0.42f);
    }

    if ( !hasDecoration ) {
        // 没有任何改写时撤销装饰指针，下一帧无需执行恢复扫描。
        m_decoratedTimelineSnapshot = nullptr;
    }
}

void TimelineCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                                uint32_t h) const
{
    // 时间线通过事件通知逻辑渲染端更新 camera viewport，不直接访问会话。
    Event::CanvasResizeEvent e;
    e.canvasName = m_name;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    // 旧尺寸和新尺寸同时发送，订阅者可判断首次注册或真实缩放。
    Event::EventBus::instance().publish(e);
}

std::vector<std::string> TimelineCanvas::getShaderSources(
    const std::string& shader_name)
{
    // Shader 源码加载约定：
    // - 输入名称是皮肤 canvas_shader_modules 中的逻辑模块键。
    // - 返回字符串保存已编译 SPIR-V 原始字节。
    // - 缓存按逻辑模块键区分，不按解析后的文件路径区分。
    // - 已缓存模块直接按值返回当前阶段列表。
    // - 当前皮肤使用 Basic2DCanvas 的 shader 模块配置。
    // - 未声明模块返回空列表且不写缓存。
    // - 声明目录不存在时记录一次警告并返回空列表。
    // - 路径存在性查询使用 error_code，避免异常机制。
    // - VertexShader.spv 是必需的第一阶段。
    // - FragmentShader.spv 是必需的末阶段。
    // - GeometryShader.spv 是可选的中间阶段。
    // - 有几何阶段时返回三项固定顺序。
    // - 无几何阶段时返回两项固定顺序。
    // - 文件读取统一通过 VKShader helper 完成。
    // - 路径进入读取器前显式转换为 UTF-8。
    // - 成功结果写入实例级缓存。
    // - 皮肤切换由 invalidateShaderSourceCache 清除旧结果。
    // - 本函数只允许在 shader 初始化或重载低频路径调用。
    // - 常规每帧录制不得调用本函数访问文件系统。
    // - 空返回值由上层 shader 管线创建流程处理。
    // - 本函数不尝试自动回退到其它皮肤目录。
    // - 本函数不编译文本 shader，也不改变二进制内容。
    // - 多个 TimelineCanvas 实例各自维护缓存生命周期。
    // - `getShaderName` 另行把实例名加入渲染器全局键。
    // - 缓存失效与纹理 atlas 重载是两个独立操作。
    // - 读取失败的诊断边界属于 VKShader，不在此吞掉错误内容。
    // shader 二进制按模块名缓存，常规命令录制不重复访问文件系统。
    if ( m_shaderSourceCache.count(shader_name) )
        return m_shaderSourceCache[shader_name];

    // Timeline 与 Basic2DCanvas 复用皮肤定义的画布 shader 模块布局。
    auto canvas_config =
        Config::SkinManager::instance().getCanvasConfig("Basic2DCanvas");
    auto it = canvas_config.canvas_shader_modules.find(shader_name);
    if ( it != canvas_config.canvas_shader_modules.end() ) {
        auto            path = it->second;
        std::error_code shaderPathError;
        // error_code 路径查询避免异常；缺失模块记录警告并返回空源码。
        if ( !std::filesystem::exists(path, shaderPathError) ||
             shaderPathError ) {
            XWARN("Timeline shader module {} not defined.", shader_name);
            return {};
        }

        std::string vertexShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "VertexShader.spv"));
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "FragmentShader.spv"));
        // 顶点和片元阶段是必需文件，读取错误由 VKShader 统一报告。

        if ( auto geometryShaderPath = path / "GeometryShader.spv";
             std::filesystem::exists(geometryShaderPath, shaderPathError) &&
             !shaderPathError ) {
            // 几何阶段可选；存在时返回顺序固定为 vertex、geometry、fragment。
            m_shaderSourceCache[shader_name] = { vertexShaderSource,
                                                 Graphic::VKShader::readFile(
                                                     Config::pathToUtf8(
                                                         geometryShaderPath)),
                                                 fragmentShaderSource };
        } else {
            // 无几何阶段时保持 vertex、fragment 两项约定。
            m_shaderSourceCache[shader_name] = { vertexShaderSource,
                                                 fragmentShaderSource };
        }
        return m_shaderSourceCache[shader_name];
    }
    // 皮肤未声明该模块时返回空列表，让上层选择错误处理或回退。
    return {};
}

std::string TimelineCanvas::getShaderName(const std::string& shader_module_name)
{
    // 画布实例名参与全局 shader 键，避免多个渲染视图缓存互相覆盖。
    return m_name + ":" + shader_module_name;
}

/// @brief 清空缓存的 shader 源码。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void TimelineCanvas::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

bool TimelineCanvas::needReload()
{
    // exchange 保证一次资源重载请求只被渲染调度消费一次。
    return std::exchange(m_needReload, false);
}

void TimelineCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                    vk::Device&         logicalDevice,
                                    vk::CommandPool& cmdPool, vk::Queue& queue)
{
    // 资源重载契约：
    // - 只在渲染资源初始化或皮肤变更的低频路径调用。
    // - physicalDevice 和 logicalDevice 必须属于当前 VKContext。
    // - cmdPool 与 queue 用于 atlas 上传，不在本函数外长期保存。
    // - 新 atlas 构建成功后 unique_ptr 自动释放旧 atlas。
    // - 无纹理命令始终拥有内置白色采样区域。
    // - 白纹理采用 RGBA8 不透明数据。
    // - Note 纹理路径由当前皮肤资源映射解析。
    // - 空 Note 路径表示皮肤未提供该可选资源。
    // - atlas 固定最大边长由现有渲染资源约定保持为 1024。
    // - build 完成前不得读取任何 atlas UV。
    // - UV map 重建前先清除旧皮肤坐标。
    // - None 纹理 UV 无条件写入。
    // - Note 纹理 UV 只在实际加入 atlas 后写入。
    // - UV map 发布到对应 canvasName，避免污染主画布。
    // - 逻辑层后续快照使用新 UV 生成 marker 几何。
    // - 当前已生成快照不会在本函数内就地重写 UV。
    // - 本函数不缓存文件路径或原始像素数据。
    // 资源重载创建新的 atlas，以值替换旧对象并由 unique_ptr 管理生命周期。
    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);

    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255 };
    // 2x2 白纹理为无自定义纹理命令提供稳定 atlas 区域。
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Common::Render::TextureID::None), white, 2, 2);

    auto& skin           = Config::SkinManager::instance();
    auto  notePath       = skin.getAssetPath("note.note");
    bool  hasNoteTexture = false;
    if ( !notePath.empty() ) {
        // Note 皮肤资源可选；只有解析到有效路径时加入 atlas。
        m_textureAtlas->addTexture(
            static_cast<uint32_t>(Common::Render::TextureID::Note), notePath);
        hasNoteTexture = true;
    }

    m_textureAtlas->build(1024);
    // atlas 构建完成后 UV 才稳定，随后重新发布给逻辑快照生成端。

    m_atlasUVs.clear();
    m_atlasUVs[static_cast<uint32_t>(Common::Render::TextureID::None)] =
        m_textureAtlas->getUV(
            static_cast<uint32_t>(Common::Render::TextureID::None));
    if ( hasNoteTexture ) {
        // 未加载 Note 时不写伪造 UV，绘制命令会使用默认 descriptor。
        m_atlasUVs[static_cast<uint32_t>(Common::Render::TextureID::Note)] =
            m_textureAtlas->getUV(
                static_cast<uint32_t>(Common::Render::TextureID::Note));
    }

    Logic::EditorEngine::instance().setAtlasUVMap(m_canvasName, m_atlasUVs);
    // UV map 按画布名隔离，时间线 marker 与主画布可使用不同 atlas。
}

/// @brief 录制 Timeline 普通颜色层的 Vulkan indexed draw 命令。
///
/// @details 录制前置条件：
/// - 上层已经绑定兼容的 graphics pipeline。
/// - 上层已经绑定当前快照的顶点和索引缓冲。
/// - pipelineLayout 的 set 0 与传入 setLayout 兼容。
/// - defaultDescriptor 在 atlas 不可用时提供安全纹理。
/// - frameIndex 由统一渲染接口传入，本实现不需要逐帧资源索引。
/// - 当前快照为空时立即返回。
///
/// @details descriptor 选择：
/// - customTextureId 在 atlas UV map 中存在时选择 atlas descriptor。
/// - 未收录的纹理 ID 选择 defaultDescriptor。
/// - atlas 对象存在但 descriptor 获取失败时再次回退。
/// - 同一 descriptor 的连续命令只绑定一次。
/// - lastBound 初始为空，首条命令一定建立有效绑定。
/// - descriptor 生命周期由渲染器池和 atlas 管理。
///
/// @details 裁剪与绘制：
/// - 快照 scissor 使用逻辑像素。
/// - `getPhysicalScissor` 统一应用 framebuffer scale。
/// - 连续相同 scissor 只设置一次动态状态。
/// - lastScissor 初始默认值遵守 CanvasScissor 比较约定。
/// - indexOffset 与 vertexOffset 直接来自快照批处理命令。
/// - 每条命令绘制一个实例。
/// - 本函数不合并、排序或修改命令列表。
/// - 本函数不创建 descriptor 或上传纹理。
/// - 本函数不等待 fence 或 queue idle。
/// @details 按快照命令顺序选择 atlas 或默认 descriptor，并缓存最近纹理与
/// scissor 以省略重复状态绑定；顶点和索引缓冲由 IRenderableView 提前绑定。
/// @warning 渲染热路径：每帧离屏录制调用；禁止分配、文件访问或等待 GPU。
void TimelineCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                      vk::PipelineLayout      pipelineLayout,
                                      vk::DescriptorSetLayout setLayout,
                                      vk::DescriptorSet       defaultDescriptor,
                                      uint32_t                frameIndex)
{
    // 无当前快照时没有可录制命令，也不能访问其命令列表。
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // descriptor 从渲染器共享池按目标 set layout 获取。
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBound = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        // atlas 中存在对应 UV 时使用 atlas descriptor，否则使用默认纹理。
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;
        if ( tex == VK_NULL_HANDLE ) {
            // atlas descriptor 创建失败时再次回退，避免绑定空句柄。
            tex = defaultDescriptor;
        }

        if ( tex != lastBound ) {
            // 连续命令使用同一纹理时省略 descriptor set 重绑。
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
            // 快照保存逻辑像素裁剪，录制前统一换算到 framebuffer 物理像素。
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
        // 每条 CanvasDrawCmd 对应一个实例数为一的 indexed draw。
    }
}

/// @brief 录制 Timeline Timing 发光层离屏绘制命令。
///
/// @details Glow pass 契约：
/// - 只消费 `glowCmds`，不重复普通颜色层命令。
/// - descriptor 选择规则与普通 pass 完全一致。
/// - scissor 物理换算规则与普通 pass 完全一致。
/// - 独立 render pass 必须从空状态缓存开始。
/// - 不依赖普通 pass 最后绑定的 descriptor。
/// - 不依赖普通 pass 最后设置的 scissor。
/// - 发光命令索引范围引用同一快照顶点和索引缓冲。
/// - 交互装饰阶段已经去重相同 marker range。
/// - 空 glow 列表由 `hasGlowDrawCmds` 让调度器提前跳过。
/// - 本函数仍防御空 currentSnapshot。
/// - atlas 缺失或 descriptor 无效时回退默认纹理。
/// - 连续相同状态按 pass 内缓存省略重复绑定。
/// - 每条命令仍绘制一个实例。
/// - 本函数不修改 glow command 或顶点颜色。
/// - 本函数不触发资源加载、同步等待或内存分配。
/// @warning 渲染热路径：每帧离屏命令录制时执行；只遍历 glow 命令列表。
void TimelineCanvas::onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                                      vk::PipelineLayout      pipelineLayout,
                                      vk::DescriptorSetLayout setLayout,
                                      vk::DescriptorSet       defaultDescriptor,
                                      uint32_t                frameIndex)
{
    // glow pass 与普通 pass 共享资源选择规则，但消费独立命令列表。
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // 发光 marker 仍使用 Note atlas 采样，只有目标 framebuffer 不同。
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet             lastBound = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;

    for ( const auto& cmd : m_currentSnapshot->glowCmds ) {
        // 交互装饰已对同一索引范围去重，录制阶段保持快照顺序。
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;
        if ( tex == VK_NULL_HANDLE ) {
            tex = defaultDescriptor;
        }

        if ( tex != lastBound ) {
            // 独立 pass 从空缓存开始，不能假设普通 pass 的绑定状态仍有效。
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
            // glow framebuffer 与普通层尺寸一致，复用相同物理裁剪换算。
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

/// @brief 判断当前 Timeline 快照是否包含发光绘制命令。
/// @return 当前快照存在发光命令时返回 true。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取命令数量。
bool TimelineCanvas::hasGlowDrawCmds() const
{
    // 调度器据此跳过没有发光内容的额外 render pass。
    return m_currentSnapshot && !m_currentSnapshot->glowCmds.empty();
}

}  // namespace MMM::Canvas
