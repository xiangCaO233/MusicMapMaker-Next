#include "BpmMeasurementToolView_Domains.h"
#include "config/EditorSettings.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include "common/platform/DocumentStorage.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/TranslationFormat.h"
#include "imgui.h"
#include "runtime/AppThreadPool.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ice/thread/ThreadPool.hpp>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 内置文件选择器的固定标识，不随本地化标题改变。
constexpr const char* MARKER_EXPORT_DIALOG = "BpmAudioMarkerExport";
/// @brief 独立段落文件导入选择器的固定标识。
constexpr const char* BPM_FILE_IMPORT_DIALOG = "BpmSegmentFileImport";
/// @brief 独立段落文件导出选择器的固定标识。
constexpr const char* BPM_FILE_EXPORT_DIALOG = "BpmSegmentFileExport";
}  // namespace

/// @brief 将后台读取的音频标记恢复到测量草稿。
/// @param result 当前选中轨道的后台分析快照。
/// @return true 表示 BPM 来自保存的数据，后续自动估算应被跳过。
/// @details 章节可以独立存在，没有 BPM 时仍允许继续自动测量。
/// 首拍相位只叠加画布偏移，不乘播放倍速。
/// 恢复后的 BPM 段保留原精度，不再次归一化或拟合。
/// 视野跳到首段附近，节拍调度器随新时间锚点重置。
/// 失败原因显示在标记面板，不阻止普通波形分析。
/// @param result 的标记数据可被移动，调用后不再复用章节列表。
bool BpmMeasurementToolView::restoreAudioMarkers(AnalysisResult& result)
{
    if ( !result.markers ) return false;
    auto& markers = *result.markers;
    // 即使新文件没有标记，也必须清掉上一音频的命名章节。
    m_markers->m_audioChapters.clear();
    if ( !markers.success ) {
        m_markers->m_markerStatus = markers.error;
        return false;
    }
    /// 只有后台结果已完成才接收章节所有权。
    /// 新轨道章节列表取代旧草稿，不把不同音频的相同名称合并。
    /// 有章节无 BPM 的情况保持现有自动测量入口可用。
    m_markers->m_audioChapters = std::move(markers.data.chapters);
    if ( markers.data.bpmSegments.empty() ) {
        if ( !m_markers->m_audioChapters.empty() )
            m_markers->m_markerStatus =
                TR("ui.tools.bpm_measure.markers_restored").toString();
        return false;
    }
    /// 精确保存的 BPM 优先于波形估算。
    /// 清空旧列表后一次性提交新锚点，避免新旧段交叉。
    /// 排序和合法性已在文件边界完成，此处只做时域转换。
    m_timingSegments.clear();
    // 文件存原始音频时间，工具画布叠加当前波形视觉偏移。
    // 不能把试听倍速乘入首拍或 BPM，否则往返时会累计改变音频时间。
    const double offset = waveformCanvasOffset();
    for ( const auto& segment : markers.data.bpmSegments ) {
        m_timingSegments.push_back({ segment.seconds + offset, segment.bpm });
    }
    syncPrimaryTimingFieldsFromSegments();
    m_viewCenter = std::clamp(m_firstBeatTime, 0.0, playbackCanvasDuration());
    resetMetronomeScheduler(m_viewCenter);
    m_markers->m_markerStatus =
        TR("ui.tools.bpm_measure.markers_restored").toString();
    m_statusText = TR("ui.tools.bpm_measure.ready").toString();
    return true;
}

