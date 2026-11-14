#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileDockLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <ice/config/config.hpp>
#include <initializer_list>
#include <nlohmann/json.hpp>

namespace MMM::UI
{
/// @brief 通过友元验证真实工具历史和磁吸入口，不复制生产判定逻辑。
struct BpmMeasurementHistoryTestAccess {
    /// @brief 只比较测试浮点结果，容差不影响生产网格定位。
    static bool close(double lhs, double rhs)
    {
        return std::abs(lhs - rhs) < 1e-9;
    }

    /// @brief 验证分析完成但试听尚未加载时，波形和频谱仍提交播放指针几何。
    /// @return 两个区域都有指针，离开视野或未选音轨时不绘制无效指针。
    /// @details 使用真实 ImGui 顶点缓冲，不复制播放指针的显隐判断。
    /// 不初始化音频后端，确保显示准备位置不依赖先按一次播放。
    /// 断言顶部三角的内部顶点，不能把已有拍线误认成恢复了播放手柄。
    /// 未播放的准备位置与视图中心一致，符合首次播放从当前视野开始的契约。
    /// 离屏场景只改变视野范围，不修改音轨身份来人为触发重新显示。
    /// 测试不执行首次拖动加载；该低频路径仍由生产加载失败状态处理。
    static bool checkPlaybackCursor(BpmMeasurementToolView& tool)
    {
        // 每个图形用例建立独立上下文，禁用 INI 后不会污染个人布局。
        // 字体图集只在本地准备，调用拍点高亮仍可检查合法绘制生命周期。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 640, 480 };
        io.DeltaTime   = 1.0F / 60.0F;
        // 无平台后端也要建立字体图集，满足真实 UI 帧的初始化契约。
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        // 先发布鼠标位置并完成准备帧，避免事件队列把首次移动与滚轮分批消费。
        // 后续每个滚轮刻度都在已经悬停的同一区域内独立提交。
        io.AddMousePosEvent(300, 120);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 640, 480 });
        ImGui::Begin("BpmCursorRegression");
        auto& list                  = *ImGui::GetWindowDrawList();
        tool.m_selectedAudioSyncKey = "unloaded-preview";
        tool.m_playbackRoute        = BpmPlaybackRoute::Audition;
        tool.m_duration             = 60;
        tool.m_viewCenter           = 0;
        bool passed                 = !tool.isSelectedTrackLoadedForPlayback();
        // 零秒指针与首拍线重合时仍需绘制顶部三角，不能把红色拍线当成成功。
        // 两种面板高度不同，但共享播放位置和时长；均调用生产绘制函数。
        for ( float bottom : { 200.0F, 400.0F } ) {
            const int before = list.VtxBuffer.Size;
            tool.drawPlaybackCursor(list, { 100, 80 }, { 500, bottom }, 0, 2);
            // 红线端点只位于上下边界，三角手柄必须产生顶部内部的实色顶点。
            // 因此只剩首拍线或播放竖线的回归不能靠“有顶点”绕过断言。
            bool triangle = false;
            for ( int vertex = before; vertex < list.VtxBuffer.Size;
                  ++vertex ) {
                const auto& point = list.VtxBuffer[vertex];
                triangle =
                    triangle || (point.col == IM_COL32(255, 40, 40, 255) &&
                                 point.pos.y > 82 && point.pos.y < 98);
            }
            passed = passed && list.VtxBuffer.Size > before && triangle;
        }
        // 记录同一个绘制列表的当前末尾，后续断言只关注生产入口新增的几何。
        // 不把窗口背景或先前区域的红色顶点重复计入指针显示结果。
        const int before = list.VtxBuffer.Size;
        // 纯粹平移到不包含准备位置的视野，仍应遵守既有离屏隐藏规则。
        tool.drawPlaybackCursor(list, { 100, 80 }, { 500, 400 }, 5, 7);
        passed = passed && list.VtxBuffer.Size == before;
        tool.m_selectedAudioSyncKey.clear();
        // 清空选择后不能遗留上一首歌的指针；绘制也不能暗中加载试听资源。
        tool.drawPlaybackCursor(list, { 100, 80 }, { 500, 400 }, 0, 2);
        passed = passed && list.VtxBuffer.Size == before &&
                 !tool.isSelectedTrackLoadedForPlayback();
        // 所有失败也先配对结束窗口和帧，避免断言失败掩盖 ImGui 生命周期问题。
        // 字体图集与顶点缓冲由本轮上下文统一销毁，不留下跨用例引用。
        ImGui::End();
        ImGui::Render();
        // 分析完成但从未按下播放时，滚轮必须以可见准备位置连续推进。
        // 使用真实鼠标滚轮事件和导航入口，不能只测纯网格算法的返回值。
        tool.m_selectedAudioSyncKey = "unloaded-preview";
        tool.m_timingSegments       = { { 0.0, 120.0 } };
        tool.m_firstBeatTime        = 0.0;
        tool.m_beatDivisor          = 4;
        tool.m_bpm                  = 120.0;
        // 两种吸附设置都应能从零秒向后滚动，并在反向时逐格回到零秒。
        // 正向越过零边界只应钳制当前位置，不能锁死随后反方向的输入。
        // 120 BPM 的四分拍间距为 0.125 秒，两个方向使用同一精确网格。
        // 第一格是边界外滚动，末尾反向输入验证边界钳制后仍能正常继续。
        // 中途连续后移两格可发现每次都重新从后端零秒计算的旧行为。
        // 该序列不按播放按钮，显示位置与准备播放位置必须始终一致。
        // 视图缩放保持不变，避免把缩放造成的内容移动误当成播放定位成功。
        constexpr std::array<float, 7>  wheels{ 1, -1, -1, 1, 1, 1, -1 };
        constexpr std::array<double, 7> positions{ 0, 0.125, 0.25, 0.125,
                                                   0, 0,     0.125 };
        for ( const bool snap : { false, true } ) {
            // 两轮使用同一工具，先清空小数余量，避免上一轮输入改变吸附步数。
            // 重置准备中心不建立音频实例，模拟首次打开分析面板的边界状态。
            tool.m_wheelSnapEnabled   = snap;
            tool.m_wheelSnapRemainder = 0;
            tool.m_viewCenter         = 0;
            for ( std::size_t step = 0; step < wheels.size(); ++step ) {
                // 每个输入独立成帧，确保后续滚动读取到上一帧的本地准备位置。
                // 不初始化音频设备，也不通过按播放来绕过首次导航场景。
                io.AddMousePosEvent(300, 120);
                io.AddMouseWheelEvent(0, wheels[step]);
                ImGui::NewFrame();
                ImGui::Begin("BpmCursorRegression");
                tool.handleTimelineNavigation(
                    { 100, 80 }, { 500, 200 }, 0, 2, true);
                // 必须检查视图中心的实际提交结果，未加载音频时后端时间始终不变。
                // 源保持未加载也证明导航没有通过隐式启动音频绕过准备位置问题。
                const bool navigated =
                    close(tool.m_viewCenter, positions[step]) &&
                    !tool.isSelectedTrackLoadedForPlayback();
                if ( !navigated ) {
                    XERROR(
                        "Unloaded BPM wheel failed: snap={}, step={}, "
                        "wheel={}, expected={}, actual={}",
                        snap,
                        step,
                        io.MouseWheel,
                        positions[step],
                        tool.m_viewCenter);
                }
                passed = passed && navigated;
                // 即使某一步失败也结束本帧，避免异常的 ImGui 栈掩盖导航回归。
                ImGui::End();
                ImGui::Render();
            }
        }
        // ImGui 生命周期在工具析构前结束，工具不得持有本轮绘制列表指针。
        ImGui::DestroyContext();
        return passed;
    }

    /// @brief 验证 BPM 编辑可撤销，导航与显示偏好既不入栈也不被还原。
    /// @return 添加、删除、参数恢复及重做分支全部正确时返回 true。
    /// @details 测量数据与显示状态在同一工具对象中，必须按用户语义区分。
    /// 先改变导航和显示，再编辑 BPM，证明撤销不是恢复整个工具快照。
    /// 恢复 BPM 后继续移动视野，证明导航不会截断已有重做分支。
    /// 添加、删除使用段落数量断言，保证列表本身参与历史恢复。
    static bool checkHistory(BpmMeasurementToolView& tool)
    {
        // 夹具在同一工具实例完成撤销重做，确保列表身份和状态不会被重建掩盖。
        // 历史先绑定音轨，恢复时不能把别的歌曲段落带入。
        tool.m_selectedAudioTrackId = "history-track";
        // 使用固定时长，不依赖真实音频后端的异步时钟。
        tool.m_duration          = 60.0;
        tool.m_timingSegments    = { { 0.2, 120.0 } };
        tool.m_bpm               = 120.0;
        tool.m_firstBeatTime     = 0.2;
        tool.m_beatLengthSeconds = 0.5;
        tool.clearMeasurementHistory();
        // 模拟导航、显示和播放参数同时变化，不能构成 BPM 编辑记录。
        tool.beginMeasurementHistoryGesture();
        tool.m_viewCenter    = 8.0;
        tool.m_zoomSeconds   = 2.0;
        tool.m_markerWidthMs = 96.0;
        tool.m_beatDivisor   = 8;
        // 倍速改变不等于修改谱面 BPM，撤销时要保留该播放偏好。
        tool.m_playbackSpeed = 0.75;
        tool.finishMeasurementHistoryGesture();
        // 导航手势结束后栈仍为空，不能只证明它排在 BPM 编辑之前。
        if ( tool.canUndoMeasurement() ) return false;
        // 连续 BPM 拖动只形成一个撤销点，不因中间值而重复追加。
        tool.beginMeasurementHistoryGesture();
        // 中间值不提交，模拟鼠标连续拖动造成多次参数变化。
        tool.m_timingSegments[0].bpm = 140.0;
        tool.m_timingSegments[0].bpm = 150.0;
        tool.m_bpm                   = 150.0;
        tool.m_beatLengthSeconds     = 0.4;
        tool.finishMeasurementHistoryGesture();
        // 只提交最终一条历史，避免每个中间值都占用撤销深度。
        if ( tool.m_measurementUndoHistory.size() != 1 ) return false;
        tool.undoMeasurementFromShortcut();
        // 撤销只恢复 BPM 数据，不把后续视野、倍速和显示偏好改回去。
        if ( !close(tool.m_bpm, 120.0) || !close(tool.m_viewCenter, 8.0) ||
             !close(tool.m_zoomSeconds, 2.0) ||
             !close(tool.m_markerWidthMs, 96.0) || tool.m_beatDivisor != 8 ||
             !close(tool.m_playbackSpeed, 0.75) )
            return false;
        tool.beginMeasurementHistoryGesture();
        // 在撤销之后进行纯导航，不能破坏可重做的测量改动。
        tool.m_viewCenter = 12.5;
        tool.finishMeasurementHistoryGesture();
        // 在别处查看不能截断重做，恢复 BPM 后仍停留当前视野。
        if ( !tool.canRedoMeasurement() ) return false;
        tool.redoMeasurementFromShortcut();
        if ( !close(tool.m_bpm, 150.0) || !close(tool.m_viewCenter, 12.5) )
            return false;
        // 新段增加和删除都应独立形成历史，保持段落完整顺序。
        tool.beginMeasurementHistoryGesture();
        // 新段数据独立加入，不用只改兼容 BPM 字段模拟增加。
        tool.m_timingSegments.push_back({ 3.0, 180.0 });
        tool.finishMeasurementHistoryGesture();
        tool.undoMeasurementFromShortcut();
        if ( tool.m_timingSegments.size() != 1 ) return false;
        tool.redoMeasurementFromShortcut();
        if ( tool.m_timingSegments.size() != 2 ) return false;
        tool.beginMeasurementHistoryGesture();
        // 删除后撤销必须恢复完整段落，而非仅还原首段参数。
        tool.m_timingSegments.pop_back();
        tool.finishMeasurementHistoryGesture();
        tool.undoMeasurementFromShortcut();
        if ( tool.m_timingSegments.size() != 2 ) return false;
        tool.clearMeasurementHistory();
        // 音轨切换后旧段落历史与未提交手势都不能再生效。
        return !tool.canUndoMeasurement() && !tool.canRedoMeasurement() &&
               !tool.m_measurementGestureBefore;
    }

    /// @brief 验证偏离网格、浮点在线位置、跨 BPM 边界与触控板小数刻度。
    /// @return 所有跳转都与当前分拍线一致时返回 true。
    /// @details 两段 BPM 不同且边界不在旧网格上，不能只靠固定步长通过。
    /// 从偏离网格的位置开始，区分相对步进和真正磁吸。
    /// 在线时必须跳到下一条，浮点误差不能使滚轮停在原位置。
    /// 小数刻度必须累积，不能每个触控板事件都跳过一条线。
    static bool checkWheel(BpmMeasurementToolView& tool)
    {
        // 首拍偏移设为二百毫秒，磁吸原点必须来自段落而不是零秒。
        // 第二段每拍一秒，与第一段半秒拍长不同，跨段结果可明确区分。
        tool.m_timingSegments   = { { 0.2, 120.0 }, { 1.0, 60.0 } };
        tool.m_firstBeatTime    = 0.2;
        tool.m_bpm              = 120.0;
        tool.m_beatDivisor      = 4;
        tool.m_wheelSnapEnabled = false;
        // 关闭时保留相对移动，不能把任意精确位置偷偷拉到分拍线上。
        if ( !close(tool.wheelSeekTarget(0.27, -1.0), 0.395) ) return false;
        tool.m_wheelSnapEnabled = true;
        // 首次从任意位置吸到方向上的下一条线，而非增加一个相对步长。
        if ( !close(tool.wheelSeekTarget(0.27, -1.0), 0.325) ||
             !close(tool.wheelSeekTarget(0.325, -1.0), 0.45) ||
             !close(tool.wheelSeekTarget(0.27, 1.0), 0.2) )
            return false;
        // 段首优先作为网格零点，不能跨过它沿用前一段 BPM。
        if ( !close(tool.wheelSeekTarget(0.95, -1.0), 1.0) ||
             !close(tool.wheelSeekTarget(1.0, -1.0), 1.25) ||
             !close(tool.wheelSeekTarget(1.0, 1.0), 0.95) ||
             !close(tool.wheelSeekTarget(0.95, -2.0), 1.25) )
            return false;
        // 音频 Seek 会截断到整数采样帧，读回值可能略早于准确拍线。
        // 若把该位置误认成尚未到线，向后滚动就会反复选择同一条线。
        const double frame = 1.0 / ice::ICEConfig::internal_format.samplerate;
        if ( !close(tool.wheelSeekTarget(0.325 - frame * 0.5, -1.0), 0.45) ||
             !close(tool.wheelSeekTarget(0.325 + frame * 0.5, 1.0), 0.2) )
            return false;
        // 段首也可能读回前半帧，两个方向均应跨入正确段落。
        // 同时验证多刻度跨段，不能在内部下一步继续卡在段首。
        if ( !close(tool.wheelSeekTarget(1.0 - frame * 0.5, -1.0), 1.25) ||
             !close(tool.wheelSeekTarget(1.0 + frame * 0.5, 1.0), 0.95) ||
             !close(tool.wheelSeekTarget(0.95 - frame * 0.5, -2.0), 1.25) )
            return false;
        // 连续二十次滚动，每次都让后端读回值丢失一部分采样精度。
        // 应稳定向前推进，而不是依赖第二次滚轮或等待播放自然越过拍线。
        double cursor = 1.25;
        for ( int tick = 0; tick < 20; ++tick ) {
            cursor = tool.wheelSeekTarget(cursor - frame * 0.5, -1.0);
            if ( !close(cursor, 1.25 + (tick + 1) * 0.25) ) return false;
        }
        // 明显偏离拍线时仍吸到最近的下一条，不能用粗糙取整吞掉一格。
        if ( !close(tool.wheelSeekTarget(0.325 - 0.001, -1.0), 0.325) )
            return false;
        // 小数输入从空余量开始，避免前一场景恰好贡献一个刻度。
        tool.m_wheelSnapRemainder = 0.0;
        // 两个小数刻度合成一格，既不每帧跳线也不丢失触控板输入。
        return close(tool.wheelSeekTarget(0.27, -0.4), 0.27) &&
               close(tool.wheelSeekTarget(0.27, -0.6), 0.325);
    }

    /// @brief 实际点击章节删除按钮，覆盖多行、单行到空列表的裁剪器生命周期。
    /// @return 每次删除只移除目标章节且下一帧仍能绘制时返回 true。
    /// @details 不直接擦除容器，鼠标事件必须经过生产面板和可见按钮。
    /// 多行状态覆盖删除后引用移动，单行状态覆盖数量归零后的收尾。
    /// 测试在独立 ImGui 上下文运行，不依赖 GPU、音频设备或外部资源。
    /// 面板仍使用真实本地化标签，展开状态与生产窗口的 ID 计算一致。
    /// 每次释放后立即核对结果，禁止用多次点击掩盖首次删除失败。
    static bool checkChapterDeletion(BpmMeasurementToolView& tool)
    {
        ImGui::CreateContext();
        auto& io              = ImGui::GetIO();
        io.IniFilename        = nullptr;
        io.DisplaySize        = { 800, 700 };
        io.DeltaTime          = 1.0F / 60.0F;
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        // 无平台后端时手动建立字体图集，否则控件尺寸和输入布局不可用。
        // 此缓冲由 ImGui 上下文持有，不自行释放或延长其生命周期。
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        // 时间范围独立于试听加载状态，章节删除不应要求先播放音频。
        // 三个章节使用不同时间与名称，使移动下标后的错误目标可被识别。
        tool.m_duration      = 60;
        tool.m_audioChapters = { { 1, "first" },
                                 { 2, "second" },
                                 { 3, "third" } };
        // 固定宿主尺寸保证三行均可见，位置从实际子窗口获取而非固定屏幕坐标。
        const auto draw = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ 800, 700 });
            ImGui::Begin("BpmChapterRegression");
            // 只预设章节树展开状态，不伪造按钮返回值或裁剪器内部状态。
            // 每一帧仍完整配对 Begin/End，让窗口和子窗口正常布局。
            ImGui::GetStateStorage()->SetInt(
                ImGui::GetID(
                    TR("ui.tools.bpm_measure.markers_chapters").data()),
                1);
            tool.renderMarkerPanel();
            ImGui::End();
            ImGui::Render();
        };
        draw();
        bool passed = true;
        // 删除首行会移动后续下标，最后一行删除则覆盖裁剪器只剩一个条目的情况。
        // 每次都验证保留行顺序，不能仅以“不崩溃”掩盖删错章节。
        for ( int remaining = 3; remaining > 0; --remaining ) {
            // 子窗口名称带宿主前缀，只匹配固定内部 ID，避免依赖显示语言。
            // 借用指针仅在当前上下文存活期间使用，不跨销毁保存。
            ImGuiWindow* child = nullptr;
            for ( auto* window : ImGui::GetCurrentContext()->Windows ) {
                if ( window->Name && std::string_view(window->Name)
                                             .find("##AudioChapterList") !=
                                         std::string_view::npos )
                    child = window;
            }
            if ( !child ) {
                passed = false;
                break;
            }
            const auto&  style = ImGui::GetStyle();
            const ImVec2 pointer{
                child->WorkRect.Max.x - 50 + style.ItemSpacing.x +
                    style.FramePadding.x + ImGui::CalcTextSize("X").x * 0.5F,
                child->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5F
            };
            // 先移动预热，再独立按下和松开，避免输入队列拆分导致未命中。
            io.AddMousePosEvent(pointer.x, pointer.y);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            draw();
            passed &= tool.m_audioChapters.size() ==
                      static_cast<std::size_t>(remaining - 1);
            // 数量不符时先停止点击，避免后续输入改变失败现场或越过空列表。
            if ( !passed ) break;
            // 首行连续删除应依次留下第二、第三章，时间字段作为稳定身份。
            if ( remaining > 1 )
                passed &= tool.m_audioChapters.front().seconds == 5 - remaining;
            draw();
        }
        // 空列表再绘制一帧，确保后续 Step 和窗口栈仍有效。
        // 空列表没有任何行控件，不能沿用上一轮的删除按钮 ID。
        // 删除后的显示刷新只允许跨正常帧推进，不通过睡眠等待清理。
        // 本测试刻意不加载文件，排除音频解码线程导致的无关失败。
        // 崩溃发生在 UI 控制流，因此真实输入与裁剪循环是本例验证边界。
        // Windows 转储定位仍保留独立日志，不把本机通过视作 Windows 二进制复测。
        draw();
        // 失败也走相同销毁路径，不把本用例的活动 ID 带入后续工具测试。
        // 这里只改工具草稿，未提交导出任务，也不会写入真实音频元数据。
        ImGui::DestroyContext();
        if ( !passed ) XERROR("BPM chapter delete regression failed");
        return passed;
    }

    /// @brief 回归真实触点的图表归属、平移、双指缩放和中断，不复制生产判定。
    /// @return 所有输入边界均符合导航契约时返回 true。
    /// @details 使用独立的原始跟踪器，防止该用例改变后续 UI 测试的全局输入。
    /// 中心取音轨中部，基础平移断言不混入首尾裁切影响。
    /// 双指测试随后在同一对象继续，验证单指切换到双指时不会重置视野。
    /// 所有步骤只推进帧号，不依赖真实时钟或睡眠。
    /// 断言观察生产输出，不另写一个手势识别器计算期望事件。
    /// @details 导航不初始化音频设备；只比较公开的图表输出与可观测边界。
    static bool checkMobileNavigation()
    {
        // 一千像素逻辑宽下，图表占八百像素；八秒视野对应每秒一百像素。
        // 这个整数比例使基础平移可精确比较，同时保留图表外侧的空白区。
        // 后续输入均经过跟踪器归一化状态，不直接修改快照的峰值或取消位。
        Graphic::MobileTouchTracker tracker;
        BpmPlotTouch                plot;
        BpmPlotTouchFrame           f{ .m_min        = { 100, 100 },
                                       .m_max        = { 900, 400 },
                                       .m_display    = { 1000, 600 },
                                       .m_center     = 30,
                                       .m_zoom       = 4,
                                       .m_duration   = 100,
                                       .m_owner      = 1,
                                       .m_frame      = 1,
                                       .m_generation = 1,
                                       .m_hovered    = true };
        /// @brief 每个输入推进一帧，导航结果立即作为下一帧的实际视野。
        const auto step =
            [&](Graphic::TouchPhase phase, int finger, float x, float y) {
                tracker.update(phase, 1, finger, x, y, f.m_frame * .016);
                const auto out = plot.update(tracker.snapshot(), f);
                if ( out.m_navigation ) {
                    f.m_center = out.m_center;
                    f.m_zoom   = out.m_zoom;
                }
                ++f.m_frame;
                return out;
            };
        // 一次 Down 后的每个 Move 都立即采纳，不等待后台配置保存。
        // 用真实阶段枚举保持与 SDL 发布入口相同的顺序和取消语义。
        using enum Graphic::TouchPhase;
        bool passed = step(Down, 1, .5, .3).m_pressed;
        // 横拖 100 像素应立即平移一秒；导航不能触发首拍或播放轻点。
        const auto pan = step(Move, 1, .6, .3);
        passed &= pan.m_navigation && close(f.m_center, 29) && !pan.m_tapped;
        // 第二指加入时建立配对，不重新使用第一指较早的 Down 作为缩放锚点。
        step(Down, 2, .8, .3);
        // 扩大两指间距应缩小半窗；容差只吸收归一化 float 坐标误差。
        // 图表中心也必须同步更新，不能只改变 zoom 后等待下一次拖动修正。
        const auto pinch = step(Move, 2, .9, .3);
        passed &= pinch.m_navigation && std::abs(f.m_zoom - 8. / 3) < 1e-5;
        // 双指结束后余指不能退化为单指轻点，第三指也必须整体封锁。
        passed &= step(Up, 2, .9, .3).m_finished;
        passed &= !step(Up, 1, .6, .3).m_tapped;
        // 第三指的加入应产生一次结束，其后任何 Up 都不得成为一次轻点。
        // 保留三次独立释放，覆盖数组压缩后首指仍存在的阶段。
        step(Down, 1, .5, .3);
        step(Down, 2, .7, .3);
        passed &= step(Down, 3, .8, .3).m_finished;
        step(Up, 3, .8, .3);
        step(Up, 2, .7, .3);
        passed &= !step(Up, 1, .5, .3).m_tapped;
        // 正常静止释放才产生轻点；Cancel 后的旧 Up 必须没有任何输出。
        step(Down, 1, .5, .3);
        passed &= step(Up, 1, .5, .3).m_tapped;
        step(Down, 1, .5, .3);
        tracker.cancel();
        // Cancel 使用跟踪器的生命周期入口，不通过伪造一个正常 Up 测取消。
        // 取消后坐标变化只用于证明旧 Up 被门闩拒绝。
        const auto cancelled = plot.update(tracker.snapshot(), f);
        ++f.m_frame;
        passed &= cancelled.m_finished && !cancelled.m_tapped &&
                  !cancelled.m_navigation;
        passed &= !step(Up, 1, .8, .3).m_tapped;
        // 外部起点不因后续移入而认领，也不能被另一图表延迟接管。
        step(Down, 1, .01, .01);
        passed &= !step(Move, 1, .5, .3).m_navigation;
        step(Up, 1, .5, .3);
        // 几何重排和隐藏缺帧都中止旧投影，防止恢复窗口时突然跳动。
        step(Down, 1, .5, .3);
        f.m_min.x += 10;
        passed &= step(Move, 1, .6, .3).m_finished;
        step(Up, 1, .6, .3);
        f.m_min.x -= 10;
        step(Down, 1, .5, .3);
        f.m_frame += 3;
        passed &= step(Move, 1, .6, .3).m_finished;
        step(Up, 1, .6, .3);
        // 零点附近实际只显示半个名义时间窗，捏合必须改变可见跨度本身。
        // 仅断言 zoom 数字会漏掉图像完全不缩放的回归，因此同时检查投影秒域。
        // 初始八秒可见窗，指距从 400 到 500 像素后应变成 6.4 秒。
        // 锚点取第二指加入时的中点，移动后仍应投影到相同的五秒位置。
        f.m_center = 0;
        f.m_zoom   = 8;
        step(Down, 1, .4, .3);
        step(Down, 2, .8, .3);
        const auto   edge  = step(Move, 2, .9, .3);
        const double left  = std::max(0., f.m_center - f.m_zoom);
        const double right = std::min(f.m_duration, f.m_center + f.m_zoom);
        passed &= edge.m_navigation && std::abs(right - left - 6.4) < 1e-4;
        passed &= std::abs(left + .6875 * (right - left) - 5) < 1e-4;
        // 配对释放不再次定位，也不把边界处的合法缩放写进测量历史。
        // 下一轮仍需要新的完整 Down，禁止依靠旧余指继续认领。
        step(Up, 2, .9, .3);
        step(Up, 1, .4, .3);
        if ( !passed ) XERROR("BPM mobile navigation regression failed");
        return passed;
    }

    /// @brief 以真实原始触点驱动既有拍点算法，验证宽命中不跳变和单笔历史。
    /// @return 原业务算法、输入隔离和历史边界全部通过时返回 true。
    /// @details 本测试只修改测量草稿，不向正式谱面写入 Timing。
    /// 音轨同步键仅表达分析已就绪，试听仍明确保持未加载状态。
    /// 不使用鼠标坐标模拟手指，所有移动经生产原始触点发布入口。
    /// ImGui 上下文提供真实帧号和绘制列表，避免手势连续性检查被绕过。
    /// 操作方案与全局触点在退出前恢复，测试不改变用户保存偏好。
    /// @details 拖动非首拍反算 BPM，撤销／重做必须同时恢复全部兼容字段。
    static bool checkMobileBeats(BpmMeasurementToolView& tool)
    {
        struct MobileShell : IWorkspacePresentation {
            /// @brief 使用真实移动分流，不以宿主 Linux 平台宏代替设备策略。
            bool allowsHoverFeedback() const override { return false; }
            /// @brief 本用例只绘制单独图表，不需要完整工作区和音频设备。
            void renderWorkspace(MainDockSpaceUI&, UIManager&) override {}
            /// @brief 使用生产中央页签分配，完整工具 update
            /// 不得用默认浮窗位置拆散节点。
            bool beginWindow(std::string_view, const std::string& title,
                             bool* open, ImGuiWindowFlags flags) override
            {
                m_layout.target(MobileDockPanel::Main, title.c_str());
                return ImGui::Begin(title.c_str(), open, flags);
            }
            /// @brief 独立测试停靠树，不读取或覆盖用户布局。
            MobileDockLayout m_layout;
        } shell;
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 600 };
        io.DeltaTime   = 1.f / 60;
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        const auto oldProfile = std::string(Interaction::activeProfile().id());
        Interaction::synchronizeProfile("touch-gestures", true);
        Interaction::synchronizeProfile("touch-gestures", true);
        // 前面已有历史测试，这里显式清除草稿事务而不重置配置单例。
        // 一段 120 BPM 可直接验证第一个非首拍移动后的反算结果。
        // 首拍保持零秒，使拖动其它拍时不能靠移动整段掩盖算法错误。
        tool.clearMeasurementHistory();
        tool.m_duration             = 20;
        tool.m_selectedAudioSyncKey = "mobile-unloaded-preview";
        tool.m_playbackRoute        = BpmPlaybackRoute::Audition;
        tool.m_timingSegments       = { { 0, 120 } };
        tool.m_bpm                  = 120;
        tool.m_firstBeatTime        = 0;
        tool.m_beatLengthSeconds    = .5;
        tool.m_viewCenter           = 2;
        tool.m_zoomSeconds          = 2;
        tool.m_mobileEdit           = true;
        /// @brief 每次输入后走实际图表入口，窗口和帧均按真实顺序配对。
        const auto frame = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ 1000, 600 });
            ImGui::Begin("BpmMobileRegression");
            WorkspaceFrameScope scope(shell);
            tool.handleMobilePlot({ 100, 100 }, { 900, 400 }, 0, 4, true, 1);
            ImGui::End();
            ImGui::Render();
        };
        using enum Graphic::TouchPhase;
        /// @brief 归一化位置只用于注入；输出断言直接读取工具的实际 Timing。
        const auto event = [&](Graphic::TouchPhase phase, float x) {
            Graphic::publishMobileTouch(
                phase, 50, 1, x, .3, ImGui::GetFrameCount() * .016);
            frame();
        };
        // 原始命中查询使用上一帧可见窗口树，先按正常生命周期提交两帧。
        // 合成鼠标始终不移入图表，专门覆盖真机快速触摸先于鼠标更新的路径。
        // 如果用旧鼠标或 ImPlot hover 作为唯一许可，后续触摸断言将失败。
        frame();
        frame();
        // 第一拍在 x200；指尖偏离 12 像素仍命中，按下不能改成 107 BPM。
        event(Down, .212f);
        bool passed = close(tool.m_bpm, 120) && !tool.canUndoMeasurement();
        // 只有越过阈值后才真正修改 BPM；按下时记录的抓取偏差必须被扣除。
        // 拖到 x232 表示原拍从 x200 到 x220，而不是把拍直接移到指尖。
        event(Move, .232f);
        passed &= std::abs(tool.m_bpm - 100) < 1e-3;
        event(Up, .232f);
        passed &= tool.canUndoMeasurement() &&
                  tool.m_measurementUndoHistory.size() == 1;
        // 一次撤销应回到整笔拖动前，不能需要逐帧撤销才能恢复 120。
        // 再重做回到最后的合法 BPM，证明结束边沿没有丢失最后一帧。
        tool.undoMeasurementFromShortcut();
        passed &= close(tool.m_bpm, 120) && tool.canRedoMeasurement();
        tool.redoMeasurementFromShortcut();
        passed &= std::abs(tool.m_bpm - 100) < 1e-3;
        // 静止轻点不制造空历史，系统 Cancel 也不能把取消坐标写到首拍。
        const auto count = tool.m_measurementUndoHistory.size();
        event(Down, .22f);
        event(Up, .22f);
        passed &= tool.m_measurementUndoHistory.size() == count;
        event(Down, .22f);
        Graphic::cancelMobileTouch();
        frame();
        passed &= tool.m_measurementUndoHistory.size() == count &&
                  !tool.m_isBeatMarkerDragging;
        // 不保留本用例的捕获和取消代际，后续桌面测试可从新 Down 开始。
        // 恢复方案需要两次安全帧，与生产同步入口的切换契约一致。
        // 最后销毁上下文，保证临时绘制列表不会跨测试被借用。
        tool.suspendMobilePlot();
        Graphic::cancelMobileTouch();
        // 完整 update
        // 必须真正进入移动中央节点；仅测分类无法发现显式位置导致脱停靠。
        // 参数页在无项目下绘制占位，不初始化 ImPlot 或音频分析后台任务。
        // 生产默认位置若仍在移动端设置，窗口 DockId 会被 ImGui
        // 清零，本断言直接失败。
        // 分页仅用于本用例避免解码任务，仍经过生产 LayoutContext
        // 和外壳停靠入口。
        // 断言同时覆盖首次出现和后续帧，不能靠下一次刷新重新停靠掩盖首帧脱落。
        // 将旧桌面定位临时恢复时本回归已确认失败，证明检查能捕获设备上的遮挡原因。
        // 正式实现恢复移动定位后再次通过，不弱化窗口的 DockId 比较。
        tool.m_mobileParameters = true;
        tool.m_selectedAudioTrackId.clear();
        tool.m_selectedAudioSyncKey.clear();
        tool.m_selectedAudioIdentityNeedsAnalysis = false;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        for ( int index = 0; index < 3; ++index ) {
            ImGui::NewFrame();
            WorkspaceFrameScope scope(shell);
            shell.m_layout.render({ 0, 0 }, { 1000, 600 }, 1, false, false);
            tool.update(nullptr);
            const auto* window =
                ImGui::FindWindowByName("BPM###BpmMeasurementTool");
            passed &= window && window->DockId ==
                                    shell.m_layout.node(MobileDockPanel::Main);
            ImGui::Render();
        }
        tool.m_mobileParameters = false;
        Interaction::synchronizeProfile(oldProfile, true);
        Interaction::synchronizeProfile(oldProfile, true);
        ImGui::DestroyContext();
        if ( !passed )
            XERROR("BPM mobile beat/history regression failed: {}", tool.m_bpm);
        return passed;
    }

    /// @brief 阻止测试析构把临时显示偏好保存到个人配置。
    /// @details 工具测试通过真实绘制入口会设置偏好脏标记。
    /// 单纯恢复 settings 数值不能清掉对象内部的延迟保存计时器。
    /// 因此退出前同时恢复单例与对象门闩，保持测试配置隔离。
    /// 本函数不改变生产保存策略，也不等待真实消抖时间经过。
    /// 测试失败同样走这里，避免失败现场触发额外个人配置写入。
    /// 不通过销毁音频设备来绕过 UI 状态检查。
    /// 后续用例仍可读取构造之前的 BPM 偏好。
    /// 保持资源和历史清理与断言结果独立，退出码最后统一决定。
    /// 此收尾只影响本地工具对象，不清除项目或全局快捷键配置。
    static void suppressPreferenceSave(BpmMeasurementToolView& tool)
    {
        // 清除延迟落盘计时器，不以真实等待方式让测试偏好保存。
        tool.m_userPreferencesDirty            = false;
        tool.m_userPreferencesSaveDelaySeconds = 0.0;
    }
};
}  // namespace MMM::UI

