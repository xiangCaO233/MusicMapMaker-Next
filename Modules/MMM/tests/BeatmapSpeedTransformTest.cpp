#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

/**
 * @file BeatmapSpeedTransformTest.cpp
 * @brief 验证谱面倍速副本的时间缩放、结构重建与真实格式资源兼容性。
 *
 * 测试分为三层：程序化 fixture 精确检查每类字段；Malody delay 用例验证来源
 * 扩展字段保持 JSON 数值语义；资源覆盖递归读取测试目录内的 `.mc`、`.mmm`、
 * `.osu` 与 `.imd`，确认数量和代表时间在转换后保持比例关系。
 *
 * 核心不变量如下：
 *
 * - 时间锚点和持续时间除以 speed，BPM 乘以 speed。
 * - 自动采样的浮点锚点与整数偏移分别按其存储精度缩放。
 * - 轨道、资源 ID、音量、元数据与协作身份不随速度改变。
 * - Polyline 子物件复制到新容器，父级引用不能指向来源谱面。
 * - 新谱面长度来自实际内容末尾，不沿用来源声明长度。
 * - 无效倍率在构造任何部分结果前整体拒绝。
 *
 * 资源覆盖只对格式共同可表达的字段作强断言。重新加载后的对象数量差异当前以
 * warning 报告，避免把既有格式降级行为误判为倍速算法失败；程序化原生字段则
 * 使用严格断言。所有生成文件写入构建输出目录，不修改 tests/data 源资源。
 */

/// @brief 浮点近似比较。
/// @param actual 实际值。
/// @param expected 期望值。
/// @param tolerance 容忍误差。
/// @return 足够接近时返回 true。
/// @details 使用严格小于容差，与边界测试的预期保持一致。
bool isNearlyEqual(double actual, double expected, double tolerance = 1e-6)
{
    // 时间和 BPM 经除法可能产生不可精确表示的小数，不使用直接相等比较。
    return std::abs(actual - expected) < tolerance;
}

/// @brief 输出测试断言。
/// @param condition 断言条件。
/// @param label 断言名称。
/// @return 条件是否成立。
/// @details 统一输出带测试域前缀的单项结果，调用方用按位与累计全部断言。
bool check(bool condition, const std::string& label)
{
    // 成功和失败都记录标签，长资源批次可以定位到具体不变量。
    if ( condition ) {
        XINFO("[speed-transform] PASS: {}", label);
    } else {
        XERROR("[speed-transform] FAIL: {}", label);
    }
    return condition;
}

/// @brief 判断路径是否为支持的谱面文件。
/// @param path 文件路径。
/// @return 支持时返回 true。
/// @details 仅用于筛选测试资源，不替代 BeatMap 对文件内容和格式的真实校验。
bool isSupportedBeatmapFile(const std::filesystem::path& path)
{
    // 扩展名按 ASCII 小写规范化，覆盖测试资源中潜在的大写后缀。
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 列表与 BeatMap 当前四种可直接加载来源格式保持一致。
    return extension == ".mc" || extension == ".mmm" || extension == ".osu" ||
           extension == ".imd";
}

/// @brief 递归收集测试资源中的谱面文件。
/// @param root 资源根目录。
/// @return 谱面文件列表。
/// @details 权限错误以 error_code 截止当前遍历，不从测试资源发现阶段抛出异常。
std::vector<std::filesystem::path> collectBeatmapFiles(
    const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> files;
    std::error_code                    error;
    // 根目录不存在或不可访问时返回空集合，由上层给出明确失败断言。
    if ( !std::filesystem::is_directory(root, error) || error ) {
        return files;
    }

    // 跳过单个无权限目录，尽量覆盖其余可读测试资源。
    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while ( !error && it != end ) {
        const auto& path = it->path();
        // 只收集普通文件，目录、符号设备与不支持后缀均不进入加载器。
        if ( it->is_regular_file(error) && !error &&
             isSupportedBeatmapFile(path) ) {
            files.push_back(path);
        }
        // 显式 error_code 递增，遍历错误不会通过异常逃出测试 runner。
        it.increment(error);
    }
    // 稳定排序保证资源编号、输出文件名和日志顺序可重复。
    std::sort(files.begin(), files.end());
    return files;
}

/// @brief 获取谱面中第一个物件的时间戳。
/// @param beatmap 谱面。
/// @return 第一个物件时间戳。
/// @details 依赖 BeatMap::sync 已建立按时间排序的顶层物件视图。
double firstNoteTime(const MMM::BeatMap& beatmap)
{
    // 空谱面以零作为无内容哨兵，调用方同时检查对象集合是否为空。
    if ( beatmap.m_allNotes.empty() ) return 0.0;
    return beatmap.m_allNotes.front().get().m_timestamp;
}

