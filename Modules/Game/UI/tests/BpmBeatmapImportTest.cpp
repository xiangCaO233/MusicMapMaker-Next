/// @file BpmBeatmapImportTest.cpp
/// @brief 验证测量工具与谱面、独立段落文件和新建向导的交换。
/// @details 以真实项目资源提供持久音轨身份，导入、历史和文件处理使用生产实现。
/// 关联谱面导入分别覆盖磁盘与活动会话；新建向导检查尚未提交的 BPM 草稿。
/// 独立文件场景模拟投放事件，先验证窗口命中，再读回输出核对时域转换。
#include "../src/ui/imgui/menu/actions/tools/BpmMeasurementToolView_Domains.h"
#include "audio/AudioMarkerService.h"
#include "common/AudioInfoUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/VisualConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
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
#include "runtime/AppThreadPool.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/layout/CLayWrapperCore.h"
#include "ui/project/ProjectDropRouter.h"
#include "ui/utils/UIWidgetUtils.h"
#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

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
    map.m_baseMapMetadata.name           = MMM::Config::pathToUtf8(path.stem());
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
    /// @brief 使用两个短 PCM 文件和繁忙文件池验证分析请求可立即替换。
    /// @param root 已打开隔离项目的构建目录，资源表包含这两个音轨 ID。
    /// @return 两个请求都在文件池仍繁忙时返回，且仅第二段分析进入 UI。
    /// @note 夹具只写构建目录，不依赖真实音频设备或播放引擎生命周期。
    /// @details 时长差异单独验证真实 PCM 已经读入，而非仅进入就绪状态。
    /// 文件池门闩同时固定任务开始顺序，排除机器负载改变测试语义。
    /// UI 对象同一时刻只由测试线程或外层观察端访问，切换点通过 future 同步。
    // 文件池阻塞仅用于测试，不接入生产线程池的调度策略。
    static bool checkAnalysisSwitch(const std::filesystem::path& root)
    {
        // 标准小端单声道 PCM。不同帧数让错误采纳旧结果可直接观察。
        const auto write = [](const auto& path, std::uint32_t frames) {
            std::ofstream output(path, std::ios::binary);
            // RIFF 字段与 PCM 样本固定小端表示，不直接写本机 uint32_t 的字节。
            const auto integer = [&](std::uint32_t value, int bytes) {
                for ( int i = 0; i < bytes; ++i )
                    output.put(static_cast<char>((value >> (i * 8)) & 255));
            };
            // 总长度包含两个块头和正文，不包含 RIFF 自己的前八个字节。
            output.write("RIFF", 4);
            integer(36 + frames * 2, 4);
            // 常规十六字节 fmt 描述未压缩 PCM，FFmpeg 可按数据长度精确求帧数。
            output.write("WAVEfmt ", 8);
            integer(16, 4);
            integer(1, 2);
            integer(1, 2);
            // 48 kHz 单声道 16 位，每帧两字节，字节率必须和块对齐一致。
            integer(48000, 4);
            integer(96000, 4);
            integer(2, 2);
            integer(16, 2);
            // 静音样本也要实际写入，不能只构造合法头部以绕过完整解码。
            output.write("data", 4);
            integer(frames * 2, 4);
            const std::string samples(frames * 2, '\0');
            output.write(samples.data(),
                         static_cast<std::streamsize>(samples.size()));
            return output.good();
        };
        // 第一条轨为 0.1 秒，第二条为 0.2 秒。采纳旧时长会稳定失败。
        if ( !write(root / "song-a.wav", 4800) ||
             !write(root / "song-b.wav", 9600) )
            return false;

        // 只初始化真实线程池，避免误走 AudioManager 已有的播放缓存。
        auto& pools = Runtime::AppThreadPool::instance();
        pools.init();
        bool passed = false;
        {
            BpmMeasurementToolView tool("BpmQueuedAnalysis");
            std::promise<void>     unblock;
            const auto             gate = unblock.get_future().share();
            // 每个文件工作者先收到一个阻塞任务。始终释放门闩后才等待测试线程。
            for ( int i = 0; i < 4; ++i )
                pools.getFileThreadPool()->enqueue([gate] { gate.wait(); });
            // 同一输入任务顺序提交选曲。外层在这期间只观察 future，
            // 保证对 UI 成员的读写不会与测试自己的断言并发。
            auto input = std::async(std::launch::async, [&tool] {
                tool.m_selectedAudioTrackId = "song-a";
                tool.m_selectedAudioSyncKey = "a";
                tool.requestAnalyzeSelectedTrack();
                // 用户可在 A 尚未探测时点 B，旧任务必须立即失去交付资格。
                tool.m_selectedAudioTrackId = "song-b";
                tool.m_selectedAudioSyncKey = "b";
                tool.requestAnalyzeSelectedTrack();
                return tool.m_job->m_analysisRequest &&
                       tool.m_job->m_analysisRequest->key == "b" &&
                       tool.m_duration == 0.0 && tool.m_waveTimes.empty();
            });
            // 即使回归为同步等待，也释放门闩再取任务结果，避免失败后无法退出。
            const bool returned = input.wait_for(std::chrono::seconds(2)) ==
                                  std::future_status::ready;
            // 此前文件池不可能完成第一条轨，returned 因而直接反映 UI 是否等待。
            unblock.set_value();
            // 工作提交端退出后才从测试主线程读取工具字段，确保观察没有数据竞争。
            if ( input.get() && returned ) {
                // 轮询实际交付路径，消化取消任务并让最新请求获得工作槽。
                // 超时仅用于测试失败退出，生产轮询使用零等待并随正常帧推进。
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while ( std::chrono::steady_clock::now() < deadline &&
                        tool.m_duration == 0.0 ) {
                    tool.consumePendingAnalysis();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // 时长验证 PCM，非空包络验证完整结果进入消费端。
                // 身份验证 B 的结果沿正常路径提交，而不是测试直接设置时长。
                passed = std::abs(tool.m_duration - 0.2) < 0.005 &&
                         !tool.m_waveTimes.empty() &&
                         tool.m_job->m_activeAnalysisKey == "b";
            }
            // 先销毁观察端并回收任务，再关闭其借用的解码线程池。
        }
        // 测试组随后还会建立别的工作区，因此恢复线程池到未初始化状态。
        pools.shutdown();
        if ( !passed )
            XERROR(
                "BPM analysis did not publish only the latest selected track");
        return passed;
    }

    /// @brief 读取真实向导草稿和提交模型，验证 UTF-8 文本未静默裁剪。
    /// @param root 已关闭工程的隔离测试目录。
    /// @return 完整元数据可提交并落盘，手工编辑优先于模板预填。
    /// @details 模板阶段刻意选用多种保留策略。空标题和原始艺术家
    /// 从来源补齐，已填写的 Unicode 艺术家保持现值；二者同时核对。
    /// 读回字节用于发现 UTF-8 被截断后由 JSON 或读取器替换的问题。
    /// 音频阶段使用已探测标签，独立于系统解码器及媒体元数据解析。
    /// 同一方法也是文件任务完成后的提交入口，故不需要模拟拖放事件。
    /// 只在工程关闭后写夹具，避免目录监听将其当作之前 BPM 场景的来源。
    static bool preservesLongTemplateMetadata(const std::filesystem::path& root)
    {
        NewBeatmapWizard wizard;
        BeatMap          source;
        auto&            meta = source.m_baseMapMetadata;
        // 第 255 字节落在三字节字符内部，旧缓冲会留下无效的 UTF-8 后缀。
        meta.title         = std::string(254, 'x') + "谱面完整标题";
        meta.title_unicode = meta.title + "♥";
        // 艺术家与专辑超过缓冲区，但已手工填写的 Unicode 艺术家优先于模板。
        // 三栏使用不同前缀，不能把某一栏的值错误复制到其他栏目。
        // 普通字段与 Unicode 对应字段额外放入不同字符，验证独立保存。
        meta.artist                 = std::string(254, 'y') + "音乐作者";
        meta.artist_unicode         = meta.artist;
        meta.album                  = std::string(260, 'z') + "专辑";
        wizard.m_artistUnicodeInput = "保留原编辑";
        wizard.applyTemplateResourceDefaults(meta);
        wizard.syncMetaFromInputs();
        // 同时匹配精确字节和内容，防止仅修补损坏字符却继续丢掉合法尾部。
        if ( wizard.m_meta.title != meta.title ||
             wizard.m_meta.title_unicode != meta.title_unicode ||
             wizard.m_meta.artist != meta.artist ||
             wizard.m_meta.artist_unicode != "保留原编辑" ||
             wizard.m_meta.album != meta.album )
            return false;
        // 使用正式谱面写出器和加载器检查存储边界，避免只核对向导内的中转结构。
        BeatMap saved;
        saved.m_baseMapMetadata = wizard.m_meta;
        const auto path         = root / "long-wizard-metadata.mmm";
        if ( !saved.saveToFile(path) ) return false;
        // 读取使用正式格式入口；路径上的真实文件必须保存有效、完整的 JSON。
        const auto restored = BeatMap::loadFromFile(path).m_baseMapMetadata;
        if ( restored.title != meta.title || restored.artist != meta.artist ||
             restored.album != meta.album ||
             restored.artist_unicode != "保留原编辑" )
            return false;
        // 已探测的标签由普通选曲与后台导入共用，同一边界不应再次复制到固定数组。
        // 长标题不宜直接成为自动文件名，但显示文本仍按原字节进入保存命令。
        // 自动文件名沿用原值，防止长标签导致新谱面因文件名分量无法创建。
        // 夹具在模板之后应用，校验音频预填的确覆盖上一来源而非保留旧标题。
        const auto                    name = wizard.m_meta.name;
        const ::MMM::Utils::AudioInfo tags{
            .title    = std::string(254, 'a') + "新音频标题",
            .artist   = std::string(254, 'b') + "新音频作者",
            .album    = meta.album,
            .duration = 1.0
        };
        wizard.applyAudioInfo(tags);
        wizard.syncMetaFromInputs();
        return wizard.m_meta.name == name &&
               wizard.m_meta.title == tags.title &&
               wizard.m_meta.title_unicode == tags.title &&
               wizard.m_meta.artist == tags.artist &&
               wizard.m_meta.artist_unicode == tags.artist &&
               wizard.m_meta.album == tags.album;
    }

    /// @brief 断言失败包含独立场景标签，不因一项失败跳过后续来源校验。
    /// @param value 当前场景的完整行为断言。
    /// @param label 日志中可识别的失败场景。
    /// @param passed 汇总结果，已有失败不得被后续成功覆盖。
    static void check(bool value, const char* label, bool& passed)
    {
        if ( !value ) XERROR("BPM import: {}", label);
        passed &= value;
    }

    /// @brief 验证独立工具导出使用相同主音轨，并保留全部测量点。
    /// @param tool 已通过谱面导入建立三段高精度红线的测量工具。
    /// @param passed 收集错误，不因为向导断言失败跳过文件导入测试。
    /// @details 尚无向导回调，是从工具菜单独立打开的路径。
    /// 测试同时检查音频 ID 与相对路径，二者在向导创建命令中各有用途。
    /// 读取草稿字段而非检测弹窗是否可见，保证测量值确实被采用。
    static void checkWizard(BpmMeasurementToolView& tool, bool& passed)
    {
        // 不触发保存和项目创建，验证用户点创建之前只改变向导草稿。
        NewBeatmapWizard wizard;
        const auto       timings = tool.makeMeasuredTimings();
        check(!wizard.openWithMeasuredTimings("missing", timings) &&
                  !wizard.m_isOpen,
              "wizard rejects missing resource",
              passed);
        // 从真实工具生成的毫秒数据回填向导，防止重复乘除 1000。
        check(
            wizard.openWithMeasuredTimings("song-a", timings) &&
                wizard.m_isOpen && wizard.m_selectedAudioTrackId == "song-a" &&
                wizard.m_selectedAudioPath == "song-a.wav" &&
                wizard.m_measuredTimings.size() == 3 &&
                std::abs(wizard.m_measuredTimings[1].m_timestamp - 123.456789) <
                    1e-9 &&
                wizard.m_measuredTimings[1].m_bpm == 180.25,
            "standalone tool opens wizard with audio and all BPM points",
            passed);
        // 同一向导再次被调用时不能重置先前选择，防止销毁已填写表单。
        check(!wizard.openWithMeasuredTimings("song-b", timings) &&
                  wizard.m_selectedAudioTrackId == "song-a",
              "already-open wizard retains its draft",
              passed);
        wizard.close();
    }

    /// @brief 以系统文件事件、真实窗口命中和后台读取验证 .mmmt 导入。
    /// @param root 独立的测试项目，段落文件与其他生产数据隔离。
    /// @param passed 汇总来自路由、文件处理和历史的检查结果。
    /// @details 不启动桌面窗口；手工布置 ImGui 根窗口和画布子窗口。
    /// 用实际 GLFW 文件事件驱动路由，避免直接调用导入绕过鼠标命中。
    /// 保留另一个本地路由以便显式推进该事件，不进入整个 UIManager 帧。
    /// 使用应用的文件工作池，波形分析是否就绪由测试明确控制。
    /// 不连接音频引擎；用可精确表示的波形偏移检测时间轴转换。
    /// 导出后直接读回文件，验证工作线程输出也是纯 BPM 列表。
    /// 输入和重导出文件路径不同，以免用覆盖旧文件掩盖写入失败。
    /// 负首拍仍有效，波形偏移不能被原样保存在文件里。
    /// 导入和导出不应联动章节，章节测试值留在测量工具的本地草稿。
    /// 异步任务只在测试中显式等待，实际界面使用每帧轮询。
    /// 导入成功后必须保持章节和初始测量的历史快照。
    /// @post 工作线程、窗口所有权、ImGui 及 Clay 都按逆序清理。
    static void checkStandaloneFile(const std::filesystem::path& root,
                                    bool&                        passed)
    {
        // 核对当前波形偏移与文件相对时间的双向转换，再还原测试配置。
        // 四分之一秒以内的偏移不触达播放范围限制，能独立验证换算。
        auto&       visual = Config::AppConfig::instance().getVisualConfig();
        const float previousOffset  = visual.waveformVisualOffset;
        visual.waveformVisualOffset = .0625f;
        Runtime::AppThreadPool::instance().init();
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 800 };
        unsigned char* pixels{};
        int            width{}, height{};
        // 两帧布局需要文本度量，CPU 字体图集足以供位置命中使用。
        // ini 关闭，避免命中坐标受此前窗口位置影响。
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        // 管理器安装 Clay
        // 测字回调时必须有独立上下文，不能复用已析构窗口的布局。
        auto& clay        = CLayWrapperCore::instance();
        auto  clayContext = clay.createWindowContext();
        clay.makeCurrent(clayContext.context);
        {
            UIManager         manager;
            ProjectDropRouter router;
            auto              view =
                std::make_unique<BpmMeasurementToolView>("BpmMeasurementTool");
            auto& tool = *view;
            view->setOpen(true);
            // 视图按实际菜单注册名查询，路由可以验证工具是否存在且打开。
            // 观察引用在 manager 析构之前有效，测试不持有共享所有权。
            manager.registerView("BpmMeasurementTool", std::move(view));
            // 章节用于验证 BPM 单独导入不会清除原有音频元数据。
            // 初始 BPM 在文件中不存在，便于验证失败、等待与撤销分支。
            tool.m_selectedAudioTrackId     = "song-a";
            tool.m_timingSegments           = { { 0, 99 } };
            tool.m_markers->m_audioChapters = { { .25, "chapter" } };
            tool.syncPrimaryTimingFieldsFromSegments();
            // 大写后缀和高精度负首拍一并经过真实读取入口。
            // 不用文件名暗示导入目标，目标由当前所选音轨决定。
            const auto                   path = root / "drag.MMMT";
            const Audio::AudioMarkerData data{
                {}, { { -.123456789, 130.23456 }, { 4.123456789, 180.25 } }
            };
            check(Audio::AudioMarkerService::writeBpmSegmentFile(path, data)
                      .empty(),
                  "write standalone file fixture",
                  passed);
            // 建立活动窗口后再投放。实际鼠标位置保持在外面，路由应使用事件坐标。
            // 两帧让子窗口进入上帧命中列表，避免命中还未提交过布局的窗口。
            // 波形区只取真实子窗口的坐标归属，无须 FFT 结果或 GPU 纹理。
            for ( int frame = 0; frame < 2; ++frame ) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({ 80, 100 });
                ImGui::SetNextWindowSize({ 460, 360 });
                ImGui::Begin("Measure###BpmMeasurementTool");
                ImGui::BeginChild("Waveform");
                ImGui::EndChild();
                ImGui::End();
                ImGui::Render();
            }
            // 第一项投放不在这个窗口内，不能因为工具处于打开状态而被接收。
            // 第二项指向波形子窗口，必须由根窗口身份匹配接收。
            Event::GLFWDropEvent drop;
            drop.paths = { Config::pathToUtf8(path) };
            drop.pos   = { 750, 250 };
            Event::EventBus::instance().publish(drop);
            router.update(true, &manager);
            // 命中失败时不会创建任务，尚未发生任何文件读取或段落替换。
            check(!tool.m_markers->m_bpmFileImport.valid(),
                  "drop outside tool ignored",
                  passed);
            drop.pos = { 210, 240 };
            Event::EventBus::instance().publish(drop);
            router.update(true, &manager);
            // 生产入口接收大写后缀后必定有等待交付的结果，命中本身不改段落。
            check(tool.m_markers->m_bpmFileImport.valid(),
                  "drop on waveform opens .mmmt",
                  passed);
            if ( tool.m_markers->m_bpmFileImport.valid() ) {
                // 测试等待文件线程完成；生产入口仍使用零超时轮询。
                tool.m_markers->m_bpmFileImport.wait();
                const auto offset = tool.waveformCanvasOffset();
                // 先留一个尚未提交的音频分析，确保导入会等嵌入标记之后应用。
                tool.m_selectedAudioIdentityNeedsAnalysis = true;
                tool.consumeBpmSegmentFileOperations(true);
                check(
                    tool.m_markers->m_bpmFileImport.valid() && tool.m_bpm == 99,
                    "import waits for audio analysis without changing timing",
                    passed);
                tool.m_selectedAudioIdentityNeedsAnalysis = false;
                // 明确结束分析后消费已读文件，测试不用等待固定时长猜测顺序。
                // 核对首段时间和 BPM，保证两种数据一起按新的起点恢复。
                tool.consumeBpmSegmentFileOperations(true);
                check(tool.m_timingSegments.size() == 2 &&
                          tool.m_bpm == 130.23456 &&
                          std::abs(tool.m_firstBeatTime - offset + .123456789) <
                              1e-12 &&
                          tool.m_markers->m_audioChapters.size() == 1,
                      "file drop restores precise BPM and retains chapters",
                      passed);
                // 验证导出的是音频时间，而非画布经偏移后的绝对位置。
                // 再导出必须建立新文件，避免旧夹具掩盖误报写入成功。
                // 文件中没有音频波形或章节，章节仍可在原工具独立编辑。
                // 单独检查文件包含末段 BPM，防止只序列化首拍与起始 BPM。
                // 文件只需经过服务读回即可测试数值，平台选择器不参与时域转换。
                const auto output = root / "exported.mmmt";
                tool.exportBpmSegmentFile(output);
                check(tool.m_markers->m_bpmFileExport.valid(),
                      "queue BPM export",
                      passed);
                if ( tool.m_markers->m_bpmFileExport.valid() ) {
                    // 等生产写任务、而非测试用文件对象，计算最终可读结果。
                    // consume 与实际窗口共用，完成后的 future
                    // 槽应允许下一次导出。
                    tool.m_markers->m_bpmFileExport.wait();
                    tool.consumeBpmSegmentFileOperations(true);
                    // 服务读回验证文件可再次拖入，避免只检查任务返回或文件存在。
                    const auto exported =
                        Audio::AudioMarkerService::readBpmSegmentFile(output);
                    check(exported.success && exported.data.chapters.empty() &&
                              exported.data.bpmSegments.size() == 2 &&
                              std::abs(
                                  exported.data.bpmSegments.front().seconds -
                                  data.bpmSegments.front().seconds) < 1e-12 &&
                              exported.data.bpmSegments.back().bpm ==
                                  data.bpmSegments.back().bpm,
                          "export writes audio-relative BPM only",
                          passed);
                }
                // 同一条历史应还原首段和整份列表，不能只复原列表显示。
                tool.undoMeasurementFromShortcut();
                check(tool.m_timingSegments.size() == 1 && tool.m_bpm == 99,
                      "one undo restores previous BPM after file drop",
                      passed);
                // 再次读取同一文件，并在结果交付前改变当前音轨身份。
                // 选择切换会刷新实际路径；交付方只应读到新的缓存身份。
                tool.importBpmSegmentFile(path);
                check(tool.m_markers->m_bpmFileImport.valid(),
                      "submit second file import",
                      passed);
                if ( tool.m_markers->m_bpmFileImport.valid() ) {
                    tool.m_markers->m_bpmFileImport.wait();
                    // 同项目存在两条合法主音轨，不用清空选择来模拟切换。
                    tool.m_selectedAudioTrackId = "song-b";
                    tool.refreshSelectedAudioIdentity();
                    tool.consumeBpmSegmentFileOperations(true);
                    check(!tool.m_markers->m_bpmFileImport.valid() &&
                              tool.m_bpm == 99,
                          "switching audio discards pending file import",
                          passed);
                }
            }
        }
        // 先解除当前上下文，再销毁 Arena，避免全局 Clay 留下悬空指针。
        // 测试工具未启用偏好写出，不会记录测试布局或测量历史。
        clay.makeCurrent(nullptr);
        clay.destroyWindowContext(clayContext);
        ImGui::DestroyContext();
        Runtime::AppThreadPool::instance().shutdown();
        // 恢复全局内存配置，避免影响同进程后续的波形检查。
        visual.waveformVisualOffset = previousOffset;
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
        checkWizard(tool, passed);
        // 无红线来源在失败前不能清空数据或写入空撤销记录。
        tool.m_importSelectedIndex = 1;
        const auto historySize =
            tool.m_history->m_measurementUndoHistory.size();
        check(
            !tool.importSelectedBeatmapTimings() &&
                tool.m_timingSegments.size() == 3 &&
                tool.m_history->m_measurementUndoHistory.size() == historySize,
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
            const auto count  = tool.m_history->m_measurementUndoHistory.size();
            t.m_interpolation = TimingInterpolation{};
            check(
                !tool.importSelectedBeatmapTimings() &&
                    tool.m_history->m_measurementUndoHistory.size() == count &&
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
        // 对同一条真实导入路径逐一施加工作者、交付槽和 UI 待消费状态。
        // 三次拒绝均以原撤销栈长度为基线，不能给撤销菜单留下空操作。
        // 前面的弹窗仍持有同一选择，这里只改变分析作业的活跃状态。
        // 完成各阶段后立即复位对应状态，避免前一个条件遮蔽后一个条件。
        const auto count = tool.m_history->m_measurementUndoHistory.size();
        // 模拟正在分析、结果刚写入槽但尚未发布、完成但尚未消费三个阶段。
        // 这些状态都可能在下一帧恢复嵌入标记，不能让它们覆盖新导入。
        tool.m_job->m_analysisRunning.store(true, std::memory_order_relaxed);
        check(!tool.importSelectedBeatmapTimings(),
              "running analysis rejected",
              passed);
        tool.m_job->m_analysisRunning.store(false, std::memory_order_relaxed);
        // 结果槽状态有意独立于完成标志，模拟生产者尚未发布完成的边界。
        // 测试没有工作线程，直接写槽不会形成跨线程竞争。
        // 在友元作用域显式构造再移入结果槽，兼容旧 Clang 的嵌套类型构造检查。
        // 仍放入真实待交付结果，不能以完成标志代替这一交付边界测试。
        tool.m_job->m_pendingResult = BpmMeasurementToolView::AnalysisResult{};
        check(!tool.importSelectedBeatmapTimings(),
              "queued result rejected",
              passed);
        tool.m_job->m_pendingResult.reset();
        // 完成位仍为真时，UI 消费流程还没结束，也应保留原工具列表。
        // 最后清掉所有模拟状态，不能让工具析构误判仍有工作待交付。
        tool.m_job->m_analysisFinished.store(true, std::memory_order_release);
        check(!tool.importSelectedBeatmapTimings(),
              "unconsumed analysis rejected",
              passed);
        tool.m_job->m_analysisFinished.store(false, std::memory_order_release);
        check(tool.m_history->m_measurementUndoHistory.size() == count,
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
        checkStandaloneFile(root, passed);
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
    // 导入回归只需资源记录，排队测试随后为这两个路径各生成一小段 PCM。
    // 保留两个不同资源身份，避免工具错误地把具有相同类型的来源合并。
    // 自造音频留在构建目录，测试不消费实际工程媒体。
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
    // 保存测试注入的资源，向导与生产一致地从发布快照读取。
    const bool passed =
        controller.saveProject() &&
        MMM::UI::BpmBeatmapImportTestAccess::run(root) &&
        MMM::UI::BpmBeatmapImportTestAccess::checkAnalysisSwitch(root);
    // 显式关闭隔离项目，使会话清理完成后才结束测试进程。
    // 夹具保留在构建输出下供失败诊断，不向源码资源目录复制。
    controller.closeProject();
    if ( !MMM::UI::BpmBeatmapImportTestAccess::preservesLongTemplateMetadata(
             root) ) {
        XERROR("Wizard truncated template or audio metadata");
        return 1;
    }
    return passed ? 0 : 1;
}