/// @brief 轮询文件导出结果并更新可见状态。
/// @details 文件线程只返回值语义结果，不回调已关闭的工具。
/// 使用零超时检查，导出期间音频和 UI 继续运行。
/// 成功状态区分内嵌和配套模式，后者明确显示配套路径。
/// AU XMP 始终随音频导出，独立展示路径以便用户一起分享。
/// 所有字符串提交都在 UI 所属线程完成。
/// @warning 每次 UI 更新调用；不得新增文件探测或阻塞 future 等待。
void BpmMeasurementToolView::consumeMarkerExport()
{
    // 零超时只轮询完成状态，不阻塞逻辑、播放和绘制线程。
    if ( !m_markers->m_markerExportFuture.valid() ||
         m_markers->m_markerExportFuture.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    /// ready 已确认任务完成，此处 get 不会等待编码或文件 IO。
    /// future 被消费后允许下一次导出，任务失败也不锁死按钮。
    /// 标记状态只属于工具，不改变项目脏标志。
    const auto result = m_markers->m_markerExportFuture.get();
    if ( !result.success ) {
        m_markers->m_markerStatus = result.error;
    } else if ( result.sidecarPath.empty() ) {
        m_markers->m_markerStatus =
            TR("ui.tools.bpm_measure.markers_exported").toString();
    } else {
        // 配套路径必须明确展示，用户分享时才能带上完整标记。
        m_markers->m_markerStatus =
            TR_FMT("ui.tools.bpm_measure.markers_sidecar",
                   Config::pathToUtf8(result.sidecarPath));
    }
    if ( result.success ) {
        // XMP 是 AU 的外部标记载体，内嵌成功也不能隐藏此配套路径。
        // 固定格式名不需要翻译；沿用成功提示所在区域，避免新增交互步骤。
        m_markers->m_markerStatus +=
            "\nAU XMP: " + Config::pathToUtf8(result.xmpPath);
    }
}

/// @brief 冻结测量数据并启动后台音频标记导出。
/// @param destination 用户保存选择器确认的新文件路径。
/// @details 选择原格式时保留原扩展名，显式 WAV 或 MP3 才允许转码。
/// 用户章节按名称保存，缺失名称的 BPM 起点补充自动章节。
/// 后台快照不包含 UI 对象、会话引用或工具生命周期指针。
/// 波形偏移在提交前移除，输出使用原始文件秒数。
/// 服务负责路径冲突、临时文件、格式回读及源文件保护。
/// 仅冻结一次，不在后台读取随后变化的面板状态。
/// @warning 显式导出低频入口，可能遍历测量段落但不读取音频样本。
void BpmMeasurementToolView::startMarkerExport(
    const std::filesystem::path& destination)
{
    const auto source = selectedAudioAbsolutePath();
    auto*      pool   = Runtime::AppThreadPool::instance().getFileThreadPool();
    if ( !source || !pool || m_markers->m_markerExportFuture.valid() ) return;
    /// 快照的每个字段独立于工具关闭和选中轨道切换。
    /// 文件路径按原生 path 保存，避免中文路径在 Windows 变成窄字符乱码。
    /// 现有任务未消费前拒绝重复排队，防止竞争相同的输出文件。
    Audio::AudioMarkerExportOptions options;
    options.inputPath  = *source;
    options.outputPath = destination;
    // 文件选择器可能只返回用户键入的主体名称。
    // 补扩展名由明确选项决定，不能假设所有声音都属于 MP3。
    // 已有扩展名保持用户指定，格式转换限制由服务执行。
    if ( options.outputPath.extension().empty() ) {
        // 保存选择器没有补扩展名时沿用显式选项，而不是误识别为未知容器。
        options.outputPath += m_markers->m_markerExportFormat == 1 ? ".wav"
                              : m_markers->m_markerExportFormat == 2
                                  ? ".mp3"
                                  : Config::pathToUtf8(source->extension());
    }
    /// 整拍生成只改变外部可见标记，不扩大精确 BPM 序列。
    /// 章节名称来源于草稿，导出不会把试听状态写进元数据。
    /// 用户命名的章节在同位置优先于自动生成的段落名。
    options.includeBeats  = m_markers->m_exportBeatMarkers;
    options.data.chapters = m_markers->m_audioChapters;
    const double offset   = waveformCanvasOffset();
    for ( std::size_t i = 0; i < m_timingSegments.size(); ++i ) {
        const auto&  segment = m_timingSegments[i];
        const double seconds = segment.timestampSeconds - offset;
        options.data.bpmSegments.push_back({ seconds, segment.bpm });
        // 用户的命名章节优先；没有名称的 BPM 段自动生成章节，便于外部软件查看。
        const bool named =
            std::any_of(options.data.chapters.begin(),
                        options.data.chapters.end(),
                        [seconds](const auto& chapter) {
                            return std::abs(chapter.seconds - seconds) < 1e-7;
                        });
        if ( !named )
            options.data.chapters.push_back(
                { seconds,
                  TR_FMT("ui.tools.bpm_measure.marker_segment_title",
                         i + 1,
                         segment.bpm) });
    }
    m_markers->m_markerStatus =
        TR("ui.tools.bpm_measure.markers_exporting").toString();
    // 任务仅捕获值语义快照，关闭工具或切换项目不影响导出生命周期。
    m_markers->m_markerExportFuture =
        pool->enqueue([options = std::move(options)] {
            return Audio::AudioMarkerService::exportFile(options);
        });
}

/// @brief 按全局偏好打开原生或内置保存选择器。
/// @details 默认名称追加 -marked，避免暗示可以覆盖原音频。
/// 原生选择器返回的内存必须用对应 NFD 接口释放。
/// 内置选择器保存固定 ID，展示由后续 UI 帧推进。
/// 两类选择器都只把确定的文件路径传给导出入口。
/// 用户取消时不提交文件任务，也不清除原有测量数据。
/// 筛选器只提示扩展名，最终格式合法性由服务独立校验。
/// @warning 显式用户命令才打开原生对话框，禁止每帧自动触发。
void BpmMeasurementToolView::openMarkerExportPicker()
{
    const auto source = selectedAudioAbsolutePath();
    if ( !source ) return;
    const std::string ext = m_markers->m_markerExportFormat == 1 ? ".wav"
                            : m_markers->m_markerExportFormat == 2
                                ? ".mp3"
                                : Config::pathToUtf8(source->extension());
    // 自动建议的新名称不修改来源 stem 或原资源登记信息。
    // 保存筛选器使用所选格式，但仍允许用户取消后继续测量。
    // 原格式选项只借用扩展名，不复制来源文件的绝对根位置到元数据。
    const std::string defaultName =
        Config::pathToUtf8(source->stem()) + "-marked" + ext;
    const std::string folder = Config::pathToUtf8(source->parent_path());
    const std::string suffix = ext.empty() ? "wav" : ext.substr(1);
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        PlayPopupOpenFeedback();
        nfdu8char_t*            path = nullptr;
        const nfdu8filteritem_t filter{ "Audio", suffix.c_str() };
        const auto              selected = NativeFileDialog::saveFile(
            &path, &filter, 1, folder.c_str(), defaultName.c_str());
        if ( selected == NFD_OKAY && path )
            startMarkerExport(Config::utf8ToPath(path));
        if ( path ) NFD_FreePathU8(path);
        if ( selected == NFD_ERROR )
            m_markers->m_markerStatus =
                TR("ui.tools.bpm_measure.markers_picker_failed").toString();
        return;
    }
    // 内置选择器跨帧展示，文件事务在用户确认后才启动。
    IGFD::FileDialogConfig config;
    config.path              = folder;
    config.fileName          = defaultName;
    config.countSelectionMax = 1;
    config.flags             = ImGuiFileDialogFlags_Modal;
    ImGuiFileDialog::Instance()->OpenDialog(
        MARKER_EXPORT_DIALOG,
        TR("ui.tools.bpm_measure.markers_export").toString(),
        ext.c_str(),
        config);
    PlayPopupOpenFeedback();
}