/// @brief 获取谱面中最后一个物件的时间戳。
/// @param beatmap 谱面。
/// @return 最后一个物件时间戳。
/// @details 该值不包含 Hold 持续时间，仅用于验证顶层锚点比例。
double lastNoteTime(const MMM::BeatMap& beatmap)
{
    // 空集合与 firstNoteTime 使用同一哨兵约定。
    if ( beatmap.m_allNotes.empty() ) return 0.0;
    return beatmap.m_allNotes.back().get().m_timestamp;
}

/// @brief 构造包含普通物件、长条、折线和多类 Timing 的测试谱面。
/// @return 测试谱面。
/// @details
/// 各时间值刻意互不相同，使失败可定位到对应对象类型。自动采样的实际触发时间
/// 晚于其他内容，用于证明 map_length 读取锚点加偏移，而不是只看 Note 末尾。
/// Polyline 同时含 Hold 和 Flick 子节点，覆盖派生容器与父引用重建。
MMM::BeatMap makeFixture()
{
    MMM::BeatMap beatmap;
    // 元数据包含需要缩放、覆盖、清除和保留的四类代表字段。
    beatmap.m_baseMapMetadata.name            = "Source";
    beatmap.m_baseMapMetadata.version         = "Hard";
    beatmap.m_baseMapMetadata.preference_bpm  = 120.0;
    beatmap.m_baseMapMetadata.map_length      = 90000.0;
    beatmap.m_baseMapMetadata.video_starttime = 1000;
    beatmap.m_baseMapMetadata.track_count     = 4;
    beatmap.m_baseMapMetadata.bgm_track_count = 2;
    beatmap.m_baseMapMetadata.main_audio_path = "legacy-source.ogg";
    beatmap.m_baseMapMetadata.song_file_hint  = "source-hint.ogg";

    // BPM 时间点同时设置三个等价频率字段，变换后必须继续保持一致。
    MMM::Timing bpm;
    bpm.m_timestamp             = 1000.0;
    bpm.m_timingEffect          = MMM::TimingEffect::BPM;
    bpm.m_bpm                   = 120.0;
    bpm.m_beat_length           = 500.0;
    bpm.m_timingEffectParameter = 120.0;
    beatmap.m_timings.push_back(bpm);

    // SCROLL 时间点只缩放锚点，倍率和 beat_length 语义保持不变。
    MMM::Timing scroll;
    scroll.m_timestamp             = 3000.0;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_beat_length           = 1.5;
    scroll.m_timingEffectParameter = 1.5;
    beatmap.m_timings.push_back(scroll);

    // 普通 Note 提供最简单的单锚点缩放样本。
    MMM::Note note;
    note.m_timestamp = 2000.0;
    note.m_track     = 1;
    beatmap.m_noteData.notes.push_back(note);

    // 顶层 Hold 同时验证起点与持续时间使用相同倍率。
    MMM::Hold hold;
    hold.m_timestamp = 4000.0;
    hold.m_duration  = 600.0;
    hold.m_track     = 2;
    beatmap.m_noteData.holds.push_back(hold);

    // 根折线的起点与首子节点一致，便于检查重建后的权威锚点。
    MMM::Polyline polyline;
    polyline.m_timestamp = 5000.0;
    polyline.m_track     = 3;

    // 子 Hold 存入谱面拥有型容器，Polyline 只保存引用包装器。
    MMM::Hold subHold;
    subHold.m_timestamp = 5000.0;
    subHold.m_duration  = 1000.0;
    subHold.m_track     = 3;
    subHold.m_isSubNote = true;
    beatmap.m_noteData.holds.push_back(subHold);
    // 在追加其他同类对象前立即取得引用，避免容器扩容后地址失效。
    auto& subHoldRef = beatmap.m_noteData.holds.back();
    polyline.m_subNotes.push_back(subHoldRef);
    polyline.m_subHolds.push_back(subHoldRef);

    // 子 Flick 覆盖另一派生类型和折线末端时间。
    MMM::Flick subFlick;
    subFlick.m_timestamp = 6000.0;
    subFlick.m_track     = 4;
    subFlick.m_isSubNote = true;
    beatmap.m_noteData.flicks.push_back(subFlick);
    // 通用和专用子列表必须引用同一个拥有型对象。
    auto& subFlickRef = beatmap.m_noteData.flicks.back();
    polyline.m_subNotes.push_back(subFlickRef);
    polyline.m_subFlicks.push_back(subFlickRef);

    // 自动采样选择不可整除偏移 101，验证 1.5 倍后的整数舍入为 67。
    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = 7200.0;
    sample.m_offsetMs        = 101;
    sample.m_track           = 5;
    sample.m_audioResourceId = "stem.ogg";
    sample.m_volume          = 0.75F;
    sample.m_metadata.sample_properties[MMM::SampleMetadataType::MMM]["label"] =
        "stem";
    beatmap.m_audioSamples.push_back(sample);

    // 根对象最后加入后统一同步顶层视图，返回值可直接用于边界计算。
    beatmap.m_noteData.polylines.push_back(polyline);
    beatmap.sync();
    return beatmap;
}

