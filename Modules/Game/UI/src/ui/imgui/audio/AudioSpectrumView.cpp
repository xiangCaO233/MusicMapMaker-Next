#include "ui/imgui/audio/AudioSpectrumView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fftw3.h>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <latch>
#include <mutex>
#include <system_error>
#include <utility>

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

namespace MMM::UI
{

/// @brief 把离线 AudioBuffer 适配为 GraphicEqualizer 的输入节点。
///
/// 节点不拥有 PCM，只在紧随 setBuffer 的同步 process 调用中复制当前块。它不参与
/// 实时音频图，也不执行格式转换或采样率转换。
class BufferSourceNodeProxy : public ice::IAudioNode
{
public:
    /// @brief 设置下一次同步处理读取的非拥有输入缓冲。
    /// @param buffer 输入 PCM，可为空以禁用复制。
    void setBuffer(const ice::AudioBuffer* buffer) { m_buffer = buffer; }

    /// @brief 把当前输入缓冲复制到效果链提供的输出缓冲。
    /// @param buffer 目标音频缓冲。
    /// @warning 低频离线计算路径；不分配内存，但逐通道复制 PCM。
    void process(ice::AudioBuffer& buffer) override
    {
        // 空输入保持目标缓冲不变。
        if ( !m_buffer ) return;
        // 逐通道处理，格式由调用方提前保证一致。
        for ( uint16_t ch = 0; ch < m_buffer->num_channels(); ++ch ) {
            // 只复制输入与目标共同有效的帧数。
            size_t frames =
                std::min(buffer.num_frames(), m_buffer->num_frames());
            // raw_ptrs 暴露连续 float PCM，按帧数换算字节。
            std::memcpy(buffer.raw_ptrs()[ch],
                        m_buffer->raw_ptrs()[ch],
                        frames * sizeof(float));
        }
    }

private:
    /// @brief 当前离线输入块的非拥有观察指针。
    /// @warning 不得跨异步任务或下一次缓冲生命周期保留使用。
    const ice::AudioBuffer* m_buffer{ nullptr };
};

/// @brief 构造频谱视图并按当前配置初始化分析细节和工作缓冲。
/// @param name 窗口可见名称。
///
/// 活动与待提交细节参数初始一致；后台计算成功后才把 pending 参数切换为活动值。
/// PCM 工作缓冲在构造阶段分配对象，但具体容量由计算任务按块准备。
AudioSpectrumView::AudioSpectrumView(const std::string& name)
    : IUIView(name), IRenderableView(name)
{
    // 细节等级来自视觉配置，并通过统一 profile 展开为时间与频率分辨率。
    m_spectrumDetailLevel =
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel;
    const auto profile = Config::spectrumDetailProfile(m_spectrumDetailLevel);
    // 活动参数描述当前缓存与 GPU 纹理的真实分辨率。
    m_cacheSegmentsPerSecond = profile.segmentsPerSecond;
    m_numFrequencyBins       = profile.frequencyBins;
    // pending 参数由下一次重算任务写入，完成前不影响当前纹理解释。
    m_pendingSpectrumDetailLevel    = m_spectrumDetailLevel;
    m_pendingCacheSegmentsPerSecond = m_cacheSegmentsPerSecond;
    m_pendingNumFrequencyBins       = m_numFrequencyBins;
    // 两个缓冲分别保存原始 PCM 和应用预览 EQ 后的 PCM。
    m_processBuffer = std::make_unique<ice::AudioBuffer>();
    m_rawBuffer     = std::make_unique<ice::AudioBuffer>();
}

/// @brief 停止后台频谱计算并等待 GPU 停止使用视图资源。
///
/// 后台任务捕获 this 并写入成员缓存，析构前必须请求停止并 join future。Vulkan
/// 纹理 与离屏资源可能仍被提交的命令引用，销毁成员前等待逻辑设备空闲。
/// @warning 析构低频阻塞路径：可能等待计算任务和 Vulkan device
/// idle，不得逐帧调用。
AudioSpectrumView::~AudioSpectrumView()
{
    if ( m_calcFuture.valid() ) {
        // 协作式停止让后台循环在块或阶段边界尽快退出。
        m_calcStopSource.request_stop();
        // future 等待保证任务不再访问本实例成员。
        m_calcFuture.wait();
        // 清空 future 状态，避免后续误判仍有任务。
        m_calcFuture = std::future<void>{};
    }

    // VKContext 可能已在应用关闭顺序中释放，只有存在时才等待设备。
    auto context = Graphic::VKContext::get();
    if ( context ) {
        // waitIdle 保证描述符、纹理和离屏附件不再被 GPU 使用。
        (void)context->get().getLogicalDevice().waitIdle();
    }
}

/// @brief 更新频谱窗口、工具栏、离屏几何与时间轴交互覆盖层。
/// @param sourceManager 非拥有 UI 管理器，用于查询项目切换状态。
///
/// 普通帧读取已完成的频谱缓存和分块纹理，按当前视觉时间窗生成少量
/// Quad；后台重算 在显式参数变化、配置细节变化或同步效果请求时启动。
///
/// UI 线程通过原子进度和完成标志观察任务。只有完成后才切换 pending 分辨率参数，
/// 准备整轨纹理分块并逐帧上传，避免渲染线程读到尺寸不匹配的半成品。
///
/// 频谱表面由 Vulkan 离屏绘制，ImGui Image 展示描述符；声道标题、悬浮提示和拖拽
/// 命令作为 ImGui 覆盖层绘制，不进入频谱纹理。
/// @warning UI
/// 热路径：窗口可见时每帧调用；不得等待后台任务、完整解码或设备空闲。
void AudioSpectrumView::update(UIManager* sourceManager)
{
    // 首次打开采用可读默认尺寸，后续尊重停靠和用户调整。
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

    // 可见标题可变化，`###` 后缀固定 ImGui 窗口身份。
    std::string windowTitle = m_name + "###AudioSpectrumViewGlobal";
    // LayoutContext 负责窗口 Begin/End 与关闭状态。
    LayoutContext layoutContext(
        m_layoutCtx, windowTitle, true, ImGuiWindowFlags_None, &m_isOpen);

    if ( sourceManager && sourceManager->isProjectTransitionInProgress() ) {
        // 工程切换时不读取可能失效的主音轨或 Session。
        Utils::renderProjectTransitionPlaceholder();
        return;
    }

    // AudioManager 是当前主音轨与播放时间的运行态真值。
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();

    if ( !track ) {
        // 无主音轨时保留窗口并显示加载提示。
        ImGui::Text("%s", TR("ui.audio_manager.initial_hint").data());
        return;
    }

    // 配置细节改变且当前无任务时，启动新分辨率重算。
    const auto configuredDetail =
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel;
    if ( configuredDetail != m_spectrumDetailLevel &&
         !m_isCalculating.load(std::memory_order_relaxed) ) {
        startAsyncRecalculate();
    }

    // 计算状态每帧请求同一固定 ID 模态保持打开。
    if ( m_isCalculating.load() ) {
        ::MMM::UI::FeedbackOpenPopup("###SpectrumCalcModal");
    }

    {
        // 进度模态使用独立样式作用域和当前 DPI。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin((TR("ui.spectrum.calc_modal.title").toString() +
                               "###SpectrumCalcModal")
                                  .c_str()) ) {
            // 进度是后台任务发布的零到一原子快照。
            float progress = m_calcProgress.load();
            ImGui::Text("%s", TR("ui.spectrum.calc_modal.text").data());
            ImGui::Spacing();
            ImGui::ProgressBar(progress, ImVec2(400, 0));
            ImGui::Text("%.0f%%", progress * 100.0f);

            if ( m_calcFinished.load() ) {
                // 完成标志消费后先退出计算态，再切换全部分辨率参数。
                m_calcFinished.store(false);
                m_isCalculating.store(false);
                m_spectrumDetailLevel = m_pendingSpectrumDetailLevel;
                // 三个 active 参数必须在同一帧整体切换。
                m_cacheSegmentsPerSecond = m_pendingCacheSegmentsPerSecond;
                m_numFrequencyBins       = m_pendingNumFrequencyBins;
                // 新缓存需要重新创建纹理并从第零块开始增量上传。
                m_textureReloadStarted = false;
                m_nextChunkUploadIndex = 0;
                prepareFullGlobalTextures();
                // 数据切换完成后关闭进度模态。
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();

            // 仍在计算时提前返回，不渲染可能即将被替换的频谱资源。
            if ( m_isCalculating.load() ) return;
        }
    }

    // 通用视觉偏移控制时间轴，频谱专用偏移只移动频谱内容。
    const auto& visualConfig = Config::AppConfig::instance().getVisualConfig();
    float       globalVisualOffset = visualConfig.getEffectiveVisualOffset();
    float       spectrumVisualOffset =
        visualConfig.getSpectrumEffectiveVisualOffset();
    // AudioManager 时间作为逻辑快照缺失时的回退。
    double audioTime  = audioManager.getCurrentTime();
    double visualTime = audioTime + globalVisualOffset;
    double totalTime  = audioManager.getTotalTime();

    // 默认跟随逻辑层平滑视觉时间，播放时继续使用快照做亚帧补偿。
    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    auto snapshot = Logic::EditorEngine::instance()
                        .getSyncBuffer(activeCameraId.empty() ? "Basic2DCanvas"
                                                              : activeCameraId)
                        ->getReadingSnapshot();
    if ( snapshot ) {
        // 拖拽快照直接使用记录值，避免外推越过当前指针。
        visualTime = snapshot->currentTime;
        audioTime  = snapshot->playbackTime;
        if ( !snapshot->isPreviewDragging ) {
            // 正常播放使用 steady_clock 当前秒数外推到本帧时间。
            const double now =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            visualTime = snapshot->resolveCurrentTimeAt(now);
            audioTime  = snapshot->resolvePlaybackTimeAt(now);
        }
    }
    // 拖动期间优先使用频谱本地中心，避免主画布预览已更新而频谱仍等待逻辑快照。
    if ( m_seekDragOwnerChannel >= 0 ) {
        // 只有获得拖拽所有权的声道可以覆盖本帧视觉中心。
        visualTime = m_seekDragViewCenter;
    }

    // 工具栏宽度全部基于当前主题字体和 FramePadding 测量。
    ImGuiStyle& style  = ImGui::GetStyle();
    float       frameH = ImGui::GetFrameHeight();
    /// 计算滑块数值文本与最小轨道共同需要的宽度。
    auto calcSliderItemWidth = [&](float minWidth, const char* widestValue) {
        return std::max(
            minWidth,
            ImGui::CalcTextSize(widestValue).x + style.FramePadding.x * 2.0f);
    };
    /// 计算“标签 + 间距 + 滑块”控制组总宽度。
    auto calcSliderGroupWidth = [&](float sliderW, const char* label) {
        return sliderW + style.ItemSpacing.x + ImGui::CalcTextSize(label).x;
    };
    /// 计算按钮本地化文本与左右 FramePadding 的宽度。
    auto calcButtonWidth = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    };
    /// 在同一工具栏行的相邻控制组之间绘制竖向分隔线。
    auto drawSep = [&](Clay_BoundingBox r, bool) {
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(r.x, r.y + 2.0f),
            ImVec2(r.x, r.y + r.height - 2.0f),
            ImGui::GetColorU32(ImGuiCol_Separator));
    };

    // 顶部 VBox 容纳一个或多个根据窗口宽度自动换行的控制行。
    CLayVBox    topContainer;
    const float toolbarDpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    /// 把非负浮点尺寸向上取整到 Clay 使用的 uint16 像素。
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    // 行 padding 和组间距分别取 DPI 基线与主题值的较大者。
    const float rowPadding =
        std::ceil(std::max(4.0f * toolbarDpiScale, style.FramePadding.x));
    const float spacing =
        std::ceil(std::max(8.0f * toolbarDpiScale, style.ItemSpacing.x));
    topContainer.setPadding(0, 0, 0, 0)
        .setSpacing(toLayoutPixels(
            std::max(4.0f * toolbarDpiScale, style.ItemSpacing.y * 0.5f)));
    // deque 保证追加换行时已交给 Clay 的行引用仍保持稳定。
    std::deque<CLayHBox> rows;
    CLayHBox*            currentRow = nullptr;
    float                currentW   = 0.0f;
    float                availW     = ImGui::GetContentRegionAvail().x;

    // 三个滑块按最宽格式文本预留轨道宽度。
    const float zoomSliderW    = calcSliderItemWidth(100.0f, "10.0000s");
    const float maxFreqSliderW = calcSliderItemWidth(120.0f, "24000.0000 Hz");
    const float logBiasSliderW = calcSliderItemWidth(120.0f, "20.0000");

    /// 追加固定尺寸控制组，当前行不足时创建新行。
    /// @param id Clay 稳定元素 ID。
    /// @param w 组宽度。
    /// @param h 组高度。
    /// @param drawCb 在 Clay 矩形中提交 ImGui 控件的回调。
    auto pushGroup = [&](const std::string& id, float w, float h, auto drawCb) {
        // 新行第一组没有分隔线，已有行需计入分隔与间距。
        bool  addSep = false;
        float totalW = w;
        if ( currentRow ) {
            totalW += 1.0f + spacing;  // Sep + spacing
        }
        if ( !currentRow || currentW + totalW > availW ) {
            // 当前行无法容纳时从 deque 取得一条新行。
            rows.emplace_back();
            currentRow = &rows.back();
            currentRow
                ->setPadding(toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding),
                             toLayoutPixels(rowPadding))
                .setSpacing(toLayoutPixels(spacing));
            // 根 VBox 保存新行引用并允许横向增长。
            topContainer.addLayout(
                ("Row_" + std::to_string(rows.size())).c_str(),
                *currentRow,
                Sizing::Grow(),
                Sizing::Fit());
            currentW = rowPadding * 2.0f;
        } else {
            // 同行追加需要先绘制分隔线。
            addSep = true;
        }

        if ( addSep ) {
            // 分隔线采用一像素固定宽度和当前组高度。
            currentRow->addElement(
                id + "_Sep", Sizing::Fixed(1.0f), Sizing::Fixed(h), drawSep);
            currentW += 1.0f + spacing;
        }
        // 目标组使用测量后的固定尺寸，避免内部 ImGui 控件被压缩。
        currentRow->addElement(id, Sizing::Fixed(w), Sizing::Fixed(h), drawCb);
        currentW += w + spacing;
    };

    // 缩放滑块控制播放头左右各自显示的时间跨度。
    pushGroup("ZoomSlider",
              calcSliderGroupWidth(zoomSliderW, TR("ui.waveform.zoom").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  // 标签和滑块在同一个 Clay 组中横向排列。
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  ImGui::AlignTextToFramePadding();
                  ImGui::Text("%s", TR("ui.waveform.zoom").data());
                  ImGui::SameLine();
                  ImGui::SetNextItemWidth(zoomSliderW);
                  ::MMM::UI::FeedbackSliderFloat(
                      "##zoom", &m_zoom, 0.1f, 10.0f, "%.4fs");
              });
    // 最大频率改变 FFT 频率映射，释放滑块后启动一次重算。
    pushGroup(
        "MaxFreqSlider",
        calcSliderGroupWidth(maxFreqSliderW, TR("ui.spectrum.max_freq").data()),
        frameH,
        [&](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y });
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", TR("ui.spectrum.max_freq").data());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(maxFreqSliderW);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##max_freq", &m_maxFreq, 2000.0f, 24000.0f, "%.4f Hz") ) {
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 拖动中只更新数值显示，结束编辑时才执行昂贵分析。
                    startAsyncRecalculate();
                }
            }
        });
    // 对数偏置调整低频到高频的纵向分配，同样在释放后重算。
    pushGroup(
        "LogBiasSlider",
        calcSliderGroupWidth(logBiasSliderW, TR("ui.spectrum.log_bias").data()),
        frameH,
        [&](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y });
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", TR("ui.spectrum.log_bias").data());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(logBiasSliderW);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##log_bias", &m_logBias, 0.01f, 20.0f, "%.4f") ) {
                if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                    // 合并连续滑块帧，只提交最终偏置配置。
                    startAsyncRecalculate();
                }
            }
        });
    // 同步效果按钮使用当前主音轨 EQ 显式重建完整频谱。
    pushGroup("SyncEffectsBtn",
              calcButtonWidth(TR("ui.spectrum.sync_effects").data()),
              frameH,
              [&](Clay_BoundingBox r, bool) {
                  ImGui::SetCursorScreenPos({ r.x, r.y });
                  if ( ::MMM::UI::FeedbackButton(
                           TR("ui.spectrum.sync_effects").data()) ) {
                      // start helper 会处理已有任务的停止与替换。
                      startAsyncRecalculate();
                  }
              });

    // 控制组登记完毕后统一布局并推进窗口游标。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = topContainer.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    // 每帧轻量同步预览 EQ 参数；完整 FFT 只由显式重算任务执行。
    syncEQ();

    // 频谱主体由离屏 Vulkan 绘制，ImGui 在其上叠加交互层。
    ImVec2 surfacePos = ImGui::GetCursorScreenPos();
    ImVec2 avail      = ImGui::GetContentRegionAvail();
    // 无有效绘制面积时不创建离屏目标或几何。
    if ( avail.x <= 1.0f || avail.y <= 1.0f ) return;

    // 两个声道各由一行标题和一块等高 Plot 组成。
    float textH    = ImGui::GetTextLineHeightWithSpacing();
    float plotH    = std::max(1.0f, (avail.y - 2.0f * textH) * 0.5f);
    float surfaceH = textH * 2.0f + plotH * 2.0f;
    if ( surfaceH > avail.y ) {
        // 浮点累计超过可用高度时重新反算 Plot 高度。
        surfaceH = avail.y;
        plotH    = std::max(1.0f, (surfaceH - 2.0f * textH) * 0.5f);
    }

    // 当前可见窗口以视觉播放头为中心，左右各展开 m_zoom 秒。
    double viewStart = visualTime - m_zoom;
    double viewEnd   = visualTime + m_zoom;

    // 离屏目标逻辑尺寸与 framebuffer DPI 缩放分别传入渲染器。
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    setTargetSize(static_cast<uint32_t>(std::max(1.0f, avail.x)),
                  static_cast<uint32_t>(std::max(1.0f, surfaceH)),
                  framebufferScale.x,
                  framebufferScale.y);

    // 每帧重建当前可见区 Quad 与 DrawCmd，容器容量可复用。
    m_vertices.clear();
    m_indices.clear();
    m_spectrumDrawCmds.clear();
    // 左声道从第一标题下方开始生成分块纹理几何。
    buildChannelGeometry(m_texturesL,
                         textH,
                         avail.x,
                         plotH,
                         viewStart,
                         viewEnd,
                         spectrumVisualOffset);
    // 右声道位于第二标题下方，使用相同时间窗和偏移。
    buildChannelGeometry(m_texturesR,
                         textH + plotH + textH,
                         avail.x,
                         plotH,
                         viewStart,
                         viewEnd,
                         spectrumVisualOffset);

    // 离屏目标已创建时以 ImGui Image 显示，否则用 Dummy 保持布局尺寸。
    vk::DescriptorSet surfaceTexture = getDescriptorSet();
    if ( surfaceTexture != VK_NULL_HANDLE ) {
        ImGui::Image(reinterpret_cast<ImTextureID>(
                         static_cast<VkDescriptorSet>(surfaceTexture)),
                     ImVec2(avail.x, surfaceH));
    } else {
        // 初始化或重建中的空描述符不应折叠后续交互区域。
        ImGui::Dummy(ImVec2(avail.x, surfaceH));
    }

    // 声道标题直接绘制到窗口 DrawList，不写入离屏纹理。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText(surfacePos,
                      ImGui::GetColorU32(ImGuiCol_Text),
                      TR("ui.spectrum.channel_l").data());
    drawList->AddText(ImVec2(surfacePos.x, surfacePos.y + textH + plotH),
                      ImGui::GetColorU32(ImGuiCol_Text),
                      TR("ui.spectrum.channel_r").data());

    // 计算两块 Plot 的屏幕矩形，供 InvisibleButton 和覆盖绘制使用。
    ImVec2 leftMin = ImVec2(surfacePos.x, surfacePos.y + textH);
    ImVec2 leftMax = ImVec2(surfacePos.x + avail.x, leftMin.y + plotH);
    ImVec2 rightMin =
        ImVec2(surfacePos.x, surfacePos.y + textH + plotH + textH);
    ImVec2 rightMax = ImVec2(surfacePos.x + avail.x, rightMin.y + plotH);

    // 左右声道分别维护交互 ID，但共享拖拽所有权状态。
    renderChannelInteractionOverlay("##SeekL",
                                    0,
                                    leftMin,
                                    leftMax,
                                    viewStart,
                                    viewEnd,
                                    globalVisualOffset,
                                    totalTime,
                                    visualTime,
                                    snapshot);
    renderChannelInteractionOverlay("##SeekR",
                                    1,
                                    rightMin,
                                    rightMax,
                                    viewStart,
                                    viewEnd,
                                    globalVisualOffset,
                                    totalTime,
                                    visualTime,
                                    snapshot);

    // 主表面之后恢复 ImGui 游标，避免后续控件覆盖频谱。
    ImGui::SetCursorScreenPos(ImVec2(surfacePos.x, surfacePos.y + surfaceH));
}

