#include "ui/imgui/tools/BpmMeasurementToolView.h"
#include "audio/AudioManager.h"
#include "canvas/TimeFormatUtils.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/project/Project.h"
#include "ui/UIManager.h"
#include "ui/utils/UIThemeUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fftw3.h>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <limits>

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

namespace MMM::UI
{
namespace
{
/// @brief 获取 FFTW 计划互斥锁，保护全局 planner 状态。
std::mutex& fftwPlanMutex()
{
    static std::mutex mutex;
    return mutex;
}

/// @brief 生成音频资源在下拉框中的显示文本。
/// @param resource 音频资源。
/// @return 适合 UI 展示的标签。
std::string makeAudioResourceLabel(const AudioResource& resource)
{
    const char* typeText =
        resource.m_type == AudioTrackType::Main ? "Main" : "Effect";
    return resource.m_id + " [" + typeText + "] - " + resource.m_path;
}

}  // namespace

/// @brief 构造 BPM 测量工具窗口。
BpmMeasurementToolView::BpmMeasurementToolView(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    m_statusText = TR("ui.tools.bpm_measure.select_track").data();
}

/// @brief 销毁窗口并等待后台分析任务和 GPU 资源释放。
BpmMeasurementToolView::~BpmMeasurementToolView()
{
    stopAnalysisWorker();

    auto context = Graphic::VKContext::get();
    if ( context ) {
        (void)context->get().getLogicalDevice().waitIdle();
    }
    m_spectrumTextures.clear();
}

/// @brief 打开窗口并选中指定项目音频轨道。
/// @param audioTrackId 项目内音频资源 ID；为空时仅打开窗口。
void BpmMeasurementToolView::openWithAudioTrack(const std::string& audioTrackId)
{
    m_isOpen = true;

    if ( audioTrackId.empty() ) {
        return;
    }

    if ( m_selectedAudioTrackId != audioTrackId ) {
        m_selectedAudioTrackId = audioTrackId;
        requestAnalyzeSelectedTrack();
    }
}

/// @brief 更新并绘制 BPM 测量工具 UI。
/// @param sourceManager 当前 UI 管理器。
/// @warning UI 热路径：每帧执行；不得在此处扫描文件系统、重新解码整段音频或创建
/// FFT 计划。
void BpmMeasurementToolView::update(UIManager* sourceManager)
{
    (void)sourceManager;
    consumePendingAnalysis();

    ImGui::SetNextWindowSize(ImVec2(980.0f, 640.0f), ImGuiCond_FirstUseEver);
    LayoutContext layoutContext(m_layoutCtx,
                                TR("ui.tools.bpm_measure.title").data(),
                                true,
                                ImGuiWindowFlags_None,
                                &m_isOpen);

    if ( ImGui::BeginTable(
             "##BpmMeasureLayout",
             2,
             ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp) ) {
        ImGui::TableSetupColumn(
            "##Analysis", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(
            "##Controls", ImGuiTableColumnFlags_WidthFixed, 280.0f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        renderAnalysisPanel();
        ImGui::TableNextColumn();
        renderControlPanel();

        ImGui::EndTable();
    }
}

/// @brief 判断频谱纹理是否需要上传。
/// @warning 渲染准备热路径：每帧查询；只读取低频变更脏位。
bool BpmMeasurementToolView::needReload()
{
    return m_texturesNeedReload;
}

/// @brief 上传后台分析生成的频谱纹理。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 上传命令池。
/// @param queue 上传队列。
/// @warning 低频资源准备路径：可能等待 GPU
/// 空闲并上传纹理，只能由音轨切换或重新分析触发。
void BpmMeasurementToolView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                            vk::Device&         logicalDevice,
                                            vk::CommandPool&    cmdPool,
                                            vk::Queue&          queue)
{
    if ( !m_texturesNeedReload ) {
        return;
    }

    (void)logicalDevice.waitIdle();
    m_spectrumTextures.clear();

    for ( const auto& chunk : m_pendingSpectrumChunks ) {
        if ( chunk.pixels.empty() || chunk.width == 0 || chunk.height == 0 ) {
            continue;
        }
        m_spectrumTextures.push_back(
            std::make_unique<Graphic::VKTexture>(chunk.pixels.data(),
                                                 chunk.width,
                                                 chunk.height,
                                                 physicalDevice,
                                                 logicalDevice,
                                                 cmdPool,
                                                 queue));
    }

    m_pendingSpectrumChunks.clear();
    m_texturesNeedReload = false;
}

/// @brief 尝试消费后台分析结果。
void BpmMeasurementToolView::consumePendingAnalysis()
{
    if ( !m_analysisFinished.load(std::memory_order_acquire) ) {
        return;
    }

    std::optional<AnalysisResult> result;
    {
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        if ( m_pendingResult ) {
            result = std::move(m_pendingResult);
            m_pendingResult.reset();
        }
    }

    if ( !result ) {
        m_analysisFinished.store(false, std::memory_order_release);
        return;
    }

    m_waveTimes                 = std::move(result->waveTimes);
    m_waveMin                   = std::move(result->waveMin);
    m_waveMax                   = std::move(result->waveMax);
    m_pendingSpectrumChunks     = std::move(result->spectrumChunks);
    m_duration                  = result->duration;
    m_spectrumSegmentsPerSecond = result->spectrumSegmentsPerSecond;
    m_spectrumSegmentCount      = result->spectrumSegmentCount;
    m_spectrumBinCount          = result->spectrumBinCount;
    m_texturesNeedReload        = true;
    m_statusText                = TR("ui.tools.bpm_measure.ready").data();
    m_analysisFinished.store(false, std::memory_order_release);
}

/// @brief 绘制右侧测量参数面板。
void BpmMeasurementToolView::renderControlPanel()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        ImGui::TextColored(Utils::UIThemeUtils::getWarningColor(),
                           "%s",
                           TR("ui.tools.no_active_session").data());
        return;
    }

