#include "logic/BmsPackageExportService.h"
#include "config/EditorConfig.h"
#include "logic/BeatmapSession.h"
#include "logic/session/context/SessionContext.h"

#include "common/LogicCommands.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <miniz.h>
#include <string>
#include <utility>
#include <vector>

/// @file BmsPackageExportServiceTest.cpp
/// @brief 使用程序生成的音频验证标准 BMS ZIP 的完整导出事务。
///
/// 常规 CTest 不读取用户项目、不创建窗口，也不打开音频播放设备。
/// 四个场景分别覆盖裁头、负红线补头、提前物件和 Main 独立 offset。
/// 同一个源音乐被四张谱面共享，输出却必须有各自正确的内容起点。
/// 检查真实 WAV 帧数，而非仅比较文件存在或 BMS 时间字段。
/// 检查补入段为零值，并验证其后仍存在原始非零声音。
/// 普通 Key 采样使用另一稳定 ID，确保资源解析和相对路径改写参与测试。
/// 一颗物件故意离开拍线，导出不得借首拍对齐之名改变其相对节奏。
/// 重新载入 BMS 后比较长条时长，避免尾部在整体平移中被误算。
/// ZIP 解包使用生产链接的读取器，暴露错误的目录名或损坏的压缩条目。
/// 旧包内容在失败场景前做完整快照，缺少依赖不能提前截断最终文件。
/// 原曲和来源谱面也做二进制快照，变换不能回写用户素材。
/// 所有输出均位于调用者给出的构建目录，与源码媒体资源隔离。
/// 可选第三个参数仅用于人工复现，自动化测试不依赖外部谱面。
namespace
{
/// @brief 二进制快照同时用于 ZIP 读取和源资源不变检查。
/// @param path 只读文件路径。
/// @return 完整字节；打开失败时为空。
/// @note 用于对比的来源文件由夹具明确写出，空值不能作为有效音频样本。
/// @note 返回值拥有全部内容，不借用关闭后的流缓冲。
/// @note 比较二进制内容，避免文本换行转换掩盖来源被修改的问题。
std::vector<unsigned char> bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), {} };
}

/// @brief 生成两秒非零 48kHz 单声道 PCM，便于测量真实裁头和补头。
/// @param path 构建输出目录中的 WAV 路径。
/// @return 写出成功时为真。
/// @note 十六位固定非零信号使音频头部可与补入的零值段明确区分。
/// @note 48kHz 每毫秒恰好四十八帧，整数毫秒补偿可以精确断言。
/// @note 两秒输入足以验证裁头不会裁空，不需读取任何外部音乐文件。
/// @note 所有字段以小端写出，测试输入不依赖宿主字节序。
bool makeAudio(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    const auto    little = [&](std::uint32_t value, int size) {
        for ( int i = 0; i < size; ++i )
            output.put(static_cast<char>(value >> (8 * i)));
    };
    // 固定 PCM 格式，不依赖设备、网络或外部媒体生成程序。
    output.write("RIFF", 4);
    little(36 + 96000 * 2, 4);
    output.write("WAVEfmt ", 8);
    little(16, 4);
    little(1, 2);
    little(1, 2);
    little(48000, 4);
    little(96000, 4);
    little(2, 2);
    little(16, 2);
    output.write("data", 4);
    little(96000 * 2, 4);
    for ( int i = 0; i < 96000; ++i ) little(4096, 2);
    return output.good();
}

/// @brief 解析编码输出的 PCM 帧数与非零内容起点。
/// @param path 从实际 ZIP 解出的 WAV。
/// @param phase 独立给定的裁头毫秒数，负值表示前导静音。
/// @return 总时长、前导零值和原声音均正确时为真。
/// @note 预期 phase 来自测试参数表，不调用生产对齐计算器产生期望值。
/// @note 编码后可能是不同声道布局，按输出块对齐宽度计算静音区间。
/// @note 已知输入每一帧非零，因此前导零值长度能直接证明补头位置。
/// @note 本校验不以音频文件名推断时间偏移，也不读取谱面声明替代 PCM。
/// @note 常规夹具输入只用整数毫秒，帧数比较没有采样率换算容差。
/// @note 容器可包含附加元数据块，扫描必须按实际块长度推进。
/// @note 声道数和 PCM 宽度来自 fmt，不假定编码器保持输入布局。
/// @note 补头失败与尾部截短必须分别检测，只比文件总大小无法区分。
/// @warning 只扫描测试生成的有限 WAV，不作为通用媒体验证入口。
bool checkAudio(const std::filesystem::path& path, int phase)
{
    const auto data = bytes(path);
    const auto read = [&](std::size_t at, int size) {
        std::uint32_t value = 0;
        for ( int i = 0; i < size; ++i )
            value |= std::uint32_t(data[at + i]) << (8 * i);
        return value;
    };
    // data 块与 fmt 块分别定位，音频头中允许存在非音频辅助字段。
    // 缺少任一必要块都会使下面的帧数断言失败。
    unsigned    rate = 0, frame = 0;
    std::size_t start = 0, length = 0;
    // 编码器可插入额外 RIFF 块，不能假定数据恒在第四十四字节开始。
    for ( std::size_t at = 12; at + 8 <= data.size(); ) {
        const auto size = read(at + 4, 4);
        if ( size > data.size() - at - 8 ) return false;
        const std::string tag(data.begin() + at, data.begin() + at + 4);
        if ( tag == "fmt " && size >= 16 ) {
            rate  = read(at + 12, 4);
            frame = read(at + 20, 2);
        }
        if ( tag == "data" ) {
            start  = at + 8;
            length = size;
        }
        at += 8 + size + (size & 1U);
    }
    // 总帧数由原始两秒减去裁头相位得到，不读取文档声明的时长。
    // 负相位增加帧数；正相位必须实际减少帧数而非只移动事件锚点。
    if ( rate != 48000 || !frame || !start ||
         length / frame != static_cast<unsigned>(2000 - phase) * 48U )
        return false;
    const auto padding =
        static_cast<std::size_t>(std::max(0, -phase)) * 48U * frame;
    // 数量正确仍不足以证明同步：补头必须真的是零值，不能把尾部补零冒充。
    return std::all_of(data.begin() + start,
                       data.begin() + start + padding,
                       [](auto v) { return v == 0; }) &&
           std::any_of(data.begin() + start + padding,
                       data.begin() + start + length,
                       [](auto v) { return v != 0; });
}