/// @brief 展示音频标记导出选项与命名章节草稿。
/// @details 输出任务忙碌时不能再次导出，已冻结任务不受选项变化影响。
/// 章节行通过 clipper 裁剪，不逐帧遍历完整音频或标记文件。
/// 位置输入使用原音频秒数，名称不承担 BPM 序列化职责。
/// 章节草稿与测量段列表独立；改变章节不改变音频内容。
/// 内置文件选择器的确认事件最终也进入后台事务。
/// @warning 每帧 UI 路径，不得执行磁盘读取、包遍历或等待后台任务。
void BpmMeasurementToolView::renderMarkerPanel()
{
    // 面板只展示工具已有数据，不在这里读音频标签。
    // 标签恢复随轨道分析一次执行，普通每帧刷新不重复导入。
    // 导出状态也由零超时轮询推进，频谱和播放不需等待。
    ImGui::Separator();
    ImGui::TextUnformatted(TR("ui.tools.bpm_measure.markers_title").data());
    // 导入可先于波形完成，导出需要当前音轨的测量数据。
    // 进度使用 future
    // 是否占用槽位判断，未确认文件目标时也避免重复开系统选择器。 .mmmt
    // 按钮独立于下方音频格式和整拍标记，选择 WAV 或 MP3 不影响段落文件。
    ImGui::BeginDisabled(m_selectedAudioTrackId.empty() ||
                         m_markers->m_bpmFileImport.valid() ||
                         m_markers->m_bpmFilePickerLifetime);
    if ( FeedbackButton(TR("ui.tools.bpm_measure.bpm_file_import").data(),
                        ImVec2(-1.0f, 0.0f)) )
        openBpmSegmentFilePicker(false);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(
        m_selectedAudioTrackId.empty() || m_waveTimes.empty() ||
        m_selectedAudioIdentityNeedsAnalysis ||
        m_job->m_analysisRunning.load(std::memory_order_relaxed) ||
        m_job->m_analysisFinished.load(std::memory_order_acquire) ||
        m_markers->m_bpmFileExport.valid() ||
        m_markers->m_bpmFilePickerLifetime);
    if ( FeedbackButton(TR("ui.tools.bpm_measure.bpm_file_export").data(),
                        ImVec2(-1.0f, 0.0f)) )
        openBpmSegmentFilePicker(true);
    ImGui::EndDisabled();
    // 原格式是默认选项，不根据可用编码器偷偷改变文件类型。
    // 其他格式只在保存时显式选择，相关耗时由后台任务承担。
    // 下拉框本身不建立音频解码器，也不查询磁盘格式。
    const char* formats[] = {
        TR("ui.tools.bpm_measure.markers_original_format").data(),
        "WAV",
        "MP3"
    };
    FeedbackCombo(TR("ui.tools.bpm_measure.markers_format").data(),
                  &m_markers->m_markerExportFormat,
                  formats,
                  3);
    FeedbackCheckbox(TR("ui.tools.bpm_measure.markers_beats").data(),
                     &m_markers->m_exportBeatMarkers);
    ImGui::TextWrapped("%s", TR("ui.tools.bpm_measure.markers_help").data());
    const bool busy =
        m_markers->m_markerExportFuture.valid() ||
        m_job->m_analysisRunning.load(std::memory_order_relaxed) ||
        m_waveTimes.empty();
    /// 波形尚未完成时不能把旧音频的测量结果导出到新轨道。
    /// 导出期间按钮不可重复提交，普通章节编辑仍保持可用。
    /// 启用状态只读已发布的分析标志，不等待分析线程锁。
    ImGui::BeginDisabled(busy);
    if ( FeedbackButton(TR("ui.tools.bpm_measure.markers_export").data(),
                        ImVec2(-1.0f, 0.0f)) )
        openMarkerExportPicker();
    ImGui::EndDisabled();
    if ( !m_markers->m_markerStatus.empty() )
        ImGui::TextWrapped("%s", m_markers->m_markerStatus.c_str());
    if ( ImGui::CollapsingHeader(
             TR("ui.tools.bpm_measure.markers_chapters").data()) ) {
        // 章节位置以音频秒数展示，不把波形视觉偏移写入文件。
        // 此编辑仅修改工具草稿，导出前才冻结；不会改原始音频资源。
        if ( FeedbackButton(TR("ui.tools.bpm_measure.markers_add").data()) ) {
            m_markers->m_audioChapters.push_back(
                { std::max(0.0, m_viewCenter - waveformCanvasOffset()),
                  TR_FMT("ui.tools.bpm_measure.marker_chapter_title",
                         m_markers->m_audioChapters.size() + 1) });
        }
        /// 章节列表保留固定视口，避免大量名称撑大工具窗口。
        /// clipper 的固定行高与同一行时间、名称、删除控件保持一致。
        /// 只遍历可见行，章节文件读取不属于绘制职责。
        ImGui::BeginChild("##AudioChapterList", ImVec2(0.0f, 180.0f));
        // 裁剪器整个遍历期间保持章节数量不变，删除请求在绘制结束后提交。
        int              chapterToDelete = -1;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_markers->m_audioChapters.size()),
                      ImGui::GetFrameHeightWithSpacing());
        while ( clipper.Step() ) {
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) {
                auto& chapter =
                    m_markers->m_audioChapters[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(90.0f);
                // 输入暂存值通过有限性检查后才更新章节。
                // 真实音频时间范围独立于视野范围，缩放不改变章节位置。
                // 导出时服务再次校验，UI 校验不能代替文件边界校验。
                double seconds = chapter.seconds;
                if ( ImGui::InputDouble(
                         "##Time", &seconds, 0.0, 0.0, "%.6fs") &&
                     std::isfinite(seconds) )
                    chapter.seconds = std::clamp(seconds, 0.0, m_duration);
                ImGui::SameLine();
                // 固定缓冲避免每帧动态分配；上限与服务标签预算一致。
                // 输入最长支持 4096 字节，与文件服务的名称预算一致。
                // 固定缓冲零初始化保证当前字符串短于缓冲时有结束符。
                // 仅发生实际编辑时回写字符串，未编辑的可见行不重新分配名称。
                std::array<char, 4097> title{};
                std::copy_n(chapter.title.data(),
                            std::min(chapter.title.size(), title.size() - 1),
                            title.data());
                ImGui::SetNextItemWidth(
                    std::max(40.0f, ImGui::GetContentRegionAvail().x - 50.0f));
                if ( ImGui::InputText("##Title", title.data(), title.size()) )
                    chapter.title = title.data();
                ImGui::SameLine();
                // 只登记当前下标，不使本轮 DisplayEnd 或章节引用提前失效。
                if ( FeedbackSmallButton("X") ) chapterToDelete = i;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        // Step 返回 false 已完成裁剪器收尾，之后不再手动 End 或继续 Step。
        // 擦除只改变章节草稿，不删除对应 BPM 段或原音频文件。
        if ( chapterToDelete >= 0 )
            m_markers->m_audioChapters.erase(
                m_markers->m_audioChapters.begin() + chapterToDelete);
    }
    // 跨帧对话框只在确认时读取文件路径。
    // 取消和关闭均结束选择器，不修改原音频或章节草稿。
    // 路径确认后文件操作仍经线程池，不在 Display 内同步写音频。
    if ( ImGuiFileDialog::Instance()->Display(MARKER_EXPORT_DIALOG) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() )
            startMarkerExport(Config::utf8ToPath(
                ImGuiFileDialog::Instance()->GetFilePathName()));
        ImGuiFileDialog::Instance()->Close();
    }
}
/// @brief 读取独立段落，绑定当前音轨身份以拒绝切歌后的迟到结果。
/// @param path 用户投放或文件选择器确认的路径，可能是 SAF 工作副本。
/// @details 独立文件无需重扫音频；只有结果提交时才把音频秒数变为画布秒数。
/// 任务保持单槽，避免先后读取的两个文件以相反顺序替换同一个草稿。
/// 读取失败或音轨已换都不生成历史；切换检查由 UI 消费结果统一执行。
/// 使用应用文件池保证即使文件来自较慢的移动存储也不阻塞波形绘制。
/// @warning 低频输入入口；当前轨道变化时可刷新绝对路径身份。
void BpmMeasurementToolView::importBpmSegmentFile(
    const std::filesystem::path& path)
{
    // 已有导入即使读取完成也可能在等波形结果，不能直接覆盖其交付槽。
    if ( m_markers->m_bpmFileImport.valid() ) return;
    // 系统投放先于视图本帧更新，需要先确认所选资源依然属于当前项目。
    refreshSelectedAudioIdentity();
    if ( m_selectedAudioSyncKey.empty() ) {
        m_markers->m_markerStatus =
            TR("ui.tools.bpm_measure.bpm_file_select_audio").toString();
        return;
    }
    auto* pool = Runtime::AppThreadPool::instance().getFileThreadPool();
    if ( !pool ) return;
    // 使用规范化绝对路径区分同名音轨，避免不同项目的资源 ID 偶然相同。
    // 路径身份由当前 UI 持有，读取任务不访问工具或项目资源表。
    m_markers->m_bpmFileImportAudioKey = m_selectedAudioSyncKey;
    m_markers->m_markerStatus =
        TR("ui.tools.bpm_measure.bpm_file_importing").toString();
    m_markers->m_bpmFileImport = pool->enqueue(
        [path] { return Audio::AudioMarkerService::readBpmSegmentFile(path); });
}