    if ( m_selectedAudioTrackId.empty() ) {
        m_selectedAudioLabel = TR("ui.tools.bpm_measure.select_track").data();
    } else {
        bool foundSelected = false;
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_id == m_selectedAudioTrackId ) {
                m_selectedAudioLabel = makeAudioResourceLabel(resource);
                foundSelected        = true;
                break;
            }
        }
        if ( !foundSelected ) {
            m_selectedAudioTrackId.clear();
            m_selectedAudioLabel =
                TR("ui.tools.bpm_measure.select_track").data();
        }
    }

    ImGui::SeparatorText(TR("ui.tools.bpm_measure.audio").data());
    if ( ImGui::BeginCombo("##BpmMeasureAudioTrack",
                           m_selectedAudioLabel.c_str()) ) {
        for ( const auto& resource : project->m_audioResources ) {
            const bool  isSelected = resource.m_id == m_selectedAudioTrackId;
            std::string label      = makeAudioResourceLabel(resource);
            if ( ImGui::Selectable(label.c_str(), isSelected) ) {
                m_selectedAudioTrackId = resource.m_id;
                m_selectedAudioLabel   = label;
                requestAnalyzeSelectedTrack();
            }
            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if ( project->m_audioResources.empty() ) {
        ImGui::TextDisabled("%s", TR("ui.tools.bpm_measure.no_audio").data());
    }

    const bool hasSelection = !m_selectedAudioTrackId.empty();
    if ( !hasSelection ) {
        ImGui::BeginDisabled();
    }
    if ( ImGui::Button(TR("ui.tools.bpm_measure.reload").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        requestAnalyzeSelectedTrack();
    }
    if ( !hasSelection ) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.params").data());

    float bpm = static_cast<float>(m_bpm);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.bpm").data(),
                          &bpm,
                          0.01f,
                          1.0f,
                          999.0f,
                          "%.3f") ) {
        m_bpm               = std::clamp<double>(bpm, 1.0, 999.0);
        m_beatLengthSeconds = 60.0 / m_bpm;
    }

    constexpr double minBeatLength = 60.0 / 999.0;
    float            beatLength    = static_cast<float>(m_beatLengthSeconds);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.beat_length").data(),
                          &beatLength,
                          0.0001f,
                          static_cast<float>(minBeatLength),
                          60.0f,
                          "%.6f") ) {
        m_beatLengthSeconds =
            std::clamp<double>(beatLength, minBeatLength, 60.0);
        m_bpm = 60.0 / m_beatLengthSeconds;
    }

    float firstBeat = static_cast<float>(m_firstBeatTime);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.first_beat").data(),
                          &firstBeat,
                          0.001f,
                          0.0f,
                          static_cast<float>(std::max(0.001, m_duration)),
                          "%.6f") ) {
        m_firstBeatTime =
            std::clamp<double>(firstBeat, 0.0, std::max(0.0, m_duration));
    }

    float markerWidth = static_cast<float>(m_markerWidthMs);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.marker_width").data(),
                          &markerWidth,
                          0.1f,
                          4.0f,
                          1000.0f,
                          "%.1f") ) {
        m_markerWidthMs = std::clamp<double>(markerWidth, 4.0, 1000.0);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.view").data());

    float center = static_cast<float>(m_viewCenter);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.center").data(),
                          &center,
                          0.01f,
                          0.0f,
                          static_cast<float>(std::max(0.001, m_duration)),
                          "%.3f") ) {
        m_viewCenter =
            std::clamp<double>(center, 0.0, std::max(0.0, m_duration));
    }

    float zoom = static_cast<float>(m_zoomSeconds);
    if ( ImGui::DragFloat(TR("ui.tools.bpm_measure.zoom").data(),
                          &zoom,
                          0.05f,
                          0.1f,
                          120.0f,
                          "%.2f") ) {
        m_zoomSeconds = std::clamp<double>(zoom, 0.1, 120.0);
    }

    if ( ImGui::Button(TR("ui.tools.bpm_measure.center_first").data(),
                       ImVec2(-1.0f, 0.0f)) ) {
        m_viewCenter =
            std::clamp<double>(m_firstBeatTime, 0.0, std::max(0.0, m_duration));
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.status").data());
    if ( m_analysisRunning.load(std::memory_order_relaxed) ) {
        const float progress =
            m_analysisProgress.load(std::memory_order_relaxed);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
        ImGui::Text("%s %.0f%%",
                    TR("ui.tools.bpm_measure.analyzing").data(),
                    progress * 100.0f);
    } else {
        ImGui::TextWrapped("%s", m_statusText.c_str());
    }

    if ( m_duration > 0.0 ) {
        ImGui::TextDisabled(
            "%s %.3fs", TR("ui.tools.bpm_measure.duration").data(), m_duration);
    }
}