/// @brief 追加一个引用单块频谱纹理的矩形和对应 DrawCmd。
/// @param x Quad 左边界离屏逻辑坐标。
/// @param y Quad 上边界离屏逻辑坐标。
/// @param w Quad 宽度。
/// @param h Quad 高度。
/// @param uv0X 纹理可见片段左侧 U 坐标。
/// @param uv1X 纹理可见片段右侧 U 坐标。
/// @param texture 非拥有纹理指针，必须覆盖本帧命令录制。
///
/// 每个 Quad 追加四个白色顶点和两个顺时针三角形。DrawCmd 保存本 Quad 的索引偏移
/// 与纹理，渲染阶段据此切换描述符并绘制六个索引。
/// @warning 渲染热路径：当前可见纹理块调用；只追加预留容器，不得创建 GPU 资源。
void AudioSpectrumView::addSpectrumQuad(float x, float y, float w, float h,
                                        float uv0X, float uv1X,
                                        Graphic::VKTexture* texture)
{
    // 空纹理或非正面积不会产生有效绘制命令。
    if ( !texture || w <= 0.0f || h <= 0.0f ) return;

    // 保存追加前偏移，使本 Quad 的索引和 DrawCmd 指向正确范围。
    const uint32_t baseIndex   = static_cast<uint32_t>(m_vertices.size());
    const uint32_t indexOffset = static_cast<uint32_t>(m_indices.size());

    // 顶点顺序为左上、右上、左下、右下，Y 纹理坐标覆盖完整高度。
    m_vertices.push_back(
        { { x, y, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv0X, 0.0f } });
    m_vertices.push_back(
        { { x + w, y, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv1X, 0.0f } });
    m_vertices.push_back(
        { { x, y + h, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv0X, 1.0f } });
    m_vertices.push_back(
        { { x + w, y + h, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { uv1X, 1.0f } });

    // 两个三角形共享右上与左下顶点。
    m_indices.push_back(baseIndex + 0U);
    m_indices.push_back(baseIndex + 1U);
    m_indices.push_back(baseIndex + 2U);
    m_indices.push_back(baseIndex + 1U);
    m_indices.push_back(baseIndex + 3U);
    m_indices.push_back(baseIndex + 2U);

    // 每块纹理单独形成 DrawCmd，索引数固定为六。
    m_spectrumDrawCmds.push_back({ texture, 6U, indexOffset });
}

/// @brief 为一个声道当前可见时间窗建立纹理分块 Quad。
/// @param textures 按全局时间顺序排列的频谱纹理块。
/// @param plotY 声道 Plot 在离屏目标内的顶部坐标。
/// @param plotW Plot 宽度。
/// @param plotH Plot 高度。
/// @param viewStart 当前视觉时间窗起点。
/// @param viewEnd 当前视觉时间窗终点。
/// @param spectrumVisualOffset 频谱相对音频时间的有效偏移。
///
/// 全局缓存每个横向像素代表一个时间段，纹理按 MAX_TEXTURE_W
/// 切块。函数只为与当前 时间窗相交的块生成 Quad，并用局部 U 坐标裁出交集部分。
/// @warning 渲染热路径：每声道每帧调用；不得复制纹理或遍历 PCM 缓存。
void AudioSpectrumView::buildChannelGeometry(
    const std::vector<std::unique_ptr<Graphic::VKTexture>>& textures,
    float plotY, float plotW, float plotH, double viewStart, double viewEnd,
    float spectrumVisualOffset)
{
    // 无纹理或无有效绘制区域时直接返回。
    if ( textures.empty() || plotW <= 0.0f || plotH <= 0.0f ) return;

    // FFT 窗口中心相对读取起点延迟半个窗口，需要从显示时间中扣除。
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const double fftOffset =
        sampleRate > 0.0 ? (2048.0 / 2.0) / sampleRate : 0.0;
    // 视觉时间先扣频谱专用偏移和 FFT 中心偏移，得到缓存音频时间。
    const double audioViewStart = viewStart - spectrumVisualOffset - fftOffset;
    const double audioViewEnd   = viewEnd - spectrumVisualOffset - fftOffset;
    // 秒数乘缓存段密度映射到全局横向像素坐标。
    const double pixelStart = audioViewStart * m_cacheSegmentsPerSecond;
    const double pixelEnd   = audioViewEnd * m_cacheSegmentsPerSecond;
    const double pixelWidth = pixelEnd - pixelStart;
    // 反向或退化时间窗不生成几何。
    if ( pixelWidth <= 0.0 ) return;

    for ( std::size_t i = 0; i < textures.size(); ++i ) {
        // 纹理数组槽位与全局固定宽度块索引一一对应。
        auto* texture = textures[i].get();
        if ( !texture ) continue;

        // 最后一块可小于 MAX_TEXTURE_W，结束位置使用真实纹理宽度。
        const double texGlobalStart = static_cast<double>(i * MAX_TEXTURE_W);
        const double texGlobalEnd   = texGlobalStart + texture->width();
        if ( texGlobalEnd < pixelStart || texGlobalStart > pixelEnd ) {
            // 完全位于视窗外的块无需生成顶点。
            continue;
        }

        // 交集边界同时用于屏幕位置和纹理 U 坐标。
        const double intersectStart = std::max(texGlobalStart, pixelStart);
        const double intersectEnd   = std::min(texGlobalEnd, pixelEnd);
        // 仅接触边界没有可见宽度。
        if ( intersectEnd <= intersectStart ) continue;

        // U 坐标相对当前纹理块真实宽度归一化。
        const float uv0X = static_cast<float>(
            (intersectStart - texGlobalStart) / texture->width());
        const float uv1X = static_cast<float>((intersectEnd - texGlobalStart) /
                                              texture->width());
        // X 和 W 相对整个当前时间窗映射到 Plot 像素。
        const float x = static_cast<float>((intersectStart - pixelStart) /
                                           pixelWidth * plotW);
        const float w = static_cast<float>((intersectEnd - intersectStart) /
                                           pixelWidth * plotW);
        // Quad 覆盖完整声道高度，只沿 X 裁剪纹理。
        addSpectrumQuad(x, plotY, w, plotH, uv0X, uv1X, texture);
    }
}

/// @brief 绘制一个声道的播放头、主视野框和 Seek 拖拽覆盖层。
/// @param seekId 当前声道 InvisibleButton 的稳定 ID。
/// @param channelIndex 声道索引，用于独占拖拽所有权。
/// @param groupMin Plot 屏幕左上角。
/// @param groupMax Plot 屏幕右下角。
/// @param viewStart 当前视觉时间窗起点。
/// @param viewEnd 当前视觉时间窗终点。
/// @param globalVisualOffset 通用视觉时间相对音频时间偏移。
/// @param totalTime 音轨总时长。
/// @param visualTime 当前视觉播放头时间。
/// @param snapshot 逻辑层只读渲染快照，可为空。
///
/// 两声道共享一个拖拽所有者，防止同一鼠标动作在左右覆盖层重复提交。接近水平边缘
/// 时按帧时间非阻塞推进本地视窗，持续发布预览鼠标命令，释放时立即提交最终
/// Seek。
/// @warning UI 热路径：每声道每帧调用；只做常量几何与事件发布，不得阻塞等待。
void AudioSpectrumView::renderChannelInteractionOverlay(
    const char* seekId, int channelIndex, ImVec2 groupMin, ImVec2 groupMax,
    double viewStart, double viewEnd, float globalVisualOffset,
    double totalTime, double visualTime,
    const Common::Render::RenderSnapshot* snapshot)
{
    // 屏幕矩形必须有正面积才能建立交互 Item。
    const float width  = groupMax.x - groupMin.x;
    const float height = groupMax.y - groupMin.y;
    if ( width <= 0.0f || height <= 0.0f ) return;

    // 极小时间范围无法稳定把鼠标坐标映射到时间。
    const double viewRange = viewEnd - viewStart;
    if ( viewRange <= 0.001 ) return;

    // 红色播放头只在当前声道可见时间窗内绘制。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if ( visualTime >= viewStart && visualTime <= viewEnd ) {
        // 视觉时间归一化到零到一，再映射到屏幕 X。
        const float relativePos =
            static_cast<float>((visualTime - viewStart) / viewRange);
        const float lineX = groupMin.x + relativePos * width;
        drawList->AddLine(ImVec2(lineX, groupMin.y),
                          ImVec2(lineX, groupMax.y),
                          IM_COL32(255, 0, 0, 255),
                          2.0f);
    }

    if ( snapshot && snapshot->hasBeatmap ) {
        // 主画布可见时间范围映射到当前频谱 Plot。
        const float xStart =
            groupMin.x +
            static_cast<float>((snapshot->visibleTimeStart - viewStart) /
                               viewRange) *
                width;
        const float xEnd =
            groupMin.x +
            static_cast<float>((snapshot->visibleTimeEnd - viewStart) /
                               viewRange) *
                width;
        // 覆盖框裁剪到声道屏幕矩形，允许主视野部分超出频谱窗口。
        const float drawX1 = std::clamp(xStart, groupMin.x, groupMax.x);
        const float drawX2 = std::clamp(xEnd, groupMin.x, groupMax.x);

        if ( drawX2 > drawX1 ) {
            // 半透明填充表示范围，较高透明度边框标出准确边界。
            drawList->AddRectFilled(ImVec2(drawX1, groupMin.y),
                                    ImVec2(drawX2, groupMax.y),
                                    IM_COL32(128, 0, 255, 40));
            drawList->AddRect(ImVec2(drawX1, groupMin.y),
                              ImVec2(drawX2, groupMax.y),
                              IM_COL32(128, 0, 255, 180),
                              0.0f,
                              0,
                              1.5f);
        }
    }

    // 透明交互 Item 覆盖完整声道，视觉内容由 DrawList 绘制。
    ImGui::SetCursorScreenPos(groupMin);
    ImGui::InvisibleButton(seekId, ImVec2(width, height));

    // 本帧输入状态在 InvisibleButton 后立即读取。
    const bool isInteractionActive  = ImGui::IsItemActive();
    const bool isInteractionHovered = ImGui::IsItemHovered();
    const bool isLeftMouseDown      = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if ( isInteractionActive && m_seekDragOwnerChannel != channelIndex ) {
        // 首个获得 Active 的声道成为本次拖拽唯一所有者。
        m_seekDragOwnerChannel = channelIndex;
        // 本地滚动中心从当前视觉播放头开始。
        m_seekDragViewCenter = visualTime;
    }
    const bool ownsSeekDrag = m_seekDragOwnerChannel == channelIndex;
    // 鼠标位置即使离开矩形也用于活动拖拽和边缘滚动。
    const ImVec2 mousePos = ImGui::GetMousePos();

    double interactionViewStart = viewStart;
    double interactionViewEnd   = viewEnd;
    if ( ownsSeekDrag && isLeftMouseDown ) {
        // 本地先行推进频谱视野；逻辑层仍同步推进主画布，并在松开时由最终 seek
        // 对齐。
        // 固定边缘区只决定何时启动滚动，速度仍由超出距离连续控制。
        constexpr float edgeScrollMargin = 20.0f;
        float           edgeDistance     = 0.0f;
        if ( mousePos.x < groupMin.x + edgeScrollMargin ) {
            // 左边缘内或外产生负距离，向更早时间滚动。
            edgeDistance = mousePos.x - (groupMin.x + edgeScrollMargin);
        } else if ( mousePos.x > groupMax.x - edgeScrollMargin ) {
            // 右边缘内或外产生正距离，向更晚时间滚动。
            edgeDistance = mousePos.x - (groupMax.x - edgeScrollMargin);
        }

        // 灵敏度来自预览区统一视觉配置，零值关闭边缘滚动。
        const float edgeScrollSensitivity =
            std::max(0.0f,
                     Config::AppConfig::instance()
                         .getVisualConfig()
                         .previewConfig.edgeScrollSensitivity);
        if ( std::abs(edgeDistance) > 0.001f && edgeScrollSensitivity > 0.0f ) {
            // DeltaTime 钳制到 100ms，避免卡顿帧造成大幅时间跳跃。
            const double frameSeconds =
                std::clamp<double>(ImGui::GetIO().DeltaTime, 0.0, 0.1);
            // 位移速度与越过边缘阈值的像素距离成正比。
            const double scrollDelta = static_cast<double>(edgeDistance) *
                                       edgeScrollSensitivity * frameSeconds;
            // 可滚动视觉时间范围由通用偏移和音轨总时长确定。
            const double minVisualTime =
                static_cast<double>(globalVisualOffset);
            const double maxVisualTime =
                minVisualTime + std::max(0.0, totalTime);
            m_seekDragViewCenter =
                std::clamp(m_seekDragViewCenter + scrollDelta,
                           minVisualTime,
                           maxVisualTime);
            // 中心变化后立刻更新本次鼠标映射所用的交互时间窗。
            interactionViewStart = m_seekDragViewCenter - m_zoom;
            interactionViewEnd   = m_seekDragViewCenter + m_zoom;
        }
    }

    // 所有鼠标时间换算使用可能已被边缘滚动更新的窗口。
    const double interactionViewRange =
        std::max(0.001, interactionViewEnd - interactionViewStart);
    // X 坐标裁剪到声道范围，拖出左右边界时停留在窗口端点。
    const float relX =
        std::clamp((mousePos.x - groupMin.x) / width, 0.0f, 1.0f);
    // 视觉时间用于预览，扣通用偏移后得到最终音频 Seek 时间。
    const double hoverVisualTime =
        interactionViewStart + relX * interactionViewRange;
    const double hoverAudioTime = hoverVisualTime - globalVisualOffset;

    if ( isInteractionActive || isInteractionHovered ) {
        // Tooltip 遵循当前时间显示模式及快照节拍信息。
        const auto timeText =
            MMM::UI::Utils::formatCanvasTime(hoverVisualTime, snapshot);
        ImGui::SetTooltip("%s", timeText.c_str());

        // 绿色悬浮线跟随裁剪后的时间位置。
        const float hoverLineX = groupMin.x + relX * width;
        drawList->AddLine(ImVec2(hoverLineX, groupMin.y),
                          ImVec2(hoverLineX, groupMax.y),
                          IM_COL32(0, 255, 0, 150),
                          1.0f);

        if ( isInteractionActive ) {
            // 拖拽期间每帧发布相对坐标、区域尺寸和预览视觉时间。
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetMousePosition{ "AudioSpectrum",
                                            mousePos.x - groupMin.x,
                                            mousePos.y - groupMin.y,
                                            width,
                                            height,
                                            true,
                                            true,
                                            hoverVisualTime }));

            if ( snapshot && snapshot->hasBeatmap ) {
                // 保存主画布可见范围相对快照中心的左右时间偏移。
                const double offsetStart =
                    snapshot->visibleTimeStart - snapshot->currentTime;
                const double offsetEnd =
                    snapshot->visibleTimeEnd - snapshot->currentTime;
                // 把相对跨度平移到当前频谱悬浮时间形成预览框。
                const float preX1 =
                    groupMin.x +
                    static_cast<float>(
                        (hoverVisualTime + offsetStart - interactionViewStart) /
                        interactionViewRange) *
                        width;
                const float preX2 =
                    groupMin.x +
                    static_cast<float>(
                        (hoverVisualTime + offsetEnd - interactionViewStart) /
                        interactionViewRange) *
                        width;
                // 预览框允许部分超出当前窗口，绘制前裁剪到 Plot。
                const float drawPreX1 =
                    std::clamp(preX1, groupMin.x, groupMax.x);
                const float drawPreX2 =
                    std::clamp(preX2, groupMin.x, groupMax.x);

                if ( drawPreX2 > drawPreX1 ) {
                    // 拖拽预览使用比静态主视野框更高的透明度。
                    drawList->AddRectFilled(ImVec2(drawPreX1, groupMin.y),
                                            ImVec2(drawPreX2, groupMax.y),
                                            IM_COL32(128, 0, 255, 80));
                    drawList->AddRect(ImVec2(drawPreX1, groupMin.y),
                                      ImVec2(drawPreX2, groupMax.y),
                                      IM_COL32(128, 0, 255, 230),
                                      0.0f,
                                      0,
                                      1.5f);
                }
            }
        }
    }

    if ( ownsSeekDrag && !isLeftMouseDown ) {
        // 鼠标释放由所有者声道提交一次最终音频 Seek。
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdSeek{ hoverAudioTime }));
        // 同帧发布 inactive 鼠标状态，结束逻辑层预览拖拽。
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ "AudioSpectrum",
                                        mousePos.x - groupMin.x,
                                        mousePos.y - groupMin.y,
                                        width,
                                        height,
                                        false,
                                        false,
                                        -1.0 }));
        // 清除所有者，下一次点击可由任意声道获取。
        m_seekDragOwnerChannel = -1;
    }
}