/// @brief 解开生产服务生成的完整 ZIP，再从真实条目读取谱面和音频。
/// @param path ZIP 输出路径。
/// @param root 本场景独立的解包目录。
/// @return 归档和所有条目均可读时为真。
/// @note 仅处理本测试刚生成的归档，条目路径由生产服务的安全校验控制。
/// @note 读取归档字节后使用内存接口，避免窄字符 API 影响路径测试。
/// @note 每个提取缓冲均立即写入独立输出文件并显式释放。
/// @note 失败仍结束 reader，避免重复运行测试时遗留解压状态。
bool unpack(const std::filesystem::path& path,
            const std::filesystem::path& root)
{
    const auto     data = bytes(path);
    mz_zip_archive zip{};
    if ( !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0) )
        return false;
    bool            ok = true;
    std::error_code error;
    // 逐个条目提取，不能只查中央目录就判定包中资源可用。
    // 测试输出路径与输入目录分开，防止解包覆盖用来比较的来源快照。
    for ( unsigned i = 0; i < mz_zip_reader_get_num_files(&zip); ++i ) {
        mz_zip_archive_file_stat stat{};
        if ( !mz_zip_reader_file_stat(&zip, i, &stat) ) {
            ok = false;
            break;
        }
        const auto target = root / MMM::Config::utf8ToPath(stat.m_filename);
        std::filesystem::create_directories(target.parent_path(), error);
        std::size_t size    = 0;
        void*       content = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if ( !content || error ) {
            if ( content ) mz_free(content);
            ok = false;
            break;
        }
        std::ofstream output(target, std::ios::binary);
        output.write(static_cast<const char*>(content), size);
        // 流已获得自己的字节副本，提取缓冲不需要跨文件或跨场景存活。
        // 任一写入失败仍会退出循环并结束 reader。
        mz_free(content);
        if ( !output.good() ) {
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

/// @brief 为单个难度准备带稳定资源 ID 的原生谱面。
/// @param path 来源 MMM 路径。
/// @param first 首红线时间，毫秒。
/// @param early 是否额外放置会阻止裁头的零点物件。
/// @param offset 主音轨独立触发偏移，毫秒。
/// @return 来源成功保存时为真。
/// @note 来源使用 MMM，准备阶段不能提前执行待测的 BMS 时间变换。
/// @note 主音乐 ID 与实际文件名不同，能暴露直接把资源 ID 写入 WAV 定义的错误。
/// @note 物件模板与预期时间分离，重新读取时按绝对毫秒比较。
/// @note 首 BPM 参数同时设置真实 BPM 与拍长，避免依赖空模型默认回退。
/// @note 早期物件只有 early 场景添加，其他场景应该走最小裁头策略。
bool makeChart(const std::filesystem::path& path, double first, bool early,
               std::int64_t offset)
{
    MMM::BeatMap map;
    map.m_baseMapMetadata.map_path       = path;
    map.m_baseMapMetadata.track_count    = 4;
    map.m_baseMapMetadata.song_file_hint = "../audio/music.wav";
    MMM::Timing bpm;
    bpm.m_timestamp    = first;
    bpm.m_timingEffect = MMM::TimingEffect::BPM;
    bpm.m_bpm = bpm.m_timingEffectParameter = 120;
    bpm.m_beat_length                       = 500;
    map.m_timings.push_back(bpm);
    map.m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_timestamp       = 0,
                               .m_offsetMs        = offset,
                               .m_audioResourceId = "main-id" });
    // 第二颗音符故意不在拍线上，验证统一原点校正不会偷偷吸附它。
    for ( const double time : { first + 1000, first + 1234 } ) {
        auto& note       = map.m_noteData.notes.emplace_back();
        note.m_timestamp = time;
        note.m_track     = 0;
        note.m_sampleBinding =
            MMM::AudioSampleBinding{ .m_audioResourceId = "key-id" };
    }
    // 早期物件放在零点；减去正相位会丢失它，因此必须改用等价负相位。
    // 这个对象不绑定采样，确保无声物件同样参与负时间保护。
    if ( early ) map.m_noteData.notes.emplace_back().m_timestamp = 0;
    auto& hold       = map.m_noteData.holds.emplace_back();
    hold.m_timestamp = first + 1500;
    hold.m_duration  = 250;
    hold.m_track     = 3;
    map.sync();
    return map.saveToFile(path);
}