/// @brief 绘制左侧波形和频谱面板。
void BpmMeasurementToolView::renderAnalysisPanel()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if ( avail.x <= 1.0f || avail.y <= 1.0f ) {
        return;
    }

    ImGui::Text("%s", TR("ui.tools.bpm_measure.waveform").data());
    const float textH      = ImGui::GetTextLineHeightWithSpacing();
    const float spacingY   = ImGui::GetStyle().ItemSpacing.y;
    const float waveHeight = std::max(120.0f, (avail.y - textH * 2.0f) * 0.42f);
    const float specHeight =
        std::max(160.0f, avail.y - waveHeight - textH * 2.0f - spacingY * 2.0f);

    renderWaveformPlot(ImVec2(avail.x, waveHeight));

    ImGui::Text("%s", TR("ui.tools.bpm_measure.spectrum").data());
    renderSpectrumImage(ImVec2(avail.x, specHeight));
}

/// @brief 绘制波形图。
/// @param size 绘制区域尺寸。
void BpmMeasurementToolView::renderWaveformPlot(const ImVec2& size)
{
    const double viewStart = std::max(0.0, m_viewCenter - m_zoomSeconds);
    const double viewEnd   = std::min(std::max(m_duration, viewStart + 0.001),
                                      m_viewCenter + m_zoomSeconds);

    if ( ImPlot::BeginPlot("##BpmMeasureWaveform",
                           size,
                           ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                               ImPlotFlags_NoMouseText) ) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1,
                          nullptr,
                          ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(
            ImAxis_X1, viewStart, viewEnd, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.05, 1.05, ImGuiCond_Always);

        if ( !m_waveTimes.empty() ) {
            const int count = static_cast<int>(std::min<size_t>(
                m_waveTimes.size(),
                static_cast<size_t>(std::numeric_limits<int>::max())));
            ImPlot::PlotShaded("##WaveEnvelope",
                               m_waveTimes.data(),
                               m_waveMin.data(),
                               m_waveMax.data(),
                               count,
                               ImPlotSpec(ImPlotProp_FillAlpha, 0.55f));
        }

        ImPlot::PushPlotClipRect();
        ImVec2 plotMin = ImPlot::GetPlotPos();
        ImVec2 plotMax = ImVec2(plotMin.x + ImPlot::GetPlotSize().x,
                                plotMin.y + ImPlot::GetPlotSize().y);
        drawBeatMarkers(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        handleTimelineNavigation(plotMin, plotMax, viewStart, viewEnd);
        ImPlot::PopPlotClipRect();

        if ( ImPlot::IsPlotHovered() ) {
            ImPlotPoint  mousePos = ImPlot::GetPlotMousePos();
            const double hoverTime =
                std::clamp<double>(mousePos.x, 0.0, std::max(0.0, m_duration));
            ImGui::SetTooltip("%s",
                              Canvas::formatCanvasTime(hoverTime).c_str());
            if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
                m_firstBeatTime = hoverTime;
            }
        }

        ImPlot::EndPlot();
    }
}

