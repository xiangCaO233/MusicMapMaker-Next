/// @file BpmBeatmapImportTest.cpp
/// @brief 以真实项目文件、打开会话和确认弹窗验证关联谱面红线导入。
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/Project.h"
#include "mmm/sample/AudioSample.h"
#include "mmm/timing/TimingEffect.h"
#include "mmm/timing/TimingInterpolation.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>

namespace
{
/// @brief 构造毫秒模型事件，红线和其它效果明确使用不同字段值。
/// @param time 谱面毫秒时间。
/// @param effect 原始效果类型，不能靠参数正负猜测红绿线。
/// @param value 对应效果的参数。
MMM::Timing timing(double time, MMM::TimingEffect effect, double value)
{
    // 使用格式模型的毫秒单位，不能复用工具秒值作为夹具。
    // BPM 与派生拍长同时赋值，避免解析器纠正不一致数据掩盖单位错误。
    MMM::Timing result;
    result.m_timestamp             = time;
    result.m_timingEffect          = effect;
    result.m_timingEffectParameter = value;
    result.m_bpm         = effect == MMM::TimingEffect::BPM ? value : 120;
    result.m_beat_length = 60000 / result.m_bpm;
    return result;
}
/// @brief 读取只读来源的完整内容，确保导入未向谱面写出任何更改。
/// @param path 已成功写出的隔离谱面路径。
/// @return 原始字节，供前后逐字比较；不做换行或编码归一化。
std::string contents(const std::filesystem::path& path)
{
    // 按二进制读回，既检查语义数据也检查源文件没有被顺手重存。
    // 不通过重新加载后比较对象，否则重写格式而不改数据也可能漏报。
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>() };
}
/// @brief 写入原生谱面夹具，路径仅来自构建输出目录。
/// @param path 只允许测试输出目录内的原生谱面目标。
/// @param song 主歌曲提示，用于走生产项目资源关联规则。
/// @param red 为假时只有非 BPM 效果，覆盖“有关联但无红线”的拒绝分支。
/// @return 原生谱面序列化成功时为真。
bool writeMap(const std::filesystem::path& path, const char* song, bool red)
{
    MMM::BeatMap map;
    // 每份谱面拥有独立名称，但关联由歌曲路径决定而不是标题相似度。
    // 明确轨道数和参考 BPM，让无红线来源仍是一份合法可加载谱面。
    map.m_baseMapMetadata.name           = path.stem().string();
    map.m_baseMapMetadata.map_path       = path;
    map.m_baseMapMetadata.song_file_hint = song;
    map.m_baseMapMetadata.track_count    = 4;
    map.m_baseMapMetadata.preference_bpm = 120;
    // 红线跨越负时间、亚毫秒精度和工具当前音频末尾，不能被导入限幅。
    if ( red ) {
        map.m_timings.push_back(timing(-2000.125, MMM::TimingEffect::BPM, 120));
        map.m_timings.push_back(
            timing(123.456789, MMM::TimingEffect::BPM, 180.25));
        map.m_timings.push_back(timing(5010.9876, MMM::TimingEffect::BPM, 240));
    }
    // 非红线也放在可辨识位置，不能因先排序或读错参数混入 BPM 段落。
    map.m_timings.push_back(timing(200, MMM::TimingEffect::SCROLL, 2.5));
    map.m_timings.push_back(timing(300, MMM::TimingEffect::JUMP, 1.5));
    map.m_timings.push_back(timing(400, MMM::TimingEffect::HS, 2));
    // 真实保存再交给生产加载器，覆盖原生字段编码与时间单位转换。
    // 不把已经构造好的内存模型直接塞给导入函数，避免绕过磁盘分支。
    return map.saveToFile(path);
}
}  // namespace

namespace MMM::UI
{
/// @brief 测试只准备来源与选择，实际解析、导入及历史使用生产实现。
struct BpmBeatmapImportTestAccess {
    /// @brief 断言失败包含独立场景标签，不因一项失败跳过后续来源校验。
    /// @param value 当前场景的完整行为断言。
    /// @param label 日志中可识别的失败场景。
    /// @param passed 汇总结果，已有失败不得被后续成功覆盖。
    static void check(bool value, const char* label, bool& passed)
    {
        if ( !value ) XERROR("BPM import: {}", label);
        passed &= value;
    }