/// @brief 验证共享音乐的两张谱面、真实 PCM 与失败时源文件保护。
/// @param root 构建目录下的独立测试输出。
/// @return 所有首拍、时间差、归档和资源身份断言通过时为真。
/// @note 主音乐共享资源身份，四张难度仅首拍与 offset 不同。
/// @note 提前音符不得被裁掉，生产服务应选择相位等价的补静音方案。
/// @note 包内音频总长度和非零起点都用独立整数参数断言。
/// @note 失败场景复用已完成的输出路径，验证旧包内容没有变化。
/// @note 测试项目仅为内存资源表，不初始化或写入用户项目描述文件。
/// @note 四个音频副本位于各自难度命名空间，不能只比较最后生成的一份。
/// @note 主曲裁头应减少总帧数，负红线和独立 offset 应增加总帧数。
/// @note 原始 Key 文件不会因为包中有音乐副本就被判定为可省略资源。
/// @note 选择源谱面而未选择对应音乐是确定的失败用例，不依赖设备故障。
/// @note 同一输出路径可重复运行测试，不假定此前没有导出残留。
bool run(const std::filesystem::path& root)
{
    std::error_code error;
    std::filesystem::create_directories(root / "audio", error);
    std::filesystem::create_directories(root / "charts", error);
    if ( error || !makeAudio(root / "audio/music.wav") ||
         !makeAudio(root / "audio/key.wav") )
        return false;
    // 同一音频分别需要裁掉 100ms 与补入 100ms，必须生成两个独立副本。
    if ( !makeChart(root / "charts/a.mmm", 100, false, 0) ||
         !makeChart(root / "charts/b.mmm", -100, false, 0) ||
         !makeChart(root / "charts/early.mmm", 100, true, 0) ||
         !makeChart(root / "charts/offset.mmm", 100, false, 223) )
        return false;
    // 在任何导出之前记录原素材，后续比较不能读取已经被变换过的期望值。
    // 这些来源只由测试准备阶段写出，生产服务被验证为只读访问者。
    const auto   original = bytes(root / "audio/music.wav");
    const auto   source   = bytes(root / "charts/a.mmm");
    MMM::Project project;
    project.m_projectRoot = root;
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "main-id",
                            .m_path = "audio/music.wav",
                            .m_type = MMM::AudioTrackType::Main });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "key-id",
                            .m_path = "audio/key.wav",
                            .m_type = MMM::AudioTrackType::Effect });
    const auto result = MMM::Logic::BmsPackageExportService::exportPackage(
        project,
        { "charts/a.mmm",
          "charts/b.mmm",
          "charts/early.mmm",
          "charts/offset.mmm",
          "audio/music.wav",
          "audio/key.wav" },
        root / "package.zip",
        {},
        MMM::BmsPackageAlignmentMode::RewriteAudio,
        false);
    if ( !result.success ) {
        XERROR("BMS package: {}", result.errorMessage);
        return false;
    }
    // 从 ZIP 中重读才覆盖条目名称、引用路径与实际编码文件的共同契约。
    // 不直接检查服务工作目录，因为该目录应在返回时已经回收。
    const auto extracted = root / "extracted";
    if ( !unpack(root / "package.zip", extracted) ) return false;
    for ( const auto* name : { "a", "b", "early", "offset" } ) {
        const std::string stem(name);
        const auto        chart =
            MMM::BeatMap::loadFromFile(extracted / "charts" / (stem + ".bms"));
        const int    phase    = stem == "a"       ? 100
                                : stem == "b"     ? -100
                                : stem == "early" ? -400
                                                  : -123;
        const double noteTime = stem == "early" ? 1500 : 1000;
        if ( chart.m_noteData.holds.size() != 1 ||
             chart.m_audioSamples.size() != 1 ||
             chart.m_noteData.holds.front().m_duration != 250 ||
             std::none_of(chart.m_noteData.notes.begin(),
                          chart.m_noteData.notes.end(),
                          [&](const auto& n) {
                              return std::abs(n.m_timestamp - noteTime) < .01;
                          }) ||
             std::none_of(chart.m_noteData.notes.begin(),
                          chart.m_noteData.notes.end(),
                          [&](const auto& n) {
                              return std::abs(n.m_timestamp - noteTime - 234) <
                                     .01;
                          }) ||
             !checkAudio(extracted / "charts" / (stem + "__bms_main.wav"),
                         phase) )
            return false;
    }
    // 没有 Key 绑定继续使用原音乐时，不应在包中重复保存原曲和对齐副本。
    if ( std::filesystem::exists(extracted / "audio/music.wav") ||
         original != bytes(root / "audio/music.wav") ||
         source != bytes(root / "charts/a.mmm") )
        return false;
    // Key 资源按字节保存，不能接受主音乐的全局裁头或补静音处理。
    // 这里检查解包后的实际文件，而不是仅比较重映射后的字段文本。
    if ( bytes(extracted / "audio/key.wav") != bytes(root / "audio/key.wav") )
        return false;
    // 复用一个成功包来测试失败，确保旧文件没有被截断为只有 ZIP 头的空壳。
    // 选中列表故意省去 Main，虽然项目仍能解析它，但不能隐式补入。
    // 缺少依赖的错误必须发生在打开最终输出之前。
    // 全字节快照包含压缩条目和中央目录，比仅比较文件大小更严格。
    const auto saved  = bytes(root / "package.zip");
    const auto failed = MMM::Logic::BmsPackageExportService::exportPackage(
        project,
        { "charts/a.mmm" },
        root / "package.zip",
        {},
        MMM::BmsPackageAlignmentMode::RewriteAudio,
        false);
    // 缺少明确选择的主音乐必须整体失败，而且旧包不能被提前截断。
    return !failed.success && saved == bytes(root / "package.zip");
}
/// @brief 两种模式必须共同平移后续变速、STOP、BGM 与物件。
/// @param root 已准备好音频夹具的构建输出目录。
/// @return 重读归档后的各类事件、音乐字节和持续时间均正确时为真。
/// @note 首拍为 100ms、初始拍长为 500ms，事件模式应统一后移 400ms。
/// @note 重写模式物件前移 100ms，Main 的 223ms 偏移变为补入 123ms 静音。
/// @note 两条后续红线使用不同 BPM，能暴露只移动音符或只移动第一段的问题。
/// @note STOP 的位置参与平移，持续秒数不应被乘以位移或新的 BPM。
/// @note 自动采样另有局部 offset，测试必须比较实际触发时间而非单独锚点。
/// @note 音乐原字节与解包结果比较，不能只检查“没有调用编码器”的状态。
/// @note 来源快照在两个模式之前取得，两个导出都不能写回项目谱面。
/// @note 同一输入同时覆盖两种策略，排除夹具差异掩盖时序行为的可能。
bool checkAlignmentModes(const std::filesystem::path& root)
{
    // 使用不同于常规四张难度的路径，避免覆盖先前用来验证裁头的夹具。
    // 音频继续共享原始资源表，因此两种模式必须通过同一资源身份解析链路。
    // 不直接构造 BMS 输入，确保测试涵盖 MMM 加载后的实际导出路径。
    const auto source = root / "charts/timings.mmm";
    if ( !makeChart(source, 100, false, 223) ) return false;
    auto map = MMM::BeatMap::loadFromFile(source);
    // 后续红线按音乐时间保存，不能在导出时再次减去各段自己的首拍。
    // 第二条红线跨过首个四拍小节，覆盖跨小节积分与事件编码。
    for ( const auto& [time, bpm] :
          { std::pair{ 1100.0, 150.0 }, std::pair{ 2100.0, 100.0 } } ) {
        MMM::Timing timing;
        timing.m_timestamp    = time;
        timing.m_timingEffect = MMM::TimingEffect::BPM;
        timing.m_bpm = timing.m_timingEffectParameter = bpm;
        timing.m_beat_length                          = 60000 / bpm;
        map.m_timings.push_back(timing);
    }
    // STOP 放在第二个 BPM 段内，其单位为秒而不是 BMS 的 1/48 拍。
    // 序列化器需要按所在 BPM 换算，再由加载器还原原始持续时间。
    // 这同时检查红线平移后 STOP 没有意外使用前一段的速度换算。
    MMM::Timing stop;
    stop.m_timestamp             = 1500;
    stop.m_timingEffect          = MMM::TimingEffect::STOP;
    stop.m_timingEffectParameter = .25;
    map.m_timings.push_back(stop);
    // 自动 Key 样本与主音乐走不同的资源绑定，不能误用 Main 的归零规则。
    // 700ms 锚点与 17ms offset 不可交换，导出应保留其和 717ms 的位置关系。
    // 它早于第一颗 Note，仍须跟随相同位移，不能只遍历可见物件做补偿。
    map.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp = 700, .m_offsetMs = 17, .m_audioResourceId = "key-id" });
    map.sync();
    if ( !map.saveToFile(source) ) return false;
    // sync 只用于准备来源模型，不应进入被测导出事务的音频设备路径。
    // 保存后的字节才作为不可变基线，避免将夹具初始化误报为来源修改。
    // 资源表只驻留内存，不生成项目数据库或修改用户的当前项目选择。
    const auto   snapshot = bytes(source);
    MMM::Project project;
    project.m_projectRoot = root;
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "main-id",
                            .m_path = "audio/music.wav",
                            .m_type = MMM::AudioTrackType::Main });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "key-id",
                            .m_path = "audio/key.wav",
                            .m_type = MMM::AudioTrackType::Effect });
    for ( const auto mode : { MMM::BmsPackageAlignmentMode::RewriteAudio,
                              MMM::BmsPackageAlignmentMode::ShiftEvents } ) {
        // 两个策略用独立归档和解包目录，旧条目不能替代本轮生成的结果。
        // 第二个模式必须继续读取原始 MMM，不能读取第一个模式的转换产物。
        // 循环中的预期值是人工指定的毫秒量，不复用生产相位计算实现。
        const bool events  = mode == MMM::BmsPackageAlignmentMode::ShiftEvents;
        const auto name    = events ? "events" : "rewrite";
        const auto archive = root / (std::string(name) + ".zip");
        const auto extracted = root / name;
        const auto result = MMM::Logic::BmsPackageExportService::exportPackage(
            project,
            { "charts/timings.mmm", "audio/music.wav", "audio/key.wav" },
            archive,
            {},
            mode,
            false);
        if ( !result.success || !unpack(archive, extracted) ) return false;
        const auto restored =
            MMM::BeatMap::loadFromFile(extracted / "charts/timings.bms");
        // 在初始 120 BPM 下，100ms 首拍后移 400ms 恰好落在 500ms 拍线上。
        // 后续 BPM 的时间差保持不变，不能分别按 150 或 100 BPM 计算位移。
        // 重写模式采用前移 100ms，因此两种结果的物件间距应完全相同。
        const double delta = events ? 400 : -100;
        // 初始 BPM 延长到零点；其余红线和 STOP 都必须使用唯一的共同位移。
        // 按类型、参数及时间联合查找，不能把错误的同刻事件误判为正确红线。
        const auto timingAt =
            [&](MMM::TimingEffect effect, double parameter, double time) {
                return std::any_of(
                    restored.m_timings.begin(),
                    restored.m_timings.end(),
                    [&](const auto& timing) {
                        return timing.m_timingEffect == effect &&
                               std::abs(timing.m_timingEffectParameter -
                                        parameter) < .001 &&
                               std::abs(timing.m_timestamp - time) < .03;
                    });
            };
        // 初始 BPM 使用精确零点断言，其余红线必须在各自原位置加共同增量。
        // STOP 同时匹配效果类型和持续秒数，防止只保留位置而丢掉停顿参数。
        // 容差覆盖两次文本换算误差，远小于一毫秒，不能容纳额外吸附。
        if ( !timingAt(MMM::TimingEffect::BPM, 120, 0) ||
             !timingAt(MMM::TimingEffect::BPM, 150, 1100 + delta) ||
             !timingAt(MMM::TimingEffect::BPM, 100, 2100 + delta) ||
             !timingAt(MMM::TimingEffect::STOP, .25, 1500 + delta) )
            return false;
        // 离拍 Note 不量化，Hold 时长和起点分别校验，避免只验证头部。
        for ( const double time : { 1100.0, 1334.0 } )
            if ( std::none_of(restored.m_noteData.notes.begin(),
                              restored.m_noteData.notes.end(),
                              [&](const auto& note) {
                                  return std::abs(note.m_timestamp - time -
                                                  delta) < .03;
                              }) )
                return false;
        // 长条跨越 STOP 的时刻；内部模型的持续时间仍是原始音乐坐标差。
        // 停顿等待由播放器单独解释，打包不能把停顿秒数再次加进长条长度。
        // 样本数量必须精确为二，防止默认主音乐合成逻辑重复插入 BGM。
        if ( restored.m_noteData.holds.size() != 1 ||
             std::abs(restored.m_noteData.holds.front().m_timestamp - 1600 -
                      delta) > .03 ||
             std::abs(restored.m_noteData.holds.front().m_duration - 250) >
                 .03 ||
             restored.m_audioSamples.size() != 2 )
            return false;
        // BMS 没有局部 offset 字段，重读后比较合并后的真实触发位置。
        // 两种策略均应平移独立效果采样；只有重写策略把 Main 锚定为零。
        for ( const double time : { events ? 623.0 : 0.0, 717 + delta } )
            if ( std::none_of(restored.m_audioSamples.begin(),
                              restored.m_audioSamples.end(),
                              [&](const auto& sample) {
                                  return std::abs(sample.effectiveTimestamp() -
                                                  time) < .03;
                              }) )
                return false;
        if ( events ) {
            // 原音乐应直接入包；不得同时出现未使用的重编码 WAV 副本。
            if ( bytes(extracted / "audio/music.wav") !=
                     bytes(root / "audio/music.wav") ||
                 std::filesystem::exists(extracted /
                                         "charts/timings__bms_main.wav") )
                return false;
        } else if ( !checkAudio(extracted / "charts/timings__bms_main.wav",
                                -123) )
            return false;
        // 检查每一种策略结束后的来源，不能只在最后检查而漏掉中途回写。
        // 不接受“第二种模式恢复了原文件”来掩盖第一种模式修改过源素材。
        // 这项比较与归档内音乐验证共同约束来源只读和输出独立两端。
        if ( snapshot != bytes(source) ) return false;
    }
    return true;
}