/// @brief 绘制频谱图。
/// @param size 绘制区域尺寸。
void BpmMeasurementToolView::renderSpectrumImage(const ImVec2& size)
{
    const double viewStart  = std::max(0.0, m_viewCenter - m_zoomSeconds);
    const double viewEnd    = std::min(std::max(m_duration, viewStart + 0.001),
                                       m_viewCenter + m_zoomSeconds);
    const double viewRange  = std::max(0.001, viewEnd - viewStart);
    const double pixelStart = viewStart * m_spectrumSegmentsPerSecond;
    const double pixelEnd   = viewEnd * m_spectrumSegmentsPerSecond;
    const double pixelWidth = std::max(1.0, pixelEnd - pixelStart);

    ImVec2      imageMin = ImGui::GetCursorScreenPos();
    ImVec2      imageMax = ImVec2(imageMin.x + size.x, imageMin.y + size.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(imageMin, imageMax, IM_COL32(12, 14, 18, 255));

    ImGui::BeginGroup();
    if ( !m_spectrumTextures.empty() ) {
        if ( pixelStart < 0.0 ) {
            const float emptyW =
                static_cast<float>((0.0 - pixelStart) / pixelWidth * size.x);
            ImGui::Dummy(ImVec2(emptyW, size.y));
            ImGui::SameLine(0.0f, 0.0f);
        }

        for ( size_t i = 0; i < m_spectrumTextures.size(); ++i ) {
            const auto&  texture = m_spectrumTextures[i];
            const double texStart =
                static_cast<double>(i * static_cast<size_t>(MAX_TEXTURE_W));
            const double texEnd = texStart + texture->width();

            if ( texEnd < pixelStart || texStart > pixelEnd ) {
                continue;
            }

            const double intersectStart = std::max(texStart, pixelStart);
            const double intersectEnd   = std::min(texEnd, pixelEnd);
            const float  uv0x = static_cast<float>((intersectStart - texStart) /
                                                   texture->width());
            const float  uv1x = static_cast<float>((intersectEnd - texStart) /
                                                   texture->width());
            const float  screenW = static_cast<float>(
                (intersectEnd - intersectStart) / pixelWidth * size.x);

            ImGui::Image(texture->getImTextureID(),
                         ImVec2(screenW, size.y),
                         ImVec2(uv0x, 0.0f),
                         ImVec2(uv1x, 1.0f));
            ImGui::SameLine(0.0f, 0.0f);
        }
    } else {
        ImGui::Dummy(size);
    }
    ImGui::EndGroup();

    imageMin = ImGui::GetItemRectMin();
    imageMax = ImGui::GetItemRectMax();
    drawBeatMarkers(*drawList, imageMin, imageMax, viewStart, viewEnd);

    ImGui::SetCursorScreenPos(imageMin);
    ImGui::InvisibleButton(
        "##BpmMeasureSpectrumHover",
        ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y));
    handleTimelineNavigation(imageMin, imageMax, viewStart, viewEnd);
    if ( ImGui::IsItemHovered() ) {
        const ImVec2 mousePos = ImGui::GetMousePos();
        const double relX     = std::clamp<double>(
            (mousePos.x - imageMin.x) / std::max(1.0f, imageMax.x - imageMin.x),
            0.0,
            1.0);
        const double hoverTime = std::clamp<double>(
            viewStart + relX * viewRange, 0.0, std::max(0.0, m_duration));
        ImGui::SetTooltip("%s", Canvas::formatCanvasTime(hoverTime).c_str());
        if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
            m_firstBeatTime = hoverTime;
        }
    }
}

