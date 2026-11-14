#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/Project.h"
#include "mmm/sample/AudioSample.h"
#include "mmm/timing/BpmNormalization.h"
#include "mmm/timing/TimingEffect.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include "ui/workspace/IWorkspacePresentation.h"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file BpmMeasurementToolView_Import.cpp
/// @brief 从项目关联谱面读取红线的低频导入边界与缓存选择弹窗。
/// @details 本文件不负责应用到谱面；导入结果仍由工具原有应用流程提交。
namespace MMM::UI
{
namespace
{
/// @brief 项目入口的相对路径以项目根解析，绝对路径保持身份，不访问文件系统。
/// @param project 当前项目，根目录由打开项目的入口确定。
/// @param path 持久化谱面入口或已经打开会话的路径。
/// @return 可用于比较来源身份的绝对规范路径。
std::filesystem::path importBeatmapPath(const Project&               project,
                                        const std::filesystem::path& path)
{
    // 词法规范化只消除点段，不在确认弹窗每帧查询磁盘或跟随符号链接。
    return (path.is_absolute() ? path : project.m_projectRoot / path)
        .lexically_normal();
}
/// @brief 解析真正的歌曲关联，不使用预览接口的项目默认资源兜底。
/// @param project 提供资源 ID、路径和 Main 类型的当前项目。
/// @param map 只读来源，歌曲提示优先于最早的 Main 自动采样。
/// @param path 来源路径，用于按谱面目录解析相对音频引用。
/// @return 项目表内的观察指针，无有效歌曲关联时为空。
/// @warning 仅在导入低频读取调用，索引建立及采样遍历禁止放入绘制帧。
const AudioResource* boundSongResource(const Project&               project,
                                       const BeatMap&               map,
                                       const std::filesystem::path& path)
{
    // 显式歌曲提示允许引用任意资源类型，它比采样推断更接近作者意图。
    // 旧 main_audio_path 仅在新提示为空时兼容，不在新提示失效时越级使用。
    const auto& meta = map.m_baseMapMetadata;
    const auto& hint = meta.song_file_hint.empty() ? meta.main_audio_path
                                                   : meta.song_file_hint;
    if ( const auto* resource =
             Logic::ProjectResourceService::findAudioResourceForReference(
                 project, path, Config::pathToUtf8(hint)) )
        return resource;
    // 批量解析复用一次资源索引，避免每条采样重建索引。
    // string_view 只借用本次只读模型，不存入弹窗或跨越模型生命周期。
    std::vector<std::string_view> references;
    references.reserve(map.m_audioSamples.size());
    for ( const auto& sample : map.m_audioSamples )
        references.push_back(sample.m_audioResourceId);
    const auto resolved =
        Logic::ProjectResourceService::resolveAudioResourceReferences(
            project, path, references);
    const AudioResource* earliest  = nullptr;
    double               timestamp = std::numeric_limits<double>::infinity();
    // Main 采样才表示歌曲候选；仅含按键音的谱面不能被推断为绑定歌曲。
    // 起点包含采样偏移，并允许负值；相同时刻保持来源中的先后次序。
    for ( std::size_t i = 0; i < resolved.size(); ++i ) {
        const auto*  resource = resolved[i];
        const double time     = map.m_audioSamples[i].effectiveTimestamp();
        if ( resource && resource->m_type == AudioTrackType::Main &&
             std::isfinite(time) && time < timestamp ) {
            earliest  = resource;
            timestamp = time;
        }
    }
    // 没有引用就没有关联，不借用项目首资源为它制造绑定。
    // 返回指针仅供当前 makeOption 比较 ID，不会被缓存。
    return earliest;
}
}  // namespace

/// @brief 读取当前音轨关联谱面的红线快照，未关联或无法加载时不构造候选。
/// @param path 当前项目中谱面的绝对或相对路径。
/// @return 可选来源及校验错误；已有来源即使无有效红线也显示具体拒绝原因。
/// @details 已打开谱面从 ECS 读取未保存的红线，不能信任尚未同步的源模型
/// Timing。
/// @warning 用户请求低频路径，文件加载及会话锁不得移入弹窗常态绘制。
std::optional<BpmMeasurementToolView::BeatmapImportOption>
BpmMeasurementToolView::readBoundBeatmapTimings(
    const std::filesystem::path& path) const
{
    auto&       engine  = Logic::EditorEngine::instance();
    const auto* project = engine.getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) return std::nullopt;
    // 项目资源表与窗口音轨 ID 属于 UI 侧当前上下文，二者缺一不可。
    // 不在这里自动创建资源或选择另一音轨，避免改变用户正在测量的歌曲。
    const auto absolute = importBeatmapPath(*project, path).lexically_normal();
    // 绑定沿用项目引用解析规则，兼容歌曲提示、Main 采样以及历史相对路径。
    // 不根据文件名相似、项目资源顺序或旧 BeatmapEntry 单音轨字段猜测来源。
    const auto makeOption =
        [&](const BeatMap& map) -> std::optional<BeatmapImportOption> {
        const auto* resource = boundSongResource(*project, map, absolute);
        // 关联依据是资源身份而非显示名；同名文件可能来自不同子目录。
        // Main 音轨解析失败时不能把任意伴奏或按键音当成测量音乐。
        if ( !resource || resource->m_id != m_selectedAudioTrackId )
            return std::nullopt;
        // 候选保存值快照，不持有会话指针或资源表元素引用。
        // 弹窗存续期间允许关闭谱面和切换项目，确认时会重新验证来源。
        BeatmapImportOption option;
        option.m_path  = absolute;
        option.m_label = map.m_baseMapMetadata.name;
        if ( !map.m_baseMapMetadata.version.empty() )
            option.m_label += " [" + map.m_baseMapMetadata.version + "]";
        // 难度名并不唯一，附带项目入口路径以便区分同名谱面。
        // 路径只参与标签和身份检查，不作为格式化字符串传给 UI。
        option.m_label += " · " + Config::pathToUtf8(path);
        return option;
    };
    // 红线曲线无法用工具当前离散段落表示；完整拒绝比悄悄取起点 BPM 更安全。
    // 非有限时间、越界 BPM 也不能通过钳制伪装为成功导入。
    /// @brief 统一校验模型毫秒转秒后和 ECS 原生秒值的红线数据。
    /// @details 错误留在候选中显示原因，任何一个错误都禁止整份导入。
    /// @param option 本次读取的值语义候选，不引用工具当前测量列表。
    /// @param seconds 已转换到工具单位的原始时间。
    /// @param bpm 红线效果参数，不从绿线参数或参考拍长反推。
    /// @param curve 为真表示来源包含当前工具无法表达的插值。
    const auto append = [](BeatmapImportOption& option,
                           double               seconds,
                           double               bpm,
                           bool                 curve) {
        if ( curve )
            option.m_error = TR("ui.tools.bpm_measure.import_curve").data();
        else if ( !std::isfinite(seconds) || !std::isfinite(bpm) ||
                  bpm < MIN_NORMALIZED_BPM || bpm > MAX_NORMALIZED_BPM )
            option.m_error = TR("ui.tools.bpm_measure.import_invalid").data();
        // 错误候选的载荷不会提交，仍保留数量用于诊断；不部分吞掉坏点。
        // 红线重复时间是来源本身的数据，不在读取阶段替用户修正。
        option.m_segments.push_back({ seconds, bpm });
    };
    /// @brief 为有效快照建立时间顺序，并保留无红线来源的明确错误。
    /// @details 稳定排序保持同一时间多个事件的来源顺序。
    /// @param option 完整收集后的候选，按值移交并避免保留临时模型引用。
    /// @return 带有排序后数据或明确拒绝原因的候选。
    /// @warning 只在低频来源读取末尾排序，不在弹窗绘制时重复执行。
    const auto finish = [](BeatmapImportOption option) {
        // 只改变顺序，不合并邻近时间、不钳制负值或音频结束后的原始红线。
        // 无效数据不会参与排序，确认入口会根据错误原子拒绝整个导入。
        if ( option.m_error.empty() )
            std::stable_sort(option.m_segments.begin(),
                             option.m_segments.end(),
                             [](const auto& a, const auto& b) {
                                 return a.timestampSeconds < b.timestampSeconds;
                             });
        // 无红线和没有关联谱面是不同情况：前者仍可在来源列表中解释。
        // 不合成默认段落，防止用户误以为已经读取到了谱面 BPM。
        if ( option.m_segments.empty() )
            option.m_error = TR("ui.tools.bpm_measure.import_no_bpm").data();
        return option;
    };
    {
        // 显式请求才持锁；快照复制保证会话在本次读取期间不会被关闭销毁。
        // 路径身份用于匹配项目入口，不能用可能重排的标签序号作为来源。
        std::lock_guard lock(engine.getSessionMutex());
        const auto      entries = engine.getSessionEntries();
        for ( const auto& entry : entries ) {
            // 占位画布没有实际谱面，不能由其默认元数据产生候选。
            // 会话列表快照仍在锁作用域中使用，不泄露到渲染帧。
            if ( entry.isLogoPlaceholder || !entry.session ) continue;
            const auto& ctx = entry.session->getContext();
            if ( !ctx.currentBeatmap ) continue;
            // 打开的模型路径可能已绝对化；与项目相对入口按同一规则解析。
            // 只匹配路径而不匹配标题，防止不同谱面共用名称时读错内存。
            const auto openPath =
                importBeatmapPath(
                    *project, ctx.currentBeatmap->m_baseMapMetadata.map_path)
                    .lexically_normal();
            if ( openPath != absolute ) continue;
            auto option = makeOption(*ctx.currentBeatmap);
            // 已打开来源即使改成其它音轨，也不能回退磁盘旧绑定来错误匹配。
            if ( !option ) return std::nullopt;
            // 时间线编辑的权威数据是 ECS，源 BeatMap 只提供绑定和标签。
            // 未保存红线已可能不同于磁盘，不能从源模型重新导入旧时间。
            const auto timings =
                ctx.timelineRegistry.view<const Logic::TimelineComponent>();
            for ( const auto entity : timings ) {
                const auto& timing =
                    timings.get<const Logic::TimelineComponent>(entity);
                // 红线类型由 effect 明确判定；绿线的值即使看似 BPM 也排除。
                // ECS 时间已为秒，不能再次除以一千。
                if ( timing.m_effect == TimingEffect::BPM )
                    append(*option,
                           timing.m_timestamp,
                           timing.m_value,
                           timing.m_interpolation.has_value());
            }
            return finish(std::move(*option));
        }
    }
    // 未打开来源按项目实际路径加载；失败不能清空测量列表或制造默认 120 BPM。
    auto map = BeatMap::loadFromFile(absolute);
    if ( map.m_baseMapMetadata.map_path.empty() ) return std::nullopt;
    auto option = makeOption(map);
    if ( !option ) return std::nullopt;
    // 磁盘模型采用毫秒而工具采用秒，转换只发生在这一个边界。
    // 文件解析器已经完成格式识别，不另写 osu 等格式的私有读取分支。
    for ( const auto& timing : map.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::BPM )
            append(*option,
                   timing.m_timestamp / 1000.0,
                   timing.m_timingEffectParameter,
                   timing.m_interpolation.has_value());
    }
    return finish(std::move(*option));
}

