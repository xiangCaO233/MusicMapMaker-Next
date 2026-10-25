#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"

#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <vector>

namespace
{
/// @file MalodyInitialAudioExportTest.cpp
/// @brief MC 首拍策略与真实离线音频编码的端到端回归测试。
///
/// 夹具程序化生成 PCM WAV，不依赖外部工具、音频设备或网络资源。
/// 源音频开头就是非零信号，因此追加的零值帧可以被明确测量。
/// 48kHz 下每毫秒恰好四十八帧，500ms 与 600ms 可作精确整数断言。
/// 原始数据保留完整字节快照，用于发现导出意外覆盖输入文件的问题。
/// 另存为跨目录执行，覆盖资源路径解析与新歌曲引用回写。
/// MCZ 场景实际调用项目打包命令，再解压验证，不以 ZIP 文件存在判定成功。
/// 合并原点对齐的场景检查净静音长度，防止先裁后补重复处理两次音频。
/// 主音轨 offset 单独覆盖，避免同时平移 SOUND 与文件造成双重延迟。
/// 输出集中在构建目录，各场景使用独立子目录，便于失败后人工查看产物。
/// @brief 写入 WAV 使用的小端无符号整数，避免依赖宿主端序。
/// @param output 已打开的二进制流。
/// @param value 要拆成字节的字段值。
/// @tparam T 字段的固定宽度无符号类型。
template<class T> void writeLittle(std::ofstream& output, T value)
{
    for ( std::size_t index = 0; index < sizeof(T); ++index )
        // 每轮仅写最低有效字节，保证 WAV 整数字段在不同宿主端序下一致。
        output.put(static_cast<char>((value >> (index * 8)) & 255));
}

/// @brief 创建 100ms、48kHz、双声道 PCM WAV，所有帧都具有非零内容。
/// @param path 构建树中的独立测试文件。
/// @return 文件成功写出时为真。
/// @note 非零开头能区分真实的前导静音与只修改文件头时长。
bool writeAudio(const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary);
    // 最小 PCM 容器没有压缩、元数据和可选扩展块，源声音长度完全由夹具控制。
    // 编码后的文件则允许额外 RIFF 块，验证器会扫描块边界定位 PCM 数据。
    output.write("RIFF", 4);
    writeLittle(output, std::uint32_t(36 + 4800 * 4));
    output.write("WAVEfmt ", 8);
    writeLittle(output, std::uint32_t(16));
    writeLittle(output, std::uint16_t(1));
    writeLittle(output, std::uint16_t(2));
    writeLittle(output, std::uint32_t(48000));
    writeLittle(output, std::uint32_t(192000));
    writeLittle(output, std::uint16_t(4));
    writeLittle(output, std::uint16_t(16));
    // 双声道十六位样本每帧四字节，后续帧数断言独立于编码器输出头部长度。
    output.write("data", 4);
    writeLittle(output, std::uint32_t(4800 * 4));
    // 每个声道写入固定正值；测试不依赖压缩音频或外部媒体夹具。
    for ( int index = 0; index < 4800 * 2; ++index )
        writeLittle(output, std::uint16_t(4096));
    return output.good();
}

/// @brief 完整读取文件字节，用于检查原音频没有被覆盖及重新编码的 PCM。
/// @param path 待读取的输出树路径。
/// @return 打开失败时返回空数组。
std::vector<unsigned char> readBytes(const std::filesystem::path& path)
{
    // 原始文件比较使用二进制字节，避免文本换行转换影响“未修改”判断。
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input),
             std::istreambuf_iterator<char>() };
}

/// @brief 从已验证边界的 WAV 字节中读取无符号小端字段。
/// @param bytes 完整文件。
/// @param offset 字段起点。
/// @param count 字段字节数，只传入二或四。
/// @return 字段的无符号数值。
std::uint32_t readLittle(const std::vector<unsigned char>& bytes,
                         std::size_t offset, std::size_t count)
{
    std::uint32_t value = 0;
    // 调用者已经验证块长度；读取器本身不跨越当前 RIFF 块边界。
    for ( std::size_t index = 0; index < count; ++index )
        value |= static_cast<std::uint32_t>(bytes[offset + index])
                 << (index * 8);
    return value;
}