/// @brief 查询渲染器是否需要继续上传频谱纹理。
/// @return 存在待开始或未完成的纹理重载时返回 true。
/// @warning 渲染热路径：只读取布尔标志，不得在查询中创建资源。
bool AudioSpectrumView::needReload()
{
    return m_texturesNeedReload;
}

/// @brief 在渲染线程按帧增量上传待处理的左右声道纹理块。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 纹理上传使用的命令池。
/// @param queue 执行上传命令的队列。
///
/// 重载开始时建立 loading
/// 集合，之后每帧最多上传固定数量左右块对。全部完成后整体
/// 交换为活动纹理，旧纹理移入 retired
/// 集合延迟释放，避免正在飞行的命令引用失效。
/// @warning 渲染路径：允许有界 GPU 上传；不得一次上传全部长音轨纹理或
/// waitIdle。
/// @details pendingChunks 是 CPU RGBA 数据，loadingTextures
/// 是本轮已上传但尚未展示的 GPU 集合，textures 是当前活动集合，retiredTextures
/// 把上一活动集合额外保留一轮。
/// 这些状态不能提前合并，否则增量上传期间左右声道会出现新旧块混绘。
///
/// 无效块对仍推进上传索引但不会向 loading 集合追加纹理。后台准备正常情况下不会
/// 生成无效块，该分支只防御空音轨或尺寸状态异常。
void AudioSpectrumView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    cmdPool,
                                       vk::Queue&          queue)
{
    // 无待重载状态时保持现有活动纹理。
    if ( !m_texturesNeedReload ) return;

    if ( !m_textureReloadStarted ) {
        // 新一轮开始前释放上一轮已经安全退休的集合。
        m_retiredTexturesL.clear();
        m_retiredTexturesR.clear();
        // loading 集合从空开始并按待上传块数预留容量。
        m_loadingTexturesL.clear();
        m_loadingTexturesR.clear();
        // 左右块由后台准备阶段保证相同数量和索引对应关系。
        m_loadingTexturesL.reserve(m_pendingChunksL.size());
        m_loadingTexturesR.reserve(m_pendingChunksR.size());
        // 上传游标从第一个块对开始。
        m_nextChunkUploadIndex = 0;
        m_textureReloadStarted = true;
    }

    // 每帧上传量受常量限制，避免长频谱造成单帧卡顿。
    std::size_t uploadedThisFrame = 0;
    while ( m_nextChunkUploadIndex < m_pendingChunksL.size() &&
            uploadedThisFrame < MAX_UPLOAD_CHUNK_PAIRS_PER_FRAME ) {
        const auto& chunkL = m_pendingChunksL[m_nextChunkUploadIndex];
        // 左右声道使用相同全局横向块索引。
        const auto& chunkR = m_pendingChunksR[m_nextChunkUploadIndex];
        ++m_nextChunkUploadIndex;
        ++uploadedThisFrame;

        if ( chunkL.pixels.empty() || chunkR.pixels.empty() ||
             chunkL.width == 0 || chunkL.height == 0 || chunkR.width == 0 ||
             chunkR.height == 0 ) {
            // 任一声道块无效时跳过整对，保持数组索引对应。
            continue;
        }

        // VKTexture 构造同步上传 RGBA8 左声道像素。
        m_loadingTexturesL.push_back(std::make_unique<Graphic::VKTexture>(
            chunkL.pixels.data(),
            chunkL.width,
            chunkL.height,
            physicalDevice,
            logicalDevice,
            cmdPool,
            queue,
            Graphic::VKTexturePixelFormat::Rgba8));
        // 右声道使用相同尺寸约定独立创建纹理。
        m_loadingTexturesR.push_back(std::make_unique<Graphic::VKTexture>(
            chunkR.pixels.data(),
            chunkR.width,
            chunkR.height,
            physicalDevice,
            logicalDevice,
            cmdPool,
            queue,
            Graphic::VKTexturePixelFormat::Rgba8));
    }

    if ( m_nextChunkUploadIndex >= m_pendingChunksL.size() ) {
        // 全部块遍历后，旧活动纹理移入退休集合延迟析构。
        m_retiredTexturesL = std::move(m_texturesL);
        m_retiredTexturesR = std::move(m_texturesR);
        // loading 集合整体成为新活动纹理，左右声道在同一帧切换。
        m_texturesL = std::move(m_loadingTexturesL);
        m_texturesR = std::move(m_loadingTexturesR);

        // CPU 像素块已被 GPU 纹理消费，可以释放大容量缓存。
        m_pendingChunksL.clear();
        m_pendingChunksR.clear();
        // 重置状态机，下一轮重算可以重新从零开始。
        m_nextChunkUploadIndex = 0;
        m_textureReloadStarted = false;
        m_texturesNeedReload   = false;
    }
}