/// @brief 运行程序化 fixture 覆盖。
/// @return 通过时返回 true。
/// @details
/// 以 1.5 倍生成副本，逐项检查元数据、Timing、顶层物件、Polyline 子节点和
/// 自动采样。随后用短声明长度、负半毫秒偏移和零倍率三个变体覆盖边界规则。
/// 所有断言累计执行，单项失败不会阻止同一 fixture 的其余诊断。
/// @par 失败定位
/// - 元数据名称或路径失败时检查 createSpeedVersion 的覆盖顺序。
/// - map_length 失败时检查自动采样有效时间是否参与内容末尾。
/// - BPM 与拍长同时失败时检查 scaleTiming 的字段同步。
/// - 只有 SCROLL 参数失败时检查非 BPM 效果的提前返回边界。
/// - 顶层数量失败时检查 copyScaledNotes 对子物件的过滤。
/// - Polyline 引用失败时检查目标拥有型容器追加和引用重建顺序。
/// - 自动采样偏移失败时检查整数毫秒舍入规则。
/// - 负半值变体失败时检查 std::round 的符号语义。
/// - 无效倍率失败时检查公共入口是否在复制前完成验证。
bool runFixtureCoverage()
{
    // Arrange：来源对象由 makeFixture 建立完整且可重复的结构。
    auto                              source = makeFixture();
    MMM::BeatmapSpeedTransformOptions options;
    // 输出路径和音频提示只保存为元数据，本测试不实际生成音频文件。
    options.speed     = 1.5;
    options.mapPath   = "Source_1_5x.mmm";
    options.audioPath = "audio_1_5x.wav";
    options.name      = "Source 1.5x";
    options.version   = "Hard 1.5x";

    // Act：公共入口应返回独立 BeatMap，不修改 source。
    auto result =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, options);

    // Assert：先确认调用成功，再继续收集全部字段差异。
    bool ok = true;
    ok &= check(result.success, "transform succeeds");
    ok &= check(result.beatmap.m_baseMapMetadata.name == "Source 1.5x",
                "name updated");
    ok &= check(result.beatmap.m_baseMapMetadata.version == "Hard 1.5x",
                "version updated");
    // 参考 BPM 与 speed 同向增长，而时间字段与 speed 反向缩短。
    ok &= check(
        isNearlyEqual(result.beatmap.m_baseMapMetadata.preference_bpm, 180.0),
        "metadata bpm scaled");
    // 自动采样实际触发点 7200/1.5 + round(101/1.5) 决定内容末尾 4867。
    ok &= check(
        isNearlyEqual(result.beatmap.m_baseMapMetadata.map_length, 4867.0),
        "map length uses content end");
    ok &= check(
        isNearlyEqual(
            MMM::BeatmapSpeedTransform::calculateContentEndTime(result.beatmap),
            4867.0),
        "content end time calculated");
    // 视频偏移是整数毫秒，666.666... 按最近整数写成 667。
    ok &= check(result.beatmap.m_baseMapMetadata.video_starttime == 667,
                "video start time scaled");
    // 新模型清除旧单音频路径，并把目标音频保存为歌曲文件提示。
    ok &= check(result.beatmap.m_baseMapMetadata.main_audio_path.empty(),
                "legacy main audio path cleared");
    ok &= check(result.beatmap.m_baseMapMetadata.song_file_hint ==
                    std::filesystem::path("audio_1_5x.wav"),
                "song file hint updated");

    // Timing 数量和顺序不变，分别验证 BPM 与非 BPM 的不同缩放语义。
    ok &= check(result.beatmap.m_timings.size() == 2, "timing count kept");
    ok &= check(
        isNearlyEqual(result.beatmap.m_timings[0].m_timestamp, 666.6666666667),
        "bpm timing timestamp scaled");
    // BPM、效果参数和拍长应保持互为一致的表示。
    ok &= check(isNearlyEqual(result.beatmap.m_timings[0].m_bpm, 180.0),
                "bpm timing value scaled");
    ok &= check(isNearlyEqual(result.beatmap.m_timings[0].m_beat_length,
                              333.3333333333),
                "bpm beat length scaled");
    // SCROLL 只移动锚点，1.5 倍滚速参数不能被再次乘速。
    ok &= check(isNearlyEqual(result.beatmap.m_timings[1].m_timestamp, 2000.0),
                "scroll timing timestamp scaled");
    ok &= check(
        isNearlyEqual(result.beatmap.m_timings[1].m_timingEffectParameter, 1.5),
        "scroll multiplier kept");

    // 顶层分类容器数量必须保持，子 Hold 仍存放在 Hold 拥有型容器内。
    ok &= check(result.beatmap.m_noteData.notes.size() == 1,
                "standalone note count kept");
    // 普通 Note 只检查锚点除速，轨道等不变字段由后续结构断言覆盖。
    ok &=
        check(isNearlyEqual(result.beatmap.m_noteData.notes.front().m_timestamp,
                            1333.3333333333),
              "note timestamp scaled");
    // 两个 Hold 分别是顶层与折线子节点，缺一说明复制时漏掉某个域。
    ok &= check(result.beatmap.m_noteData.holds.size() == 2,
                "hold and sub hold copied");
    ok &=
        check(isNearlyEqual(result.beatmap.m_noteData.holds.front().m_timestamp,
                            2666.6666666667),
              "hold timestamp scaled");
    ok &= check(isNearlyEqual(
                    result.beatmap.m_noteData.holds.front().m_duration, 400.0),
                "hold duration scaled");
    ok &= check(result.beatmap.m_noteData.polylines.size() == 1,
                "polyline copied");

    // 安全取得唯一折线后检查父级引用是否重新绑定到目标子对象。
    const auto& polyline = result.beatmap.m_noteData.polylines.front();
    ok &= check(polyline.m_subNotes.size() == 2, "polyline sub notes rebound");
    // 根锚点采用缩放后的首子节点，不沿用复制前的旧地址或旧时间。
    ok &= check(isNearlyEqual(polyline.m_timestamp, 3333.3333333333),
                "polyline timestamp follows first sub note");
    ok &= check(isNearlyEqual(polyline.m_subNotes.front().get().m_timestamp,
                              3333.3333333333),
                "polyline first sub timestamp scaled");
    ok &= check(
        isNearlyEqual(polyline.m_subNotes.back().get().m_timestamp, 4000.0),
        "polyline second sub timestamp scaled");

    // 自动采样保持单个对象，避免转换器误把歌曲提示合成为额外事件。
    ok &= check(result.beatmap.m_audioSamples.size() == 1,
                "automatic sample count kept");
    if ( result.beatmap.m_audioSamples.size() == 1 ) {
        // 只有数量满足时才访问首项，避免失败诊断本身越界。
        const auto& sample = result.beatmap.m_audioSamples.front();
        ok &= check(isNearlyEqual(sample.m_timestamp, 4800.0),
                    "automatic sample anchor scaled");
        // 101/1.5=67.333...，整数偏移按最近毫秒得到 67。
        ok &= check(sample.m_offsetMs == 67,
                    "automatic sample offset rounded to nearest millisecond");
        ok &= check(isNearlyEqual(sample.effectiveTimestamp(), 4867.0),
                    "automatic sample trigger time scaled");
        // 时间之外的轨道、资源和音量必须按值复制。
        ok &= check(sample.m_track == 5 &&
                        sample.m_audioResourceId == "stem.ogg" &&
                        isNearlyEqual(sample.m_volume, 0.75F),
                    "automatic sample identity kept");
        // 来源扩展属性是值语义的一部分，也不能在副本构造中丢失。
        ok &= check(sample.m_metadata.getValue<std::string>(
                        MMM::SampleMetadataType::MMM, "label") == "stem",
                    "automatic sample metadata kept");
    }

    // 变体一：来源声明长度短于实际内容，结果仍由实际末尾重新计算。
    auto shortSource                         = makeFixture();
    shortSource.m_baseMapMetadata.map_length = 3000.0;
    auto shortResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(shortSource, options);
    ok &= check(shortResult.success, "short metadata transform succeeds");
    ok &= check(
        isNearlyEqual(shortResult.beatmap.m_baseMapMetadata.map_length, 4867.0),
        "map length ignores divided metadata tail");

    // 变体二：-1/2 毫秒验证负半值按远离零舍入为 -1。
    auto tieSource                              = makeFixture();
    tieSource.m_audioSamples.front().m_offsetMs = -1;
    auto tieOptions                             = options;
    tieOptions.speed                            = 2.0;
    auto tieResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(tieSource, tieOptions);
    ok &= check(tieResult.success &&
                    tieResult.beatmap.m_audioSamples.size() == 1 &&
                    tieResult.beatmap.m_audioSamples.front().m_offsetMs == -1,
                "negative half millisecond rounds away from zero");

    // 变体三：零倍率无法建立时间映射，必须在复制前拒绝。
    MMM::BeatmapSpeedTransformOptions invalidOptions;
    invalidOptions.speed = 0.0;
    auto invalidResult =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, invalidOptions);
    ok &= check(!invalidResult.success, "invalid speed rejected");
    // 返回累计结果，保留同轮全部断言日志。
    return ok;
}