/// @brief 真实另存为命令仅可选择事件后移，不能写出音乐副本。
/// @param root 已包含多红线夹具和原始音频的测试输出根目录。
/// @return 四种后缀在开关两侧均保持引用、时序和来源只读时为真。
/// @note 复用相同来源是为了直接比较资源包与单文件的时序规则。
/// @note 通过命令入队、会话更新和最终磁盘文件验证，不能只测内部助手。
/// @note 每个后缀分别载入，PMS 使用九轨但保留物件的原始轨序。
/// @note 无项目资源表时使用真实的来源相对路径，覆盖独立谱面的另存为。
/// @note 输出位于另一个目录，错误保留来源目录引用将无法定位原素材。
/// @note 两种选择连续作用于同一会话，确保首次导出不污染第二次导出的来源。
/// @note 检查文件内容而非存在性，旧产物不能掩盖本轮未更新的问题。
/// @note 单文件目录只允许出现八个谱面，不接受多余 WAV 或临时音频。
/// @note 主音乐的 offset 为 223ms，事件后移后的实际起点应为 623ms。
/// @note 后续红线必须跟随物件后移，初始 BPM 的来源值仍应保留在会话中。
/// @warning 只在构建目录准备输入和输出，不创建用户项目或音频设备。
bool checkSingleFileExports(const std::filesystem::path& root)
{
    const auto      outputRoot = root / "single" / "exports";
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    if ( error ) return false;
    // 源音乐和 Key 是不同资源，分别保留快照，不能靠同名文件通过引用校验。
    // 输出多一层目录，原引用必须从 ../audio 改写为 ../../audio 才能正确播放。
    const auto music = bytes(root / "audio/music.wav");
    const auto key   = bytes(root / "audio/key.wav");
    // BME/BML 不因后缀不同回退到无选项分支，PMS 只更改合法键数。
    // 不把同一个转换后的对象再次当成来源，避免前一次位移混入后一次期望。
    for ( const auto* extension : { ".bms", ".bme", ".bml", ".pms" } ) {
        auto source = std::make_shared<MMM::BeatMap>(
            MMM::BeatMap::loadFromFile(root / "charts/timings.mmm"));
        if ( std::string_view(extension) == ".pms" )
            source->m_baseMapMetadata.track_count = 9;
        // 独立会话没有项目表，将夹具稳定 ID 解析为来源文件旁的真实资源。
        // 这里只改准备阶段的输入，随后两个保存命令均不得改写这些字段。
        for ( auto& note : source->m_noteData.notes )
            if ( note.m_sampleBinding )
                note.m_sampleBinding->m_audioResourceId = "../audio/key.wav";
        for ( auto& sample : source->m_audioSamples )
            sample.m_audioResourceId = sample.m_audioResourceId == "main-id"
                                           ? "../audio/music.wav"
                                           : "../audio/key.wav";
        // 真实命令处理器还负责快照、资源引用和保存结果，助手测试不能替代它。
        // 每个后缀拥有独立会话，避免元数据键数和 ECS 的旧布局互相污染。
        MMM::Logic::BeatmapSession session;
        session.pushCommand(MMM::Logic::LogicCommand{
            MMM::Logic::CmdLoadBeatmap{ .beatmap = source } });
        session.update(0, MMM::Config::EditorConfig{}, false);
        // 先完成载入再保存，两个选项都必须实际进入有谱面的正式命令处理器。
        // 不直接修改 ECS 时间，导出快照负责完成所有坐标变换。
        for ( const bool align : { false, true } ) {
            const auto path =
                outputRoot /
                (std::string(align ? "shifted" : "original") + extension);
            session.pushCommand(
                MMM::Logic::LogicCommand{ MMM::Logic::CmdSaveBeatmapAs{
                    .bmsShiftEventsToGrid = align,
                    .bmsMyO2Grid          = false,
                    .path                 = MMM::Config::pathToUtf8(path) } });
            session.update(0, MMM::Config::EditorConfig{}, false);
            // 重读最终文件同时覆盖通道合并与引用写出，命令入队不是成功证据。
            // 共同位移固定为 400ms，可发现误用了资源包的前移裁剪策略。
            // 输出谱面与输入音频位于不同深度，引用必须使用重映射后的路径。
            const auto   result = MMM::BeatMap::loadFromFile(path);
            const double delta  = align ? 400 : 0;
            // 预期是原坐标加同一固定增量，不借用被测助手计算对齐量。
            // 原位置模式必须原样保留首拍偏移，而不是隐式选择对齐。
            if ( result.m_noteData.notes.size() != 2 ||
                 result.m_noteData.holds.size() != 1 ||
                 result.m_audioSamples.size() != 2 )
                return false;
            if ( std::abs(result.m_noteData.notes.front().m_timestamp - 1100 -
                          delta) > .03 ||
                 std::abs(result.m_noteData.notes.back().m_timestamp - 1334 -
                          delta) > .03 ||
                 std::abs(result.m_noteData.holds.front().m_timestamp - 1600 -
                          delta) > .03 ||
                 std::abs(result.m_noteData.holds.front().m_duration - 250) >
                     .03 )
                return false;
            // 检查第二段红线，避免只移动 Note 而继续沿用旧速度切换位置。
            // 初始 BPM 的特殊归零已由共享的资源包回归独立覆盖。
            if ( std::none_of(result.m_timings.begin(),
                              result.m_timings.end(),
                              [&](const auto& timing) {
                                  return timing.m_timingEffect ==
                                             MMM::TimingEffect::BPM &&
                                         timing.m_timingEffectParameter ==
                                             150 &&
                                         std::abs(timing.m_timestamp - 1100 -
                                                  delta) < .03;
                              }) )
                return false;
            // 同时检查两个自动采样，避免只移动整曲 Main 而遗留自动效果音。
            // 按资源文件身份决定期望起点，不依赖导出后数组的排序实现。
            for ( const auto& sample : result.m_audioSamples ) {
                // BMS 合并局部 offset 到实际触发位置，不能再次叠加或丢失它。
                // 资源引用必须仍指向原始文件，而非某个不存在的对齐副本。
                const auto audio =
                    MMM::Config::utf8ToPath(sample.m_audioResourceId);
                const bool main = audio.filename() == "music.wav";
                if ( std::abs(sample.effectiveTimestamp() - (main ? 223 : 717) -
                              delta) > .03 ||
                     bytes(outputRoot / audio) != (main ? music : key) )
                    return false;
            }
            // 来源仍是被载入的原模型，不能因为保存成功而将其时间线移到新原点。
            // 特别要检查先导出原位置、再导出后移模式时没有累计两次位移。
            const auto& active = *session.getContext().currentBeatmap;
            // ECS 同步可能改变容器遍历顺序，应按时间和资源身份查找原对象。
            // 不能把同一组物件的顺序变化误判为导出移动了编辑中的时间轴。
            if ( std::none_of(active.m_timings.begin(),
                              active.m_timings.end(),
                              [](const auto& timing) {
                                  return timing.m_timingEffect ==
                                             MMM::TimingEffect::BPM &&
                                         timing.m_timestamp == 100;
                              }) ||
                 std::none_of(active.m_noteData.notes.begin(),
                              active.m_noteData.notes.end(),
                              [](const auto& note) {
                                  return note.m_timestamp == 1100;
                              }) ||
                 active.m_audioSamples.front().effectiveTimestamp() != 223 ||
                 active.m_baseMapMetadata.map_path !=
                     root / "charts/timings.mmm" )
                return false;
        }
    }
    // 这一检查在四个后缀与两个选项之后执行，任何分支重写音频都会暴露。
    // 输出目录里不能出现额外音频，即便源文件字节仍未改变也不接受生成副本。
    // 最终再次比较完整来源字节，长度或修改时间相同不能证明未编码。
    // 目录检查限定本测试的输出目录，不扫描或删除用户项目内的媒体文件。
    if ( music != bytes(root / "audio/music.wav") ||
         key != bytes(root / "audio/key.wav") )
        return false;
    for ( std::filesystem::directory_iterator it(outputRoot, error), end;
          !error && it != end;
          it.increment(error) ) {
        const auto extension = MMM::Config::pathToUtf8(it->path().extension());
        if ( extension != ".bms" && extension != ".bme" &&
             extension != ".bml" && extension != ".pms" )
            return false;
    }
    // 文件系统遍历失败也必须失败，不能用未遍历到音频来证明没有输出音频。
    // 所有检查结束后保留谱面产物，供人工复核实际 WAV 引用和 BGM 触发点。
    return !error;
}

