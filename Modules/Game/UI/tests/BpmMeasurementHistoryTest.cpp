#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include <imgui.h>

#include <cmath>
#include <ice/config/config.hpp>
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
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 640, 480 };
        io.DeltaTime   = 1.0F / 60.0F;
        // 无平台后端也要建立字体图集，满足真实 UI 帧的初始化契约。
        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
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

    /// @brief 阻止测试析构把临时显示偏好保存到个人配置。
    static void suppressPreferenceSave(BpmMeasurementToolView& tool)
    {
        // 清除延迟落盘计时器，不以真实等待方式让测试偏好保存。
        tool.m_userPreferencesDirty            = false;
        tool.m_userPreferencesSaveDelaySeconds = 0.0;
    }
};
}  // namespace MMM::UI

/// @brief 验证 BPM 专属历史、磁吸跨段导航与新开关持久化兼容。
/// @return 所有路径成立时返回零，不创建音频设备或 ImGui 上下文。
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
        MMM::UI::BpmMeasurementHistoryTestAccess::checkPlaybackCursor(tool);
    // 先恢复偏好再抑制析构落盘，保持测试无用户设置副作用。
    settings.bpmMeasurementToolPreferences = original;
    MMM::UI::BpmMeasurementHistoryTestAccess::suppressPreferenceSave(tool);
    // 所有路径统一经过偏好清理，再用退出码报告结果。
    return passed ? 0 : 1;
}