/// @brief 验证 Malody time.delay 在倍速后保持数值语义并可往返。
/// @param outputRoot 测试输出目录。
/// @return 通过时返回 true。
/// @details
/// 程序化写出同时含正、负 delay 的最小 MC JSON，以 2 倍转换后检查内存元数据、
/// 保存 JSON 类型和再次导入得到的绝对时间。该流程区分局部 delay 与主 SOUND
/// 对齐规则，确保无配对 SOUND 时不会把 delay 当作全局音频偏移移除。
/// @par 失败定位
/// - 来源加载无 Timing 时先检查最小 MC 的 meta、time 与 note 结构。
/// - 内存 delay 缺失时检查 LoadMalodyMap 是否保留来源扩展属性。
/// - delay 类型仍为字符串时检查变换写回是否使用 JSON number。
/// - 正负值只错一项时检查对应 Timing 的属性表索引。
/// - 保存后字段丢失时检查 SaveMalodyMap 的 time 元数据合并。
/// - 重载绝对时间失败时检查 delay 与拍位时间的组合顺序。
/// - 输出目录失败属于测试环境问题，不表述为变速算法错误。
/// - 本用例不覆盖与主 SOUND 同值配对后的全局偏移折叠。
bool runMalodyDelayRoundTrip(const std::filesystem::path& outputRoot)
{
    std::error_code createError;
    // 所有临时输入和输出都限制在构建测试目录，源资源保持只读。
    std::filesystem::create_directories(outputRoot, createError);
    if ( createError ) {
        return check(false, "Malody delay output directory created");
    }

    // 使用独立文件名，避免与资源覆盖阶段按序号生成的结果冲突。
    const auto sourcePath = outputRoot / "speed_delay_source.mc";
    const auto outputPath = outputRoot / "speed_delay_transformed.mc";
    // 两个 Timing 的 delay 分别验证正向延后与负向提前。
    nlohmann::json sourceJson{
        { "meta",
          { { "creator", "MMM" },
            { "version", "Delay" },
            { "mode", 0 },
            { "mode_ext", { { "column", 4 } } },
            { "song",
              { { "title", "Delay" },
                { "artist", "MMM" },
                { "file", "source.ogg" },
                { "bpm", 120.0 } } } } },
        { "time",
          nlohmann::json::array(
              { { { "beat", nlohmann::json::array({ 0, 0, 1 }) },
                  { "bpm", 120.0 },
                  { "delay", 300.0 } },
                { { "beat", nlohmann::json::array({ 4, 0, 1 }) },
                  { "bpm", 150.0 },
                  { "delay", -80.0 } } }) },
        { "note",
          nlohmann::json::array(
              { { { "beat", nlohmann::json::array({ 2, 0, 1 }) },
                  { "column", 1 } } }) }
    };

    {
        // 先物化最小来源文件，确保测试覆盖真实 LoadMalodyMap 边界。
        std::ofstream sourceFile(sourcePath,
                                 std::ios::binary | std::ios::trunc);
        if ( !sourceFile ) {
            return check(false, "Malody delay source opened");
        }
        // 紧凑 JSON 足以验证字段语义，排版不是此测试目标。
        sourceFile << sourceJson.dump();
    }

    // 通过公共 BeatMap 入口加载，不能直接构造 Timing 绕过格式元数据。
    MMM::BeatMap source = MMM::BeatMap::loadFromFile(sourcePath);
    MMM::BeatmapSpeedTransformOptions options;
    options.speed     = 2.0;
    options.mapPath   = outputPath.filename();
    options.audioPath = "transformed.ogg";
    options.name      = "Delay 2x";
    options.version   = "Delay 2x";

    // 2 倍速度应把两个 delay 分别缩短为 150 与 -40 毫秒。
    auto result =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, options);
    bool ok = check(result.success, "Malody delay transform succeeds");
    ok &= check(result.beatmap.m_timings.size() == 2,
                "Malody delay timing count kept");
    if ( result.beatmap.m_timings.size() == 2 ) {
        // 数量正确后再读取两项来源属性，避免失败路径产生越界。
        const auto& firstProperties =
            result.beatmap.m_timings[0]
                .m_metadata.timing_properties[MMM::TimingMetadataType::MALODY];
        const auto& secondProperties =
            result.beatmap.m_timings[1]
                .m_metadata.timing_properties[MMM::TimingMetadataType::MALODY];
        const auto firstDelayIt  = firstProperties.find("delay");
        const auto secondDelayIt = secondProperties.find("delay");
        // 属性表保存 JSON 文本，再次解析可同时验证数值类型与数值内容。
        const auto firstDelay =
            firstDelayIt == firstProperties.end()
                ? nlohmann::json{}
                : nlohmann::json::parse(firstDelayIt->second, nullptr, false);
        const auto secondDelay =
            secondDelayIt == secondProperties.end()
                ? nlohmann::json{}
                : nlohmann::json::parse(secondDelayIt->second, nullptr, false);
        // 首个正 delay 缩短一半并规范化为 JSON number。
        ok &= check(firstDelayIt != firstProperties.end() &&
                        firstDelay.is_number() &&
                        isNearlyEqual(firstDelay.get<double>(), 150.0),
                    "first Malody delay scaled as a number");
        // 第二个负 delay 保持符号，只缩放绝对时间长度。
        ok &= check(secondDelayIt != secondProperties.end() &&
                        secondDelay.is_number() &&
                        isNearlyEqual(secondDelay.get<double>(), -40.0),
                    "later Malody delay scaled as a number");
    }

    // 保存到 `.mc` 强制经过 SaveMalodyMap，验证元数据能够重新写出。
    ok &= check(result.beatmap.saveToFile(outputPath),
                "transformed Malody delay map saved");
    nlohmann::json savedJson;
    {
        // 使用非抛出解析读取生成物，损坏文件以 discarded 进入失败断言。
        std::ifstream outputFile(outputPath);
        if ( outputFile ) {
            savedJson = nlohmann::json::parse(outputFile, nullptr, false);
        }
    }
    // 同时检查字段存在、数组数量、JSON 类型与两个方向的精确缩放值。
    ok &= check(
        !savedJson.is_discarded() && savedJson.contains("time") &&
            savedJson["time"].size() == 2 &&
            savedJson["time"][0]["delay"].is_number() &&
            savedJson["time"][1]["delay"].is_number() &&
            isNearlyEqual(savedJson["time"][0]["delay"].get<double>(), 150.0) &&
            isNearlyEqual(savedJson["time"][1]["delay"].get<double>(), -40.0),
        "Malody delay remains numeric in exported mc");

    // 最后一层重新加载生成物，验证 delay 参与绝对 Timing 锚点的方式未漂移。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
    // 没有同值主 SOUND 配对时，首 timing.delay 保持普通局部延迟语义。
    ok &= check(reloaded.m_timings.size() == 2 &&
                    isNearlyEqual(reloaded.m_timings[0].m_timestamp, 150.0) &&
                    isNearlyEqual(reloaded.m_timings[1].m_timestamp, 1110.0),
                "scaled Malody timing anchors survive mc round trip");
    // 返回所有内存、写出与重载阶段的累计结果。
    return ok;
}

