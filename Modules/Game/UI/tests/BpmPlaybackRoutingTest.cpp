#include "ui/imgui/menu/actions/tools/BpmPlaybackRouting.h"
#include "ui/imgui/WindowIdUtils.h"
#include "ui/imgui/audio/SpectrumTimeMapping.h"
#include "ui/imgui/menu/actions/tools/BpmAutomaticMeasurementPolicy.h"

#include <cmath>
#include <initializer_list>
#include <string_view>

/// @file BpmPlaybackRoutingTest.cpp
/// @brief BPM 测量工具的音轨播放、空格所有权和稳定窗口 ID 路由测试。
/// @details 场景覆盖与编辑器同轨同步、异轨试听、工具输入拦截、双重 `###`
/// 标题解析、自动测量启动门槛和频谱半窗映射，所有路径均为纯策略调用。

namespace
{
using MMM::UI::BpmPlaybackRoute;
using MMM::UI::BpmSpaceShortcutDisposition;
using MMM::UI::isBpmMeasurementToolStableWindowId;
using MMM::UI::resolveBpmPlaybackRoute;
using MMM::UI::resolveBpmSpaceShortcutDisposition;
using MMM::UI::shouldDirectlyControlBpmAudioTransport;
using MMM::UI::shouldDispatchBpmPlaybackToEditor;
using MMM::UI::shouldStartBpmAutomaticMeasurement;
using MMM::UI::shouldToggleBpmPlaybackFromSpace;
using MMM::UI::WindowIdUtils::stableWindowId;

/// @brief 检查播放路由是否符合预期。
/// @param selectedKey BPM 工具选中音轨键。
/// @param activeKey 活动谱面主音轨键。
/// @param expected 期望路由。
/// @return 路由符合预期时返回 true。
/// @note 相同非空键映射同步路由，不同键映射试听，空选择映射不可用。
bool checkRoute(std::string_view selectedKey, std::string_view activeKey,
                BpmPlaybackRoute expected)
{
    // 键值只表示逻辑音轨身份，不要求对应文件实际存在。
    return resolveBpmPlaybackRoute(selectedKey, activeKey) == expected;
}

/// @brief 验证 BPM 节拍器使用与播放头一致的视觉/音频时间转换。
/// @return 固定偏移的符号、慢速播放和临近拍点索引都正确时为真。
/// @note 使用独立预期时间，不以频谱半窗补偿代替声音校准。
bool checkMetronomeTiming()
{
    using MMM::UI::bpmMetronomeAudioTime;
    using MMM::UI::bpmMetronomeNextBeatIndex;
    bool ok = true;
    // 独立首拍来自音频分析；任何视觉校准值都不能移动其声音落点。
    // 同步编辑器路由仍保留画布到音频的转换，以免改变既有播放契约。
    for ( double visualOffset : { -.035, 0.0, .15 } ) {
        const double independentOffset = MMM::UI::bpmMetronomeCoordinateOffset(
            MMM::UI::BpmPlaybackRoute::Audition, visualOffset);
        ok &= independentOffset == 0.0;
        ok &= MMM::UI::bpmMetronomeCoordinateOffset(
                  MMM::UI::BpmPlaybackRoute::SynchronizedWithEditor,
                  visualOffset) == visualOffset;
        for ( double rate : { .25, .5, .75, 1.0 } ) {
            // 实际首拍仍在一秒源位置，只按播放速度换算墙钟时刻。
            ok &= bpmMetronomeAudioTime(1.0, independentOffset) / rate ==
                  1.0 / rate;
        }
        // 不受显示偏移影响的调度游标必须保留尚未到达的一秒拍点。
        ok &= bpmMetronomeNextBeatIndex(.99, 1, .5, 0, independentOffset) == 0;
        ok &= bpmMetronomeNextBeatIndex(1.01, 1, .5, 0, independentOffset) == 1;
    }
    // 编辑器同步路由下，-35 ms 画布偏移需反向转换为 1.035 秒。
    // 反号会变成 0.965 秒，漏掉偏移则仍在一秒响，两者都不能通过。
    ok &= std::abs(bpmMetronomeAudioTime(1, -.035) - 1.035) < 1e-12;
    // 播放到 1.025 秒时该拍仍在前方，不能因错误索引跳过这次声音。
    // 已跨过 1.035 秒且不允许补响时，才进入下一拍。
    ok &= bpmMetronomeNextBeatIndex(1.025, 1, .5, 0, -.035) == 0;
    ok &= bpmMetronomeNextBeatIndex(1.04, 1, .5, 0, -.035) == 1;
    // 短补响窗口允许刚错过的拍立即响，窗口外不集中追赶历史拍点。
    ok &= bpmMetronomeNextBeatIndex(1.04, 1, .5, .01, -.035) == 0;
    // 正偏移的行为反向，不能把补偿方向写死为推迟声音。
    ok &= std::abs(bpmMetronomeAudioTime(1, .035) - .965) < 1e-12;
    ok &= bpmMetronomeNextBeatIndex(.97, 1, .5, 0, .035) == 1;
    ok &= bpmMetronomeAudioTime(1, 0) == 1;
    // 偏移按音轨秒应用，慢速下的墙钟差会自然扩大，不能再乘一次倍率。
    // 0.25x 下 35 ms 音轨差对应 140 ms 听感差，而非 8.75 ms。
    for ( double rate : { .25, 1.0, 2.0 } ) {
        const double beatWallTime = bpmMetronomeAudioTime(1, -.035) / rate;
        ok &= std::abs(beatWallTime - 1 / rate - .035 / rate) < 1e-12;
    }
    // 段首与预约时刻使用相同变换，变速边界不能提前切到下一段。
    ok &= bpmMetronomeAudioTime(2, -.035) > 2.025;
    // 零点前拍线保留有符号时间，硬件范围裁剪由实际调度循环负责。
    ok &= bpmMetronomeAudioTime(-.1, -.035) < 0;
    return ok;
}
}  // namespace