    /// @brief 验证按钮确认走同一个导入入口，而非只调用私有函数得到假成功。
    /// @details UI 状态只在独立 CPU 上下文内创建，不启动真实音频或图形窗口。
    /// @param tool 当前已经选定歌曲、可枚举真实关联来源的工具。
    /// @return 实际确认按钮触发导入并关闭模态时为真。
    /// 工具的候选枚举仍读取隔离项目，不向 UI 注入伪造选项。
    /// 这里仅覆盖单次确认，不将测试环境的坐标作为生产布局常量。
    static bool popup(BpmMeasurementToolView& tool)
    {
        // 使用明显不同的旧列表，单凭关闭弹窗不能算作成功导入。
        // 清空历史后，只有真实确认才能产生可撤销记录。
        tool.m_timingSegments = { { 0, 99 } };
        tool.syncPrimaryTimingFieldsFromSegments();
        tool.clearMeasurementHistory();
        tool.requestOpenImportTimingPopup();
        // 只创建 CPU UI 上下文；不接入用户桌面窗口或 GPU 后端。
        // ini 写出被关闭，测试不会污染用户窗口布局。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1200, 800 };
        io.DeltaTime   = .02f;
        // 每个测试帧精确送入一种鼠标状态，禁用跨帧事件缓冲以消除时序歧义。
        // 固定帧间隔仅用于 UI 计算，不使用 sleep 等待交互。
        io.ConfigInputTrickleEventQueue = false;
        // 默认字体图集满足 CPU 文本测量要求；不需要导入用户皮肤资源。
        // 关闭反馈音，避免测试确认时真的播放应用提示音。
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        SetInteractionFeedbackEnabled(false);
        /// @brief 在独立宿主内绘制生产模态，并以实际鼠标按下／松开触发按钮。
        /// @details 不调用私有导入函数作为这项 UI 回归的替代。
        const auto frame = [&](ImVec2 point, bool down = false) {
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            ImGui::Begin("BpmImportHost");
            tool.renderImportTimingPopup(nullptr);
            ImGui::End();
            ImGui::Render();
        };
        // 首帧创建模态，次帧让自动尺寸稳定后再按实际窗口位置定位。
        // 鼠标先放到视口外，防止默认坐标意外悬浮或激活控件。
        frame({ -100, -100 });
        frame({ -100, -100 });
        const auto title = TR("ui.tools.bpm_measure.import_popup").toString() +
                           "###BpmImportTiming";
        // 可见标题来自翻译，但固定 ID 与生产一致；定位实际生成的窗口。
        // 同时检查弹窗栈，防止只找到上次关闭后仍保留的窗口对象。
        const auto* window = ImGui::FindWindowByName(title.c_str());
        bool        passed =
            window && !ImGui::GetCurrentContext()->OpenPopupStack.empty();
        if ( passed ) {
            // 使用按钮文字与当前样式测量宽度，不硬编码屏幕像素坐标。
            // 确认与取消位于末行，窗口底边反推中心可适应说明换行。
            const auto labelWidth =
                ImGui::CalcTextSize(
                    TR("ui.tools.bpm_measure.import_confirm").data())
                    .x;
            const ImVec2 point{ window->DC.CursorStartPos.x + labelWidth * .5f +
                                    ImGui::GetStyle().FramePadding.x,
                                window->Pos.y + window->Size.y -
                                    ImGui::GetStyle().WindowPadding.y -
                                    ImGui::GetFrameHeight() * .5f };
            // 先悬浮，再按下，再松开，复现 FeedbackButton 的实际点击路径。
            // 不能只触发 MouseClicked 而漏掉按钮默认的松开确认行为。
            frame(point);
            frame(point, true);
            frame(point);
            // 成功必须同时关闭模态、替换红线并生成历史，三者缺一即失败。
            // 这能区分按钮命中失败与导入入口内部逻辑失败。
            passed &= ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
                      tool.m_timingSegments.size() == 3 &&
                      tool.canUndoMeasurement();
        }
        // 模态断言完成后才销毁上下文，避免访问失效的窗口数据。
        // 不把测试 UI 状态带入同一测试进程中的其它用例。
        ImGui::DestroyContext();
        return passed;
    }