/// @brief 将后台分析运行期和结果尚未被 UI 消费的交付期视为同一个禁止导入窗口。
/// @return 分析可能继续恢复测量参数时为真。
/// @warning 仅点击路径持结果槽短锁；该锁不覆盖解码、FFT 或会话更新。
bool BpmMeasurementToolView::hasAnalysisPendingForImport() const
{
    // 工作线程先写结果槽再发布完成标志，单独读取 running 会漏掉交付间隙。
    // 读取槽与生产者使用同一把锁，不消费或丢弃其原有分析结果。
    std::lock_guard lock(m_pendingResultMutex);
    return m_analysisRunning.load(std::memory_order_relaxed) ||
           m_analysisFinished.load(std::memory_order_acquire) ||
           m_pendingResult.has_value();
}

/// @brief 按当前项目入口顺序建立一次关联谱面列表，然后请求打开模态。
/// @details 只保留当前音乐的来源；读不到、绑定其它音乐的谱面不混入候选。
/// 此请求不修改任何测量段落，也不在来源打开前自动选择导入。
/// 无红线或无法无损表达的关联来源保留在候选中，显示具体原因。
/// 关联列表是一次性快照，确认函数必须重新读取，不能直接提交缓存。
/// @warning 用户按钮低频路径，扫描期间可能读取多个谱面文件，不在每帧执行。
void BpmMeasurementToolView::requestOpenImportTimingPopup()
{
    const auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ||
         hasAnalysisPendingForImport() )
        return;
    // 冻结打开时身份，弹窗缓存只用于展示，不承诺来源内容永远不变。
    // 每次打开重新枚举项目，避免沿用上一次项目或歌曲的候选下标。
    m_importProjectRoot  = project->m_projectRoot.lexically_normal();
    m_importAudioTrackId = m_selectedAudioTrackId;
    m_importOptions.clear();
    m_importSelectedIndex = 0;
    m_importError.clear();
    // 项目列表是来源目录，不要求用户先把每份关联谱面打开成编辑标签。
    for ( const auto& entry : project->m_beatmaps ) {
        auto option =
            readBoundBeatmapTimings(Config::utf8ToPath(entry.m_filePath));
        if ( option ) m_importOptions.push_back(std::move(*option));
    }
    // 没有候选也打开弹窗给出说明；不能让按钮静默失效。
    // OpenPopup 延迟到主工具窗口，确保打开与绘制使用相同 ID 栈。
    m_shouldOpenImportTimingPopup = true;
}