/// @brief 把主音轨 EQ 拓扑和参数同步到频谱预览节点。
///
/// EQ 关闭时释放预览效果链；频段数量变化时重建节点，普通增益和 Q 改变只更新已有
/// 频段。该对象供同步状态展示，后台任务会复制 EQSettings 并创建独立工作者节点。
/// @warning UI 热路径：每帧调用；只有频段拓扑变化时允许分配节点。
void AudioSpectrumView::syncEQ()
{
    // AudioManager 是当前主音轨 EQ 配置真值。
    auto& audioManager = Audio::AudioManager::instance();
    if ( !audioManager.isMainTrackEQEnabled() ) {
        // 关闭时后台重算直接分析原始 PCM。
        m_previewEQ.reset();
        return;
    }

    if ( !m_previewEQ || m_previewEQ->get_band_count() !=
                             audioManager.getMainTrackEQBandCount() ) {
        // 频率列表决定 GraphicEqualizer 拓扑和频段顺序。
        std::vector<double> freqs;
        size_t              count = audioManager.getMainTrackEQBandCount();
        for ( size_t i = 0; i < count; ++i ) {
            // 按 AudioManager 稳定索引提取中心频率。
            freqs.push_back(audioManager.getMainTrackEQBandFrequency(i));
        }
        // 预览链使用独立代理源，不持有实际 PCM。
        m_previewEQ = std::make_shared<ice::GraphicEqualizer>(freqs);
        m_previewEQ->set_inputnode(std::make_shared<BufferSourceNodeProxy>());
    }

    // 增益与 Q 不改变频段数量，可逐项更新而无需重建。
    for ( size_t i = 0; i < m_previewEQ->get_band_count(); ++i ) {
        m_previewEQ->set_band_gain_db(i,
                                      audioManager.getMainTrackEQBandGain(i));
        m_previewEQ->set_band_q_factor(i, audioManager.getMainTrackEQBandQ(i));
    }
}