/// @brief 检查实际编码文件的 PCM 长度、前导静音及静音后的原音频内容。
/// @param path 实际由 MC 引用的新增 WAV。
/// @param milliseconds 独立测试表中的预期静音长度。
/// @return 前导字节全静音、后面有声音且总帧数精确时返回真。
/// @note 遍历 RIFF 块，不能假定编码器总使用四十四字节的固定头。
bool checkPaddedAudio(const std::filesystem::path& path, unsigned milliseconds)
{
    const auto bytes = readBytes(path);
    // 数据区起点使用零作未找到哨兵，合法 WAV 的 PCM 不可能从字节零开始。
    std::size_t   dataStart = 0, dataSize = 0;
    std::uint32_t rate = 0, blockAlign = 0;
    // 只解析验证所需的 fmt/data，不把未知辅助块误当成 PCM 数据。
    for ( std::size_t offset = 12; offset + 8 <= bytes.size(); ) {
        const auto size  = readLittle(bytes, offset + 4, 4);
        const auto start = offset + 8;
        // 块长度来自输出文件，先验证范围再构造迭代器，损坏文件直接判失败。
        if ( size > bytes.size() - start ) return false;
        const std::string tag(bytes.begin() + offset,
                              bytes.begin() + offset + 4);
        if ( tag == "fmt " && size >= 16 ) {
            rate       = readLittle(bytes, start + 4, 4);
            blockAlign = readLittle(bytes, start + 12, 2);
        }
        if ( tag == "data" ) {
            dataStart = start;
            dataSize  = size;
        }
        offset = start + size + (size & 1U);
        // RIFF 的奇数字节块含一个填充字节，忽略它才能继续准确扫描下一块。
    }
    if ( rate != 48000 || !blockAlign || !dataStart ) return false;
    const std::size_t silenceBytes = milliseconds * 48U * blockAlign;
    // 总长度同时验证没有丢掉源声音，也没有把尾部静音错当成前导补偿。
    if ( dataSize != (milliseconds * 48U + 4800U) * blockAlign ) return false;
    // 前导区必须是编码后的真实零值，不能仅靠文档偏移模拟音频后移。
    if ( !std::all_of(bytes.begin() + dataStart,
                      bytes.begin() + dataStart + silenceBytes,
                      [](unsigned char value) { return value == 0; }) )
        return false;
    // 原始声音应从静音后出现；检查整段尾部允许解码器自身的短滤波过渡。
    return std::any_of(bytes.begin() + dataStart + silenceBytes,
                       bytes.begin() + dataStart + dataSize,
                       [](unsigned char value) { return value != 0; });
}