/// @brief 验证用户不设置任何兼容参数时的完整 BMS 默认导出行为。
/// @param root 已准备好原生谱面和真实 WAV 的构建测试目录。
/// @return 单文件与资源包都后移首拍并实际吸附 Note、BGM 时为真。
/// @note 期望时间由 120 BPM 与固定的原始毫秒位置独立给定。
/// @note 活动来源与磁盘来源均不得被导出快照的位移污染。
/// @note 旧相位精度回归显式关闭内部吸附，本例专门覆盖默认开启路径。
/// @pre run 已生成 charts/a.mmm 及 audio/music.wav、audio/key.wav。
/// @note 来源首红线为 100ms，测试不通过生产相位助手计算期望值。
/// @note 第二颗键音故意离拍，确保仅修改默认布尔值不足以通过。
/// @note Main 起点也需要量化，避免只处理键音而遗漏 BGM 通道。
/// @note 比较资源文件字节，能发现默认策略意外进入音频重写路径。
/// @note 单文件的资源相对路径和资源包的稳定 ID 需分别验证。
/// @note 长条数量独立比较，避免长条退化成点按但总事件数不变。
/// @note 默认包对齐策略来自正式命令结构，不由测试手写选项替代。
/// @note 使用与普通相位测试不同的输出名，旧产物不能相互掩盖。
/// @note 导出后再次检查来源文件与活动内存，覆盖两个副作用边界。
/// @note 本例不启动音频设备，真实素材只用于依赖和归档完整性检查。
/// @warning 所有写入只发生在调用方给出的构建测试目录。
bool checkDefaultLossyExport(const std::filesystem::path& root)
{
    const auto path       = root / "default_single.bms";
    const auto sourcePath = root / "charts/a.mmm";
    const auto snapshot   = bytes(sourcePath);
    auto       source =
        std::make_shared<MMM::BeatMap>(MMM::BeatMap::loadFromFile(sourcePath));
    // 单文件会话按真实相对路径解析资源，不依赖测试项目表。
    for ( auto& note : source->m_noteData.notes )
        if ( note.m_sampleBinding )
            note.m_sampleBinding->m_audioResourceId = "../audio/key.wav";
    source->m_audioSamples.front().m_audioResourceId = "../audio/music.wav";
    MMM::Logic::BeatmapSession session;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = source } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    // 不写任何 BMS 开关，验证默认值确实经过正式命令处理器。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdSaveBeatmapAs{
        .path = MMM::Config::pathToUtf8(path) } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    /// @brief 按独立时间表验证后移与吸附，而非只检查文件存在。
    const auto valid = [](const MMM::BeatMap& map) {
        // 首 BPM 在 100ms，整体后移 400ms 后首拍落在 500ms。
        // 两颗键音成为 1500ms 与 1734ms，后者应吸附到 1718.75ms。
        // BGM 从 400ms 吸附至 406.25ms；Hold 的既有格点无需移动。
        return map.m_noteData.notes.size() == 2 &&
               map.m_noteData.holds.size() == 1 &&
               map.m_audioSamples.size() == 1 &&
               std::abs(map.m_noteData.notes[0].m_timestamp - 1500) < .01 &&
               std::abs(map.m_noteData.notes[1].m_timestamp - 1718.75) < .01 &&
               std::abs(map.m_audioSamples[0].effectiveTimestamp() - 406.25) <
                   .01;
    };
    if ( !valid(MMM::BeatMap::loadFromFile(path)) ) return false;
    // 包导出沿用相同默认网格策略，并从原生来源重新构建副本。
    // 资源索引保留稳定 ID，覆盖与单文件不同的依赖解析入口。
    MMM::Project project;
    project.m_projectRoot = root;
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "main-id",
                            .m_path = "audio/music.wav",
                            .m_type = MMM::AudioTrackType::Main });
    project.m_audioResources.push_back(
        MMM::AudioResource{ .m_id   = "key-id",
                            .m_path = "audio/key.wav",
                            .m_type = MMM::AudioTrackType::Effect });
    const MMM::Logic::CmdPackBeatmap defaults;
    // 默认策略必须保留音乐文件的字节内容，不进入裁剪或编码路径。
    if ( defaults.bmsAlignmentMode !=
         MMM::BmsPackageAlignmentMode::ShiftEvents )
        return false;
    const auto result = MMM::Logic::BmsPackageExportService::exportPackage(
        project,
        { "charts/a.mmm", "audio/music.wav", "audio/key.wav" },
        root / "default.zip",
        {},
        defaults.bmsAlignmentMode);
    if ( !result.success ||
         !unpack(root / "default.zip", root / "default_package") )
        return false;
    // 重新从归档读取，确保包内文件同样采用默认有损网格。
    // 来源校验在全部导出完成后进行，能发现累计位移或磁盘重写。
    return valid(MMM::BeatMap::loadFromFile(root /
                                            "default_package/charts/a.bms")) &&
           snapshot == bytes(sourcePath) &&
           std::any_of(source->m_noteData.notes.begin(),
                       source->m_noteData.notes.end(),
                       [](const auto& note) {
                           return std::abs(note.m_timestamp - 1100) < .01;
                       }) &&
           bytes(root / "default_package/audio/music.wav") ==
               bytes(root / "audio/music.wav");
}