/// @brief 捕获当前分析参数并在应用线程池启动一次完整频谱重算。
///
/// 同一时刻只允许一个任务。已完成但尚未清空的 future
/// 会先等待并回收；当前正在计算
/// 时请求被合并忽略，避免多个任务并发写入同一缓存。
///
/// EQ、最大频率、对数偏置和细节 profile 全部按值捕获，后台任务不会读取变化中的
/// UI 控件。开始前清空上一轮待上传 CPU 块，但保留当前活动 GPU
/// 纹理供进度期间显示状态。
/// @warning 用户触发的低频入口；回收旧 future
/// 可能短暂等待，不得从普通绘制分支调用。
void AudioSpectrumView::startAsyncRecalculate()
{
    // 正在运行的任务已覆盖当前重算需求，不启动竞争任务。
    if ( m_isCalculating.load() ) return;

    if ( m_calcFuture.valid() ) {
        // 这里只回收已经通过状态标志完成的旧任务 future。
        m_calcFuture.wait();
        m_calcFuture = std::future<void>{};
    }

    // 把 AudioManager EQ 状态复制为自包含后台设置。
    auto&      audioManager = Audio::AudioManager::instance();
    EQSettings eq;
    eq.enabled = audioManager.isMainTrackEQEnabled();
    if ( eq.enabled ) {
        // 三个数组按频段索引一一对应。
        size_t count = audioManager.getMainTrackEQBandCount();
        for ( size_t i = 0; i < count; ++i ) {
            eq.freqs.push_back(audioManager.getMainTrackEQBandFrequency(i));
            eq.gains.push_back(audioManager.getMainTrackEQBandGain(i));
            eq.qs.push_back(audioManager.getMainTrackEQBandQ(i));
        }
    }

    // 先发布计算态和零进度，再重置本轮完成标志。
    m_isCalculating.store(true);
    m_calcProgress.store(0.0f);
    m_calcFinished.store(false);
    // 新结果尚未生成，取消上一轮未开始的纹理重载状态。
    m_texturesNeedReload   = false;
    m_textureReloadStarted = false;
    m_nextChunkUploadIndex = 0;
    // CPU 待上传块属于上一轮活动参数，开始新任务前释放。
    m_pendingChunksL.clear();
    m_pendingChunksR.clear();

    // 细节等级和派生 profile 在任务启动点冻结。
    const auto detailLevel =
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel;
    const auto detailProfile = Config::spectrumDetailProfile(detailLevel);

    // 应用线程池负责后台任务生命周期，不创建临时裸线程。
    auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        // 无线程池时恢复非计算态并记录配置错误。
        m_isCalculating.store(false);
        XERROR("AppThreadPool is not initialized before spectrum calculation.");
        return;
    }

    // 每轮使用新的 stop_source，析构可请求当前任务协作停止。
    m_calcStopSource                = std::stop_source{};
    const std::stop_token stopToken = m_calcStopSource.get_token();
    m_calcFuture = appThreadPool->enqueue([this,
                                           stopToken,
                                           eq      = std::move(eq),
                                           maxFreq = m_maxFreq,
                                           logBias = m_logBias,
                                           detailLevel,
                                           detailProfile]() {
        // 后台入口只接收值快照和停止令牌。
        backgroundRecalculate(
            stopToken, eq, maxFreq, logBias, detailLevel, detailProfile);
    });
}