/// @brief 经真实另存为命令导出零点有或没有用户绿线的谱面。
/// @param root 本场景独占的输出目录。
/// @param referenceBpm 谱面参考 BPM，首红线固定为 120。
/// @param userScroll 是否在零点放置用户自定义 2.5 倍绿线。
/// @param expectedPadding 独立给定的音频前导毫秒数。
/// @param mainOffset 主音轨独立 offset，补静音时不能再叠加到 SOUND 锚点。
/// @param sameNamedEffect 是否放置其他目录内与 Main 同名的效果音。
/// @param firstTime 原首红线时刻，非零值验证回卷后不足整拍的实际补偿。
/// @return 音频、MC、源模型及源文件共同满足同步契约时返回真。
/// @note 使用不同 BPM 检查补的一拍来自参考 BPM，而不是用户首红线。
/// @details 同 BPM 无论是否有绿线均不新增前导段，不同 BPM 才补参考段。
/// 不同 BPM 的零点场景腾出参考一拍，音乐和物件使用同样毫秒位移。
/// Main offset 场景只移动文件内容，触发 offset 自身仍按原值保留。
/// 同名效果音场景保留完整路径身份，不允许以文件名碰撞触发多 Main 拒绝。
/// @note 不清理已有导出资源，文件名冲突由正式事务负责安全处理。
bool checkExport(const std::filesystem::path& root, double referenceBpm,
                 bool userScroll, unsigned expectedPadding,
                 std::int64_t mainOffset = 0, bool sameNamedEffect = false,
                 double firstTime = 0)
{
    std::error_code error;
    const auto      exportDirectory = root / "export";
    std::filesystem::create_directories(exportDirectory, error);
    // 只创建本场景专属目录，禁止测试输出进入源码 tests/data。
    const auto input = root / "input.wav";
    if ( error || !writeAudio(input) ) return false;
    // 写源音频失败后不尝试导出，避免把缺失音乐误判成前导逻辑错误。
    const auto original   = readBytes(input);
    const auto effectPath = root / "effects" / "input.wav";
    if ( sameNamedEffect ) {
        // 使用真实同名文件，覆盖完整路径优先选择，而不只是 JSON 层标签匹配。
        // 文件放在源谱面的子目录，跨目录另存为后仍应能正确解析同一份资源。
        std::filesystem::create_directories(effectPath.parent_path(), error);
        if ( error || !writeAudio(effectPath) ) return false;
    }
    // 会话载入真实 MMM，再通过命令处理器导出，避免绕过项目资源和快照逻辑。
    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 4;
    source->m_baseMapMetadata.bgm_track_count = 1;
    source->m_baseMapMetadata.preference_bpm  = referenceBpm;
    source->m_baseMapMetadata.song_file_hint  = "input.wav";
    source->m_baseMapMetadata.map_path        = root / "source.mmm";
    MMM::Timing bpm;
    bpm.m_timestamp             = firstTime;
    bpm.m_bpm                   = 120;
    bpm.m_beat_length           = 500;
    bpm.m_timingEffect          = MMM::TimingEffect::BPM;
    bpm.m_timingEffectParameter = 120;
    source->m_timings.push_back(bpm);
    // 用户绿线数值明确不是一倍，后移后仍应由用户值接管后续滚速。
    if ( userScroll ) {
        auto scroll           = bpm;
        scroll.m_timingEffect = MMM::TimingEffect::SCROLL;
        scroll.m_beat_length = scroll.m_timingEffectParameter = 2.5;
        source->m_timings.push_back(scroll);
    }
    // 使用真实 Main 自动采样；效果音文件不需要重编码或改动。
    source->m_audioSamples.push_back(
        MMM::AudioSampleEvent{ .m_timestamp       = 0,
                               .m_offsetMs        = mainOffset,
                               .m_track           = 4,
                               .m_audioResourceId = "input.wav" });
    if ( sameNamedEffect ) {
        // 这个采样的文件名与 Main 相同，但它应跟随谱面移动且不能接受补静音。
        // 时间晚于首拍会让它在自动采样数组中后出现，覆盖旧循环覆盖 Main
        // 的缺陷。
        source->m_audioSamples.push_back(
            MMM::AudioSampleEvent{ .m_timestamp       = 750,
                                   .m_track           = 4,
                                   .m_audioResourceId = "effects/input.wav" });
    }
    auto& note       = source->m_noteData.notes.emplace_back();
    note.m_timestamp = firstTime + 1000;
    note.m_track     = 1;
    source->sync();
    if ( !source->saveToFile(source->m_baseMapMetadata.map_path) ) return false;
    // 来源使用 MMM，不让测试准备阶段提前执行待测的 MC 初始化变换。
    MMM::Logic::BeatmapSession session;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = source } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    const auto chartPath = exportDirectory / "chart.mc";
    // 跨目录输出覆盖实际引用改写，避免测试仅在源音频旁边恰好找到同名文件。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdSaveBeatmapAs{
        .path = MMM::Config::pathToUtf8(chartPath) } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    std::ifstream stream(chartPath);
    // 无异常解析使缺失或损坏输出变成测试失败，而不是中止整个测试进程。
    const auto document = nlohmann::json::parse(stream, nullptr, false);
    if ( document.is_discarded() ) return false;
    // 文档失败立即返回，不从损坏的 meta.song.file 猜测资源路径。
    const auto soundPath =
        exportDirectory /
        MMM::Config::utf8ToPath(
            document["meta"]["song"]["file"].get<std::string>());
    // 必须检查 MC 实际引用的文件，不能误把源 WAV 当作新编码成功证据。
    if ( !std::filesystem::is_regular_file(soundPath, error) || error )
        return false;
    if ( expectedPadding && !checkPaddedAudio(soundPath, expectedPadding) )
        return false;
    if ( !expectedPadding && readBytes(soundPath) != original ) return false;
    // 无需补头的路径应直接引用原资源，不能悄悄进行一次无意义重编码。
    // 补头场景前面已验证 PCM，无补头场景要求原文件字节完全相同。
    // 比较字节而非修改时间，能发现覆盖编码但长度未变的源文件。
    // 通过正式加载器确认谱面位移等于音频里的静音，不仅仅断言编码完成。
    auto loaded = MMM::BeatMap::loadFromFile(chartPath);
    // 物件后移完整拍长，Main 采样保持原触发点，两者职责分别检查。
    if ( loaded.m_noteData.notes.size() != 1 ||
         std::abs(loaded.m_noteData.notes[0].m_timestamp - firstTime - 1000 -
                  expectedPadding) > .1 )
        return false;
    if ( loaded.m_audioSamples.size() != (sameNamedEffect ? 2U : 1U) ||
         std::abs(loaded.m_audioSamples.front().effectiveTimestamp() -
                  mainOffset) > .1 )
        return false;
    if ( sameNamedEffect ) {
        // Main 仍位于零点排在前面；第二个采样保留局部身份和相同的时间位移。
        // 源效果音字节必须不变，不能因文件名相同而被编码为前导音乐副本。
        const auto& effect = loaded.m_audioSamples.back();
        // 音效仍引用原始 input.wav，生成的主音轨文件另有 lead-in 文件名。
        if ( std::abs(effect.effectiveTimestamp() - 750 - expectedPadding) >
                 .1 ||
             MMM::Config::utf8ToPath(effect.m_audioResourceId).filename() !=
                 "input.wav" ||
             readBytes(effectPath) != original )
            return false;
    }
    // Main 触发时间保持原值，因为音乐内容的后移已经写入文件前导静音。
    // 活动模型和原始音频都必须保持未修改，导出不接管原来的保存路径。
    return readBytes(input) == original &&
           source->m_timings.front().m_timestamp == firstTime &&
           source->m_baseMapMetadata.map_path == root / "source.mmm";
}
/// @brief 通过真实项目打包命令验证 MCZ 内谱面和主音频使用相同补偿。
/// @param root 场景独占目录，含已验证的源 MMM 与源 WAV。
/// @param referenceBpm 本谱面参考 BPM。
/// @param alignOrigin 是否额外启用旧的非 OGG 原点对齐选项。
/// @param firstTime 原首红线时刻，与源 PCM 的零点独立。
/// @param padding 独立给出的参考段规范化位移，不从输出推导。
/// @return 包内 MC 与实际编码音频同步且源文件未改动时返回真。
/// @note 100 BPM 参考段补 600ms，再结合 100ms 原点对齐后净补偿为 500ms。
/// @details 同一源文件先做另存为验证，再进入真实项目打包路径。
/// 解包检查条目选择、格式转换、音频替换和资源引用一致性。
/// 净补偿长度由场景表给出，不从包内红线反推音频期望。
bool checkPackage(const std::filesystem::path& root, double referenceBpm,
                  bool alignOrigin, double firstTime = 0,
                  unsigned padding = 600)
{
    const unsigned expectedPadding = referenceBpm == 120 ? 0 : padding;
    if ( !checkExport(
             root, referenceBpm, true, expectedPadding, 0, false, firstTime) )
        return false;
    const auto sourcePath = root / "source.mmm";
    if ( alignOrigin ) {
        auto source = MMM::BeatMap::loadFromFile(sourcePath);
        // 主音轨仍从零点开始；只改变内容与 timing，复现旧裁头选项的真实相位。
        for ( auto& timing : source.m_timings ) timing.m_timestamp += 100;
        for ( auto& note : source.m_noteData.notes ) note.m_timestamp += 100;
        source.sync();
        if ( !source.saveToFile(sourcePath) ) return false;
    }
    const auto originalMap = readBytes(sourcePath);
    // 包事务同时检查源谱面与源音频，临时对齐副本不能回写项目文件。
    const auto originalAudio = readBytes(root / "input.wav");
    MMM::Logic::EditorEngine::instance();
    auto& controller = MMM::Logic::ProjectController::instance();
    if ( !controller.openProject(root).m_opened ) return false;
    // 项目资源 ID 重映射参与本用例，不能只调用底层 JSON 保存器。
    // 明确指定 Main 身份，不依赖目录扫描按文件大小猜测资源用途。
    controller.currentProject()->m_audioResources = { MMM::AudioResource{
        .m_id   = "input.wav",
        .m_path = "input.wav",
        .m_type = MMM::AudioTrackType::Main } };
    const auto      output                        = root / "package.mcz";
    std::error_code error;
    std::filesystem::remove(output, error);
    MMM::Logic::BeatmapSession session;
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdPackBeatmap{
        .exportPath                   = MMM::Config::pathToUtf8(output),
        .selectedProjectRelativePaths = { "source.mmm", "input.wav" },
        .alignNonOggMainAudioToOrigin = alignOrigin } });
    session.update(0, MMM::Config::EditorConfig{}, false);
    (void)controller.closeProject();
    // 项目关闭后再读包，能暴露打包是否仍错误借用项目生命周期中的缓冲区。
    // 包读取发生在同步命令完成之后；失败不能被旧输出文件掩盖。
    mz_zip_archive archive{};
    const auto     outputUtf8 = MMM::Config::pathToUtf8(output);
    if ( !mz_zip_reader_init_file(&archive, outputUtf8.c_str(), 0) )
        return false;
    const auto unpacked = root / "unpacked";
    std::filesystem::create_directories(unpacked, error);
    bool extracted = !error && mz_zip_reader_get_num_files(&archive) == 2;
    // 两个必需条目之外出现其他文件也视为失败，防止夹带临时编码产物。
    for ( mz_uint index = 0; extracted && index < 2; ++index ) {
        mz_zip_archive_file_stat stat{};
        extracted = mz_zip_reader_file_stat(&archive, index, &stat) != 0;
        if ( !extracted ) break;
        // 只提取已知根路径条目；夹具不需要处理任意归档目录结构。
        const std::string name = stat.m_filename;
        // 本夹具只允许两项根目录文件，不接受目录路径作为音频成功证据。
        if ( name != "source.mc" && name != "input.wav" ) {
            extracted = false;
            break;
        }
        const auto path = MMM::Config::pathToUtf8(unpacked / name);
        extracted       = mz_zip_reader_extract_to_file(
                              &archive, index, path.c_str(), 0) != 0;
    }
    mz_zip_reader_end(&archive);
    // 无论中途哪一步失败都关闭读取器，再执行后续 PCM 与谱面检查。
    if ( !extracted ) return false;
    // 本次解包失败就停止，不能把旧目录的残留文件当成本次归档成功。
    const unsigned expectedAudioPadding = alignOrigin ? 500 : expectedPadding;
    // 组合场景裁去 100ms、参考段补入 600ms，音频净增加 500ms。
    // Note 事先多出的 100ms 抵消裁切，其最终位置仍与直接补头一致。
    const double expectedNoteTime = firstTime + 1000 + expectedPadding;
    auto         chart = MMM::BeatMap::loadFromFile(unpacked / "source.mc");
    // 比较实际 PCM 的静音长度和回读 Note 时间，不能只检查压缩包非空。
    return checkPaddedAudio(unpacked / "input.wav", expectedAudioPadding) &&
           chart.m_noteData.notes.size() == 1 &&
           std::abs(chart.m_noteData.notes.front().m_timestamp -
                    expectedNoteTime) < .1 &&
           readBytes(sourcePath) == originalMap &&
           readBytes(root / "input.wav") == originalAudio;
}
}  // namespace