/// @brief 验证 BPM 专属历史、磁吸跨段导航与新开关持久化兼容。
/// @return 所有路径成立时返回零；仅使用测试 ImGui 上下文，不创建音频设备。
int main()
{
    // JSON 与工具行为分别验证，避免偏好默认值恰好掩盖序列化缺失。
    // 本场景不启动音频图，历史恢复不应需要外部播放状态才能完成。
    auto& settings = MMM::Config::AppConfig::instance().getEditorSettings();
    // 保存构造前偏好，测试结束恢复相同配置身份。
    const auto original = settings.bpmMeasurementToolPreferences;
    MMM::UI::BpmMeasurementToolView tool("BpmMeasurementHistoryTest");
    MMM::Config::BpmMeasurementToolPreferences preference;
    // 启用值必须真实参与 JSON 编码与恢复，而非依靠默认构造。
    preference.wheelSnapEnabled = true;
    // 新开关跨启动保留，而旧配置没有该字段时仍默认关闭。
    const bool persisted =
        nlohmann::json(preference)
            .get<MMM::Config::BpmMeasurementToolPreferences>()
            .wheelSnapEnabled;
    // 缺失字段验证旧版本兼容，用户升级后不会被强制启用磁吸。
    const bool legacy = nlohmann::json::object()
                            .get<MMM::Config::BpmMeasurementToolPreferences>()
                            .wheelSnapEnabled;
    // 各职责顺序执行，失败时不继续制造其他历史状态。
    const bool passed =
        persisted && !legacy &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkHistory(tool) &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkWheel(tool) &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkPlaybackCursor(tool) &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkChapterDeletion(tool) &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkMobileNavigation() &&
        MMM::UI::BpmMeasurementHistoryTestAccess::checkMobileBeats(tool);
    // 先恢复偏好再抑制析构落盘，保持测试无用户设置副作用。
    settings.bpmMeasurementToolPreferences = original;
    MMM::UI::BpmMeasurementHistoryTestAccess::suppressPreferenceSave(tool);
    // 所有路径统一经过偏好清理，再用退出码报告结果。
    return passed ? 0 : 1;
}