/// @brief 在指定矩形区域叠加拍线和黄色拍框。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
void BpmMeasurementToolView::drawBeatMarkers(ImDrawList&   drawList,
                                             const ImVec2& rectMin,
                                             const ImVec2& rectMax,
                                             double        viewStart,
                                             double        viewEnd) const
{
    if ( m_beatLengthSeconds <= 0.0 || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x ) {
        return;
    }

    const double markerSeconds =
        std::clamp<double>(m_markerWidthMs / 1000.0, 0.004, 1.0);
    const double halfMarkerSeconds = markerSeconds * 0.5;
    const double firstIndex =
        std::ceil((viewStart - m_firstBeatTime - halfMarkerSeconds) /
                  m_beatLengthSeconds);
    const double lastIndex = std::floor(
        (viewEnd - m_firstBeatTime + halfMarkerSeconds) / m_beatLengthSeconds);
    const double width = rectMax.x - rectMin.x;

    auto timeToX = [&](double time) {
        return rectMin.x + static_cast<float>((time - viewStart) /
                                              (viewEnd - viewStart) * width);
    };

    const ImU32 white  = IM_COL32(255, 255, 255, 255);
    const ImU32 fill   = IM_COL32(255, 220, 60, 70);
    const ImU32 border = IM_COL32(255, 220, 60, 235);

    drawList.PushClipRect(rectMin, rectMax, true);
    for ( double index = firstIndex; index <= lastIndex; index += 1.0 ) {
        const double beatTime = m_firstBeatTime + index * m_beatLengthSeconds;
        const float  lineX    = timeToX(beatTime);
        const float  halfBoxWidth =
            std::max(3.0f,
                     static_cast<float>(halfMarkerSeconds /
                                        (viewEnd - viewStart) * width));
        if ( lineX < rectMin.x - halfBoxWidth ||
             lineX > rectMax.x + halfBoxWidth ) {
            continue;
        }

        drawList.AddLine(
            ImVec2(lineX, rectMin.y), ImVec2(lineX, rectMax.y), white, 1.0f);

        const float boxX1 = lineX - halfBoxWidth;
        const float boxX2 = lineX + halfBoxWidth;
        if ( boxX2 <= boxX1 ) {
            continue;
        }

        drawList.AddRectFilled(
            ImVec2(boxX1, rectMin.y), ImVec2(boxX2, rectMax.y), fill);
        drawList.AddRect(ImVec2(boxX1, rectMin.y),
                         ImVec2(boxX2, rectMax.y),
                         border,
                         0.0f,
                         0,
                         1.5f);
    }
    drawList.PopClipRect();
}