/// @brief 以音频相对时间冻结段落，异步写出并发布到选定路径。
/// @param path 保存选择器确认的 .mmmt 目标，授权文档保留选择器返回的完整路径。
/// @details 冻结后继续修改或关闭测量工具都不会影响写出内容。
/// 音频章节和整拍标记只属于已有音频导出选项，不写入独立段落。
/// 最终成功要求目标文件写入及文档发布都通过，取消的选择不产生工作项。
/// @warning 显式操作，仅复制段落快照；校验、编码及发布都在文件线程完成。
void BpmMeasurementToolView::exportBpmSegmentFile(
    const std::filesystem::path& path)
{
    auto* pool = Runtime::AppThreadPool::instance().getFileThreadPool();
    if ( !pool || m_markers->m_bpmFileExport.valid() ) return;

    Audio::AudioMarkerData data;
    // 音频相对时间可跨主题和视觉偏移往返，不能乘试听倍速。
    const double offset = waveformCanvasOffset();
    data.bpmSegments.reserve(m_timingSegments.size());
    for ( const auto& segment : m_timingSegments )
        data.bpmSegments.push_back(
            { segment.timestampSeconds - offset, segment.bpm });

    // 任务开始后使用自己的状态提示，避免将独立段落误报为音频编码。
    m_markers->m_markerStatus =
        TR("ui.tools.bpm_measure.bpm_file_exporting").toString();
    // 捕获只读的序列和目标路径，池任务不越过 UI 线程读取视图成员。
    m_markers->m_bpmFileExport = pool->enqueue([path, data = std::move(data)] {
        auto error = Audio::AudioMarkerService::writeBpmSegmentFile(path, data);
        if ( error.empty() ) {
            // SAF 工作副本写入后还需向系统选中的文档发布。
            if ( !Common::publishDocument(path, error) && error.empty() )
                error = "无法发布 BPM 段落文件";
        } else {
            // SAF 的保存命令可能已在外部创建占位文档。
            // 丢弃接口只清理为本次输出创建的空文档，桌面路径不受影响。
            std::string cleanupError;
            (void)Common::discardUnwrittenDocument(path, cleanupError);
        }
        return error;
    });
}