/// @brief 校验真实谱面资源的倍速变换结果。
/// @param inputPath 输入谱面路径。
/// @param outputPath 输出 MMM 谱面路径。
/// @param speed 倍速倍率。
/// @return 通过时返回 true。
/// @details
/// 对单个真实资源执行加载、变换、数量与代表时间检查，再保存为原生 MMM 并
/// 重新加载。格式降级可能改变细节，因此强约束集中在变换结果，重载数量差异
/// 保留为诊断 warning，不把来源格式已知兼容行为扩大为本算法失败。
/// @par 失败定位
/// - 加载为空时先区分资源本身为空与对应格式读取器拒绝。
/// - 物件数量变化时检查顶层和 Polyline 子物件复制路径。
/// - 首尾同时偏移时检查公共锚点缩放公式。
/// - 只有末尾错误时检查 Hold 持续时间或折线末端处理。
/// - Timing 锚点错误与 BPM 错误分别对应除速和乘速路径。
/// - Sample 时间错误需区分浮点锚点与整数偏移两套规则。
/// - 资源身份或音量错误说明值复制边界发生非时间字段改写。
/// - 保存失败属于原生 MMM 写出边界，日志保留具体来源文件。
/// - 重载数量 warning 不改变本用例强断言结果。
bool validateResourceBeatmap(const std::filesystem::path& inputPath,
                             const std::filesystem::path& outputPath,
                             double                       speed)
{
    // Arrange：公共加载入口保留各格式真实解析和兼容迁移行为。
    MMM::BeatMap source = MMM::BeatMap::loadFromFile(inputPath);
    // 显式同步统一引用视图，首尾物件辅助函数才能使用稳定排序。
    source.sync();

    bool ok = true;
    // 空对象和空 Timing 资源仍可含自动采样，当前只记录覆盖强度提示。
    if ( source.m_allNotes.empty() && source.m_timings.empty() ) {
        XWARN("[speed-transform] Resource map has no notes or timings: {}",
              inputPath.string());
    } else {
        ok &= check(true, "resource loaded: " + inputPath.string());
    }

    // 所有资源使用同一 1.5 倍和原生输出，便于跨格式比较结果。
    MMM::BeatmapSpeedTransformOptions options;
    options.speed     = speed;
    options.mapPath   = outputPath.filename();
    options.audioPath = "coverage_audio.ogg";
    options.name      = source.m_baseMapMetadata.name + " coverage";
    options.version   = source.m_baseMapMetadata.version + " coverage";

    // 在变换前保存来源基准，避免后续误用结果对象反推期望值。
    const double sourceContentEnd =
        MMM::BeatmapSpeedTransform::calculateContentEndTime(source);
    const double sourceFirst = firstNoteTime(source);
    const double sourceLast  = lastNoteTime(source);

    // Act：生成独立副本并同步其统一物件引用视图。
    auto result =
        MMM::BeatmapSpeedTransform::createSpeedVersion(source, options);
    result.beatmap.sync();

    // 首先验证结构数量保持，确保后续逐项比较面对同一对象集合。
    ok &= check(result.success, "resource transform succeeds");
    ok &= check(result.beatmap.m_allNotes.size() == source.m_allNotes.size(),
                "resource note count kept");
    ok &= check(result.beatmap.m_timings.size() == source.m_timings.size(),
                "resource timing count kept");
    ok &= check(
        result.beatmap.m_audioSamples.size() == source.m_audioSamples.size(),
        "resource automatic sample count kept");

    // 有正向内容边界时，结果长度应精确等于来源边界除以速度。
    if ( sourceContentEnd > 0.0 ) {
        ok &= check(isNearlyEqual(result.beatmap.m_baseMapMetadata.map_length,
                                  sourceContentEnd / speed,
                                  0.501),
                    "resource content end scaled");
    }
    // 非空统一视图分别检查首尾，能发现整体偏移或局部漏缩放。
    if ( !source.m_allNotes.empty() && !result.beatmap.m_allNotes.empty() ) {
        ok &= check(
            isNearlyEqual(firstNoteTime(result.beatmap), sourceFirst / speed),
            "resource first note time scaled");
        ok &= check(
            isNearlyEqual(lastNoteTime(result.beatmap), sourceLast / speed),
            "resource last note time scaled");
    }

    // 取较小数量保证失败场景仍能输出已有对应项的字段诊断。
    const auto timingCount =
        std::min(source.m_timings.size(), result.beatmap.m_timings.size());
    bool timingTimestampsOk = true;
    bool bpmValuesOk        = true;
    for ( std::size_t i = 0; i < timingCount; ++i ) {
        // 所有 Timing 锚点都除速，只有 BPM 效果的频率乘速。
        const auto& before = source.m_timings[i];
        const auto& after  = result.beatmap.m_timings[i];
        if ( !isNearlyEqual(
                 after.m_timestamp, before.m_timestamp / speed, 1e-3) ) {
            XERROR(
                "[speed-transform] timing timestamp mismatch at {}: {} vs {}",
                i,
                after.m_timestamp,
                before.m_timestamp / speed);
            timingTimestampsOk = false;
        }
        // 无效或非 BPM 值不纳入频率比较，保持转换器的兼容回退边界。
        if ( before.m_timingEffect == MMM::TimingEffect::BPM &&
             before.m_bpm > 0.0 &&
             !isNearlyEqual(after.m_bpm, before.m_bpm * speed, 1e-3) ) {
            XERROR("[speed-transform] bpm mismatch at {}: {} vs {}",
                   i,
                   after.m_bpm,
                   before.m_bpm * speed);
            bpmValuesOk = false;
        }
    }
    ok &= check(timingTimestampsOk, "resource timing timestamps scaled");
    ok &= check(bpmValuesOk, "resource bpm values scaled");

    // 自动采样同样以安全较小数量逐项比较，数量差异已在前面单独断言。
    const auto sampleCount      = std::min(source.m_audioSamples.size(),
                                      result.beatmap.m_audioSamples.size());
    bool       sampleTimelineOk = true;
    for ( std::size_t i = 0; i < sampleCount; ++i ) {
        // 期望偏移使用与生产实现一致的远离零 round 规则。
        const auto& before         = source.m_audioSamples[i];
        const auto& after          = result.beatmap.m_audioSamples[i];
        const auto  expectedOffset = static_cast<std::int64_t>(
            std::round(static_cast<long double>(before.m_offsetMs) / speed));
        // 一次组合检查覆盖两个时间字段及所有必须保持不变的身份字段。
        if ( !isNearlyEqual(
                 after.m_timestamp, before.m_timestamp / speed, 1e-3) ||
             after.m_offsetMs != expectedOffset ||
             after.m_track != before.m_track ||
             after.m_audioResourceId != before.m_audioResourceId ||
             !isNearlyEqual(after.m_volume, before.m_volume) ) {
            XERROR("[speed-transform] automatic sample mismatch at {}", i);
            sampleTimelineOk = false;
        }
    }
    ok &= check(sampleTimelineOk, "resource automatic sample timeline scaled");

    // 生成物目录由测试创建，失败通过 error_code 转换为普通断言。
    std::error_code createError;
    std::filesystem::create_directories(outputPath.parent_path(), createError);
    ok &= check(!createError, "resource output directory created");
    ok &= check(result.beatmap.saveToFile(outputPath),
                "resource transformed map saved");
    // 只有变换与保存全部成功才读取输出，避免次生诊断覆盖首个失败原因。
    if ( ok ) {
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        // 原生加载后重建统一引用视图，再与保存前领域对象比较数量。
        reloaded.sync();
        // 当前资源覆盖将格式往返差异记为 warning，精确结构由 fixture 强校验。
        if ( reloaded.m_allNotes.size() != result.beatmap.m_allNotes.size() ) {
            XWARN(
                "[speed-transform] transformed MMM reload note count differs "
                "for {}: saved={} reloaded={}",
                inputPath.string(),
                result.beatmap.m_allNotes.size(),
                reloaded.m_allNotes.size());
        } else {
            check(true, "resource transformed map reload note count");
        }
        // Timing 数量诊断独立于玩家物件，便于定位原生写出问题。
        if ( reloaded.m_timings.size() != result.beatmap.m_timings.size() ) {
            XWARN(
                "[speed-transform] transformed MMM reload timing count "
                "differs for {}: saved={} reloaded={}",
                inputPath.string(),
                result.beatmap.m_timings.size(),
                reloaded.m_timings.size());
        } else {
            check(true, "resource transformed map reload timing count");
        }
        // 自动采样数量单独检查，防止格式迁移合成或遗漏事件。
        if ( reloaded.m_audioSamples.size() !=
             result.beatmap.m_audioSamples.size() ) {
            XWARN(
                "[speed-transform] transformed MMM reload sample count "
                "differs for {}: saved={} reloaded={}",
                inputPath.string(),
                result.beatmap.m_audioSamples.size(),
                reloaded.m_audioSamples.size());
        } else {
            check(true, "resource transformed map reload sample count");
        }
    }

    // 返回强断言累计状态；warning 不改变资源用例结果。
    return ok;
}