/// @brief 覆盖同轨同步、异轨隔离、聚焦空格、无活动谱面和无选择场景。
/// @return 所有断言通过时返回 0。
/// @details 运行时布尔断言与 constexpr 策略断言共同验证公开辅助函数契约。
int main()
{
    // ok 累积所有运行时条件，使后续策略在前项失败后仍得到执行。
    bool ok = true;
    ok &= checkMetronomeTiming();
    // 48 kHz 的 2048 帧窗口从 1 秒开始，其能量中心位于 1.021333… 秒。
    // 该中心在图表中必须仍对应缓存第 1 秒，防止 BPM 图比音频工具提前半窗。
    // 这里只允许浮点运算舍入误差，不能使用毫秒级容限掩盖漏掉的补偿。
    constexpr double HALF_WINDOW_48K = 1024.0 / 48000.0;
    ok &= std::abs(MMM::UI::spectrumWindowStartAtVisualTime(
                       1.0 + HALF_WINDOW_48K, 0.0, 48000.0) -
                   1.0) < 1e-12;
    // 正负专用偏移只改变内容显示位置，回查同一窗口时都应得到相同起点。
    // 用户用额外偏移校准过音频工具时，BPM 工具必须保持同样的偏移符号。
    for ( double offset : { -0.035, 0.0, 0.15 } ) {
        ok &= std::abs(MMM::UI::spectrumWindowStartAtVisualTime(
                           1.0 + HALF_WINDOW_48K + offset, offset, 48000.0) -
                       1.0) < 1e-12;
    }
    // 不同内部采样率使用自己的半窗时长，不能固定为某个毫秒补偿值。
    // 文件若经重采样，读取窗宽度依据内部格式，不能依据导入文件元数据。
    ok &= std::abs(MMM::UI::spectrumWindowStartAtVisualTime(
                       2.0 + 1024.0 / 44100.0, 0.0, 44100.0) -
                   2.0) < 1e-12;
    // 音频开始前仍保留负坐标供裁剪，不把频谱硬移到零秒。
    ok &= MMM::UI::spectrumWindowStartAtVisualTime(0.0, 0.0, 48000.0) < 0.0;
    // 未准备好采样格式时仍保留专用偏移，但不产生无限或未定义时间。
    ok &= MMM::UI::spectrumWindowStartAtVisualTime(1.0, 0.2, 0.0) == 0.8;
    // 工具与活动谱面选择同一音轨时复用编辑器 transport。
    ok &= checkRoute("/project/audio/main.ogg",
                     "/project/audio/main.ogg",
                     BpmPlaybackRoute::SynchronizedWithEditor);
    // 选择另一音轨时进入独立试听，不能改变编辑器播放状态。
    ok &= checkRoute("/project/audio/preview.ogg",
                     "/project/audio/main.ogg",
                     BpmPlaybackRoute::Audition);
    // 没有活动谱面仍可试听明确选择的音轨。
    ok &= checkRoute(
        "/project/audio/preview.ogg", {}, BpmPlaybackRoute::Audition);
    // 没有任何选中音轨时播放入口不可用。
    ok &= checkRoute(
        {}, "/project/audio/main.ogg", BpmPlaybackRoute::Unavailable);
    // 同步路由派发到编辑器，且不能由工具直接控制底层 transport。
    ok &= shouldDispatchBpmPlaybackToEditor(
        BpmPlaybackRoute::SynchronizedWithEditor);
    static_assert(!shouldDirectlyControlBpmAudioTransport(
        BpmPlaybackRoute::SynchronizedWithEditor));
    // 试听路由恰好相反，由 BPM 工具直接控制音频。
    static_assert(
        shouldDirectlyControlBpmAudioTransport(BpmPlaybackRoute::Audition));
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Audition);
    // 不可用路由不会误派发给编辑器。
    ok &= !shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute::Unavailable);
    // 普通聚焦状态允许空格，文本输入或弹窗状态会拦截。
    ok &= shouldToggleBpmPlaybackFromSpace(false, false);
    ok &= !shouldToggleBpmPlaybackFromSpace(true, false);
    ok &= !shouldToggleBpmPlaybackFromSpace(false, true);
    // 自动测量只允许在没有正在运行的任务时启动。
    ok &= shouldStartBpmAutomaticMeasurement(false);
    ok &= !shouldStartBpmAutomaticMeasurement(true);

    // 真实窗口标题可能经过多层追加，解析必须使用最后一个 ### 标记。
    constexpr std::string_view DOUBLE_MARKER_WINDOW =
        "BPM测量工具###BpmMeasurementToolWindow###BpmMeasurementTool";
    // 子窗口稳定 ID 保留父窗口前缀，仍应归属于 BPM 工具。
    constexpr std::string_view DOUBLE_MARKER_CHILD =
        "BPM测量工具###BpmMeasurementToolWindow###BpmMeasurementTool/"
        "##BpmMeasureControlsChild_A1B2C3D4";
    // 先单独验证通用窗口 ID 解析，再交给 BPM 所有权判定。
    ok &= stableWindowId(DOUBLE_MARKER_WINDOW) == "BpmMeasurementTool";
    ok &=
        isBpmMeasurementToolStableWindowId(stableWindowId(DOUBLE_MARKER_CHILD));
    // 精确父 ID 和其子窗口有效，相似前缀不能误判为工具窗口。
    ok &= isBpmMeasurementToolStableWindowId("BpmMeasurementTool");
    ok &= isBpmMeasurementToolStableWindowId(
        "BpmMeasurementTool/##BpmMeasureControlsChild_A1B2C3D4");
    ok &= !isBpmMeasurementToolStableWindowId("Canvas_0");
    ok &= !isBpmMeasurementToolStableWindowId("BpmMeasurementToolUnexpected");
    // 工具聚焦且无输入冲突时，空格由工具切换自身播放。
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, true) ==
          BpmSpaceShortcutDisposition::ToggleTool;
    // 文本输入、弹窗或不可播放状态仍消费按键，但不切换 transport。
    ok &= resolveBpmSpaceShortcutDisposition(true, true, false, true) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, true, true) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, false) ==
          BpmSpaceShortcutDisposition::ConsumeOnly;
    // 工具未聚焦时完全放弃空格所有权，由上层编辑器继续路由。
    ok &= resolveBpmSpaceShortcutDisposition(false, false, false, true) ==
          BpmSpaceShortcutDisposition::NotOwned;
    // 异轨试听即使由空格启动，也绝不能派发到活动谱面的编辑器。
    const BpmPlaybackRoute focusedDifferentTrackRoute = resolveBpmPlaybackRoute(
        "/project/audio/preview.ogg", "/project/audio/main.ogg");
    // 组合断言保护焦点策略与音轨策略之间的边界。
    ok &= focusedDifferentTrackRoute == BpmPlaybackRoute::Audition;
    ok &= resolveBpmSpaceShortcutDisposition(true, false, false, true) ==
          BpmSpaceShortcutDisposition::ToggleTool;
    ok &= !shouldDispatchBpmPlaybackToEditor(focusedDifferentTrackRoute);
    // 单一退出码交给 CTest；失败项可通过调试器逐段定位。
    return ok ? 0 : 1;
}