/// @brief 密集离拍物件在整体后移后仍须写入同一条 MyO2 兼容数据行。
/// @param root 所有新文件限定于构建测试输出。
/// @return 原精度、音符数量和同通道单行约束均成立时为真。
/// @note 这组固定伪随机偏移没有小于等于 65536 的公共格点。
/// @note 正常量化误差保持 0.01ms，不能为了通过导出而增加吸附误差。
/// @note 不能使用标准多行叠加作为回退，MyO2 只保留同通道的最后一行。
/// @note 输入只包含静音 Note，测试与音频资源定位和外部设备无关。
/// @note 32 颗音符始终位于同一小节、同一轨，真实输出行数可以直接检查。
/// @note 固定宽度整数运算保证所有平台得到同样的分数偏移序列。
bool checkDenseSingleRow(const std::filesystem::path& root)
{
    MMM::BeatMap map;
    map.m_baseMapMetadata.track_count = 4;
    MMM::Timing bpm;
    bpm.m_timestamp             = 100;
    bpm.m_timingEffect          = MMM::TimingEffect::BPM;
    bpm.m_timingEffectParameter = bpm.m_bpm = 120;
    bpm.m_beat_length                       = 500;
    map.m_timings.push_back(bpm);
    std::uint32_t seed = 17;
    // 各音符相隔约 40ms，新增的微小偏移只验证存储精度，不产生同键冲突。
    // 故意保留离拍位置，不把输入预先吸附成容易写出的固定拍线。
    for ( int i = 0; i < 32; ++i ) {
        // 无符号乘加按 32 位回绕，固定种子不依赖宿主的有符号溢出行为。
        seed             = seed * 1664525U + 1013904223U;
        auto& note       = map.m_noteData.notes.emplace_back();
        note.m_track     = 0;
        note.m_timestamp = 100 + i * 40 + (seed % 1000000U) / 1000000.0;
    }
    std::string error;
    const auto  path = root / "dense_single.bms";
    // 原精度序列化仍供内部诊断；该测试不验证面向用户的默认有损策略。
    map.m_metadata.map_properties[MMM::MapMetadataType::BMS]["myo2_grid"] = "0";
    // 只允许共同后移；公共保存器继续使用普通单行策略而非 compact_rows。
    if ( !MMM::Logic::BmsPackageExportService::shiftEventsToGrid(map, error) ||
         !map.saveToFile(path) )
        return false;
    const auto restored = MMM::BeatMap::loadFromFile(path);
    if ( restored.m_noteData.notes.size() != map.m_noteData.notes.size() )
        return false;
    // 格点应保证每颗物件的绝对时间误差，而非只检查整体长度和数量。
    // 点按没有资源身份差异，时间联合匹配可发现不正确的合并或丢失。
    for ( const auto& note : map.m_noteData.notes )
        if ( std::none_of(restored.m_noteData.notes.begin(),
                          restored.m_noteData.notes.end(),
                          [&](const auto& output) {
                              return std::abs(output.m_timestamp -
                                              note.m_timestamp) <= .01;
                          }) )
            return false;
    std::ifstream input(path);
    std::string   line;
    int           rows = 0;
    // 核查真实文本的同小节同通道行数，不以支持多行的 MMM 读取器代替 MyO2。
    // 源音符均已后移到 500～1741ms，仍全部属于首小节的 16 通道。
    while ( std::getline(input, line) ) rows += line.starts_with("#00016:");
    return rows == 1;
}