/// @brief 运行真实资源覆盖测试。
/// @param resourceRoot 资源根目录。
/// @param outputRoot 输出根目录。
/// @return 通过时返回 true。
/// @details
/// 资源按稳定路径顺序依次转换，输出名使用序号避免原始特殊字符影响目标路径。
/// 所有文件都执行，即使中途出现失败也继续收集后续格式的诊断和通过计数。
/// @par 覆盖边界
/// - 递归发现覆盖四种受支持谱面扩展名及其子目录。
/// - 路径排序保证同一资源集每次获得相同输出编号。
/// - 每个资源固定使用 1.5 倍，精确倍率边界由 fixture 覆盖。
/// - 输出统一为 MMM，不覆盖来源文件，也不向 tests/data 写入结果。
/// - 单文件失败不短路批次，最终汇总仍反映全部资源状态。
/// - 空资源集合明确失败，不能用零个用例伪装覆盖通过。
/// - 该批次不是所有来源格式字段的无损往返验收。
/// - 音频转码不属于本测试，audioPath 仅作为谱面元数据提示。
bool runResourceCoverage(const std::filesystem::path& resourceRoot,
                         const std::filesystem::path& outputRoot)
{
    // 发现阶段只筛选扩展名，实际格式有效性由每个资源用例验证。
    const auto files = collectBeatmapFiles(resourceRoot);
    bool       ok    = true;
    ok &= check(!files.empty(), "resource beatmap files discovered");

    // passed 仅用于汇总日志，最终成功仍由累计布尔值决定。
    std::size_t passed = 0;
    for ( std::size_t i = 0; i < files.size(); ++i ) {
        // 输出当前进度和来源路径，批量失败可直接定位具体资源。
        XINFO("[speed-transform] Resource case {} / {}: {}",
              i + 1,
              files.size(),
              files[i].string());
        // 统一保存为 MMM，将不同来源归一到完整领域格式后再加载检查。
        const auto outputPath =
            outputRoot / ("speed_transform_case_" + std::to_string(i) + ".mmm");
        if ( validateResourceBeatmap(files[i], outputPath, 1.5) ) {
            // 单个资源全部强断言通过后才计入通过数量。
            ++passed;
        } else {
            // 保留失败状态但不提前退出，继续覆盖其余文件格式。
            ok = false;
        }
    }

    XINFO("[speed-transform] Resource coverage passed {}/{}",
          passed,
          files.size());
    // 空资源目录已由前置断言标记失败，汇总仍会显示 0/0。
    return ok;
}

}  // namespace