/// @brief 后台频谱重算使用完整缓存，多个 FFT 工作者不能竞争流式预读页。
/// @param stopToken 析构或任务替换请求的协作式停止令牌。
/// @param eq 启动时捕获的主音轨 EQ 设置。
/// @param maxFreq 频谱纵轴最高频率。
/// @param logBias 频率分桶的指数偏置。
/// @param detailLevel 待提交的频谱细节等级。
/// @param detailProfile 对应的时间段密度和频率箱数量。
///
/// 算法先准备 Hann 窗和对数频率箱，再把每个声道的时间段分区交给有限数量工作者。
/// 每个工作者拥有 FFTW 输入、输出、Plan、音频缓冲和 EQ 节点，只有音轨对象与只读
/// 参数共享。结果写入互不重叠的热力图区间。
///
/// 左声道完成后再处理右声道，以把线程池并发度限制在请求工作者的一半并避免两声道
/// 同时争用解码器。单声道输入直接复制左热力图。
///
/// 完成前只更新原子进度；所有热力图和 pending
/// 参数在工作结束后由外层任务一次写入， 最后发布 calcFinished。UI 线程随后生成
/// RGBA 块并安排 GPU 上传。
/// @warning 仅由低频后台任务调用，允许加载和等待 PCM，不能移入绘制回调。
void AudioSpectrumView::backgroundRecalculate(
    std::stop_token stopToken, const EQSettings& eq, float maxFreq,
    float logBias, Config::SpectrumDetailLevel detailLevel,
    Config::SpectrumDetailProfile detailProfile)
{
    // 启动后立即收到停止请求时不读取音轨或分配 FFT 资源。
    if ( stopToken.stop_requested() ) {
        m_isCalculating.store(false);
        return;
    }

    // 主音轨可能是实时流式实例，离线绝对帧扫描需要独立分析实例。
    auto& audioManager = Audio::AudioManager::instance();
    auto  track        = audioManager.getBGMTrack();
    if ( track &&
         track->cachingStrategy() == ice::CachingStrategy::STREAMING ) {
        // 缺页返回正长度静音，离线 FFT 不能以 read 返回值判断完整音频已到达。
        // 分析实例不会驱逐播放实例预读页或改变其游标。
        track = audioManager.loadTrackForAnalysis(track->path());
    }
    if ( !track ) {
        // 音轨在任务排队期间被卸载时恢复计算态，保留旧纹理。
        m_isCalculating.store(false);
        return;
    }

    // 时间、采样率和 profile 共同决定热力图尺寸。
    double       totalTime         = audioManager.getTotalTime();
    double       sampleRate        = ice::ICEConfig::internal_format.samplerate;
    const double segmentsPerSecond = detailProfile.segmentsPerSecond;
    const int    frequencyBins     = detailProfile.frequencyBins;
    // 末端额外段覆盖浮点向下取整后的音轨边界。
    int numTotalSegments = static_cast<int>(totalTime * segmentsPerSecond) + 1;
    // 输出固定绘制左右声道，输入声道数用于决定是否复制左侧。
    uint16_t numChannels = ice::ICEConfig::internal_format.channels;

    // 2048 点 FFT 在时间与频率分辨率之间保持现有平衡。
    const int fftSize = 2048;
    // hopSize 把每秒段数映射为相邻 FFT 窗起点帧距。
    const size_t hopSize = static_cast<size_t>(sampleRate / segmentsPerSecond);

    // 复用应用线程池，并只占用请求工作者的一半以给其他任务留出容量。
    auto*     appThreadPool    = MMM::Runtime::AppThreadPool::instance().get();
    const int requestedWorkers = std::max<int>(
        1, MMM::Runtime::AppThreadPool::instance().requestedWorkerCount());
    const int numWorkers = std::max(1, requestedWorkers / 2);

    // 日志记录本轮工作规模和频率映射参数，便于诊断耗时。
    XINFO(
        "Spectrum async recalculate: {} workers reserved from {}, {} "
        "segments, {} bins, "
        "maxFreq: {}, logBias: {}",
        numWorkers,
        requestedWorkers,
        numTotalSegments,
        frequencyBins,
        maxFreq,
        logBias);

    // Hann 窗降低 FFT 分段边界造成的频谱泄漏。
    std::vector<float> window(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        window[i] =
            0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) *
                                    static_cast<float>(i) / (fftSize - 1)));
    }

    /// @brief 预计算的 FFT bin 范围。
    ///
    /// start 与 end 均为包含端点的 FFTW 输出索引，限制在奈奎斯特 bin 以内。
    struct FrequencyBinRange {
        /// @brief 起始 FFT bin。
        int start{ 0 };

        /// @brief 结束 FFT bin。
        int end{ 0 };
    };

    // 可听频率下限固定 20Hz，上限至少比下限高 1Hz。
    const float fmin   = 20.0f;
    const float fmax   = std::max(maxFreq, fmin + 1.0f);
    const float k      = logBias;
    const float fRange = fmax - fmin;
    // exp(k)-1 作为归一化分母，使 progress=1 精确映射到 fmax。
    const float expKMinus1 = std::exp(k) - 1.0f;
    /// 把零到一的纵向进度映射为线性或指数频率。
    auto getFreq = [&](float progress) {
        if ( std::abs(k) < 1e-4f ) {
            // 偏置接近零时使用线性极限，避免除以接近零的 expKMinus1。
            return fmin + fRange * progress;
        }
        return fmin + fRange * (std::exp(k * progress) - 1.0f) / expKMinus1;
    };

    // 每个显示频率箱预先映射为一段 FFT bin，工作者只做数组读取。
    std::vector<FrequencyBinRange> binRanges;
    binRanges.reserve(static_cast<size_t>(frequencyBins));
    for ( int b = 0; b < frequencyBins; ++b ) {
        // 相邻归一化边界通过相同频率曲线转换。
        const float freqStart =
            getFreq(static_cast<float>(b) / static_cast<float>(frequencyBins));
        const float freqEnd = getFreq(static_cast<float>(b + 1) /
                                      static_cast<float>(frequencyBins));
        // Hz 乘 FFT 点数再除采样率得到离散 bin 索引。
        int bStart = static_cast<int>(freqStart * fftSize / sampleRate);
        int bEnd   = static_cast<int>(freqEnd * fftSize / sampleRate);
        // 两端钳制到实数 FFT 输出的零到奈奎斯特范围。
        bStart = std::clamp(bStart, 0, fftSize / 2);
        bEnd   = std::clamp(bEnd, bStart, fftSize / 2);
        binRanges.push_back({ bStart, bEnd });
    }

    // -80dB 到 -10dB 映射到八位热力图强度。
    const float scaleMin   = -80.0f;
    const float scaleMax   = -10.0f;
    const float scaleRange = scaleMax - scaleMin;
    /// 把 dB 值钳制并量化为 0 到 255 的强度。
    auto dbToIntensity = [&](float db) -> std::uint8_t {
        const float t = std::clamp((db - scaleMin) / scaleRange, 0.0f, 1.0f);
        return static_cast<std::uint8_t>(std::lround(t * 255.0f));
    };

    // 热力图采用“频率箱主序 × 时间段次序”的连续布局。
    std::vector<std::uint8_t> heatmapL(
        static_cast<size_t>(frequencyBins) * numTotalSegments, 0U);
    std::vector<std::uint8_t> heatmapR(
        static_cast<size_t>(frequencyBins) * numTotalSegments, 0U);

    // 进度按已完成声道时间段计数，单声道总工作量只算一份。
    std::atomic<int> completedSegments{ 0 };
    int              totalWork = numTotalSegments * (numChannels > 1 ? 2 : 1);

    // FFTW Plan 创建和销毁使用进程级互斥，执行阶段可并行。
    static std::mutex s_fftwPlanMutex;

    /// 处理一个声道的连续时间段区间。
    /// @param chIdx 输入声道索引。
    /// @param heatmap 对应声道输出数组。
    /// @param startSeg 起始段索引，包含。
    /// @param endSeg 结束段索引，不包含。
    auto processChannel = [&](int                        chIdx,
                              std::vector<std::uint8_t>& heatmap,
                              int                        startSeg,
                              int                        endSeg) {
        // 每个工作者拥有独立 FFTW 对齐输入和复数输出缓冲。
        double* localIn =
            static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
        fftw_complex* localOut = static_cast<fftw_complex*>(
            fftw_malloc(sizeof(fftw_complex) * (fftSize / 2 + 1)));

        // Plan 创建由 FFTW 全局互斥保护，避免库内部非线程安全规划状态竞争。
        fftw_plan localPlan;
        {
            std::lock_guard<std::mutex> lock(s_fftwPlanMutex);
            localPlan =
                fftw_plan_dft_r2c_1d(fftSize, localIn, localOut, FFTW_ESTIMATE);
        }

        // 原始与 EQ 后缓冲按固定 FFT 窗大小预分配。
        auto localRawBuffer  = std::make_unique<ice::AudioBuffer>();
        auto localProcBuffer = std::make_unique<ice::AudioBuffer>();
        localRawBuffer->resize(ice::ICEConfig::internal_format, fftSize);
        localProcBuffer->resize(ice::ICEConfig::internal_format, fftSize);

        // 每个工作者使用独立输入代理和 EQ 状态，滤波器历史不会跨线程竞争。
        auto localBufferSource = std::make_shared<BufferSourceNodeProxy>();
        std::shared_ptr<ice::GraphicEqualizer> localEQ;
        if ( eq.enabled ) {
            // 频率列表构造拓扑，prepare 使用内部音频格式与 FFT 块上限。
            localEQ = std::make_shared<ice::GraphicEqualizer>(eq.freqs);
            localEQ->prepare(ice::ICEConfig::internal_format,
                             static_cast<std::size_t>(fftSize));
            localEQ->set_inputnode(localBufferSource);
            for ( size_t i = 0; i < eq.gains.size(); ++i ) {
                // 捕获数组按频段索引对应，工作者节点获得相同增益和 Q。
                localEQ->set_band_gain_db(i, eq.gains[i]);
                localEQ->set_band_q_factor(i, eq.qs[i]);
            }
        }

        for ( int t = startSeg; t < endSeg; ++t ) {
            // 每个时间段边界检查协作停止，限制析构等待时间。
            if ( stopToken.stop_requested() ) {
                break;
            }

            // 时间段索引乘 hop 得到 FFT 窗绝对起始帧。
            size_t startFrame = static_cast<size_t>(t) * hopSize;
            // 末端不足完整 FFT 窗时停止本区间，不用零填充制造额外频谱。
            if ( startFrame + fftSize > track->num_frames() ) break;

            // 分析音轨按绝对帧读取完整窗口。
            track->read(*localRawBuffer, startFrame, fftSize);

            // 默认直接分析原始声道数据。
            float* chanData = localRawBuffer->raw_ptrs()[chIdx];
            if ( localEQ ) {
                // EQ 启用时同步处理同一多声道块，再选择目标声道。
                localBufferSource->setBuffer(localRawBuffer.get());
                localEQ->process(*localProcBuffer);
                chanData = localProcBuffer->raw_ptrs()[chIdx];
            }

            // 把 float PCM 乘 Hann 窗并转换到 FFTW double 输入。
            for ( int i = 0; i < fftSize; ++i )
                localIn[i] = static_cast<double>(chanData[i] * window[i]);

            // Plan 与输入输出属于当前工作者，可无锁执行。
            fftw_execute(localPlan);

            for ( int b = 0; b < frequencyBins; ++b ) {
                // 当前显示箱读取预计算的包含式 FFT bin 范围。
                const auto [bStart, bEnd] = binRanges[b];

                // 以范围内最大幅度保留窄带峰值，避免平均稀释瞬态。
                float maxMag = 0.0f;
                for ( int i = bStart; i <= bEnd; ++i ) {
                    const float real  = static_cast<float>(localOut[i][0]);
                    const float imag  = static_cast<float>(localOut[i][1]);
                    const float magSq = real * real + imag * imag;
                    // 比较平方幅度可避免每个 FFT bin 求 sqrt。
                    if ( magSq > maxMag ) maxMag = magSq;
                }
                // 最终只对最大值开方并归一化到 FFT 点数，再转 dB。
                const float db =
                    (maxMag > 1e-9f)
                        ? 20.0f * std::log10(std::sqrt(maxMag) /
                                             static_cast<float>(fftSize))
                        : -100.0f;
                // 频率主序布局下，同一箱的连续时间段相邻。
                heatmap[static_cast<size_t>(b) * numTotalSegments + t] =
                    dbToIntensity(db);
            }

            // relaxed 原子只承担进度统计，不同步热力图数据。
            completedSegments.fetch_add(1, std::memory_order_relaxed);
            m_calcProgress.store(static_cast<float>(completedSegments.load(
                                     std::memory_order_relaxed)) /
                                     totalWork,
                                 std::memory_order_relaxed);
        }

        {
            // Plan 销毁与创建使用同一 FFTW 全局互斥。
            std::lock_guard<std::mutex> lock(s_fftwPlanMutex);
            fftw_destroy_plan(localPlan);
        }
        // FFTW 对齐缓冲必须使用 fftw_free 配对释放。
        fftw_free(localIn);
        fftw_free(localOut);
    };

    /// 把一个声道的全部时间段分区并调度到有限工作者。
    /// @param chIdx 输入声道索引。
    /// @param heatmap 该声道目标热力图。
    auto runChannel = [&](int chIdx, std::vector<std::uint8_t>& heatmap) {
        // 声道开始前已停止时不再创建子任务。
        if ( stopToken.stop_requested() ) {
            return;
        }

        // 向上取整得到每个工作者的连续时间段数。
        const int segsPerWorker =
            (numTotalSegments + numWorkers - 1) / numWorkers;
        // ranges 在调度前完整建立，子任务只读取稳定数组值捕获。
        std::vector<std::pair<int, int>> ranges;
        ranges.reserve(static_cast<size_t>(numWorkers));

        for ( int w = 0; w < numWorkers; ++w ) {
            // 最后一个区间截断到总段数。
            int startSeg = w * segsPerWorker;
            int endSeg   = std::min(startSeg + segsPerWorker, numTotalSegments);
            // 工作者数量多于段数时跳过空尾区间。
            if ( startSeg >= numTotalSegments ) break;
            ranges.emplace_back(startSeg, endSeg);
        }

        if ( ranges.empty() ) {
            // 空音轨或退化 profile 没有可调度工作。
            return;
        }

        if ( !appThreadPool || ranges.size() <= 1 ) {
            // 无线程池或单区间时在当前后台任务直接处理，避免嵌套调度开销。
            const auto [startSeg, endSeg] = ranges.front();
            processChannel(chIdx, heatmap, startSeg, endSeg);
            return;
        }

        // latch 数量等于实际非空区间，每个子任务完成时递减一次。
        std::latch done(static_cast<std::ptrdiff_t>(ranges.size()));
        for ( const auto [startSeg, endSeg] : ranges ) {
            appThreadPool->enqueue_void([&, chIdx, startSeg, endSeg]() {
                // 区间互不重叠，可以并行写同一 heatmap 不同位置。
                processChannel(chIdx, heatmap, startSeg, endSeg);
                done.count_down();
            });
        }
        // 当前后台主任务等待所有子区间，UI 线程不参与该阻塞。
        done.wait();
    };

    // 左声道先占用工作者处理完整时间轴。
    runChannel(0, heatmapL);
    if ( stopToken.stop_requested() ) {
        // 停止时不提交部分热力图或 pending 参数。
        m_isCalculating.store(false);
        return;
    }

    if ( numChannels > 1 ) {
        // 多声道输入再处理右声道。
        runChannel(1, heatmapR);
        if ( stopToken.stop_requested() ) {
            // 右声道中途停止同样丢弃整轮结果。
            m_isCalculating.store(false);
            return;
        }
    } else {
        // 单声道复制左热力图，使渲染仍可固定绘制左右两块。
        heatmapR = heatmapL;
    }

    // 所有工作完成后把局部结果移动到视图 CPU 缓存。
    m_cachedIntensityL       = std::move(heatmapL);
    m_cachedIntensityR       = std::move(heatmapR);
    m_cachedNumTotalSegments = numTotalSegments;
    // pending 参数描述新缓存布局，UI 完成分支会整体切换为 active。
    m_pendingSpectrumDetailLevel    = detailLevel;
    m_pendingCacheSegmentsPerSecond = segmentsPerSecond;
    m_pendingNumFrequencyBins       = frequencyBins;

    // 先发布完整进度，再发布完成标志供 UI 线程消费。
    m_calcProgress.store(1.0f);
    m_calcFinished.store(true);
}

