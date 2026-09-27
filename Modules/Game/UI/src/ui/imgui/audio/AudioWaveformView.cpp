#include "ui/imgui/audio/AudioWaveformView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "implot.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ice/config/config.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace MMM::UI
{
/// @brief 波形离线 EQ 单次处理的最大帧数。
///
/// 使用一秒内部采样率作为有界工作块，限制离线重算时临时缓冲容量。
constexpr std::size_t WAVEFORM_EQ_BLOCK_FRAMES{ 44100U };

/// @brief 把已有 AudioBuffer 暴露给离线 EffectNode 的轻量源节点。
///
/// 节点不拥有输入缓冲，只在 `GraphicEqualizer::process`
/// 同步调用期间复制当前块。 调用方必须保证 `setBuffer`
/// 指向的对象在一次处理结束前保持有效。
///
/// 输出帧数取输入与目标容量的较小值，避免越界；通道数按输入缓冲遍历。该节点只
/// 用于设置窗口的显式波形重算，不连接实时音频回调。
///
/// 节点不会转换采样率、声道布局或样本格式。调用方应使用
/// `ICEConfig::internal_format`
/// 准备输入和输出，使这里保持为单一职责的数据桥接层。
class BufferSourceNode : public ice::IAudioNode
{
public:
    /// @brief 设置下一次离线处理读取的非拥有缓冲指针。
    /// @param buffer 输入 PCM 缓冲，可为空以禁用复制。
    void setBuffer(const ice::AudioBuffer* buffer) { m_buffer = buffer; }

    /// @brief 把当前输入块复制到 EffectNode 请求的目标缓冲。
    /// @param buffer 由效果链提供的输出缓冲。
    /// @warning 仅用于低频离线计算；不会分配内存，但执行逐通道 PCM 复制。
    void process(ice::AudioBuffer& buffer) override
    {
        // 空输入表示当前没有可提供的 PCM，保持目标缓冲现状。
        if ( !m_buffer ) return;
        // 每个输入通道复制相同帧范围，目标格式由调用方预先准备。
        for ( uint16_t ch = 0; ch < m_buffer->num_channels(); ++ch ) {
            // 输入不足或目标较小时只复制共同有效的帧数。
            size_t frames =
                std::min(buffer.num_frames(), m_buffer->num_frames());
            // raw_ptrs 指向连续 float PCM，按实际帧数复制字节。
            std::memcpy(buffer.raw_ptrs()[ch],
                        m_buffer->raw_ptrs()[ch],
                        frames * sizeof(float));
        }
    }

private:
    /// @brief 当前离线输入缓冲的非拥有观察指针。
    /// @warning 只在同步 `process` 调用前设置，不得跨异步任务保留。
    const ice::AudioBuffer* m_buffer{ nullptr };
};

/// @brief 波形预览 EQ 共用的缓冲源节点。
///
/// 当前波形视图处理在 UI 低频重算路径串行执行，因此节点可复用；若未来并行计算，
/// 必须改为每任务独立实例，不能并发改写输入指针。
static std::shared_ptr<BufferSourceNode> g_bufferSource =
    std::make_shared<BufferSourceNode>();

/// @brief 构造波形视图并预分配固定采样点数组。
/// @param name 视图显示名称。
///
/// PCM 工作缓冲在构造时创建，显示数组按 `m_samplePoints`
/// 固定长度初始化，避免每帧 更新包络时调整容器容量。
///
/// 完整包络缓存要等用户请求同步效果后才按音轨长度分配，因此构造阶段不会因工程中
/// 的长音频产生额外内存占用。
AudioWaveformView::AudioWaveformView(const std::string& name) : IUIView(name)
{
    // processBuffer 保存 EQ 后 PCM，rawBuffer 保存音轨原始块。
    m_processBuffer = std::make_unique<ice::AudioBuffer>();
    m_rawBuffer     = std::make_unique<ice::AudioBuffer>();
    // 时间轴与左右声道最小/最大包络必须保持完全相同长度。
    m_times.resize(m_samplePoints);
    m_viewMinL.resize(m_samplePoints);
    m_maxEnvelopeL.resize(m_samplePoints);
    m_viewMinR.resize(m_samplePoints);
    m_maxEnvelopeR.resize(m_samplePoints);
}

/// @brief 销毁波形视图及其离线 PCM 工作缓冲。
AudioWaveformView::~AudioWaveformView() = default;

/// @brief 更新并绘制双声道波形窗口。
/// @param sourceManager 非拥有 UI 管理器，用于查询工程切换状态。
///
/// 每帧读取音频播放状态与逻辑层同步快照，构造自适应换行控制条，并绘制左右声道
/// 包络。拖拽 Plot 时持续发布预览鼠标位置，释放后发布最终 Seek。
///
/// 波形缓存只在用户点击同步效果按钮后通过 `fullRecalculate` 重建。普通 update
/// 不 解码音频或扫描完整 PCM，仅从预计算缓存抽取当前可见包络。
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
    LayoutContext layoutContext(
        m_layoutCtx, windowTitle, true, ImGuiWindowFlags_None, &m_isOpen);

    if ( sourceManager && sourceManager->isProjectTransitionInProgress() ) {
        // 工程切换时不读取可能失效的音轨或 Session，只显示占位提示。
        Utils::renderProjectTransitionPlaceholder();
        return;
    }

    // AudioManager 是当前 BGM 音轨、播放时间与速度的运行态真值。
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();

    if ( !track ) {
        // 无主音轨时保持窗口可见并给出加载提示。
        ImGui::Text("%s", TR("ui.audio_manager.initial_hint").data());
        return;
    }

    if ( m_isCalculating ) {
        // 离线计算期间使用中心模态阻止重复触发重算或波形交互。
        ::MMM::UI::FeedbackOpenPopup("ProcessingWaveform");
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin("ProcessingWaveform") ) {
            // 文本只描述进行中状态，模态生命周期由作用域 helper 管理样式。
            ImGui::Text("%s", TR("ui.waveform.processing.text").data());
            ImGui::EndPopup();
        }
        return;
    }

    // 通用与波形专用视觉偏移分别读取，避免混淆音频 Seek 时间。
    const auto& visualConfig = Config::AppConfig::instance().getVisualConfig();
    float       globalVisualOffset = visualConfig.getEffectiveVisualOffset();
    float       waveformVisualOffset =
        visualConfig.getWaveformEffectiveVisualOffset();
    // AudioManager 值是快照缺失时的安全回退。
    double audioTime  = audioManager.getCurrentTime();
    double visualTime = audioTime + globalVisualOffset;
    double totalTime  = audioManager.getTotalTime();
    double speed      = audioManager.getPlaybackSpeed();

    // 优先使用逻辑层平滑时间，使预览拖拽和主画布视野保持实时同步。
    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    // 空相机 ID 使用 Basic2DCanvas 的稳定同步缓冲。
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
            visualTime = snapshot->resolveCurrentTimeAt(now);
            audioTime  = snapshot->resolvePlaybackTimeAt(now);
        }
    }

    // 控制条宽度由当前主题字体、padding 与 item spacing 计算。
    ImGuiStyle& style  = ImGui::GetStyle();
    float       frameH = ImGui::GetFrameHeight();
    /// 计算“标签 + 内间距 + 固定滑块”组合所需宽度。
    auto calcSliderWidth = [&](float sliderW, const char* label) {
        return sliderW + style.ItemInnerSpacing.x +
               ImGui::CalcTextSize(label).x;
    };
    /// 计算按钮文本和左右 FramePadding 所需宽度。
    auto calcButtonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    /// 在相邻控制组之间绘制竖向分隔线。
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
    topContainer.setPadding(0, 0, 0, 0)
        .setSpacing(toLayoutPixels(
            std::max(4.0f * dpiScale, style.ItemSpacing.y * 0.5f)));
    // deque 保证新增换行时已交给 Clay 的行引用保持稳定。
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
            totalW += 1.0f + spacing;  // Sep + spacing
        }
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
    // 同步效果按钮显式触发完整 PCM 与当前 EQ 的离线重算。
    pushGroup("SyncEffectsBtn",
              calcButtonWidth(TR("ui.waveform.sync_effects").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  if ( ::MMM::UI::FeedbackButton(
                           TR("ui.waveform.sync_effects").data()) ) {
                      fullRecalculate();
                  }
              });

    // 控制组登记完成后统一布局并推进父窗口游标。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = topContainer.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    // 每帧轻量同步 EQ 参数；包络抽样只读取已经存在的缓存。
    syncEQ();
    updateEnvelopes(visualTime, totalTime, speed, waveformVisualOffset);

    // 剩余高度在左右声道两个纵向 Plot 之间平均分配。
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
        double viewStart = visualTime - m_zoom;
        double viewEnd   = visualTime + m_zoom;

        // 上一帧活动状态让拖拽越过 Plot 悬浮边界时仍保持预览。
        static bool s_lastActive[2] = { false, false };
        // 固定 ID 映射左右声道索引，两个 Plot 分别维护拖拽状态。
        int chanIdx = (std::string(id) == "##WaveL") ? 0 : 1;

        if ( ImPlot::BeginPlot(id,
                               ImVec2(-1, channelH),
                               ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                                   ImPlotFlags_NoMouseText) ) {

            // X 轴使用当前视觉时间窗，Y 轴固定到标准 PCM 振幅范围附近。
            ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
            ImPlot::SetupAxis(
                ImAxis_Y1,
                label,
                ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(
                ImAxis_X1, viewStart, viewEnd, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.1, 1.1, ImGuiCond_Always);

            // 最小与最大包络之间的填充表示该像素时间桶中的振幅范围。
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

            // 默认悬浮时间回退视图起点，只有 hover/drag 时读取鼠标 Plot 坐标。
            double currentHoverVisualTime = viewStart;
            double currentHoverAudioTime  = viewStart - globalVisualOffset;
            if ( ImPlot::IsPlotHovered() || s_lastActive[chanIdx] ) {
                // ImPlot 返回 X 轴视觉时间，再扣通用偏移得到实际 Seek
                // 音频时间。
                ImPlotPoint mousePlotPos = ImPlot::GetPlotMousePos();
                currentHoverVisualTime   = mousePlotPos.x;
                currentHoverAudioTime =
                    currentHoverVisualTime - globalVisualOffset;
            }

            // 悬浮绿色竖线跟随指针；拖拽时额外预览平移后的主画布视野框。
            if ( ImPlot::IsPlotHovered() || s_lastActive[chanIdx] ) {
                // Plot 屏幕矩形用于后续鼠标坐标归一化命令。
                ImVec2 plotMin = ImPlot::GetPlotPos();
                ImVec2 plotMax = { plotMin.x + ImPlot::GetPlotSize().x,
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
            s_lastActive[chanIdx] = ImGui::IsItemActive();

            if ( ImGui::IsItemHovered() || ImGui::IsItemActive() ) {
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
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
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
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdSeek{ hoverAudioTime }));
                    // 同帧发布 inactive 鼠标状态，结束主画布预览拖拽。
                    ImVec2 mousePos = ImGui::GetMousePos();
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
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

/// @brief 把主音轨 EQ 拓扑与参数同步到波形离线预览链。
///
/// EQ 关闭时释放预览节点；频段数量变化时重建 GraphicEqualizer，普通增益与 Q
/// 变化 只更新已有节点参数。该函数不读取 PCM，也不执行完整波形重算。
///
/// 频率决定滤波器拓扑，增益和 Q 只影响现有频段参数。这个差异使普通滑块调整可以
/// 避免重复分配节点，同时保证频段增删后不会沿用尺寸不匹配的效果链。
/// @warning UI 热路径：波形窗口每帧调用；仅在频段拓扑改变时允许分配节点。
void AudioWaveformView::syncEQ()
{
    // AudioManager 是主音轨 EQ 开关、频率和参数的运行态真值。
    auto& audioManager = Audio::AudioManager::instance();
    if ( !audioManager.isMainTrackEQEnabled() ) {
        // 关闭 EQ 后波形重算直接复制原始 PCM。
        m_previewEQ.reset();
        return;
    }

    if ( !m_previewEQ || m_previewEQ->get_band_count() !=
                             audioManager.getMainTrackEQBandCount() ) {
        // 只有节点缺失或频段数量改变才重建拓扑。
        // 频率值按管理器索引顺序提取，索引随后也作为参数同步的稳定频段标识。
        std::vector<double> freqs;
        size_t              count = audioManager.getMainTrackEQBandCount();
        for ( size_t i = 0; i < count; ++i ) {
            // 频率列表按 AudioManager 频段顺序构造。
            freqs.push_back(audioManager.getMainTrackEQBandFrequency(i));
        }
        // GraphicEqualizer 共享 BufferSourceNode 作为同步离线输入。
        // 共享所有权让效果链在当前视图存活期间持有源节点，但源节点不持有 PCM。
        m_previewEQ = std::make_shared<ice::GraphicEqualizer>(freqs);
        // prepare 使用引擎内部格式和固定最大工作块。
        m_previewEQ->prepare(ice::ICEConfig::internal_format,
                             WAVEFORM_EQ_BLOCK_FRAMES);
        m_previewEQ->set_inputnode(g_bufferSource);
    }

    // 增益和 Q 可在不重建频段拓扑的情况下逐项更新。
    // 频段数量已经在上方对齐，循环内可安全查询相同索引的管理器参数。
    for ( size_t i = 0; i < m_previewEQ->get_band_count(); ++i ) {
        m_previewEQ->set_band_gain_db(i,
                                      audioManager.getMainTrackEQBandGain(i));
        m_previewEQ->set_band_q_factor(i, audioManager.getMainTrackEQBandQ(i));
    }
}

/// @brief 在显式重算时准备完整 PCM，避免把流式缺页静音写入波形缓存。
///
/// 函数把整个音轨按 `m_cachePointsPerSecond` 切成时间桶，每桶读取有界 PCM
/// 块，应用 当前预览 EQ
/// 后记录左右声道最小/最大值。输出缓存供每帧可见窗口重采样。
///
/// 每个桶保存极值而非单点采样，可以在缩小视野时保留短促峰值。缓存密度与 UI
/// 展示点数相互独立，放大时可能重复读取同一桶，但不会重新访问音轨。
///
/// 流式播放实例的页缓存面向实时回调，不能保证任意绝对帧立即可读。这里改用分析
/// 实例，确保离线顺序扫描得到真实 PCM，同时不扰动正在播放的游标和预取状态。
/// @warning 低频离线操作，可能等待完整解码；不能作为逐帧读取入口。
void AudioWaveformView::fullRecalculate()
{
    // 无主音轨时保持现有缓存，不进入计算模态状态。
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();
    if ( !track ) return;

    if ( track->cachingStrategy() == ice::CachingStrategy::STREAMING ) {
        // 分析音轨与正在播放的流式音轨分别保活，不驱逐预读页或改变播放游标。
        // 分析实例允许顺序读取完整 PCM，而不把实时缓存缺页当成静音。
        // 路径来自已加载音轨，分析实例应解析同一资源而不修改工程关联。
        track = audioManager.loadTrackForAnalysis(track->path());
        if ( !track ) return;
    }

    // 进入计算状态后 update 会显示模态，阻止用户重复发起重算。
    m_isCalculating   = true;
    double totalTime  = audioManager.getTotalTime();
    double sampleRate = ice::ICEConfig::internal_format.samplerate;
    // 每秒固定缓存点数加一，覆盖音轨末端边界。
    // 末端额外点同时吸收浮点乘法向下取整，避免接近总时长时立即越界。
    size_t totalPoints =
        static_cast<size_t>(totalTime * m_cachePointsPerSecond) + 1;

    // 四个缓存长度完全一致，并用静音初始化未覆盖桶。
    // assign 会替换上一轮效果结果，确保 EQ 配置变化后没有残留旧极值。
    m_cachedMinL.assign(totalPoints, 0.0f);
    m_cachedMaxL.assign(totalPoints, 0.0f);
    m_cachedMinR.assign(totalPoints, 0.0f);
    m_cachedMaxR.assign(totalPoints, 0.0f);

    // 读取 PCM 前同步 EQ 拓扑与参数，保证整轮缓存使用同一配置。
    syncEQ();

    // 两个工作缓冲预分配为最大块，循环内只调整有效帧数。
    // 固定容量把临时内存与音轨总时长解耦，长音频只增长极值缓存。
    const size_t chunkSize = WAVEFORM_EQ_BLOCK_FRAMES;
    m_rawBuffer->resize(ice::ICEConfig::internal_format, chunkSize);
    m_processBuffer->resize(ice::ICEConfig::internal_format, chunkSize);

    for ( size_t p = 0; p < totalPoints; ++p ) {
        // 每个缓存索引映射到固定时长桶的起止秒数和帧区间。
        double startTime  = p / m_cachePointsPerSecond;
        double endTime    = (p + 1) / m_cachePointsPerSecond;
        size_t startFrame = static_cast<size_t>(startTime * sampleRate);
        size_t endFrame   = static_cast<size_t>(endTime * sampleRate);
        // 非递增帧区间没有可读 PCM，保持初始化的静音包络。
        size_t frames = (endFrame > startFrame) ? (endFrame - startFrame) : 0;
        if ( frames == 0 ) continue;

        // 单桶异常大于工作块时截断，保证缓冲访问有界。
        // 当前默认密度下桶远小于一秒；此保护允许未来参数改变仍不越过容量。
        if ( frames > chunkSize ) frames = chunkSize;

        // 分析音轨按绝对起始帧读取当前桶。
        // read 的有效帧数与后续 resize、复制及极值循环必须保持一致。
        track->read(*m_rawBuffer, startFrame, frames);

        if ( m_previewEQ ) {
            // 共享源指向当前原始块，EQ 输出写入 processBuffer。
            // 输入指针仅在紧随其后的同步 process 调用中有效，不会逃逸到下一桶。
            g_bufferSource->setBuffer(m_rawBuffer.get());
            m_processBuffer->resize(ice::ICEConfig::internal_format, frames);
            m_previewEQ->process(*m_processBuffer);
        } else {
            // EQ 关闭时逐通道复制原始数据，后续包络路径保持一致。
            // 保留独立输出缓冲使极值统计无需分叉，也不会改写解码器提供的数据。
            m_processBuffer->resize(ice::ICEConfig::internal_format, frames);
            for ( uint16_t ch = 0; ch < m_rawBuffer->num_channels(); ++ch )
                std::memcpy(m_processBuffer->raw_ptrs()[ch],
                            m_rawBuffer->raw_ptrs()[ch],
                            frames * sizeof(float));
        }

        // 单声道输入把左声道样本同时用于右声道包络。
        // 双声道以上只展示前两个声道；波形窗口的 UI 契约固定为左右两行。
        float** data     = m_processBuffer->raw_ptrs();
        int     channels = m_processBuffer->num_channels();
        float   minL = 0, maxL = 0, minR = 0, maxR = 0;
        // 单次遍历同时计算左右声道的最小值和最大值。
        // 极值从零初始化，使全正或全负桶仍包含零基线并保持填充区域可见。
        for ( size_t f = 0; f < frames; ++f ) {
            float sL = data[0][f];
            float sR = (channels > 1) ? data[1][f] : sL;
            if ( sL < minL ) minL = sL;
            if ( sL > maxL ) maxL = sL;
            if ( sR < minR ) minR = sR;
            if ( sR > maxR ) maxR = sR;
        }
        // 桶统计写入相同索引的四个缓存数组。
        m_cachedMinL[p] = minL;
        m_cachedMaxL[p] = maxL;
        m_cachedMinR[p] = minR;
        m_cachedMaxR[p] = maxR;
    }

    // 所有桶处理完毕后退出模态计算状态。
    m_isCalculating = false;
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
    // 尚未显式重算时没有包络可展示，保留当前视图数组。
    if ( m_cachedMinL.empty() ) return;

    // 可见窗口以播放头为中心，左右各展开 m_zoom 秒。
    double viewStart = visualTime - m_zoom;
    double viewEnd   = visualTime + m_zoom;

    for ( int i = 0; i < m_samplePoints; ++i ) {
        // 固定采样点均匀覆盖整个可见时间窗。
        // 分母使用点数而非点数减一，保持历史右端开区间采样语义。
        double t   = viewStart + (static_cast<double>(i) / m_samplePoints) *
                                     (viewEnd - viewStart);
        m_times[i] = t;

        // 缓存索引属于音频时间，需要扣除波形视觉偏移。
        double audioT = t - waveformVisualOffset;

        if ( audioT < 0 || audioT >= totalTime ) {
            // 音轨范围外用静音填充，Plot 时间轴仍保持连续。
            // 这也覆盖正视觉偏移造成的音轨前空白和末端窗口越界。
            m_viewMinL[i] = m_maxEnvelopeL[i] = m_viewMinR[i] =
                m_maxEnvelopeR[i]             = 0;
            continue;
        }

        // 秒数按缓存密度映射到最近的离散桶索引。
        // static_cast 对非负时间向下取整，对应 fullRecalculate 的桶起点定义。
        size_t p = static_cast<size_t>(audioT * m_cachePointsPerSecond);
        if ( p < m_cachedMinL.size() ) {
            // 四个包络数组在重算时保证相同长度，可使用同一索引。
            m_viewMinL[i]     = m_cachedMinL[p];
            m_maxEnvelopeL[i] = m_cachedMaxL[p];
            m_viewMinR[i]     = m_cachedMinR[p];
            m_maxEnvelopeR[i] = m_cachedMaxR[p];
        } else {
            // 舍入或总时长变化造成越界时防御性填充静音。
            m_viewMinL[i] = m_maxEnvelopeL[i] = m_viewMinR[i] =
                m_maxEnvelopeR[i]             = 0;
        }
    }
}

}  // namespace MMM::UI
