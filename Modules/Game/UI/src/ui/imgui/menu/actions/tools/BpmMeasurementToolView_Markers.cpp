#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

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
    m_audioChapters.clear();
    if ( !markers.success ) {
        m_markerStatus = markers.error;
        return false;
    }
    /// 只有后台结果已完成才接收章节所有权。
    /// 新轨道章节列表取代旧草稿，不把不同音频的相同名称合并。
    /// 有章节无 BPM 的情况保持现有自动测量入口可用。
    m_audioChapters = std::move(markers.data.chapters);
    if ( markers.data.bpmSegments.empty() ) {
        if ( !m_audioChapters.empty() )
            m_markerStatus =
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
    m_markerStatus = TR("ui.tools.bpm_measure.markers_restored").toString();
    m_statusText   = TR("ui.tools.bpm_measure.ready").toString();
    return true;
}

/// @brief 轮询文件导出结果并更新可见状态。
/// @details 文件线程只返回值语义结果，不回调已关闭的工具。
/// 使用零超时检查，导出期间音频和 UI 继续运行。
/// 成功状态区分内嵌和配套模式，后者明确显示配套路径。
/// 所有字符串提交都在 UI 所属线程完成。
/// @warning 每次 UI 更新调用；不得新增文件探测或阻塞 future 等待。
void BpmMeasurementToolView::consumeMarkerExport()
{
    // 零超时只轮询完成状态，不阻塞逻辑、播放和绘制线程。
    if ( !m_markerExportFuture.valid() ||
         m_markerExportFuture.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    /// ready 已确认任务完成，此处 get 不会等待编码或文件 IO。
    /// future 被消费后允许下一次导出，任务失败也不锁死按钮。
    /// 标记状态只属于工具，不改变项目脏标志。
    const auto result = m_markerExportFuture.get();
    if ( !result.success ) {
        m_markerStatus = result.error;
    } else if ( result.sidecarPath.empty() ) {
        m_markerStatus = TR("ui.tools.bpm_measure.markers_exported").toString();
    } else {
        // 配套路径必须明确展示，用户分享时才能带上完整标记。
        m_markerStatus = TR_FMT("ui.tools.bpm_measure.markers_sidecar",
                                Config::pathToUtf8(result.sidecarPath));
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
    if ( !source || !pool || m_markerExportFuture.valid() ) return;
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
        options.outputPath += m_markerExportFormat == 1 ? ".wav"
                              : m_markerExportFormat == 2
                                  ? ".mp3"
                                  : Config::pathToUtf8(source->extension());
    }
    /// 整拍生成只改变外部可见标记，不扩大精确 BPM 序列。
    /// 章节名称来源于草稿，导出不会把试听状态写进元数据。
    /// 用户命名的章节在同位置优先于自动生成的段落名。
    options.includeBeats  = m_exportBeatMarkers;
    options.data.chapters = m_audioChapters;
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
    m_markerStatus = TR("ui.tools.bpm_measure.markers_exporting").toString();
    // 任务仅捕获值语义快照，关闭工具或切换项目不影响导出生命周期。
    m_markerExportFuture = pool->enqueue([options = std::move(options)] {
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
    const std::string ext = m_markerExportFormat == 1 ? ".wav"
                            : m_markerExportFormat == 2
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
            m_markerStatus =
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
    // 原格式是默认选项，不根据可用编码器偷偷改变文件类型。
    // 其他格式只在保存时显式选择，相关耗时由后台任务承担。
    // 下拉框本身不建立音频解码器，也不查询磁盘格式。
    const char* formats[] = {
        TR("ui.tools.bpm_measure.markers_original_format").data(),
        "WAV",
        "MP3"
    };
    FeedbackCombo(TR("ui.tools.bpm_measure.markers_format").data(),
                  &m_markerExportFormat,
                  formats,
                  3);
    FeedbackCheckbox(TR("ui.tools.bpm_measure.markers_beats").data(),
                     &m_exportBeatMarkers);
    ImGui::TextWrapped("%s", TR("ui.tools.bpm_measure.markers_help").data());
    const bool busy = m_markerExportFuture.valid() ||
                      m_analysisRunning.load(std::memory_order_relaxed) ||
                      m_waveTimes.empty();
    /// 波形尚未完成时不能把旧音频的测量结果导出到新轨道。
    /// 导出期间按钮不可重复提交，普通章节编辑仍保持可用。
    /// 启用状态只读已发布的分析标志，不等待分析线程锁。
    ImGui::BeginDisabled(busy);
    if ( FeedbackButton(TR("ui.tools.bpm_measure.markers_export").data(),
                        ImVec2(-1.0f, 0.0f)) )
        openMarkerExportPicker();
    ImGui::EndDisabled();
    if ( !m_markerStatus.empty() )
        ImGui::TextWrapped("%s", m_markerStatus.c_str());
    if ( ImGui::CollapsingHeader(
             TR("ui.tools.bpm_measure.markers_chapters").data()) ) {
        // 章节位置以音频秒数展示，不把波形视觉偏移写入文件。
        // 此编辑仅修改工具草稿，导出前才冻结；不会改原始音频资源。
        if ( FeedbackButton(TR("ui.tools.bpm_measure.markers_add").data()) ) {
            m_audioChapters.push_back(
                { std::max(0.0, m_viewCenter - waveformCanvasOffset()),
                  TR_FMT("ui.tools.bpm_measure.marker_chapter_title",
                         m_audioChapters.size() + 1) });
        }
        /// 章节列表保留固定视口，避免大量名称撑大工具窗口。
        /// clipper 的固定行高与同一行时间、名称、删除控件保持一致。
        /// 只遍历可见行，章节文件读取不属于绘制职责。
        ImGui::BeginChild("##AudioChapterList", ImVec2(0.0f, 180.0f));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_audioChapters.size()),
                      ImGui::GetFrameHeightWithSpacing());
        while ( clipper.Step() ) {
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) {
                auto& chapter = m_audioChapters[static_cast<std::size_t>(i)];
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
                if ( FeedbackSmallButton("X") ) {
                    /// 当前 vector 擦除使后续下标变化，必须立即结束本轮裁剪。
                    /// 删除仅作用于命名草稿，不删除对应 BPM 段或音频文件。
                    /// 下一帧重新计算裁剪范围，避免使用旧 DisplayEnd 越界。
                    m_audioChapters.erase(m_audioChapters.begin() + i);
                    ImGui::PopID();
                    clipper.End();
                    break;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
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
}  // namespace MMM::UI