/// @brief 处理分析视图的滚轮缩放和鼠标拖动平移。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算。
void BpmMeasurementToolView::handleTimelineNavigation(const ImVec2& rectMin,
                                                      const ImVec2& rectMax,
                                                      double        viewStart,
                                                      double        viewEnd)
{
    if ( m_duration <= 0.0 || rectMax.x <= rectMin.x || viewEnd <= viewStart ) {
        m_isTimelinePanning = false;
        return;
    }

    ImGuiIO&   io    = ImGui::GetIO();
    const bool hover = ImGui::IsMouseHoveringRect(rectMin, rectMax);
    if ( hover || m_isTimelinePanning ) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    const double rectWidth   = std::max(1.0f, rectMax.x - rectMin.x);
    const double viewRange   = viewEnd - viewStart;
    auto         clampCenter = [&](double center) {
        return std::clamp(center, 0.0, std::max(0.0, m_duration));
    };

    if ( hover && io.MouseWheel != 0.0f ) {
        const double mouseRatio = std::clamp<double>(
            (io.MousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
        const double anchorTime = viewStart + mouseRatio * viewRange;
        const double zoomFactor = std::pow(0.86, io.MouseWheel);
        const double maxZoom    = std::max(0.1, std::max(m_duration, 120.0));
        const double nextZoom =
            std::clamp(m_zoomSeconds * zoomFactor, 0.01, maxZoom);
        const double nextViewStart = anchorTime - mouseRatio * nextZoom * 2.0;
        m_zoomSeconds              = nextZoom;
        m_viewCenter               = clampCenter(nextViewStart + nextZoom);
    }

    if ( hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
        m_isTimelinePanning = true;
    }

    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        m_isTimelinePanning = false;
    }

    if ( m_isTimelinePanning &&
         ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ) {
        const double timeDelta =
            -static_cast<double>(io.MouseDelta.x) / rectWidth * viewRange;
        m_viewCenter = clampCenter(m_viewCenter + timeDelta);
    }
}

/// @brief 请求重新分析当前选择的音频轨道。
void BpmMeasurementToolView::requestAnalyzeSelectedTrack()
{
    stopAnalysisWorker();
    clearAnalysisData();

    auto path = selectedAudioAbsolutePath();
    if ( !path ) {
        m_statusText = TR("ui.tools.bpm_measure.select_track").data();
        return;
    }

    std::error_code ec;
    if ( !std::filesystem::exists(*path, ec) || ec ) {
        m_statusText = TR("ui.tools.bpm_measure.file_missing").data();
        return;
    }

    const std::string pathString = Config::pathToUtf8(*path);
    auto              track =
        Audio::AudioManager::instance().loadTrackForAnalysis(pathString);
    if ( !track ) {
        m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
        return;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    m_duration = sampleRate > 0.0
                     ? static_cast<double>(track->num_frames()) / sampleRate
                     : 0.0;
    m_viewCenter =
        std::clamp<double>(m_firstBeatTime, 0.0, std::max(0.0, m_duration));
    m_statusText = TR("ui.tools.bpm_measure.analyzing").data();
    m_analysisProgress.store(0.0f, std::memory_order_relaxed);
    m_analysisFinished.store(false, std::memory_order_release);
    m_analysisRunning.store(true, std::memory_order_relaxed);

    m_analysisThread = std::make_unique<std::jthread>(
        [this, track = std::move(track), duration = m_duration](
            std::stop_token stopToken) {
            analyzeTrack(stopToken, track, duration);
        });
}

/// @brief 停止并等待当前后台分析任务。
/// @warning
/// 不可中断低频路径：会等待后台线程退出，只能在关闭窗口或重新选择音轨时执行。
void BpmMeasurementToolView::stopAnalysisWorker()
{
    if ( m_analysisThread && m_analysisThread->joinable() ) {
        m_analysisThread->request_stop();
        m_analysisThread->join();
    }
    m_analysisThread.reset();
    m_analysisRunning.store(false, std::memory_order_relaxed);
}

/// @brief 清理当前分析缓存和频谱 GPU 资源。
/// @warning 低频资源路径：可能等待 GPU 空闲，严禁放入每帧绘制路径。
void BpmMeasurementToolView::clearAnalysisData()
{
    if ( !m_spectrumTextures.empty() ) {
        auto context = Graphic::VKContext::get();
        if ( context ) {
            (void)context->get().getLogicalDevice().waitIdle();
        }
    }

    m_waveTimes.clear();
    m_waveMin.clear();
    m_waveMax.clear();
    m_pendingSpectrumChunks.clear();
    m_spectrumTextures.clear();
    m_texturesNeedReload   = false;
    m_spectrumSegmentCount = 0;
    m_spectrumBinCount     = 0;
    m_analysisProgress     = 0.0f;
    m_analysisFinished     = false;
}

/// @brief 后台分析线程执行体。
/// @param stopToken 线程停止令牌。
/// @param track 待分析音频轨道，后台线程持有共享所有权。
/// @param duration 音频时长，单位为秒。
/// @warning 后台耗时路径：执行完整音频解码和 FFT；不在 UI/渲染热路径中运行。
void BpmMeasurementToolView::analyzeTrack(
    std::stop_token stopToken, std::shared_ptr<ice::AudioTrack> track,
    double duration)
{
    if ( !track ) {
        m_analysisRunning.store(false, std::memory_order_relaxed);
        return;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const size_t totalFrames = track->num_frames();
    if ( duration <= 0.0 || sampleRate <= 0.0 || totalFrames == 0 ) {
        m_analysisRunning.store(false, std::memory_order_relaxed);
        return;
    }

    const int wavePointCount =
        std::max(2, static_cast<int>(duration * m_wavePointsPerSecond) + 1);
    const int spectrumSegmentCount = std::max(
        1, static_cast<int>(duration * m_spectrumSegmentsPerSecond) + 1);
    const int    spectrumBinCount = 128;
    const int    fftSize          = 2048;
    const size_t hopSize          = std::max<size_t>(
        1, static_cast<size_t>(sampleRate / m_spectrumSegmentsPerSecond));
    const uint16_t channelCount = ice::ICEConfig::internal_format.channels;
    const int      totalWork    = wavePointCount + spectrumSegmentCount;
    int            finishedWork = 0;

    AnalysisResult result;
    result.duration                  = duration;
    result.spectrumSegmentsPerSecond = m_spectrumSegmentsPerSecond;
    result.spectrumSegmentCount      = spectrumSegmentCount;
    result.spectrumBinCount          = spectrumBinCount;
    result.waveTimes.resize(wavePointCount);
    result.waveMin.assign(wavePointCount, 0.0);
    result.waveMax.assign(wavePointCount, 0.0);

    auto updateProgress = [&]() {
        if ( totalWork <= 0 ) {
            return;
        }
        m_analysisProgress.store(std::clamp(static_cast<float>(finishedWork) /
                                                static_cast<float>(totalWork),
                                            0.0f,
                                            1.0f),
                                 std::memory_order_relaxed);
    };

    ice::AudioBuffer waveBuffer;
    for ( int point = 0; point < wavePointCount; ++point ) {
        if ( stopToken.stop_requested() ) {
            m_analysisRunning.store(false, std::memory_order_relaxed);
            return;
        }

        const double timeStart =
            static_cast<double>(point) / m_wavePointsPerSecond;
        const double timeEnd =
            static_cast<double>(point + 1) / m_wavePointsPerSecond;
        const size_t startFrame =
            std::min(totalFrames, static_cast<size_t>(timeStart * sampleRate));
        const size_t endFrame =
            std::min(totalFrames, static_cast<size_t>(timeEnd * sampleRate));
        const size_t frameCount =
            endFrame > startFrame ? endFrame - startFrame : 0;

        result.waveTimes[point] = timeStart;
        if ( frameCount > 0 ) {
            waveBuffer.resize(ice::ICEConfig::internal_format, frameCount);
            waveBuffer.clear();
            const size_t decoded =
                track->read(waveBuffer, startFrame, frameCount);
            const size_t usableFrames = std::min(decoded, frameCount);
            double       minValue     = 0.0;
            double       maxValue     = 0.0;
            float**      data         = waveBuffer.raw_ptrs();

            for ( size_t frame = 0; frame < usableFrames; ++frame ) {
                double mixed = 0.0;
                for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                    mixed += data[ch][frame];
                }
                mixed /= std::max<uint16_t>(1, channelCount);
                minValue = std::min(minValue, mixed);
                maxValue = std::max(maxValue, mixed);
            }
            result.waveMin[point] = minValue;
            result.waveMax[point] = maxValue;
        }

        ++finishedWork;
        updateProgress();
    }

    std::vector<float> heatmap(
        static_cast<size_t>(spectrumBinCount) * spectrumSegmentCount, -100.0f);
    std::vector<double> window(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        window[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fftSize - 1)));
    }

    double* fftInput =
        static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
    fftw_complex* fftOutput = static_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * (fftSize / 2 + 1)));
    if ( !fftInput || !fftOutput ) {
        if ( fftInput ) {
            fftw_free(fftInput);
        }
        if ( fftOutput ) {
            fftw_free(fftOutput);
        }
        m_analysisRunning.store(false, std::memory_order_relaxed);
        return;
    }

    fftw_plan fftPlan = nullptr;
    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftPlan =
            fftw_plan_dft_r2c_1d(fftSize, fftInput, fftOutput, FFTW_ESTIMATE);
    }

    if ( !fftPlan ) {
        fftw_free(fftInput);
        fftw_free(fftOutput);
        m_analysisRunning.store(false, std::memory_order_relaxed);
        return;
    }

    ice::AudioBuffer spectrumBuffer;
    spectrumBuffer.resize(ice::ICEConfig::internal_format, fftSize);

    const double fmin         = 20.0;
    const double fmax         = m_maxFrequency;
    const double freqRange    = std::max(1.0, fmax - fmin);
    const double logBias      = m_logFrequencyBias;
    const double expKMinus1   = std::exp(logBias) - 1.0;
    auto         binFrequency = [&](double progress) {
        if ( std::abs(logBias) < 1e-4 ) {
            return fmin + freqRange * progress;
        }
        return fmin +
               freqRange * (std::exp(logBias * progress) - 1.0) / expKMinus1;
    };

    for ( int segment = 0; segment < spectrumSegmentCount; ++segment ) {
        if ( stopToken.stop_requested() ) {
            {
                std::lock_guard<std::mutex> lock(fftwPlanMutex());
                fftw_destroy_plan(fftPlan);
            }
            fftw_free(fftInput);
            fftw_free(fftOutput);
            m_analysisRunning.store(false, std::memory_order_relaxed);
            return;
        }

        const size_t startFrame =
            std::min(totalFrames, static_cast<size_t>(segment) * hopSize);
        const size_t frameCount =
            startFrame < totalFrames
                ? std::min<size_t>(fftSize, totalFrames - startFrame)
                : 0;

        spectrumBuffer.clear();
        const size_t decoded =
            frameCount > 0 ? track->read(spectrumBuffer, startFrame, frameCount)
                           : 0;
        float** data = spectrumBuffer.raw_ptrs();
        for ( int i = 0; i < fftSize; ++i ) {
            double mixed = 0.0;
            if ( static_cast<size_t>(i) < decoded ) {
                for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                    mixed += data[ch][i];
                }
                mixed /= std::max<uint16_t>(1, channelCount);
            }
            fftInput[i] = mixed * window[i];
        }

        fftw_execute(fftPlan);

        for ( int bin = 0; bin < spectrumBinCount; ++bin ) {
            const double freqStart =
                binFrequency(static_cast<double>(bin) / spectrumBinCount);
            const double freqEnd =
                binFrequency(static_cast<double>(bin + 1) / spectrumBinCount);
            const int binStart =
                std::clamp(static_cast<int>(freqStart * fftSize / sampleRate),
                           0,
                           fftSize / 2);
            const int binEnd =
                std::clamp(static_cast<int>(freqEnd * fftSize / sampleRate),
                           binStart,
                           fftSize / 2);

            double maxMagnitude = 0.0;
            for ( int i = binStart; i <= binEnd; ++i ) {
                const double magSq = fftOutput[i][0] * fftOutput[i][0] +
                                     fftOutput[i][1] * fftOutput[i][1];
                maxMagnitude       = std::max(maxMagnitude, magSq);
            }
            const double db =
                maxMagnitude > 1e-9
                    ? 20.0 * std::log10(std::sqrt(maxMagnitude) / fftSize)
                    : -100.0;
            heatmap[static_cast<size_t>(bin) * spectrumSegmentCount + segment] =
                static_cast<float>(std::clamp(db, -100.0, 0.0));
        }

        ++finishedWork;
        updateProgress();
    }

    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftw_destroy_plan(fftPlan);
    }
    fftw_free(fftInput);
    fftw_free(fftOutput);

    const int chunkCount =
        (spectrumSegmentCount + static_cast<int>(MAX_TEXTURE_W) - 1) /
        static_cast<int>(MAX_TEXTURE_W);
    result.spectrumChunks.reserve(static_cast<size_t>(chunkCount));

    for ( int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex ) {
        const uint32_t chunkStart =
            static_cast<uint32_t>(chunkIndex) * MAX_TEXTURE_W;
        const uint32_t chunkWidth = std::min<uint32_t>(
            MAX_TEXTURE_W,
            static_cast<uint32_t>(spectrumSegmentCount) - chunkStart);

        TextureChunkData chunk;
        chunk.width  = chunkWidth;
        chunk.height = static_cast<uint32_t>(spectrumBinCount);
        chunk.pixels.resize(static_cast<size_t>(chunk.width) * chunk.height *
                            4);

        for ( uint32_t y = 0; y < chunk.height; ++y ) {
            const int bin = spectrumBinCount - 1 - static_cast<int>(y);
            for ( uint32_t x = 0; x < chunk.width; ++x ) {
                const uint32_t globalX = chunkStart + x;
                const float    value =
                    heatmap[static_cast<size_t>(bin) * spectrumSegmentCount +
                            globalX];
                const auto   color = spectrumColorFromDb(value);
                const size_t offset =
                    (static_cast<size_t>(y) * chunk.width + x) * 4;
                std::memcpy(&chunk.pixels[offset], color.data(), 4);
            }
        }

        result.spectrumChunks.push_back(std::move(chunk));
    }

    m_analysisProgress.store(1.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        m_pendingResult = std::move(result);
    }
    m_analysisRunning.store(false, std::memory_order_relaxed);
    m_analysisFinished.store(true, std::memory_order_release);
}