/// @brief 把完整八位频谱缓存转换为可增量上传的 RGBA8 纹理块。
///
/// 横轴按 MAX_TEXTURE_W 切分，最后一块使用剩余真实宽度；纵轴把低频缓存行翻转到
/// 纹理底部。左右声道保持完全相同的块布局，便于上传阶段按索引成对处理。
///
/// 强度通过黑、红、黄、白三段热色映射写入 RGB，Alpha 固定不透明。这里只准备 CPU
/// 像素，不创建 Vulkan 资源；渲染线程在后续帧通过 reloadTextures 有界上传。
/// @warning UI 完成分支的低频 CPU 路径：按完整缓存分配和遍历，不得每帧调用。
void AudioSpectrumView::prepareFullGlobalTextures()
{
    // 没有左声道缓存表示后台计算未产生可提交结果。
    if ( m_cachedIntensityL.empty() ) return;

    // 纹理总宽等于时间段数，高度等于当前活动频率箱数。
    int totalW = m_cachedNumTotalSegments;
    int texH   = m_numFrequencyBins;

    // 新一轮 CPU 块替换任何尚未上传的旧结果。
    m_pendingChunksL.clear();
    m_pendingChunksR.clear();
    // loading GPU 集合从空开始，上传游标和状态机复位。
    m_loadingTexturesL.clear();
    m_loadingTexturesR.clear();
    m_nextChunkUploadIndex = 0;
    m_textureReloadStarted = false;

    // 整数向上取整得到横向块数量。
    const int numChunks = (totalW + MAX_TEXTURE_W - 1) / MAX_TEXTURE_W;
    m_pendingChunksL.reserve(static_cast<size_t>(numChunks));
    m_pendingChunksR.reserve(static_cast<size_t>(numChunks));

    // VKTexturePixelFormat::Rgba8 每像素固定四字节。
    constexpr size_t rgbaBytesPerPixel = 4U;
    /// 把单字节强度写成不透明热色 RGBA 像素。
    /// @param pixels 目标纹理字节数组。
    /// @param offset 当前像素 R 分量偏移。
    /// @param intensity 零到 255 的频谱强度。
    auto writeHotPixel = [](std::vector<unsigned char>& pixels,
                            size_t                      offset,
                            std::uint8_t                intensity) {
        // 强度先归一化到零到一。
        const float t = static_cast<float>(intensity) / 255.0f;
        /// 把浮点颜色分量钳制并量化成字节。
        auto toByte = [](float value) {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<unsigned char>(std::lround(clamped * 255.0f));
        };

        // 三个分量依次延迟一个三分之一强度区间，形成黑红黄白梯度。
        pixels[offset]      = toByte(t * 3.0f);
        pixels[offset + 1U] = toByte(t * 3.0f - 1.0f);
        pixels[offset + 2U] = toByte(t * 3.0f - 2.0f);
        // 频谱纹理本身始终不透明，整体混合由渲染管线控制。
        pixels[offset + 3U] = 255U;
    };

    for ( int c = 0; c < numChunks; ++c ) {
        // 块起点使用固定全局宽度步进。
        uint32_t chunkStart = static_cast<uint32_t>(c) * MAX_TEXTURE_W;
        // 最后一块截断到剩余总宽。
        uint32_t chunkW =
            std::min(MAX_TEXTURE_W, static_cast<uint32_t>(totalW) - chunkStart);

        // 左右块共享尺寸，但拥有独立像素数组。
        TextureChunkData chunkL, chunkR;
        chunkL.width = chunkR.width = chunkW;
        chunkL.height = chunkR.height = texH;
        // resize 一次性分配完整 RGBA 像素容量。
        chunkL.pixels.resize(chunkW * texH * rgbaBytesPerPixel);
        chunkR.pixels.resize(chunkW * texH * rgbaBytesPerPixel);

        for ( uint32_t py = 0; py < static_cast<uint32_t>(texH); ++py ) {
            // 缓存频率索引从低到高，纹理 Y 从上到下，因此翻转行号。
            int b = texH - 1 - static_cast<int>(py);
            for ( uint32_t px = 0; px < chunkW; ++px ) {
                // 局部 X 加块起点得到完整热力图时间段索引。
                uint32_t globalX = chunkStart + px;
                // 行主序 RGBA 偏移使用当前块宽度。
                size_t offset =
                    (static_cast<size_t>(py) * chunkW + px) * rgbaBytesPerPixel;

                // 左右缓存采用相同频率主序布局。
                writeHotPixel(
                    chunkL.pixels,
                    offset,
                    m_cachedIntensityL[b * m_cachedNumTotalSegments + globalX]);
                writeHotPixel(
                    chunkR.pixels,
                    offset,
                    m_cachedIntensityR[b * m_cachedNumTotalSegments + globalX]);
            }
        }
        // 完成块按时间顺序追加，上传阶段保持相同索引。
        m_pendingChunksL.push_back(std::move(chunkL));
        m_pendingChunksR.push_back(std::move(chunkR));
    }

    // 至少存在一个 CPU 块时通知渲染器开始增量上传。
    m_texturesNeedReload = !m_pendingChunksL.empty();
}