/// @brief 使用编辑器文件选择偏好打开独立段落的导入或保存选择器。
/// @param exporting 为真时确认保存目标，否则选择一份已有 .mmmt。
/// @details Android 缺少直读授权时经系统选择器取得文档权限。
/// 选择器从当前音频目录开始，默认名称借用音频名便于辨认。
/// 原生和内置选择器共用后台文件入口，取消不会修改测量草稿。
/// @pre 选择发起时工具已有选中的项目音轨。
/// @warning 显式点击路径，允许询问平台权限及打开文件选择器。
void BpmMeasurementToolView::openBpmSegmentFilePicker(bool exporting)
{
    const auto source = selectedAudioAbsolutePath();
    // 选择器回调只在当前窗口存活且前一选择器任务已交回时可重新发起。
    // 图形系统异步返回文件路径，窗口重新注册后不会继承旧令牌。
    // 令牌跨回调保活，实际章节和历史数据仍只由 UI 线程修改。
    // 新文件请求使用本次音轨路径快照，切歌不改变已提交文件任务的来源。
    // 退出只销毁观察令牌，独立文件任务可以把结果安全地留在 future 中。
    // 该槽只限制选择器实例，不能把后台标记导出误认为仍有选择器打开。
    if ( !source || m_markers->m_bpmFilePickerLifetime ) return;
    const auto folder = Config::pathToUtf8(source->parent_path());
    // 建议名称只用于填充选择器，最终保存路径以用户选择为准。
    const auto name = Config::pathToUtf8(source->stem()) + ".mmmt";
    const auto style =
        Config::AppConfig::instance().getEditorSettings().filePickerStyle;
    PlayPopupOpenFeedback();
    if ( NativeFileDialog::requiresSystemPicker() ||
         style == Config::FilePickerStyle::Native ) {
        // 桌面可能同步完成，Android 会跨帧持有回调，先建立同一个存活令牌。
        // 令牌不拥有视图，项目关闭后不能借文件选择回调延长 UI 生命周期。
        m_markers->m_bpmFilePickerLifetime = std::make_shared<bool>(true);
        const auto completed               = [this,
                                exporting,
                                lifetime = std::weak_ptr<bool>(
                                    m_markers->m_bpmFilePickerLifetime)](
                                   nfdresult_t        result,
                                   const std::string& path) {
            if ( lifetime.expired() ) return;
            // 所有返回状态都解除选择器占用；取消保留当前段落和旧提示。
            m_markers->m_bpmFilePickerLifetime.reset();
            if ( result == NFD_OKAY && !path.empty() ) {
                if ( exporting )
                    exportBpmSegmentFile(Config::utf8ToPath(path));
                else
                    importBpmSegmentFile(Config::utf8ToPath(path));
            } else if ( result != NFD_CANCEL ) {
                m_markers->m_markerStatus =
                    TR("ui.tools.bpm_measure.bpm_file_picker_error").toString();
            }
        };
        // 系统选择器只在启动阶段借用过滤器和建议名，回调接收独立路径值。
        const nfdu8filteritem_t filter{ "MusicMapMaker BPM", "mmmt" };
        if ( exporting )
            NativeFileDialog::saveFileAsync(
                &filter, 1, folder.c_str(), name.c_str(), completed);
        else
            NativeFileDialog::openFileAsync(
                &filter, 1, folder.c_str(), completed);
        return;
    }

    // 内置模式仍使用统一的弹窗实例，以稳定的导入和导出 ID 区分处理结果。
    // 不填充导入文件名，避免用户确认时误用默认导出名。
    IGFD::FileDialogConfig config;
    config.path              = folder;
    config.fileName          = exporting ? name : "";
    config.countSelectionMax = 1;
    config.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    // 写入服务可以替换已有文件，因此必须在选择阶段得到覆盖确认。
    if ( exporting ) config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
    ImGuiFileDialog::Instance()->OpenDialog(
        exporting ? BPM_FILE_EXPORT_DIALOG : BPM_FILE_IMPORT_DIALOG,
        TR(exporting ? "ui.tools.bpm_measure.bpm_file_export"
                     : "ui.tools.bpm_measure.bpm_file_import")
            .data(),
        ".mmmt",
        config);
}