    /// @brief 真实磁盘来源、ECS 未保存红线、原子拒绝与撤销重做联合回归。
    /// @param root 隔离项目根，不读取任何用户项目或来源测试资源目录。
    /// @return 来源筛选、精度、历史、实际按钮和失效保护全部通过时为真。
    /// @details 同一工具贯穿成功与失败路径，以检查拒绝操作不破坏已有结果。
    /// 先验证未打开来源，再验证已打开 ECS，可识别错误的磁盘回退。
    /// 所有拒绝分支发生在已有成功结果之后，防止空列表产生假通过。
    /// 时间断言同时覆盖输入单位、浮点精度和音频范围三个独立约束。
    /// 历史断言检查整份列表和首段兼容字段，避免只恢复其中一部分。
    /// 测试不启动后台分析，而是显式构造交付状态验证覆盖保护。
    static bool run(const std::filesystem::path& root)
    {
        bool  passed     = true;
        auto& controller = Logic::ProjectController::instance();
        auto* project    = controller.currentProject();
        // 以真实控制器项目提供关联入口，不手工伪造候选列表。
        // 音轨选择只设定测试上下文，红线内容始终经生产读取逻辑获取。
        BpmMeasurementToolView tool("BpmImportTest");
        tool.m_selectedAudioTrackId = "song-a";
        tool.m_selectedAudioSyncKey = "import-test";
        // 故意让音频时长小于最后一个红线时间，暴露错误的时长钳制。
        // 旧 BPM 取非夹具值，确认和撤销都必须能观察到完整替换。
        tool.m_duration       = 1;
        tool.m_timingSegments = { { 0, 99 } };
        tool.syncPrimaryTimingFieldsFromSegments();
        // 留下来源字节基线，所有导入与工具撤销之后仍应完全一致。
        // 测试读取的是隔离输出夹具，不涉及任何用户实际谱面。
        const auto before = contents(root / "a.mmm");
        tool.requestOpenImportTimingPopup();
        // 同歌的普通谱面与无红线谱面可见，绑定另一首音乐的谱面必须被排除。
        // 未绑定和引用失效的两份谱面也在项目内，不能回退项目首歌混入候选。
        // 它们有有效红线，故数量正确才能证明筛选依据是歌曲关联而非红线有无。
        check(tool.m_importOptions.size() == 2, "filter bound charts", passed);
        check(!tool.m_importOptions.empty() &&
                  tool.m_importOptions.front().m_path == root / "a.mmm",
              "project source order",
              passed);
        // 数量不符时停止下标访问，避免测试自身越界掩盖生产失败原因。
        // 两个候选顺序来自项目目录，第二份用于验证可见但无法确认的来源。
        if ( tool.m_importOptions.size() != 2 ) return false;
        check(!tool.m_importOptions[1].m_error.empty(),
              "no red source explains rejection",
              passed);
        // 首次确认发生在没有打开任何谱面会话的条件下，必须走磁盘解析。
        // 仅计算红线数量不足以证明过滤正确，后续分别核对各点时间和参数。
        check(
            tool.importSelectedBeatmapTimings(), "import closed chart", passed);
        check(tool.m_timingSegments.size() == 3, "red only", passed);
        // 采用严格秒值误差检查，能识别毫秒单位误用和亚毫秒精度丢失。
        // 保留负时间与音频末尾外事件，不沿用手动拖动的有效视野限制。
        if ( tool.m_timingSegments.size() == 3 ) {
            check(std::abs(tool.m_timingSegments[0].timestampSeconds +
                           2.000125) < 1e-12,
                  "negative first red retained",
                  passed);
            check(std::abs(tool.m_timingSegments[1].timestampSeconds -
                           .123456789) < 1e-12,
                  "submillisecond precision",
                  passed);
            check(std::abs(tool.m_timingSegments[2].timestampSeconds -
                           5.0109876) < 1e-12,
                  "red beyond current audio duration retained",
                  passed);
            check(tool.m_timingSegments[1].bpm == 180.25,
                  "BPM parameter retained",
                  passed);
        }
        // 导入的三个点作为一个事务撤回，不能要求撤销三次才能回到旧列表。
        // 同时检查兼容首段 BPM 字段，避免只有完整列表还原而 UI 留旧值。
        tool.undoMeasurementFromShortcut();
        check(tool.m_timingSegments.size() == 1 && tool.m_bpm == 99,
              "one undo restores whole old list",
              passed);
        // 重做也必须恢复导入列表，而不是重新读取可能已经变化的来源文件。
        // 当前项目不变时，历史中的秒值和首段字段应保持一致。
        tool.redoMeasurementFromShortcut();
        check(tool.m_timingSegments.size() == 3 && tool.m_bpm == 120,
              "redo restores imported list",
              passed);
        // 无红线来源在失败前不能清空数据或写入空撤销记录。
        tool.m_importSelectedIndex = 1;
        const auto historySize     = tool.m_measurementUndoHistory.size();
        check(!tool.importSelectedBeatmapTimings() &&
                  tool.m_timingSegments.size() == 3 &&
                  tool.m_measurementUndoHistory.size() == historySize,
              "empty source is atomic",
              passed);
        tool.m_importSelectedIndex = 0;
        // 上面的直接确认用于精确定位业务断言，这里额外走实际可见按钮。
        // 弹窗测试自行重置工具数据，不依赖前一次失败刚好留下的状态。
        check(popup(tool), "real popup confirmation", passed);
        check(contents(root / "a.mmm") == before,
              "disk source remains unchanged",
              passed);

        // 随后打开同一磁盘来源并让会话初始化 ECS，转入未保存编辑分支。
        // 不预先保存修改，确保成功导入确实来自内存时间线。
        auto map =
            std::make_shared<BeatMap>(BeatMap::loadFromFile(root / "a.mmm"));
        auto&      engine = Logic::EditorEngine::instance();
        const auto index  = engine.createSession(map, "Open import source");
        engine.setActiveSessionIndex(index);
        auto session = engine.getActiveSession();
        session->update(0, engine.getEditorConfig(), true);
        auto& ctx     = session->getContextMutable();
        auto  timings = ctx.timelineRegistry.view<Logic::TimelineComponent>();
        // 通过效果与时间寻找目标实体，不依赖 entt 的分配编号或迭代顺序。
        // null 哨兵必须是 entity 类型，后续确认前先检查是否真的找到红线。
        entt::entity changed = entt::null;
        for ( const auto entity : timings ) {
            auto& t = timings.get<Logic::TimelineComponent>(entity);
            if ( t.m_effect == TimingEffect::BPM && t.m_timestamp > 0 &&
                 t.m_timestamp < 1 )
                changed = entity;
        }
        check(changed != entt::null, "loaded ECS source", passed);
        if ( changed != entt::null ) {
            tool.requestOpenImportTimingPopup();
            // 在弹窗候选缓存建立之后再改红线，确认应读取最新内存而非磁盘或旧快照。
            auto& t =
                ctx.timelineRegistry.get<Logic::TimelineComponent>(changed);
            t.m_value = 201.125;
            check(tool.importSelectedBeatmapTimings() &&
                      tool.m_timingSegments[1].bpm == 201.125,
                  "unsaved live red wins",
                  passed);
            // 同一红线改成曲线后不能悄悄导入起点或部分成功。
            // 已有列表与撤销栈同时不变，才能证明失败保持原子性。
            const auto count  = tool.m_measurementUndoHistory.size();
            t.m_interpolation = TimingInterpolation{};
            check(!tool.importSelectedBeatmapTimings() &&
                      tool.m_measurementUndoHistory.size() == count &&
                      tool.m_timingSegments[1].bpm == 201.125,
                  "BPM curve rejects without flattening",
                  passed);
            // 恢复离散红线后只改变歌曲绑定，磁盘仍保存原歌曲提示。
            // 如果错误回退磁盘会重新匹配 song-a，此断言专门排除这种行为。
            t.m_interpolation.reset();
            ctx.currentBeatmap->m_baseMapMetadata.song_file_hint = "song-b.wav";
            check(!tool.importSelectedBeatmapTimings(),
                  "live rebind cannot fall back to disk",
                  passed);
            ctx.currentBeatmap->m_baseMapMetadata.song_file_hint = "song-a.wav";
        }
        // 关闭来源不刷新真实工作区展示，测试只需要让后续读取回到磁盘。
        // 本地观察指针从此不再使用，避免引用已关闭会话的上下文。
        engine.closeSession(index, false);
        // 关闭来源后允许重新读盘；切歌和换项目则必须拒绝旧弹窗。
        tool.requestOpenImportTimingPopup();
        tool.m_selectedAudioTrackId = "song-b";
        check(!tool.importSelectedBeatmapTimings(),
              "audio switch invalidates source",
              passed);
        tool.m_selectedAudioTrackId = "song-a";
        // 两个项目允许出现相同资源 ID，因此只核对音轨字符串还不够。
        // 变更根目录模拟项目替换，确认必须识别整个来源上下文失效。
        const auto originalRoot = project->m_projectRoot;
        project->m_projectRoot  = root / "other-project";
        check(!tool.importSelectedBeatmapTimings(),
              "same audio ID in another project is rejected",
              passed);
        project->m_projectRoot = originalRoot;
        // 文件仍然存在并不足以授权旧选择，项目入口已移除时必须整体拒绝。
        const auto removed = project->m_beatmaps.front();
        project->m_beatmaps.erase(project->m_beatmaps.begin());
        check(!tool.importSelectedBeatmapTimings(),
              "removed entry rejected",
              passed);
        project->m_beatmaps.insert(project->m_beatmaps.begin(), removed);
        const auto count = tool.m_measurementUndoHistory.size();
        // 模拟正在分析、结果刚写入槽但尚未发布、完成但尚未消费三个阶段。
        // 这些状态都可能在下一帧恢复嵌入标记，不能让它们覆盖新导入。
        tool.m_analysisRunning.store(true, std::memory_order_relaxed);
        check(!tool.importSelectedBeatmapTimings(),
              "running analysis rejected",
              passed);
        tool.m_analysisRunning.store(false, std::memory_order_relaxed);
        // 结果槽状态有意独立于完成标志，模拟生产者尚未发布完成的边界。
        // 测试没有工作线程，直接写槽不会形成跨线程竞争。
        // 在友元作用域显式构造再移入结果槽，兼容旧 Clang 的嵌套类型构造检查。
        // 仍放入真实待交付结果，不能以完成标志代替这一交付边界测试。
        tool.m_pendingResult = BpmMeasurementToolView::AnalysisResult{};
        check(!tool.importSelectedBeatmapTimings(),
              "queued result rejected",
              passed);
        tool.m_pendingResult.reset();
        // 完成位仍为真时，UI 消费流程还没结束，也应保留原工具列表。
        // 最后清掉所有模拟状态，不能让工具析构误判仍有工作待交付。
        tool.m_analysisFinished.store(true, std::memory_order_release);
        check(!tool.importSelectedBeatmapTimings(),
              "unconsumed analysis rejected",
              passed);
        tool.m_analysisFinished.store(false, std::memory_order_release);
        check(tool.m_measurementUndoHistory.size() == count,
              "failed confirmations preserve history",
              passed);

        // 没有显式歌曲提示时，只能通过真实 Main 自动采样建立关联。
        // 将更晚的锚点配上负偏移，验证优先级比较实际播放时间而非容器顺序。
        auto sampled = BeatMap::loadFromFile(root / "unbound.mmm");
        // 保持两个采样都有效且资源存在，排除“解析失败恰好被跳过”的假通过。
        // 锚点顺序故意与生效时间相反，负偏移必须参与歌曲关联选择。
        AudioSampleEvent later;
        later.m_audioResourceId = "song-b";
        later.m_timestamp       = -500;
        AudioSampleEvent earlier;
        earlier.m_audioResourceId = "song-a";
        earlier.m_timestamp       = 1000;
        earlier.m_offsetMs        = -2000;
        // 容器顺序同样与预期优先级相反，不允许以首采样直接替代最早 Main。
        // 该模型仍使用隔离 .mmm 序列化，生产解析器负责恢复实际偏移。
        sampled.m_audioSamples = { later, earlier };
        check(sampled.saveToFile(root / "unbound.mmm"),
              "write sample binding",
              passed);
        // 第一个采样属于另一首歌，但有效时间更早的 song-a 应成为来源歌曲。
        // 此处直接调用同一生产读取边界，不预先构造或修改候选结果。
        check(tool.readBoundBeatmapTimings(root / "unbound.mmm").has_value(),
              "earliest effective Main sample binds source",
              passed);
        // 改变资源类型模拟仅有按键音的谱面，不能继续按 Main 音乐关联。
        // 另一首 Main 歌仍存在，确保筛选不会按项目首资源重新兜底。
        project->m_audioResources[0].m_type = AudioTrackType::Effect;
        check(!tool.readBoundBeatmapTimings(root / "unbound.mmm"),
              "Effect sample does not override Main song",
              passed);
        project->m_audioResources[1].m_type = AudioTrackType::Effect;
        // 两个采样都只是 Effect 时，即使谱面里引用了当前音频也不是歌曲绑定。
        // 检查完成后恢复资源类型，不能影响工具析构和项目关闭流程。
        check(!tool.readBoundBeatmapTimings(root / "unbound.mmm"),
              "keysounds alone do not bind music",
              passed);
        project->m_audioResources[0].m_type = AudioTrackType::Main;
        project->m_audioResources[1].m_type = AudioTrackType::Main;
        // 测试只验证工具本地数据，不把测量参数写入用户全局偏好。
        // 临时交付标志均已复原，析构不需要等待任何真实后台任务。
        tool.m_userPreferencesDirty            = false;
        tool.m_userPreferencesSaveDelaySeconds = 0;
        return passed;
    }
};
}  // namespace MMM::UI