/// @brief 指示离屏频谱视图每帧都需要重新录制绘制。
/// @return 始终为 true，因为时间窗、拖拽和可见纹理块可逐帧变化。
///
/// 即使音频暂停，窗口缩放、停靠尺寸或纹理增量上传也可能改变几何和描述符集合。
/// 常脏语义让 IRenderableView 基础设施统一处理这些变化。
/// @warning 渲染热路径：常量返回，不得增加状态查询。
bool AudioSpectrumView::isDirty() const
{
    return true;
}

/// @brief 接收离屏目标尺寸变化通知。
/// @param oldW 旧像素宽度。
/// @param oldH 旧像素高度。
/// @param w 新像素宽度。
/// @param h 新像素高度。
///
/// 频谱几何每帧按逻辑尺寸重建，无需额外缓存失效；参数显式忽略以保留接口契约。
void AudioSpectrumView::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                                   uint32_t h) const
{
    (void)oldW;
    (void)oldH;
    (void)w;
    (void)h;
}

/// @brief 读取并缓存频谱离屏管线的顶点和片段 SPIR-V。
/// @param shaderName 皮肤 Canvas 配置中的 shader module 名称。
/// @return 顶点、片段二进制字符串；配置或文件缺失时返回空。
///
/// 缓存命中时不访问文件系统。未命中路径从 AudioSpectrumView Canvas
/// 配置解析，并用 error_code 验证模块目录后读取两个固定文件名。
/// @warning 低频管线创建路径：允许文件 IO，不得在每帧命令录制中调用。
std::vector<std::string> AudioSpectrumView::getShaderSources(
    const std::string& shaderName)
{
    // shaderName 是缓存键，同一皮肤版本内可直接返回值副本。
    if ( m_shaderSourceCache.count(shaderName) ) {
        return m_shaderSourceCache[shaderName];
    }

    // 皮肤为频谱视图提供 Canvas shader module 映射。
    Config::SkinData::CanvasConfig canvasConfig =
        Config::SkinManager::instance().getCanvasConfig("AudioSpectrumView");
    if ( canvasConfig.canvas_name.empty() ) {
        // 缺少 Canvas 配置无法定位任何 shader。
        XERROR("AudioSpectrumView: failed to resolve shader config.");
        return {};
    }

    // 具体 module 名必须存在于配置映射。
    auto shaderModuleIt = canvasConfig.canvas_shader_modules.find(shaderName);
    if ( shaderModuleIt == canvasConfig.canvas_shader_modules.end() ) {
        return {};
    }

    // 目录存在性使用 error_code 检查，避免资源错误通过异常退出。
    const auto      shaderPath = shaderModuleIt->second;
    std::error_code shaderPathError;
    if ( !std::filesystem::exists(shaderPath, shaderPathError) ||
         shaderPathError ) {
        XWARN("AudioSpectrumView shader module path not found: {}",
              Config::pathToUtf8(shaderPath));
        return {};
    }

    // 顺序固定为顶点后片段，与渲染器管线创建契约一致。
    std::vector<std::string> result{
        Graphic::VKShader::readFile(
            Config::pathToUtf8(shaderPath / "VertexShader.spv")),
        Graphic::VKShader::readFile(
            Config::pathToUtf8(shaderPath / "FragmentShader.spv"))
    };
    // 只有成功定位路径后才缓存结果，皮肤失效可通过 invalidate 清空。
    m_shaderSourceCache[shaderName] = result;
    return result;
}

/// @brief 为频谱视图生成全局唯一的管线 shader 名称。
/// @param shaderModuleName 皮肤模块局部名称。
/// @return 带 `AudioSpectrumView:` 前缀的名称。
std::string AudioSpectrumView::getShaderName(
    const std::string& shaderModuleName)
{
    return "AudioSpectrumView:" + shaderModuleName;
}

/// @brief 清空缓存的 shader 源码。
///
/// 皮肤热切换后旧路径和二进制不再有效，下一次管线请求会从新 Canvas
/// 配置重新读取。
/// 该操作不直接销毁管线或 GPU shader module，资源重建由渲染器生命周期负责。
/// 空缓存状态允许重复调用，不需要额外脏标志。
/// 调用方应在皮肤版本切换边界执行，而不是依赖 shader 名称变化自动淘汰旧条目。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void AudioSpectrumView::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
}

/// @brief 获取本帧频谱 Quad 顶点数组。
/// @return 由 update 重建、命令录制期间有效的只读引用。
const std::vector<Graphic::Vertex::VKBasicVertex>&
AudioSpectrumView::getVertices() const
{
    return m_vertices;
}

/// @brief 获取本帧频谱 Quad 索引数组。
/// @return 与顶点和 DrawCmd 对应的只读索引引用。
const std::vector<uint32_t>& AudioSpectrumView::getIndices() const
{
    return m_indices;
}

/// @brief 录制本帧可见频谱纹理块的索引绘制命令。
/// @param cmdBuf 当前离屏渲染命令缓冲。
/// @param pipelineLayout 已绑定图形管线的布局。
/// @param setLayout 频谱纹理描述符集合布局。
/// @param defaultDescriptor 无纹理命令使用的默认描述符。
/// @param frameIndex 当前帧槽索引；本实现无需使用。
///
/// DrawCmd 顺序与 buildChannelGeometry 追加 Quad
/// 的索引顺序一致。每条命令选择自己的 VKTexture
/// 描述符，只有描述符相对上一条变化时才重新绑定，以减少连续块状态切换。
///
/// 顶点和索引缓冲由 IRenderableView 基础流程上传；本回调只绑定 set 0
/// 描述符并调用 drawIndexed，不创建资源、不分配容器，也不等待 GPU。
/// @warning 渲染命令录制热路径：不得阻塞、文件 IO 或修改纹理生命周期集合。
void AudioSpectrumView::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                         vk::PipelineLayout      pipelineLayout,
                                         vk::DescriptorSetLayout setLayout,
                                         vk::DescriptorSet defaultDescriptor,
                                         uint32_t          frameIndex)
{
    // 当前 DrawCmd 不使用帧槽特有资源。
    (void)frameIndex;
    // 描述符池由活动 VKRenderer 持有，纹理据此取得兼容描述符。
    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    // 空句柄确保第一条命令一定绑定自己的或默认描述符。
    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    for ( const auto& cmd : m_spectrumDrawCmds ) {
        // 缺失纹理时保持管线使用调用方默认描述符。
        vk::DescriptorSet descriptor = defaultDescriptor;
        if ( cmd.texture ) {
            // 活动或退休纹理在本帧命令录制期间仍保持存活。
            descriptor = cmd.texture->getNativeDescriptorSet(pool, setLayout);
        }

        if ( descriptor != lastBound ) {
            // 仅纹理切换时更新 set 0，连续相同描述符可复用绑定。
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &descriptor,
                                      0,
                                      nullptr);
            lastBound = descriptor;
        }

        // indexOffset 指向本 Quad 六索引片段，实例数固定为一。
        cmdBuf.drawIndexed(cmd.indexCount, 1, cmd.indexOffset, 0, 0);
    }
}

}  // namespace MMM::UI