/// @brief 重新校验选中来源并替换测量列表，整个导入只产生一条工具撤销记录。
/// @return 完整导入成功时为真，失败保持原测量数据和历史不变。
/// @details 项目身份、音乐身份、项目注册入口和最新来源内容逐层验证。
/// 任一层失败均在历史事务开始之前返回，不保留部分替换结果。
/// 本函数只替换工具参数；应用到谱面仍由现有独立确认流程负责。
/// @warning 用户确认低频路径；读取会话或磁盘只发生在此输入边界。
bool BpmMeasurementToolView::importSelectedBeatmapTimings()
{
    // 错误同步到主窗口状态和当前弹窗，用户无需先关闭模态才知道失败原因。
    const auto fail = [&](std::string error) {
        m_importError = std::move(error);
        m_statusText  = m_importError;
        return false;
    };
    const auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_importSelectedIndex >= m_importOptions.size() ||
         project->m_projectRoot.lexically_normal() != m_importProjectRoot ||
         m_selectedAudioTrackId != m_importAudioTrackId ||
         hasAnalysisPendingForImport() ) {
        return fail(
            TR("ui.tools.bpm_measure.import_source_changed").toString());
    }
    // 确认时读取最新未保存内容，网络修改或音频重新绑定不会沿用旧缓存。
    // 先检查身份再取缓存下标；切换项目后旧 path 不能授权读取其它来源。
    // 下方重新加载只发生一次确认，不将候选快照视为最终数据。
    const auto& path = m_importOptions[m_importSelectedIndex].m_path;
    // 来源从项目移除后即使文件仍存在，也不能继续沿用旧选择确认。
    const bool registered = std::any_of(
        project->m_beatmaps.begin(),
        project->m_beatmaps.end(),
        [&](const auto& entry) {
            return importBeatmapPath(*project,
                                     Config::utf8ToPath(entry.m_filePath))
                       .lexically_normal() == path;
        });
    if ( !registered )
        return fail(
            TR("ui.tools.bpm_measure.import_source_changed").toString());
    auto option = readBoundBeatmapTimings(path);
    if ( !option ) {
        return fail(
            TR("ui.tools.bpm_measure.import_source_changed").toString());
    }
    // 重新读取可能发现新曲线或新坏值；即使预览有效也必须以本次为准。
    // 校验完成之前不触碰段落和撤销栈，使失败成为原子操作。
    if ( !option->m_error.empty() ) {
        return fail(option->m_error);
    }
    // 完成上一笔编辑，再捕获导入前状态；失败到这里之前都不产生历史。
    finishMeasurementHistoryGesture();
    m_importError.clear();
    beginMeasurementHistoryGesture();
    m_timingSegments = std::move(option->m_segments);
    // 导入保留原始时间精度与范围，不经过手动拖动专用的限幅／合并规则。
    // 首段兼容字段与完整段落一起进入历史快照，避免显示 BPM 与数据不一致。
    // 音频时长只是当前视图范围，不是来源红线是否有效的判据。
    syncPrimaryTimingFieldsFromSegments();
    // 旧节拍器调度引用原有段落，替换后从当前视野重新计算下一个拍点。
    // 此处不启动音频，也不改变播放位置。
    resetMetronomeScheduler(m_viewCenter);
    finishMeasurementHistoryGesture();
    m_statusText =
        TR_FMT("ui.tools.bpm_measure.import_done", m_timingSegments.size());
    return true;
}