/// @brief 运行程序化与可选真实资源倍速覆盖。
/// @param argc 参数数量；至少三个参数时启用资源和 Malody 文件往返。
/// @param argv 参数数组，依次包含程序名、资源根目录和输出根目录。
/// @return 全部启用场景通过时返回 EXIT_SUCCESS，否则返回 EXIT_FAILURE。
/// @par 运行模式
/// - 无额外参数时只运行完全程序化的内存 fixture。
/// - 提供两个路径参数时追加 Malody delay 与真实资源覆盖。
/// - 第二个路径必须是可写测试输出目录，不能指向源资源目录。
/// - 资源根目录为空或不可访问时由发现数量断言失败。
/// - 日志初始化和关闭由 runner 自身配对，不依赖其他测试进程。
/// - 失败退出码只由累计强断言决定，warning 不单独改变退出状态。
/// - runner 不接受部分路径参数，避免只有输入而无安全输出位置。
/// - 各用例自行使用 error_code 处理文件系统失败。
int main(int argc, char* argv[])
{
    // 独立初始化测试日志域，退出前在所有分支显式关闭。
    XLogger::init("BeatmapSpeedTransformTest");

    // 程序化 fixture 始终执行，不依赖外部资源路径。
    bool ok = runFixtureCoverage();
    if ( argc >= 3 ) {
        // 文件型 delay 用例与资源批量共用调用方提供的输出根目录。
        ok &= runMalodyDelayRoundTrip(argv[2]);
        ok &= runResourceCoverage(argv[1], argv[2]);
    }

    if ( !ok ) {
        // 失败也先刷新和关闭日志，再返回标准非零退出码。
        XLogger::shutdown();
        return EXIT_FAILURE;
    }

    // 只有所有已启用层次均通过才输出总成功消息。
    XINFO("BeatmapSpeedTransformTest passed.");
    XLogger::shutdown();
    return EXIT_SUCCESS;
}