/// @brief 验证无需编码、同 BPM 补头和不同 BPM 补头的真实导出事务。
/// @param argc 输出目录参数数量。
/// @param argv 构建树输出目录。
/// @return MC 与编码后 PCM 的同步断言全部通过时返回零。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    // CTest 给出隔离输出目录，测试媒体完全由当前进程生成。
    // 多次运行保留旧导出副本，验证资源名避让不会覆盖已有音频。
    XLogger::init("MalodyInitialAudioExportTest");
    MMM::Runtime::AppThreadPool::instance().init();
    // 与应用一致初始化离线工作资源，不启动窗口或播放回调。
    const std::filesystem::path root(argv[1]);
    // 同 BPM 不改音频；不同参考 BPM 根据回卷后的锚点给出独立补偿。
    const bool ok =
        checkExport(root / "unchanged", 120, false, 0) &&
        checkExport(root / "same", 120, true, 0) &&
        checkExport(root / "different", 100, false, 600) &&
        checkExport(root / "main_offset", 100, true, 600, 223) &&
        checkExport(root / "same_named_effect", 100, true, 600, 0, true) &&
        checkPackage(root / "package_same", 120, false) &&
        checkPackage(root / "package_different", 100, false) &&
        checkPackage(root / "package_combined", 100, true) &&
        // 真实 MCZ 不能继续固定补整拍；非零回卷和正参考锚点分别验证。
        checkPackage(root / "package_rolled", 100, false, 1100, 500) &&
        checkPackage(root / "package_within", 100, false, 100, 500) &&
        checkPackage(root / "package_positive", 240, false, 400, 0);
    // 任一场景失败都使端到端测试失败，成功数量不能抵扣时序或资源差异。
    if ( !ok )
        XERROR("MC initial timing and encoded audio are not synchronized");
    return ok ? 0 : 1;
}