/// @brief 在工具主窗口层选择关联谱面，只在按钮边沿加载及提交。
/// @param sourceManager 向导遮罩的同步上报宿主，不保存其所有权。
/// @details 所有来源读取由用户输入边沿触发，窗口尺寸与文本换行只依赖内存。
/// 候选错误与确认错误分开显示，切换来源不会丢失新候选自身的校验信息。
/// 控件禁用只提供视觉提示，确认入口仍负责最终来源检查。
/// @warning 无操作帧只访问缓存字段，不扫描 ECS、访问磁盘或等待会话锁。
void BpmMeasurementToolView::renderImportTimingPopup(UIManager* sourceManager)
{
    // 本地化只改变可见标题，固定内部 ID 保持弹窗身份和交互状态稳定。
    // 与应用 Timing 的确认窗口使用不同 ID，避免两个流程相互覆盖。
    const std::string title =
        TR("ui.tools.bpm_measure.import_popup").toString() +
        "###BpmImportTiming";
    if ( m_shouldOpenImportTimingPopup ) {
        // 一次请求只打开一次，常态帧不会反复触发打开音效。
        // 清除请求位与用户是否最终确认无关，取消可以安全结束本轮流程。
        FeedbackOpenPopup(title.c_str());
        m_shouldOpenImportTimingPopup = false;
    }
    // 控件尺寸与现有应用弹窗一致，移动端按当前缩放保留可点击行高。
    const bool  mobile = !workspaceAllowsHoverFeedback();
    const float scale =
        mobile ? Config::AppConfig::instance().getWindowContentScale() : 1.0f;
    const auto padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        { padding.x,
          mobile
              ? std::max(padding.y, (48 * scale - ImGui::GetFontSize()) * .5f)
              : padding.y });
    // 样式始终成对恢复；模态关闭后不影响其它 BPM 参数控件的行高。
    // 无鼠标展示沿用工作区能力判断，不额外引入平台或设备探测。
    if ( ImGui::BeginPopupModal(
             title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        // 限制说明宽度，窄屏可换行；TextUnformatted 保留此换行位置。
        // 禁止用自动撑宽的说明文本把按钮挤出移动端可见区域。
        ImGui::PushTextWrapPos(
            ImGui::GetCursorPosX() +
            std::min(520.0f * scale,
                     ImGui::GetMainViewport()->WorkSize.x * .8f));
        ImGui::TextUnformatted(TR("ui.tools.bpm_measure.import_hint").data());
        // 列表刷新后旧下标可能失效，在访问任何候选前先归位。
        // 空列表仍保持无效，不能把默认零当成存在第一个候选。
        if ( m_importSelectedIndex >= m_importOptions.size() )
            m_importSelectedIndex = 0;
        const bool valid = m_importSelectedIndex < m_importOptions.size();
        if ( m_importOptions.empty() )
            ImGui::TextUnformatted(
                TR("ui.tools.bpm_measure.import_empty").data());
        else {
            ImGui::SetNextItemWidth(std::min(
                520.0f * scale, ImGui::GetMainViewport()->WorkSize.x * .8f));
            // 下拉菜单只访问打开时快照，浏览候选不触发磁盘读取。
            // 标签携带路径，隐藏 ID 再用下标区分可能重复的显示文本。
            if ( FeedbackBeginCombo(
                     "##ImportBeatmap",
                     m_importOptions[m_importSelectedIndex].m_label.c_str()) ) {
                for ( std::size_t i = 0; i < m_importOptions.size(); ++i ) {
                    ImGui::PushID(static_cast<int>(i));
                    if ( FeedbackSelectable(m_importOptions[i].m_label.c_str(),
                                            i == m_importSelectedIndex) ) {
                        m_importSelectedIndex = i;
                        // 其它来源的确认失败信息不能污染刚选中的来源。
                        // 新候选本身的解析错误仍单独显示。
                        m_importError.clear();
                    }
                    ImGui::PopID();
                }
                FeedbackEndCombo();
            }
            if ( valid &&
                 !m_importOptions[m_importSelectedIndex].m_error.empty() )
                ImGui::TextUnformatted(
                    m_importOptions[m_importSelectedIndex].m_error.c_str());
        }
        // 已知不支持的来源可见但无法确认；错误原因保留在列表下方。
        // 按钮仍走统一反馈封装，避免绕过应用声音和颜色反馈策略。
        ImGui::BeginDisabled(
            !valid ||
            (valid && !m_importOptions[m_importSelectedIndex].m_error.empty()));
        const bool confirm =
            FeedbackButton(TR("ui.tools.bpm_measure.import_confirm").data());
        ImGui::EndDisabled();
        // 仅成功时关闭；来源变化导致的拒绝必须留在当前模态解释。
        // 取消只关闭弹窗，不调用导入，也不记录工具撤销。
        if ( confirm && importSelectedBeatmapTimings() )
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if ( FeedbackButton(TR("ui.common.cancel").data()) )
            ImGui::CloseCurrentPopup();
        if ( !m_importError.empty() )
            ImGui::TextUnformatted(m_importError.c_str());
        ImGui::PopTextWrapPos();
        // 导入可能从向导所接管的工具打开，遮罩需要知道当前模态区域。
        // sourceManager 仅在本帧借用，不跨帧保留任何管理器所有权。
        if ( sourceManager && !m_walkthroughTarget.empty() )
            sourceManager->walkthroughSpotlight().reportCurrentPopup(
                m_walkthroughTarget);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
}  // namespace MMM::UI