/// @brief 只在 CMake 提供的绝对输出目录生成谱面，不向源码目录写资源。
/// @param argc 必须有且只有一个由 CMake 传入的输出目录参数。
/// @param argv 不接受相对目录，防止测试运行位置不同导致写入源码树。
/// @return 任一夹具创建或行为断言失败时返回非零。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    const auto base = MMM::Config::utf8ToPath(argv[1]);
    if ( !base.is_absolute() ) return 1;
    // 每次运行创建独立子目录，避免重复执行读到上次留下的项目数据。
    // steady_clock 仅生成唯一名称，不参与被测音乐时间计算。
    const auto root =
        base / std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
    // 文件系统错误使用显式错误码，不使用项目禁止的异常处理。
    // 创建失败立即退出，不能在不明确的目录继续生成资源。
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if ( error ) return 1;
    // 控制器与编辑引擎采用生产单例，先初始化以建立正常的项目会话关系。
    // 无需启动渲染、音频解码或目录监视后台线程。
    MMM::Logic::EditorEngine::instance();
    auto& controller = MMM::Logic::ProjectController::instance();
    if ( !controller.openProject(root).m_opened ) return 1;
    if ( !writeMap(root / "a.mmm", "song-a.wav", true) ||
         !writeMap(root / "b.mmm", "song-b.wav", true) ||
         !writeMap(root / "no-red.mmm", "song-a.wav", false) ||
         !writeMap(root / "unbound.mmm", "", true) ||
         !writeMap(root / "missing-song.mmm", "missing.wav", true) )
        return 1;
    auto* project = controller.currentProject();
    // 两个 Main 资源都存在于项目表，用歌曲提示进行真实关联匹配。
    // 本用例不解码音乐，故无需制造无关的大型音频测试资源。
    for ( const auto* id : { "song-a", "song-b" } ) {
        MMM::AudioResource resource;
        resource.m_id   = id;
        resource.m_path = std::string(id) + ".wav";
        resource.m_type = MMM::AudioTrackType::Main;
        project->m_audioResources.push_back(resource);
    }
    // 前三份入口为有效同歌、另一歌曲、同歌无红线。
    // 最后两份保留无绑定和失效绑定，回归默认音频接口的兜底误匹配。
    // 这两份都含合法红线，不能依靠“红线为空”把它们碰巧过滤。
    // 路径刻意使用相对形式，覆盖项目根路径解析而非仅绝对路径匹配。
    project->m_beatmaps = { { "a", "a.mmm", "" },
                            { "b", "b.mmm", "" },
                            { "no-red", "no-red.mmm", "" },
                            { "unbound", "unbound.mmm", "" },
                            { "missing", "missing-song.mmm", "" } };
    // 所有来源文件都已写出；后续数量断言会同时检查有效和无效绑定。
    // 行为检查返回值是本进程唯一成功标准，文件写出成功不能替代功能通过。
    // 关闭项目在返回之前执行，确保资源生命周期也经过生产清理路径。
    const bool passed = MMM::UI::BpmBeatmapImportTestAccess::run(root);
    // 显式关闭隔离项目，使会话清理完成后才结束测试进程。
    // 夹具保留在构建输出下供失败诊断，不向源码资源目录复制。
    controller.closeProject();
    return passed ? 0 : 1;
}