/// @brief 对用户指定的真实谱面执行只读来源探针。
/// @param source 外部来源文件，永不覆盖。
/// @param root 统一的构建输出目录。
/// @return 全部音频依赖可定位并生成真实谱包时为真。
/// @note 不打开或持久化用户项目；资源表仅从同目录媒体构建。
/// @warning 仅手动追加外部路径时执行，标准 CTest 不依赖用户磁盘数据。
/// @note 外部文件只读，所有 BMS、ZIP 与解包文件进入测试输出目录。
/// @note 普通未对齐写出与打包使用同一版本 writer，大小比较可解释。
/// @note 这个探针不把用户目录创建为 MMM 项目，不影响项目文件管理状态。
/// @note 失败原因由生产服务直接记录，不以跳过依赖或删除物件使探针通过。
bool externalProbe(const std::filesystem::path& source,
                   const std::filesystem::path& root)
{
    MMM::Project project;
    project.m_projectRoot = source.parent_path();
    auto map              = MMM::BeatMap::loadFromFile(source);
    if ( map.m_baseMapMetadata.map_path.empty() ) return false;
    const auto               snapshot = bytes(source);
    std::vector<std::string> selected{ MMM::Config::pathToUtf8(
        source.filename()) };
    std::error_code          error;
    // 为这个只读探针构造稳定身份，不能把扫描结果写回真实项目描述文件。
    for ( std::filesystem::directory_iterator it(source.parent_path(), error),
          end;
          !error && it != end;
          it.increment(error) ) {
        if ( !it->is_regular_file(error) ) continue;
        const auto path = it->path().filename();
        const auto ext  = MMM::Config::pathToUtf8(path.extension());
        if ( !MMM::isKnownPackageResourceExtension(
                 MMM::PackageResourceType::Audio, ext) )
            continue;
        const auto name = MMM::Config::pathToUtf8(path);
        selected.push_back(name);
        project.m_audioResources.push_back(MMM::AudioResource{
            .m_id   = name,
            .m_path = name,
            .m_type = path == map.m_baseMapMetadata.song_file_hint.filename()
                          ? MMM::AudioTrackType::Main
                          : MMM::AudioTrackType::Effect });
    }
    if ( error ) return false;
    // 手动探针使用保留原音乐的事件模式，直接验证实际 MP3 素材的引用。
    // 输出与先前重写音乐探针分目录，便于用户在其他编辑器比较。
    // 同一个 writer 在对齐前后写出，大小比较不混入不同版本的序列化策略。
    map.m_metadata.map_properties[MMM::MapMetadataType::BMS]["myo2_grid"] = "0";
    if ( !map.saveToFile(root / "external_unaligned.bms") ) return false;
    const auto result = MMM::Logic::BmsPackageExportService::exportPackage(
        project,
        selected,
        root / "external_events.zip",
        {},
        MMM::BmsPackageAlignmentMode::ShiftEvents,
        false);
    if ( !result.success ) {
        XERROR("Real BMS export: {}", result.errorMessage);
        return false;
    }
    if ( !unpack(root / "external_events.zip", root / "external_events") )
        return false;
    auto name = source.filename();
    name.replace_extension(".bms");
    // 解包只进入构建目录，用户来源目录不能接收生成音频或转换谱面。
    // 两份纯 BMS 文本来自同版本 writer，大小差异由对齐与行表示决定。
    const auto target = root / "external_events" / name;
    XINFO("Real BMS: before={} bytes, aligned={} bytes, zip={} bytes",
          std::filesystem::file_size(root / "external_unaligned.bms"),
          std::filesystem::file_size(target),
          std::filesystem::file_size(root / "external_events.zip"));
    // 不能用丢物件来换取小文件：逐类数量必须与来源完全相等。
    // 重读调用标准 BMS 入口，覆盖稀疏多行叠加与 WAV 资源表的实际解析。
    const auto restored = MMM::BeatMap::loadFromFile(target);
    if ( map.m_noteData.notes.size() != restored.m_noteData.notes.size() ||
         map.m_noteData.holds.size() != restored.m_noteData.holds.size() ||
         restored.m_noteData.notes.empty() )
        return false;
    const double shift = map.m_noteData.notes.front().m_timestamp -
                         restored.m_noteData.notes.front().m_timestamp;
    // 首个物件的差值只定义共同原点；每个其他对象仍需独立匹配验证。
    // 这项检查不能用“导出成功”或相同总时长代替。
    // 同刻和弦可能在序列化后换序，不能按容器下标机械比较。
    // 轨号与时间共同匹配，确保内容没有被移到另一条物理轨道。
    // 所有对象必须只有同一个整体位移，离拍音符不能再次被吸附。
    // 读写各有 0.01ms 精度预算，组合比较额外容纳浮点积分尾差。
    const auto matches = [shift](const auto& original, const auto& output) {
        for ( const auto& note : original ) {
            if ( std::none_of(
                     output.begin(), output.end(), [&](const auto& result) {
                         if ( note.m_track != result.m_track ||
                              std::abs(note.m_timestamp - shift -
                                       result.m_timestamp) > .03 )
                             return false;
                         if constexpr ( requires { note.m_duration; } )
                             return std::abs(note.m_duration -
                                             result.m_duration) <= .03;
                         return true;
                     }) )
                return false;
        }
        return true;
    };
    // Hold 额外比较持续时间，防止只移动头部而使尾部留在原时间轴。
    // 数量、轨号和完整时间共同成立，才接受真实谱面探针成功。
    if ( !matches(map.m_noteData.notes, restored.m_noteData.notes) ||
         !matches(map.m_noteData.holds, restored.m_noteData.holds) )
        return false;
    // 单文件另存为仍使用 MyO2 单行策略，不能只用允许稀疏行的资源包证明兼容。
    // 同一真实谱面还必须通过正式命令生成独立 BMS，验证公共格点的实际容量。
    // 来源再次从磁盘加载，绝不能把已经对齐的包内结果当作另存为的输入。
    auto singleSource =
        std::make_shared<MMM::BeatMap>(MMM::BeatMap::loadFromFile(source));
    MMM::Logic::BeatmapSession session;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = singleSource } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    const auto singlePath = root / "external_single.bms";
    // 只移除探针自身的旧输出，防止保存失败后误读前一次成功产物。
    // 用户源文件从不作为删除或保存目标，诊断失败仍保留所有来源素材。
    std::filesystem::remove(singlePath, error);
    if ( error ) return false;
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdSaveBeatmapAs{
        .bmsShiftEventsToGrid = true,
        .bmsMyO2Grid          = false,
        .path                 = MMM::Config::pathToUtf8(singlePath) } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    const auto single = MMM::BeatMap::loadFromFile(singlePath);
    // 资源包和单文件应具有相同的整体位移，不因行表示不同再次量化物件。
    // matches 继续比较每轨物件时间和长条时长，不能仅凭文件存在判成功。
    if ( single.m_noteData.notes.size() != map.m_noteData.notes.size() ||
         single.m_noteData.holds.size() != map.m_noteData.holds.size() ||
         !matches(map.m_noteData.notes, single.m_noteData.notes) ||
         !matches(map.m_noteData.holds, single.m_noteData.holds) )
        return false;
    // 确认命令并未为了移动 BGM 而改写当前用户谱面文件。
    return snapshot == bytes(source);
}
}  // namespace

