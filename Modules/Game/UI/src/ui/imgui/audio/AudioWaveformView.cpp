#include "ui/imgui/audio/AudioWaveformView.h"
#include "WaveformAnalysis.h"
#include "audio/AudioManager.h"
#include "audio/OfflineAnalysisEQ.h"
#include "common/LogicCommands.h"
#include "common/render/RenderSnapshot.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/VisualConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "implot.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/layout/LayoutContext.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace MMM::UI
{
/// @brief 构造波形视图并预分配固定采样点数组。
/// @param name 视图显示名称。
///
/// 完整包络仅在收到用户的同步请求后才在后台分配，显示数组的长度与音轨时长无关。
AudioWaveformView::AudioWaveformView(const std::string& name)
    : IUIView(name), m_analysis(std::make_unique<WaveformAnalysis>())
{
    // 时间轴与左右声道最小/最大包络必须保持完全相同长度。
    // 绘图接口按统一点数借用连续存储，窗口内缩放不会改变这些数组的容量。
    m_times.resize(m_samplePoints);
    m_viewMinL.resize(m_samplePoints);
    m_maxEnvelopeL.resize(m_samplePoints);
    m_viewMinR.resize(m_samplePoints);
    m_maxEnvelopeR.resize(m_samplePoints);
}

/// @brief 释放触控定位；分析状态析构会请求取消后台任务。
// 图表销毁前释放自己的最后有效定位，不能让音频永久处于 Scrub。
// 宿主仍拥有会话和音频资源，这里不停止其他控制器或清空历史。
AudioWaveformView::~AudioWaveformView()
{
    m_touchNavigation.suspend();
}
/// @brief 显式关闭立即结束触控定位并取消尚未交付的波形。
/// @param open 新的窗口可见状态。
/// @details 宿主可能在关闭后停止调度 update，必须在切换前解除寻位和交付状态。
/// 已接收的包络可用于重新打开同一音轨。
void AudioWaveformView::setOpen(bool open)
{
    // 关闭无需等待后台解码；基类负责记录后续绘制调度使用的窗口状态。
    if ( !open ) {
        m_touchNavigation.suspend();
        m_analysis->cancel();
    }
    IUIView::setOpen(open);
}

/// @brief 更新并绘制双声道波形窗口。
/// @param sourceManager 非拥有 UI 管理器，用于查询工程切换状态。
///
/// 每帧读取音频播放状态与逻辑层同步快照，构造自适应换行控制条，并绘制左右声道
/// 包络。拖拽 Plot 时持续发布预览鼠标位置，释放后发布最终 Seek。
///
/// 同步效果按钮只提交离线任务，后续帧在结果就绪时替换缓存。等待期间绘制保留
/// 输入与关闭事件，继续使用已接收的同音轨包络。
///
/// 本函数同时处理三套时间：音频时间用于
/// Seek，通用视觉时间用于播放头和画布同步，
/// 波形专用偏移只影响缓存采样。修改偏移逻辑时必须保持三者的转换方向一致。
/// @warning UI 热路径：窗口可见时每帧调用；不得执行文件
/// IO、完整解码或阻塞等待。
void AudioWaveformView::update(UIManager* sourceManager)
{
    // 首次打开使用可读默认尺寸，后续尊重用户停靠和缩放布局。
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);


    // `###` 后缀固定内部窗口 ID，动态 m_name 不破坏停靠状态。
    std::string windowTitle = m_name + "###AudioWaveformViewGlobal";
    // LayoutContext 管理窗口 Begin/End 与关闭状态。
    // 标题栏关闭按钮通过 m_isOpen 直接改变可见状态，不会调用 setOpen。
    // 这个分支和显式关闭一样取消寻位与未交付波形，使关闭当帧即生效。
    LayoutContext layoutContext(
        *this, windowTitle, true, ImGuiWindowFlags_None, &m_isOpen);
    if ( !m_isOpen ) {
        // 即使 Plot 没有进入提交阶段，这里仍观察到标题栏已经关闭窗口。
        m_touchNavigation.suspend();
        m_analysis->cancel();
        return;
    }


    if ( sourceManager && sourceManager->isProjectTransitionInProgress() ) {
        m_touchNavigation.suspend();
        m_analysis->reset();
        // 工程切换时不读取可能失效的音轨或 Session，只显示占位提示。
        // 即使新工程复用相同音频文件，也需在过渡阶段清掉旧来源的显示缓存。
        Utils::renderProjectTransitionPlaceholder();
        return;
    }

    // AudioManager 是当前 BGM 音轨、播放时间与速度的运行态真值。
    // 音频管理器可以复用一个解码音轨；后台缓存的身份还要包含工程音频时长。
    // 这里只借当前控制侧状态判断来源，后台不调用音频管理器。
    auto& audioManager = Audio::AudioManager::instance();
    // 该拥有型句柄本来就是本帧判断是否有音轨的输入；任务只在按钮触发时复制。
    auto track = audioManager.getBGMTrack();

    if ( !track ) {
        m_touchNavigation.suspend();
        m_analysis->reset();
        // 无主音轨时保持窗口可见并给出加载提示。
        ImGui::Text("%s", TR("ui.audio_manager.initial_hint").data());
        return;
    }

    // 通用与波形专用视觉偏移分别读取，避免混淆音频 Seek 时间。
    // 通用偏移移动播放头和主视野覆盖；波形偏移只影响底层包络查表。
    const auto& visualConfig = Config::AppConfig::instance().getVisualConfig();
    float       globalVisualOffset = visualConfig.getEffectiveVisualOffset();
    float       waveformVisualOffset =
        visualConfig.getWaveformEffectiveVisualOffset();
    // AudioManager 值是快照缺失时的安全回退。
    // 修改波形专用偏移只改变缓存采样位置，不要求重跑 EQ 或重建时间桶。
    double audioTime  = audioManager.getCurrentTime();
    double visualTime = audioTime + globalVisualOffset;
    double totalTime  = audioManager.getTotalTime();
    double speed      = audioManager.getPlaybackSpeed();

    // 先按当前来源与 EQ 代次淘汰旧请求，再尝试接收已完成结果。
    // EQ 代次是控制侧标量；这里不通过逐频段 getter 获取滤波器锁。
    // 播放速度改变墙钟推进速率，不改变已采样的原始音频时间。
    m_analysis->poll(track, totalTime, audioManager.getMainTrackEQRevision());

    // 优先使用逻辑层平滑时间，使预览拖拽和主画布视野保持实时同步。
    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    // 空相机 ID 使用 Basic2DCanvas 的稳定同步缓冲。
    // 取包络结果早于逻辑快照读取，因为逻辑层插值只负责可见中心。
    auto snapshot = Logic::EditorEngine::instance()
                        .getSyncBuffer(activeCameraId.empty() ? "Basic2DCanvas"
                                                              : activeCameraId)
                        ->getReadingSnapshot();
    if ( snapshot ) {
        // 快照由逻辑线程发布，本帧只读取同一个不可变版本，避免字段跨版本混用。
        // 拖拽快照直接使用记录时间，避免外推越过用户当前指针位置。
        visualTime = snapshot->currentTime;
        audioTime  = snapshot->playbackTime;
        if ( !snapshot->isPreviewDragging ) {
            // 正常播放按 steady_clock 当前秒数外推逻辑与音频时间。
            const double now =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            // 两种时间共享同一个外推基准，避免偏移在播放头和 Seek 之间漂移。
            visualTime = snapshot->resolveCurrentTimeAt(now);
            audioTime  = snapshot->resolvePlaybackTimeAt(now);
        }
    }

    // 原始触控图表保持本地连续浏览中心，经典方案仍跟随既有播放快照。
    const bool touchInput = Interaction::activeProfile().touchGestures();
    // 显示中心只在触控活动浏览期间由本地导航维护，平时继续跟随快照。
    // 本地中心不改波形专用偏移，后者仍只控制采样缓存的时间对齐。
    // 两个声道以相同中心采样，连续输入不能让左右图表使用不同基线。
    // 实例 ID 改变意味着之前的局部寻位中心不能作为新谱面的参考。
    visualTime = m_touchNavigation.begin(
        visualTime, snapshot ? snapshot->beatmapInstanceId : 0);
    // 控制条宽度由当前主题字体、padding 与 item spacing 计算。
    // 顶部栏自行换行，所以计算使用当前样式与 DPI 下的实测宽度。
    // 后续 Plot 使用父窗口推进后的剩余高度，避免把进度文本算进图高。
    ImGuiStyle& style  = ImGui::GetStyle();
    float       frameH = ImGui::GetFrameHeight();
    /// @brief 计算标签、内间距及固定滑块的组合宽度。
    /// @details 宽度以当前语言的文字测量，长标签可随控制组独立换行。
    auto calcSliderWidth = [&](float sliderW, const char* label) {
        return sliderW + style.ItemInnerSpacing.x +
               ImGui::CalcTextSize(label).x;
    };
    /// @brief 计算按钮文本和左右 FramePadding 的总宽度。
    /// @details 两个边距都计入宽度，确保 Clay 给 ImGui 留出相同的命中范围。
    auto calcButtonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    /// @brief 在相邻控制组之间绘制竖向分隔线。
    /// @details 线段截掉两端内间距，与当前行控件的视觉中线对齐。
    auto drawSep = [&](Clay_BoundingBox r, bool) {
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(r.x, r.y + 2.0f),
            ImVec2(r.x, r.y + r.height - 2.0f),
            ImGui::GetColorU32(ImGuiCol_Separator));
    };

    // 顶部 VBox 按可用宽度容纳一个或多个自动换行控制行。
    CLayVBox    topContainer;
    const float dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    /// 把非负浮点尺寸向上取整到 Clay 使用的 uint16 像素。
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    // padding 与 spacing 至少随 DPI 增长，同时尊重当前 ImGui 主题。
    const float rowPadding =
        std::ceil(std::max(4.0f * dpiScale, style.FramePadding.x));
    const float spacing =
        std::ceil(std::max(8.0f * dpiScale, style.ItemSpacing.x));
    // 行自身已有左右留白，根容器不再叠一层边距。
    topContainer.setPadding(0, 0, 0, 0)
        .setSpacing(toLayoutPixels(
            std::max(4.0f * dpiScale, style.ItemSpacing.y * 0.5f)));
    // deque 保证新增换行时已交给 Clay 的行引用保持稳定。
    // 注册回调时只安排布局，真正的按钮处理在 renderInCurrent 提交矩形后执行。
    std::deque<CLayHBox> rows;
    CLayHBox*            currentRow = nullptr;
    float                currentW   = 0.0f;
    float                availW     = ImGui::GetContentRegionAvail().x;

    /// 向当前控制行追加一组固定尺寸控件，必要时自动换到新行。
    /// @param id Clay 元素稳定 ID。
    /// @param w 控制组所需宽度。
    /// @param h 控制组高度。
    /// @param drawCb 在 Clay 矩形中提交 ImGui 控件的回调。
    auto pushGroup = [&](const std::string& id, float w, float h, auto drawCb) {
        // 首组不需要前置分隔线，同行后续组计入一像素线与 spacing。
        bool  addSep = false;
        float totalW = w;
        if ( currentRow ) {
            // 换到新行时该分隔线不提交，需要在这里保守预估同行用宽。
            totalW += 1.0f + spacing;
        }
        // 换行时恢复两侧 padding，组内标签与滑块不参与拆分。
        if ( !currentRow || currentW + totalW > availW ) {
            // 当前行无法容纳时创建新行并应用统一四向 padding。
            rows.emplace_back();
            currentRow = &rows.back();
            currentRow
                ->setPadding(toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding))
                .setSpacing(toLayoutPixels(spacing));
            // 行对象存于 deque，根容器可安全保存其引用到本帧渲染。
            topContainer.addLayout(
                ("Row_" + std::to_string(rows.size())).c_str(),
                *currentRow,
                Sizing::Grow(),
                Sizing::Fit());
            // 新行已占用左右两侧 padding。
            currentW = rowPadding * 2.0f;
        } else {
            // 同行追加时在两个控制组之间插入分隔线。
            addSep = true;
        }

        if ( addSep ) {
            // 分隔线高度与目标组一致，并计入宽度追踪。
            currentRow->addElement(
                id + "_Sep", Sizing::Fixed(1.0f), Sizing::Fixed(h), drawSep);
            currentW += 1.0f + spacing;
        }
        // 控制组使用测量得到的固定宽高，避免 Clay 重新压缩内部 ImGui 控件。
        currentRow->addElement(id, Sizing::Fixed(w), Sizing::Fixed(h), drawCb);
        currentW += w + spacing;
    };

    // 缩放滑块控制播放头两侧各显示多少秒波形。
    pushGroup("ZoomSlider",
              calcSliderWidth(100.0f, TR("ui.waveform.zoom").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  // 标签与固定 100 像素滑块在同一 Clay 组内横向排列。
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  ImGui::AlignTextToFramePadding();
                  ImGui::Text("%s", TR("ui.waveform.zoom").data());
                  ImGui::SameLine();
                  ImGui::SetNextItemWidth(100);
                  ::MMM::UI::FeedbackSliderFloat(
                      "##zoom", &m_zoom, 0.1f, 10.0f, "%.4fs");
              });
    // 重置按钮把窗口恢复到一秒半宽视野。
    pushGroup("ResetZoomBtn",
              calcButtonWidth(TR("ui.waveform.reset_zoom").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  if ( ::MMM::UI::FeedbackButton(
                           TR("ui.waveform.reset_zoom").data()) )
                      m_zoom = 1.0f;
              });
    // 同步效果按钮只在用户请求时冻结 EQ 参数，空 EQ 也会计算原始包络。
    // 等待期间保持按钮位置稳定，重复点击不能为同一个快照叠加整曲扫描。
    pushGroup("SyncEffectsBtn",
              calcButtonWidth(TR("ui.waveform.sync_effects").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  if ( ::MMM::UI::FeedbackButton(
                           TR("ui.waveform.sync_effects").data()) &&
                       !m_analysis->isRunning() ) {
                      auto& pools = Runtime::AppThreadPool::instance();
                      // 文件池承担长时间扫描；流式分析解码由通用池提供，避免
                      // 一批分析任务堵住各自依赖的解码工作。
                      auto* filePool   = pools.getFileThreadPool();
                      auto* decodePool = pools.get();
                      if ( filePool && decodePool ) {
                          m_analysis->start(
                              track,
                              totalTime,
                              Audio::captureOfflineAnalysisEQ(audioManager),
                              audioManager.getMainTrackEQRevision(),
                              *filePool,
                              *decodePool);
                      }
                  }
              });

    // 控制组登记完成后统一布局并推进父窗口游标。
    // 控件回调在本行测量后的目标坐标运行，点击事件只在本帧提交一次。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = topContainer.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    // 状态文字留在窗口内，不建立会拦截关闭或切换工程的模态。
    // 同一帧先绘制控件再检查状态，点击后立即得到反馈，无需等后台开始。
    // 重算只更新当前缓存，缩放与定位仍可操作。
    // 同一音轨的已提交包络继续作为预览，用户确认同步后才替换。
    if ( m_analysis->isRunning() )
        ImGui::Text("%s", TR("ui.waveform.processing.text").data());

    // 包络抽样只读取已经接收的缓存。
    // 换轨后屏幕坐标必须立刻映射新来源；等待结果时不会继续描绘旧曲。
    updateEnvelopes(visualTime, totalTime, speed, waveformVisualOffset);

    // 两个声道和本帧包络必须使用同一旧半窗；输入的新缩放在下一帧统一绘制。
    // 本帧包络已经按当前半窗提取，不能在下声道单独采用新倍率。
    // 输入发生在绘制期间，新半窗与新包络在下一次常规 UI 帧一起使用。
    // 这不是等待同步，线程继续正常推进，也不增加固定时间延迟。
    const float drawZoom = m_zoom;
    // 剩余高度在左右声道两个纵向 Plot 之间平均分配。
    // 两个 Plot 共享缩放比例，不因后台结果到达仅改变其中一边的可见跨度。
    float totalAvailH = ImGui::GetContentRegionAvail().y;
    float channelH    = totalAvailH * 0.5f - ImGui::GetStyle().ItemSpacing.y;

    /// 绘制一个声道的波形、播放头、主视野覆盖和拖拽交互。
    /// @param id 当前声道的稳定 ImPlot ID。
    /// @param minEnv 当前窗口最小包络数组。
    /// @param maxEnv 当前窗口最大包络数组。
    /// @param label Y 轴声道标签。
    auto renderChannel = [&](const char*   id,
                             const double* minEnv,
                             const double* maxEnv,
                             const char*   label) {
        // m_zoom 表示播放头左右各自的时间跨度。
        double viewStart = visualTime - drawZoom;
        double viewEnd   = visualTime + drawZoom;

        // 上一帧活动状态让拖拽越过 Plot 悬浮边界时仍保持预览。
        static bool s_lastActive[2] = { false, false };
        // 固定 ID 映射左右声道索引，两个 Plot 分别维护拖拽状态。
        int chanIdx = (std::string(id) == "##WaveL") ? 0 : 1;

        // 触控路径自行管理定位与缩放，关闭 Plot 默认输入防止重复消费。
        if ( ImPlot::BeginPlot(id,
                               ImVec2(-1, channelH),
                               ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                                   ImPlotFlags_NoMouseText |
                                   (touchInput ? ImPlotFlags_NoInputs : 0)) ) {

            // X 轴使用当前视觉时间窗，Y 轴固定到标准 PCM 振幅范围附近。
            // Y 轴界限在两个声道保持相同，便于比较一轮重算前后的峰值。
            ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
            ImPlot::SetupAxis(
                ImAxis_Y1,
                label,
                ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(
                ImAxis_X1, viewStart, viewEnd, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.1, 1.1, ImGuiCond_Always);

            // 最小与最大包络之间的填充表示该像素时间桶中的振幅范围。
            // 左右声道借用同一份时间轴，零包络也能保持 Plot 导航可用。
            ImPlot::PlotShaded("##Wave",
                               m_times.data(),
                               minEnv,
                               maxEnv,
                               m_samplePoints,
                               ImPlotSpec(ImPlotProp_FillAlpha, 0.5f));

            // 红色播放头使用视觉时间，贯穿完整 Y 轴。
            double playheadX[2] = { visualTime, visualTime };
            double playheadY[2] = { -1.1, 1.1 };
            ImPlot::PlotLine("##Playhead",
                             playheadX,
                             playheadY,
                             2,
                             ImPlotSpec(ImPlotProp_LineColor,
                                        ImVec4(1, 0, 0, 1),
                                        ImPlotProp_LineWeight,
                                        2.0f));

            // 装饰物必须在 EndPlot 前提交，确保使用当前 Plot 坐标变换和裁剪。
            // 播放头周边元素允许超出当前视野时间，裁剪保护布局外的控件。
            ImPlot::PushPlotClipRect();

            // 主画布可见时间范围以紫色填充和两条边线覆盖在波形上。
            auto session = Logic::EditorEngine::instance().getActiveSession();
            if ( session ) {
                // 活动相机缺失时与时间读取路径相同地回退 Basic2DCanvas。
                std::string activeCameraId =
                    Logic::EditorEngine::instance().getActiveCameraId();
                auto snapshot =
                    Logic::EditorEngine::instance()
                        .getSyncBuffer(activeCameraId.empty() ? "Basic2DCanvas"
                                                              : activeCameraId)
                        ->getReadingSnapshot();
                // 仅已绑定谱面的快照提供可绘制范围，过渡态不会沿用旧边界。
                if ( snapshot && snapshot->hasBeatmap ) {
                    // 填充区域 X 取主画布可见起止时间，Y 覆盖完整振幅。
                    double boxX[2] = { snapshot->visibleTimeStart,
                                       snapshot->visibleTimeEnd };
                    double boxY[2] = { 1.0, 1.0 };
                    ImVec4 boxCol  = ImVec4(0.5f, 0.0f, 1.0f, 0.15f);
                    ImPlot::PlotShaded(
                        "##MainViewBox",
                        &boxX[0],
                        &boxY[0],
                        2,
                        -1.0,
                        ImPlotSpec(ImPlotProp_FillColor, boxCol));
                    // 左右边界使用较高不透明度，明确主画布视野范围。
                    // 填充和描边取同一组时间端点，拖动预览时保持一致。
                    double edgeL[2] = { snapshot->visibleTimeStart,
                                        snapshot->visibleTimeStart };
                    double edgeR[2] = { snapshot->visibleTimeEnd,
                                        snapshot->visibleTimeEnd };
                    double edgeY[2] = { -1.0, 1.0 };
                    ImPlot::PlotLine(
                        "##BoxEdgeL",
                        edgeL,
                        edgeY,
                        2,
                        ImPlotSpec(ImPlotProp_LineColor,
                                   ImVec4(0.5f, 0.0f, 1.0f, 0.8f)));
                    ImPlot::PlotLine(
                        "##BoxEdgeR",
                        edgeR,
                        edgeY,
                        2,
                        ImPlotSpec(ImPlotProp_LineColor,
                                   ImVec4(0.5f, 0.0f, 1.0f, 0.8f)));
                }
            }

            if ( touchInput ) {
                const ImVec2 min   = ImPlot::GetPlotPos(),
                             size  = ImPlot::GetPlotSize();
                const bool blocked = ImGui::IsPopupOpen(
                    nullptr,
                    ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                // 命中仅限实际 Plot 内容，坐标轴和顶部滑块不能成为定位起点。
                // 只有原始触屏分支消费该输入，后面的经典鼠标路径必须互斥。
                // 切谱时携带实例约束，排队的旧定位不会落到新的活动谱面。
                // 这里只更改图表半窗，不触发整段波形重算或全局时间线倍率更新。
                m_touchNavigation.update(
                    { min,
                      { min.x + size.x, min.y + size.y },
                      visualTime,
                      totalTime,
                      globalVisualOffset,
                      snapshot ? snapshot->beatmapInstanceId : 0,
                      chanIdx,
                      ImGui::IsWindowHovered(
                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                          !blocked,
                      snapshot && snapshot->hasBeatmap && !blocked },
                    m_zoom);
            }
            // 默认悬浮时间回退视图起点，只有 hover/drag 时读取鼠标 Plot 坐标。
            double currentHoverVisualTime = viewStart;
            double currentHoverAudioTime  = viewStart - globalVisualOffset;
            if ( !touchInput &&
                 (ImPlot::IsPlotHovered() || s_lastActive[chanIdx]) ) {
                // ImPlot 返回 X 轴视觉时间，再扣通用偏移得到实际 Seek
                // 音频时间。
                ImPlotPoint mousePlotPos = ImPlot::GetPlotMousePos();
                currentHoverVisualTime   = mousePlotPos.x;
                currentHoverAudioTime =
                    currentHoverVisualTime - globalVisualOffset;
            }

            // 悬浮绿色竖线跟随指针；拖拽时额外预览平移后的主画布视野框。
            if ( !touchInput &&
                 (ImPlot::IsPlotHovered() || s_lastActive[chanIdx]) ) {
                // Plot 屏幕矩形用于后续鼠标坐标归一化命令。
                // 展示仍使用转换之前的时间，指针命令才需要换成纯音频偏移坐标。
                ImVec2 plotMin         = ImPlot::GetPlotPos();
                ImVec2 plotMax         = { plotMin.x + ImPlot::GetPlotSize().x,
                                           plotMin.y + ImPlot::GetPlotSize().y };
                double hoverVisualTime = currentHoverVisualTime;
                double hoverAudioTime  = currentHoverAudioTime;

                // 绿色竖线贯穿当前声道标准振幅范围。
                double hLineX[2] = { hoverVisualTime, hoverVisualTime };
                double hLineY[2] = { -1.1, 1.1 };
                ImPlot::PlotLine(
                    "##HoverLine",
                    hLineX,
                    hLineY,
                    2,
                    ImPlotSpec(ImPlotProp_LineColor, ImVec4(0, 1, 0, 0.6f)));

                if ( s_lastActive[chanIdx] ) {
                    // 拖拽预览复用最新同步快照中的主画布相对时间跨度。
                    // 这里重新取快照，而不复用函数开头版本，使覆盖框尽快跟随逻辑线程。
                    std::string activeCameraId =
                        Logic::EditorEngine::instance().getActiveCameraId();
                    auto snapshot = Logic::EditorEngine::instance()
                                        .getSyncBuffer(activeCameraId.empty()
                                                           ? "Basic2DCanvas"
                                                           : activeCameraId)
                                        ->getReadingSnapshot();
                    // 仅已绑定谱面的快照提供可绘制范围，过渡态不会沿用旧边界。
                    if ( snapshot && snapshot->hasBeatmap ) {
                        // 先计算可见区相对当前视觉时间的左右偏移。
                        // 相对跨度不依赖 Seek
                        // 目标，可在拖拽期间平移到任意悬浮时间。
                        double offsetStart =
                            snapshot->visibleTimeStart - visualTime;
                        double offsetEnd =
                            snapshot->visibleTimeEnd - visualTime;
                        // 再把相对跨度平移到当前悬浮时间形成预览框。
                        double preBoxX[2] = { hoverVisualTime + offsetStart,
                                              hoverVisualTime + offsetEnd };
                        double preBoxY[2] = { 1.0, 1.0 };
                        ImPlot::PlotShaded(
                            "##PreviewViewBox",
                            &preBoxX[0],
                            &preBoxY[0],
                            2,
                            -1.0,
                            ImPlotSpec(ImPlotProp_FillColor,
                                       ImVec4(0.5f, 0.0f, 1.0f, 0.35f)));
                    }
                }
            }

            // 恢复 Plot 裁剪后结束本声道绘制。
            ImPlot::PopPlotClipRect();
            ImPlot::EndPlot();

            // EndPlot 后从上一 ImGui Item 获取本帧 Plot 交互状态。
            ImVec2 plotMin = ImGui::GetItemRectMin();
            ImVec2 plotMax = ImGui::GetItemRectMax();
            // 活动状态保存到下一帧，维持拖拽越界时的预览连续性。
            s_lastActive[chanIdx] = !touchInput && ImGui::IsItemActive();
            // 登记的是当前 Plot 的命中区，横向意图交给浏览、纵向意图留给父页。
            // 新解释器只读原始触点，不篡改全局鼠标回放和相邻滑块的 ActiveID。
            // 两指操作不采用原生 ImPlot
            // 拖拽，以免同时移动坐标轴或提交第二次定位。
            if ( touchInput ) workspaceRegisterHorizontalDragItem();

            if ( !touchInput &&
                 (ImGui::IsItemHovered() || ImGui::IsItemActive()) ) {
                // Tooltip 显示视觉时间；Seek 命令使用扣除全局偏移后的音频时间。
                // 即使指针离开 Plot，活动 Item
                // 仍使用上一阶段取得的坐标维持连续拖拽。
                double hoverVisualTime = currentHoverVisualTime;
                double hoverAudioTime  = currentHoverAudioTime;

                // 时间格式遵循软件设置与当前同步快照的节拍信息。
                const auto timeText =
                    MMM::UI::Utils::formatCanvasTime(hoverVisualTime, snapshot);
                ImGui::SetTooltip("%s", timeText.c_str());

                if ( ImGui::IsItemActive() ) {
                    // 拖拽期间持续发布相对 Plot 鼠标位置和预览视觉时间。
                    // 宽高随命令一起发送，逻辑层可独立把局部坐标归一化到画布视野。
                    ImVec2 mousePos = ImGui::GetMousePos();
                    Interaction::dispatchCommand(Event::LogicCommandEvent(
                        Logic::CmdSetMousePosition{ "AudioWaveform",
                                                    mousePos.x - plotMin.x,
                                                    mousePos.y - plotMin.y,
                                                    plotMax.x - plotMin.x,
                                                    plotMax.y - plotMin.y,
                                                    true,
                                                    true,
                                                    hoverVisualTime }));
                }

                if ( ImGui::IsItemDeactivated() &&
                     ImGui::GetIO().MouseReleased[0] ) {
                    // 左键释放立即提交最终音频 Seek，不使用固定时长等待。
                    // Seek 与 inactive
                    // 鼠标命令保持同帧顺序，先确定播放位置再结束预览。
                    Interaction::dispatchCommand(Event::LogicCommandEvent(
                        Logic::CmdSeek{ hoverAudioTime }));
                    // 同帧发布 inactive 鼠标状态，结束主画布预览拖拽。
                    ImVec2 mousePos = ImGui::GetMousePos();
                    Interaction::dispatchCommand(Event::LogicCommandEvent(
                        Logic::CmdSetMousePosition{ "AudioWaveform",
                                                    mousePos.x - plotMin.x,
                                                    mousePos.y - plotMin.y,
                                                    plotMax.x - plotMin.x,
                                                    plotMax.y - plotMin.y,
                                                    false,
                                                    false,
                                                    -1.0 }));
                }
            }
        }
    };

    // 左右声道复用同一绘制逻辑和时间轴，各自读取对应包络数组。
    renderChannel("##WaveL",
                  m_viewMinL.data(),
                  m_maxEnvelopeL.data(),
                  TR("ui.waveform.channel_l").data());
    renderChannel("##WaveR",
                  m_viewMinR.data(),
                  m_maxEnvelopeR.data(),
                  TR("ui.waveform.channel_r").data());
}

/// @brief 从完整缓存提取当前视觉时间窗的固定数量包络点。
/// @param visualTime 当前播放头视觉时间，单位秒。
/// @param totalTime 音轨总时长，单位秒。
/// @param speed 当前播放速度；保留用于与视图更新接口一致。
/// @param waveformVisualOffset 波形相对音频时间的有效偏移，单位秒。
///
/// 输出 X 坐标保留视觉时间，只有查缓存时转换回音频时间。因此 Plot
/// 播放头、主画布 覆盖框和悬浮提示可共用视觉坐标系，而波形内容仍与实际 PCM
/// 对齐。
///
/// `speed` 当前不参与采样：播放速度改变时间推进速率，不改变某个时间点对应的 PCM
/// 桶。参数保留在接口中，以明确调用方提供的是完整播放上下文。
/// @warning UI 热路径：每帧调用；只执行固定 `m_samplePoints` 次数组读取。
void AudioWaveformView::updateEnvelopes(double visualTime, double totalTime,
                                        double speed,
                                        float  waveformVisualOffset)
{
    // 新音轨结果到达前也更新时间轴，以静音绘制空包络，不沿用上一音轨的切片。
    // 同一帧借用同一次发布的四个极值，两个声道不会取到不同请求的数据。
    const auto& bins = m_analysis->bins();
    // cache 来自相同时间桶密度，缩放只改变这里选择的桶索引。

    // 可见窗口以播放头为中心，左右各展开 m_zoom 秒。
    double viewStart = visualTime - m_zoom;
    double viewEnd   = visualTime + m_zoom;

    for ( int i = 0; i < m_samplePoints; ++i ) {
        // 固定采样点均匀覆盖整个可见时间窗。
        // 分母使用点数而非点数减一，保持历史右端开区间采样语义。
        double t = viewStart + (static_cast<double>(i) / m_samplePoints) *
                                   (viewEnd - viewStart);
        // 即使还没有包络，先更新图表时间轴保障播放头和指针定位。
        m_times[i] = t;

        // 缓存索引属于音频时间，需要扣除波形视觉偏移。
        // 播放头位置与鼠标事件继续解释为原来的视觉 X 坐标。
        double audioT = t - waveformVisualOffset;

        if ( bins.empty() || !(audioT >= 0.0 && audioT < totalTime) ) {
            // 音轨范围外用静音填充，Plot 时间轴仍保持连续。
            // 这也覆盖正视觉偏移造成的音轨前空白和末端窗口越界。
            m_viewMinL[i] = m_maxEnvelopeL[i] = m_viewMinR[i] =
                m_maxEnvelopeR[i]             = 0;
            continue;
        }

        // 秒数按缓存密度映射到最近的离散桶索引。
        // static_cast 对非负时间向下取整，与后台分析的桶起点保持一致。
        size_t p =
            static_cast<size_t>(audioT * WaveformAnalysis::POINTS_PER_SECOND);
        // 四个字段来自一个桶，不需要分别检查各声道数组长度。
        if ( p < bins.size() ) {
            // 左右声道拷贝到各自的可见切片，满足 ImPlot 连续输入要求。
            m_viewMinL[i]     = bins[p].minL;
            m_maxEnvelopeL[i] = bins[p].maxL;
            m_viewMinR[i]     = bins[p].minR;
            m_maxEnvelopeR[i] = bins[p].maxR;
        } else {
            // 舍入、时间线变化或首个包络仍未提交都可能使索引越界。
            // 回写四个零值防止复用数组时显示上一帧残余的有声包络。
            m_viewMinL[i] = m_maxEnvelopeL[i] = m_viewMinR[i] =
                m_maxEnvelopeR[i]             = 0;
        }
    }
}

}  // namespace MMM::UI