/// @brief 查找当前选中音频轨道的绝对路径。
/// @return 成功时返回绝对路径，否则返回空。
std::optional<std::filesystem::path>
BpmMeasurementToolView::selectedAudioAbsolutePath() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) {
        return std::nullopt;
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id == m_selectedAudioTrackId ) {
            return project->m_projectRoot / Config::utf8ToPath(resource.m_path);
        }
    }

    return std::nullopt;
}

/// @brief 将 dB 值映射为热力图 RGBA 颜色。
/// @param db 频谱能量，单位为 dB。
/// @return RGBA 颜色。
std::array<unsigned char, 4> BpmMeasurementToolView::spectrumColorFromDb(
    double db) const
{
    const double scaleMin = -80.0;
    const double scaleMax = -8.0;
    const double t =
        std::clamp((db - scaleMin) / (scaleMax - scaleMin), 0.0, 1.0);
    const double r = std::clamp(3.0 * t, 0.0, 1.0);
    const double g = std::clamp(3.0 * t - 1.0, 0.0, 1.0);
    const double b = std::clamp(3.0 * t - 2.0, 0.0, 1.0);

    return { static_cast<unsigned char>(r * 255.0),
             static_cast<unsigned char>(g * 255.0),
             static_cast<unsigned char>(b * 255.0),
             255 };
}

}  // namespace MMM::UI