/// @brief 执行不依赖设备、网络或源码媒体资源的打包端到端回归。
/// @param argc 必须提供测试输出根目录。
/// @param argv CTest 指定的构建输出路径。
/// @return 首拍校正与实际音频、资源包全部一致时返回零。
/// @note 只初始化离线线程池，不创建音频设备或 UI 上下文。
/// @note 日志保留每次编码的实际帧数，失败时可以定位音频还是谱面阶段。
/// @note 输出参数按项目统一的 UTF-8 转换入口处理，不依赖系统代码页。
/// @note 无外部路径参数时只运行可移植的程序生成夹具。
/// @note 手动真实谱面探针执行前必须先通过相同构建的标准夹具。
/// @note 先检查 ZIP 格式注册，避免服务虽可调用却没有可达的格式规则。
/// @note 可选真实谱面探针只在标准夹具通过之后执行。
int main(int argc, char** argv)
{
    if ( argc != 2 && argc != 3 ) return 1;
    XLogger::init("BmsPackageExportServiceTest");
    MMM::Runtime::AppThreadPool::instance().init();
    const auto* types = MMM::findPackageSupportedFileTypes(".zip");
    if ( !types || !MMM::isPackageCandidateExtensionSupported(*types, ".bml") )
        return 2;
    // 根目录由调用方明确指定，不以当前工作目录作为隐式输出位置。
    // 所有场景在这个目录下分开保存，调试时可以检查实际 ZIP 与波形。
    const auto root = MMM::Config::utf8ToPath(argv[1]);
    if ( !run(root) || !checkAlignmentModes(root) ||
         !checkSingleFileExports(root) || !checkDenseSingleRow(root) ||
         !checkDefaultLossyExport(root) )
        return 3;
    return argc == 2 || externalProbe(MMM::Config::utf8ToPath(argv[2]), root)
               ? 0
               : 4;
}