/// @brief 处理独立文件的选择确认与完成结果。
/// @param projectReady 当前工程已完成切换，所选音轨身份可以使用。
/// @details 选择器关闭与任务完成分开处理，后者可以经过多次 UI 帧。
/// 导入优先等音频内嵌标记恢复完成，再用用户显式选择的段落替换时间数据。
/// 工程过渡期不读取变更中的资源身份；待工程稳定后再检查导入能否沿用。
/// 文件导出只冻结了数值和目标，可以在工程过渡期间消费最终写入状态。
/// 写入完成不引用音轨状态，用户可以在生成期间继续其他编辑。
/// @warning UI 热路径，仅零超时轮询，禁止等待分析或文件任务。
void BpmMeasurementToolView::consumeBpmSegmentFileOperations(bool projectReady)
{
    auto* dialog = ImGuiFileDialog::Instance();
    // 控制栏和波形在移动端可以分开显示，对话框需要独立于当前面板推进。
    // 两个候选 ID 固定，未打开的项不会进入 Display。
    for ( const bool exporting : { false, true } ) {
        const auto* key =
            exporting ? BPM_FILE_EXPORT_DIALOG : BPM_FILE_IMPORT_DIALOG;
        // 展示仅处理当前打开的 ID，不消费其他工具使用的全局选择器。
        if ( !dialog->IsOpened(key) || !dialog->Display(key) ) continue;
        // 关闭不能推迟到 projectReady 恢复后，否则旧模态会继续拦截输入。
        if ( dialog->IsOk() && projectReady ) {
            const auto path = Config::utf8ToPath(dialog->GetFilePathName());
            if ( exporting )
                exportBpmSegmentFile(path);
            else
                importBpmSegmentFile(path);
        }
        dialog->Close();
    }
    if ( m_markers->m_bpmFileExport.valid() &&
         m_markers->m_bpmFileExport.wait_for(std::chrono::seconds(0)) ==
             std::future_status::ready ) {
        auto error = m_markers->m_bpmFileExport.get();
        m_markers->m_markerStatus =
            error.empty()
                ? TR("ui.tools.bpm_measure.bpm_file_exported").toString()
                : TR_FMT("ui.tools.bpm_measure.bpm_file_failed", error);
    }
    if ( !projectReady || !m_markers->m_bpmFileImport.valid() ||
         m_markers->m_bpmFileImport.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;

    // 同一轨道的标记分析可能尚未完成，等它的结果提交后再应用显式导入。
    // 若已换歌，直接丢弃先前文件，不能等新音轨跑完再回填旧结果。
    const bool sameAudio =
        m_markers->m_bpmFileImportAudioKey == m_selectedAudioSyncKey;
    // 分析尚未启动、正在解码或已经交付但未提交时，保留已经完成的文件快照。
    if ( sameAudio && (m_selectedAudioIdentityNeedsAnalysis ||
                       hasAnalysisPendingForImport()) )
        return;
    // get 释放唯一任务槽，下一次投放或文件选择即可提交新的导入。
    auto result = m_markers->m_bpmFileImport.get();
    if ( !sameAudio ) {
        m_markers->m_markerStatus =
            TR("ui.tools.bpm_measure.bpm_file_audio_changed").toString();
        return;
    }
    if ( !result.success ) {
        // 完整性校验失败连同历史一起保持原状，允许用户换文件重试。
        m_markers->m_markerStatus =
            TR_FMT("ui.tools.bpm_measure.bpm_file_failed", result.error);
        return;
    }

    // 音频章节仍归原音轨所有；只转换独立文件内的段落到当前画布。
    // 不使用鼠标拖动的限幅，原有负首拍和超出当前视野的段落需要保留。
    // 恢复时使用当前波形偏移，因为导出工具可来自另一个主题或设备。
    std::vector<BpmTimingSegment> segments;
    segments.reserve(result.data.bpmSegments.size());
    const double offset = waveformCanvasOffset();
    for ( const auto& segment : result.data.bpmSegments )
        segments.push_back({ segment.seconds + offset, segment.bpm });
    // 合并成一条用户操作，成功后可恢复此前全部测量而无需重新读取文件。
    replaceImportedTimingSegments(std::move(segments));
    // 复用红线导入的完成提示，篇幅不随数值精度或段落数量增加。
    // 复用已有红线导入提示，反馈集中显示在导出标记面板。
    m_markers->m_markerStatus =
        TR_FMT("ui.tools.bpm_measure.import_done", m_timingSegments.size());
}
}  // namespace MMM::UI
