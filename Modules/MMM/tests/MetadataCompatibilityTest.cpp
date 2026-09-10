#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/ProjectSettings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <system_error>

/**
 * @file MetadataCompatibilityTest.cpp
 * @brief 验证项目状态与谱面元数据在新旧格式间的兼容边界。
 *
 * 本文件使用程序化生成的最小夹具，避免把只服务于单个字段组合的资源
 * 长期固化到 tests/data。每个场景只保留触发目标分支所需的字段，因此
 * 失败时可以直接判断是解析默认值、迁移规则还是保存器能力判断发生变化。
 *
 * 测试覆盖五组契约：
 *
 * 1. 项目工作区状态
 *    - 音频工具、时间点表和批注表的开关必须彼此独立往返；
 *    - 选中资源、画笔音量和自由布局不得因新增字段而丢失；
 *    - 旧布局缺少宽高时继续使用自动尺寸，而不是生成异常负值。
 *
 * 2. 背景资源事件
 *    - osu! 的字符串 Video 和数字类型 1 都表示视频背景；
 *    - 同时出现图片与视频时，视频拥有更高的显示优先级；
 *    - 只有图片时仍保留路径和偏移，不受视频兼容逻辑影响；
 *    - MMM 原生格式必须完整保存背景类型、路径、起播时间和偏移。
 *
 * 3. MMM 版本迁移
 *    - 当前保存器输出 format_version 3 的规范字段；
 *    - 玩家命中采样属于 Note，自动播放采样属于 audio_samples；
 *    - 旧 audio 字段只在对应版本中承担兼容迁移或资源提示职责；
 *    - 非法自动采样轨道会迁入第一条 BGM 轨，并留下结构化诊断；
 *    - 旧 bound_sound 会迁为嵌套 sample，且默认音量为 1。
 *
 * 4. 单音频格式能力
 *    - osu! 与 RM/IMD 只能表示一条可折叠的主音频；
 *    - 非零时间线自动采样和多层自动采样必须在写文件前拒绝；
 *    - RM/IMD 不支持玩家物件采样，osu! 则可以保存该绑定；
 *    - 保存失败不得遗留半成品，以免后续扫描误认为有效谱面；
 *    - 空 BGM 轨声明按各格式的实际表达能力分别处理。
 *
 * 5. 批注与稳定身份
 *    - 折线整体批注和各子节点批注互不覆盖；
 *    - 同一时间戳允许存在多个 Markdown 批注；
 *    - PLAYER_OBJECT 与 AUDIO_SAMPLE 通过 collaborationId 保持目标身份；
 *    - 时间戳相同不应导致不同目标种类被错误合并。
 *
 * 这些场景同时约束“能读取旧数据”和“新数据不倒退为旧表示”两件事。
 * 读取兼容允许吸收历史字段，保存则应输出当前规范结构；否则一次普通保存
 * 就可能永久丢失新语义，或把仅用于界面提示的路径重新变成播放对象。
 *
 * 所有生成文件都写入构建树传入的输出目录。测试不会修改 tests/data，
 * 也不会依赖用户配置目录，从而保证重复执行和并行构建的隔离性。
 */

/**
 * @brief 本测试的结果判读约定。
 *
 * 兼容性测试不是要求所有格式保存完全相同的数据，而是要求每个格式严格
 * 遵守自己能够表达的语义。出现失败时，应先按以下类别定位：
 *
 * - “should round trip” 失败通常表示序列化字段遗漏、字段改名未迁移，
 *   或保存前同步索引没有覆盖对应对象。
 * - “should default” 失败表示历史文档缺字段时的默认值发生变化，需确认
 *   该变化是否会改变旧项目的视觉、播放或窗口布局。
 * - “should migrate” 失败表示旧字段已被读取，但没有转换为当前统一模型；
 *   仅把路径留在 metadata 中不等于音频事件已经迁移。
 * - “should reject” 失败表示保存器接受了目标格式无法表达的状态，容易
 *   造成无提示的数据丢失，比单纯保存失败更危险。
 * - “should not leave partial files” 失败表示能力检查发生得太晚，目标
 *   路径已经被创建或截断，需要把验证移动到实际写入之前。
 * - “should not synthesize” 失败表示提示性元数据越权生成了播放内容，
 *   会让只查看或保存项目的操作改变听感。
 * - “should emit a diagnostic” 失败表示修复性迁移对上层不可见，用户
 *   无法知道文件已被自动调整，也无法决定是否回到原格式修正。
 *
 * 数值比较遵循字段本身的表示能力：
 *
 * - 时间戳和音量经过 JSON 浮点往返，使用小误差比较；
 * - offset_ms、轨道、视频起播时间等整数语义必须精确相等；
 * - 文件系统路径按 path 比较，避免平台分隔符影响字符串断言；
 * - 数组大小先于下标访问检查，保证失败报告不被越界行为覆盖；
 * - 稳定目标 ID 按字符串精确比较，禁止以时间戳近似代替身份。
 *
 * 对保存结果采用两层判定：
 *
 * - 原始 JSON 或文本层用于检查保存器是否仍输出废弃字段；
 * - 重新加载后的模型层用于检查正式读取路径能否恢复业务语义；
 * - 两者缺一不可，因为宽容读取器可能掩盖错误输出；
 * - 对二进制 IMD 则主要通过模型重载验证，并额外确认外置资源发现；
 * - 对拒绝场景检查文件不存在，约束保存操作的事务边界。
 *
 * 测试夹具的构造也遵循稳定性约定：
 *
 * - 每个临时文件使用独立名称，避免不同场景互相覆盖；
 * - 测试开始前删除需要验证“不产生文件”的旧目标；
 * - 不读取用户配置、最近项目或皮肤配置；
 * - 不依赖执行顺序提供状态，除输出目录外没有共享可变夹具；
 * - 文件内容保持最小化，让新增解析默认值不会误激活无关分支。
 * - 日志中的预期导出拒绝属于成功场景的一部分，不应只凭 error 级别判失败；
 * - 最终以进程退出码和对应文件存在性共同判断能力检查是否符合契约；
 * - 重新运行可覆盖同名成功产物，但拒绝场景会先主动清理旧文件。
 *
 * 若未来增加新的元数据字段，应根据职责选择场景：工作区字段加入配置
 * 往返组，格式公共字段加入原生往返组，外部格式专属字段加入相应格式组，
 * 需要修复的历史字段同时覆盖读取迁移、规范回写和诊断可见性。不要只在
 * source 与 loaded 间比较一个新字段，而遗漏原始保存结构和旧文件缺字段
 * 时的默认行为。
 */

namespace
{

using json = nlohmann::json;

/// @brief 校验测试条件并记录失败原因。
/// @param condition 待校验条件。
/// @param message 条件失败时输出的说明。
/// @return 条件是否成立。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("Metadata compatibility check failed: {}", message);
    }
    return condition;
}

/// @brief 将程序化测试内容写入构建输出目录。
/// @param path 输出文件路径。
/// @param content 待写入文本。
/// @return 文件是否写入成功。
bool writeTextFile(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file.is_open() ) {
        XERROR("Failed to open metadata compatibility fixture: {}",
               path.string());
        return false;
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

/**
 * @brief 验证项目音频工具的打开状态、选择和方块布局可完整往返。
 *
 * 工作区状态属于项目级 UI 持久化，不参与谱面内容序列化。测试同时放入
 * 显式尺寸和自动尺寸两种布局，确保新增自由布局能力不会要求旧配置补字段。
 * 三个窗口开关分别断言，用于维持批注表、时间点表与音频工具的独立性。
 *
 * @return 所有新旧字段均遵守各自缺省值时返回 true。
 */
bool testProjectAudioToolWorkspaceRoundTrip()
{
    // 同时打开三个面板，用于证明它们是并列状态，而非由音频工具开关派生。
    // 该组合曾经容易因工作区字段演进而只恢复其中一个窗口。
    MMM::ProjectWorkspaceState source;
    source.m_projectAudioToolOpen               = true;
    source.m_timingPointsTableOpen              = true;
    source.m_annotationTableOpen                = true;
    source.m_projectAudioToolSelectedResourceId = "main";
    source.m_projectAudioToolBrushVolume        = 0.75F;
    source.m_projectAudioToolPlacements         = {
        // 第一项显式提供尺寸，覆盖用户调整过的自由布局。
        MMM::ProjectAudioToolItemPlacement{
                    .m_audioResourceId = "main",
                    .m_x               = 12.5F,
                    .m_y               = 30.0F,
                    .m_width           = 260.0F,
                    .m_height          = 120.0F,
                    .m_zOrder          = 4,
        },
        // 第二项省略尺寸，模拟仍依赖自动尺寸的旧版项目配置。
        MMM::ProjectAudioToolItemPlacement{
                    .m_audioResourceId = "effect",
                    .m_x               = 80.0F,
                    .m_y               = 50.0F,
                    .m_zOrder          = 5,
        },
    };

    // 先经过正式 ADL 序列化入口，再从 JSON 恢复，避免测试只覆盖成员复制。
    const json serialized = source;
    const auto restored   = serialized.get<MMM::ProjectWorkspaceState>();
    // 单独构造不含宽高的历史布局，锁定新增尺寸字段的缺省语义。
    const auto legacyPlacement =
        json{
            { "m_audioResourceId", "legacy" },
            { "m_x", 4.0F },
            { "m_y", 8.0F },
            { "m_zOrder", 1 },
        }
            .get<MMM::ProjectAudioToolItemPlacement>();
    // 开关、选择和画笔状态属于工作区级字段，均应逐项保持。
    return check(restored.m_projectAudioToolOpen,
                 "project audio tool open state should round trip") &&
           check(restored.m_timingPointsTableOpen,
                 "timing table open state should round trip") &&
           check(restored.m_annotationTableOpen,
                 "annotation table open state should round trip") &&
           check(restored.m_projectAudioToolSelectedResourceId == "main",
                 "project audio selection should round trip") &&
           check(
               std::abs(restored.m_projectAudioToolBrushVolume - 0.75F) < 1e-6F,
               "project audio brush volume should round trip") &&
           check(restored.m_projectAudioToolPlacements.size() == 2,
                 "project audio placements should round trip") &&
           // 位置、显式尺寸和层级共同决定恢复后的交互布局，不能只比较资源 ID。
           check(
               restored.m_projectAudioToolPlacements[1].m_audioResourceId ==
                       "effect" &&
                   std::abs(restored.m_projectAudioToolPlacements[1].m_x -
                            80.0F) < 1e-6F &&
                   std::abs(restored.m_projectAudioToolPlacements[0].m_width -
                            260.0F) < 1e-6F &&
                   std::abs(restored.m_projectAudioToolPlacements[0].m_height -
                            120.0F) < 1e-6F &&
                   restored.m_projectAudioToolPlacements[1].m_zOrder == 5,
               "project audio placement fields should remain intact") &&
           // 零尺寸是自动布局的哨兵值，不应在读取旧配置时擅自填入固定尺寸。
           check(legacyPlacement.m_width == 0.0F &&
                     legacyPlacement.m_height == 0.0F,
                 "legacy project audio placements should keep automatic size");
}

/**
 * @brief 验证新项目继承软件配色且旧项目保持皮肤配色。
 *
 * 字段缺失与显式空字符串具有不同语义：前者来自尚未支持项目配色的旧文件，
 * 应保持当时的皮肤配色；后者由新版界面保存，表示主动继承软件全局方案。
 * 默认构造则代表新建项目，同样应继承软件方案。
 *
 * @return 新项目、旧项目和显式继承三种来源均保持约定时返回 true。
 */
bool testProjectColorPaletteDefaults()
{
    // 默认构造代表新建项目；空 JSON 则代表没有配色字段的旧项目。
    const MMM::ProjectSettings defaults;
    const auto legacy = json::object().get<MMM::ProjectSettings>();
    // 显式空字符串表示继承软件配色，语义不同于历史字段缺失。
    const auto inherited =
        json{ { "m_colorPaletteSchemeName", "" } }.get<MMM::ProjectSettings>();
    const std::string_view skinDefault =
        MMM::Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
    // 三种来源必须分开断言，防止默认值统一后破坏旧项目视觉。
    return check(defaults.m_colorPaletteSchemeName.empty(),
                 "new projects should inherit the software palette") &&
           check(legacy.m_colorPaletteSchemeName == skinDefault,
                 "projects without a palette field should use the skin "
                 "palette") &&
           check(inherited.m_colorPaletteSchemeName.empty(),
                 "explicit software palette inheritance should remain intact");
}

/**
 * @brief 验证 osu! 字符串 Video 事件能够作为唯一背景载入。
 *
 * osu! Events 允许使用可读事件名而非数字编号。夹具不提供其它背景字段，
 * 用来证明解析器独立识别 Video，并完整读取起播时间和带引号路径。
 *
 * @param outputDirectory 最小 osu! 文本夹具的输出目录。
 * @return 背景类型、路径和起播时间均正确时返回 true。
 */
bool testPureStringVideoEvent(const std::filesystem::path& outputDirectory)
{
    // 仅保留 Events 段，证明视频识别不依赖 General 或 Difficulty 元数据。
    const auto                 path = outputDirectory / "pure_string_video.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
Video,1234,"video.mp4"
)";
    // 写入失败属于夹具准备失败，不能继续用空 BeatMap 制造误导性断言。
    if ( !writeTextFile(path, content) ) return false;

    // 字符串 Video 是 osu! 文本格式允许的事件名写法。
    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    // 类型、时间和路径分别验证，避免只识别事件却丢失其中的载荷。
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "string Video event should select video background");
    ok &= check(meta.video_starttime == 1234,
                "string Video event should keep start time");
    ok &= check(meta.main_cover_path == std::filesystem::path("video.mp4"),
                "string Video event should keep video path");
    return ok;
}

/**
 * @brief 验证数字 1 视频事件优先于同文件中的图片背景事件。
 *
 * 图片事件先出现，视频事件后出现。最终背景必须由语义优先级决定，而不是
 * 简单保留首项或末项；否则同一文件在事件排序变化后会显示不同资源。
 *
 * @param outputDirectory 混合背景事件夹具的输出目录。
 * @return 视频类型、路径和起播时间覆盖图片选择时返回 true。
 */
bool testNumericVideoEventPriority(const std::filesystem::path& outputDirectory)
{
    // 图片放在视频之前，专门验证后出现的视频能提升最终背景优先级。
    const auto path = outputDirectory / "numeric_video_priority.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
0,0,"image.jpg",3,4
1,5678,"numeric.mp4"
)";
    if ( !writeTextFile(path, content) ) return false;

    // 数字事件类型 1 与字符串 Video 应进入同一视频解析语义。
    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    // 若解析器按首个背景短路，这组三项会暴露图片错误抢占视频的问题。
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "numeric video event should override image background");
    ok &= check(meta.video_starttime == 5678,
                "numeric video event should keep start time");
    ok &= check(meta.main_cover_path == std::filesystem::path("numeric.mp4"),
                "numeric video event should keep video path");
    return ok;
}

/**
 * @brief 验证没有视频事件时继续读取普通图片背景。
 *
 * 视频兼容逻辑不能把普通背景事件当作无效候选。除图片路径外还检查横纵偏移，
 * 因为偏移与背景资源来自同一事件，最容易在分支提前返回时被遗漏。
 *
 * @param outputDirectory 图片事件夹具的输出目录。
 * @return 图片类型、路径和两个偏移量全部保留时返回 true。
 */
bool testImageEventFallback(const std::filesystem::path& outputDirectory)
{
    // 不提供任何视频事件，确保兼容扩展没有吞掉传统图片背景。
    const auto path = outputDirectory / "image_background_fallback.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
0,0,"image.jpg",12,-8
)";
    if ( !writeTextFile(path, content) ) return false;

    // 图片偏移也是事件载荷的一部分，应和资源路径同步恢复。
    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    ok &= check(meta.cover_type == MMM::CoverType::IMAGE,
                "image event should remain the fallback background");
    ok &= check(meta.main_cover_path == std::filesystem::path("image.jpg"),
                "image event should keep image path");
    ok &= check(meta.bgxoffset == 12 && meta.bgyoffset == -8,
                "image event should keep background offsets");
    return ok;
}

/**
 * @brief 验证 MMM 原生格式完整往返视频背景元数据。
 *
 * 外部格式解析通过后仍可能在保存为项目原生格式时丢字段，因此该场景直接
 * 构造统一模型并执行磁盘往返。视频类型、起播时间、路径和偏移必须成组保持。
 *
 * @param outputDirectory 原生 MMM 夹具的输出目录。
 * @return 所有视频背景字段无损往返时返回 true。
 */
bool testMMMVideoMetadataRoundTrip(const std::filesystem::path& outputDirectory)
{
    // 原生格式没有外部格式降级，所有视频字段都应无损往返。
    const auto path = outputDirectory / "video_metadata_round_trip.mmm";

    // 补齐基础谱面元数据，使保存器按有效原生谱面路径输出。
    MMM::BeatMap source;
    auto&        sourceMeta    = source.m_baseMapMetadata;
    sourceMeta.name            = "Video metadata round trip";
    sourceMeta.main_cover_path = "videos/background.mp4";
    sourceMeta.cover_type      = MMM::CoverType::VIDEO;
    sourceMeta.video_starttime = 2468;
    sourceMeta.bgxoffset       = -17;
    sourceMeta.bgyoffset       = 29;
    sourceMeta.track_count     = 4;
    sourceMeta.preference_bpm  = 120.0;
    sourceMeta.map_length      = 30000.0;

    // 保存失败时路径可能不存在，后续加载没有验证价值，立即结束场景。
    if ( !source.saveToFile(path) ) {
        XERROR("Failed to save MMM metadata round-trip fixture: {}",
               path.string());
        return false;
    }

    // 从磁盘重新解析而不是检查 source，覆盖真实 JSON 字段名与转换函数。
    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    const auto&        meta   = loaded.m_baseMapMetadata;
    bool               ok     = true;
    // 背景类型决定渲染器选择图片还是视频，必须和路径一起保持。
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "MMM round trip should keep cover type");
    ok &= check(meta.video_starttime == 2468,
                "MMM round trip should keep video start time");
    ok &= check(meta.bgxoffset == -17 && meta.bgyoffset == 29,
                "MMM round trip should keep background offsets");
    ok &= check(
        meta.main_cover_path == std::filesystem::path("videos/background.mp4"),
        "MMM round trip should keep background path");
    return ok;
}

/**
 * @brief 验证当前 MMM 格式完整保存玩家命中采样与多个自动采样对象。
 *
 * 场景故意把命中采样和自动采样放在同一谱面中，防止两种相似的音频引用
 * 被合并为同一个 JSON 数组。两条自动采样使用不同轨道、偏移、音量和
 * 格式专属 metadata，从而验证保存器没有只保留“主音频”公共子集。
 *
 * 保存结果先按原始 JSON 检查当前字段，再按统一模型重载检查消费路径。
 * 文件名保留历史 v2 名称是测试沿革，不代表预期保存版本；当前断言要求
 * format_version 3，且禁止重新输出 metadata.base.audio 和 bound_sound。
 *
 * @param outputDirectory 仅用于测试产物的构建树目录。
 * @return 规范结构和模型往返全部成立时返回 true。
 */
bool testMMMVersion2AudioSampleRoundTrip(
    const std::filesystem::path& outputDirectory)
{
    // 文件名沿用 v2 场景名称，但当前保存器应主动升级为 format_version 3。
    const auto path = outputDirectory / "audio_sample_v2_round_trip.mmm";

    // 玩家轨为 0..3，随后三条 BGM 轨的绝对索引从 4 开始。
    MMM::BeatMap source;
    source.m_baseMapMetadata.name            = "Audio sample v2";
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.bgm_track_count = 3;
    source.m_baseMapMetadata.preference_bpm  = 120.0;
    source.m_baseMapMetadata.main_audio_path = "legacy-main.ogg";
    source.m_baseMapMetadata.song_file_hint  = "display-hint.ogg";

    // Note 上的绑定由玩家击打触发，不得混入自动播放时间线。
    MMM::Note note;
    note.m_timestamp = 1500.0;
    note.m_track     = 2;
    note.setSampleBinding({ "hit.wav", 0.65F });
    source.m_noteData.notes.push_back(note);

    // 第一条自动采样位于首条 BGM 轨，并携带负播放偏移和来源元数据。
    MMM::AudioSampleEvent stem;
    stem.m_timestamp       = 500.0;
    stem.m_offsetMs        = -125;
    stem.m_track           = 4;
    stem.m_audioResourceId = "stem.ogg";
    stem.m_volume          = 0.8F;
    stem.m_metadata
        .sample_properties[MMM::SampleMetadataType::MALODY]["source_x"] = "4";
    source.m_audioSamples.push_back(stem);

    // 第二条自动采样位于更后的 BGM 轨，用于防止保存器只保留主音频。
    MMM::AudioSampleEvent effect;
    effect.m_timestamp       = 1000.0;
    effect.m_offsetMs        = 250;
    effect.m_track           = 6;
    effect.m_audioResourceId = "effect.wav";
    effect.m_volume          = 0.35F;
    effect.m_metadata
        .sample_properties[MMM::SampleMetadataType::MMM]["editor_label"] =
        "effect layer";
    source.m_audioSamples.push_back(effect);
    // sync 建立统一索引，确保保存器看到的集合与实际编辑态一致。
    source.sync();

    // 先验证正式保存入口，再读取原始 JSON 检查规范字段布局。
    if ( !source.saveToFile(path) ) {
        XERROR("Failed to save MMM v2 audio sample fixture: {}", path.string());
        return false;
    }

    json saved;
    {
        // 原始 JSON 检查能够发现“读取器容错掩盖保存器退化”的情况。
        std::ifstream input(path);
        if ( !input ) return false;
        input >> saved;
    }

    bool ok = true;
    // 当前版本号是保存契约的一部分，旧版本读取能力不能成为继续输出旧结构的理由。
    ok &= check(saved.value("format_version", 0) == 3,
                "MMM native saver should declare format_version 3");
    ok &= check(saved["metadata"]["base"].value("bgm_track_count", 0) == 3,
                "MMM v2 should save BGM track count");
    ok &= check(saved["metadata"]["base"].value("song_file_hint", "") ==
                    "display-hint.ogg",
                "MMM v2 should save song file hint separately");
    ok &= check(!saved["metadata"]["base"].contains("audio"),
                "MMM v2 should not emit the legacy audio field");
    // 自动播放采样必须独立成数组，数量不能被主音频提示字段折叠。
    ok &= check(
        saved.contains("audio_samples") && saved["audio_samples"].size() == 2,
        "MMM v2 should save every automatic sample");
    if ( saved.contains("audio_samples") &&
         saved["audio_samples"].size() == 2 ) {
        // 第一项逐字段锁定命名和数值类型，特别是整数 offset_ms。
        const auto& first = saved["audio_samples"][0];
        ok &= check(first.value("audio_ref", "") == "stem.ogg",
                    "MMM v2 should save sample resource id");
        ok &= check(first.value("offset_ms", 0) == -125,
                    "MMM v2 should use integer offset_ms");
        ok &= check(!first.contains("offset"),
                    "MMM v2 should not emit transitional offset");
        // track 是统一模型中的绝对轨道，不是相对 BGM 轨编号。
        ok &= check(first.value("track", 0) == 4,
                    "MMM v2 should save absolute sample track");
        ok &= check(std::abs(first.value("volume", 0.0) - 0.8) < 1e-6,
                    "MMM v2 should save normalized sample volume");
    }
    ok &= check(saved.contains("note") && saved["note"].size() == 1,
                "MMM v2 should save the playable note");
    if ( saved.contains("note") && saved["note"].size() == 1 ) {
        // 玩家采样嵌套在 note.sample，借此与 audio_samples 的自动触发语义分离。
        const auto& savedNote = saved["note"][0];
        ok &= check(savedNote.contains("sample"),
                    "MMM v2 should use a nested playable sample binding");
        ok &= check(!savedNote.contains("bound_sound") &&
                        !savedNote.contains("bound_volume"),
                    "MMM v2 should not emit legacy bound fields");
        if ( savedNote.contains("sample") ) {
            // 资源引用与音量共同组成绑定，缺少任一项都会改变试听结果。
            ok &= check(savedNote["sample"].value("audio_ref", "") == "hit.wav",
                        "MMM v2 should save playable sample resource id");
            ok &= check(std::abs(savedNote["sample"].value("volume", 0.0) -
                                 0.65) < 1e-6,
                        "MMM v2 should save playable sample volume");
        }
    }

    // 再走一次正式加载流程，验证保存结构能够还原为统一模型。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    // 加载后重新同步引用索引，按编辑器实际消费方式进行断言。
    loaded.sync();
    ok &= check(loaded.m_baseMapMetadata.bgm_track_count == 3,
                "MMM v2 should reload BGM track count");
    ok &= check(loaded.m_baseMapMetadata.main_audio_path.empty() &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("display-hint.ogg"),
                "MMM v2 should keep song hint separate from legacy audio path");
    ok &= check(loaded.m_audioSamples.size() == 2,
                "MMM v2 should reload every automatic sample");
    if ( loaded.m_audioSamples.size() == 2 ) {
        // 既比较时序字段，也比较格式专属元数据，防止往返只保留公共子集。
        const auto& loadedStem   = loaded.m_audioSamples[0];
        const auto& loadedEffect = loaded.m_audioSamples[1];
        ok &= check(loadedStem.m_audioResourceId == "stem.ogg" &&
                        loadedStem.m_timestamp == 500.0 &&
                        loadedStem.m_offsetMs == -125 &&
                        loadedStem.m_track == 4 &&
                        std::abs(loadedStem.m_volume - 0.8F) < 1e-6F,
                    "MMM v2 should reload the first automatic sample");
        ok &= check(loadedStem.m_metadata.getValue<std::string>(
                        MMM::SampleMetadataType::MALODY, "source_x") == "4",
                    "MMM v2 should reload Malody sample metadata");
        ok &= check(loadedEffect.m_audioResourceId == "effect.wav" &&
                        loadedEffect.m_timestamp == 1000.0 &&
                        loadedEffect.m_offsetMs == 250 &&
                        loadedEffect.m_track == 6 &&
                        std::abs(loadedEffect.m_volume - 0.35F) < 1e-6F,
                    "MMM v2 should reload the second automatic sample");
        ok &= check(
            loadedEffect.m_metadata.getValue<std::string>(
                MMM::SampleMetadataType::MMM, "editor_label") == "effect layer",
            "MMM v2 should reload native sample metadata");
    }
    // 自动采样不得进入可击打物件索引，否则渲染和判定都会产生幽灵音符。
    ok &= check(loaded.m_allNotes.size() == 1,
                "automatic samples should not enter playable note list");
    if ( loaded.m_allNotes.size() == 1 ) {
        // 从统一物件引用读取绑定，覆盖 sync 后实际业务访问路径。
        const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
        ok &= check(binding.has_value() &&
                        binding->m_audioResourceId == "hit.wav" &&
                        std::abs(binding->m_volume - 0.65F) < 1e-6F,
                    "MMM v2 should reload playable sample binding");
    }
    return ok;
}

/**
 * @brief 验证 MMM 可完整保存和读取未绑定资源的静音采样草稿。
 *
 * 编辑器允许先放置采样再选择资源。空资源 ID 因而是可恢复的中间状态，
 * 不能被加载器当成损坏对象过滤；时间、轨道和音量仍需为后续编辑保留。
 *
 * @param outputDirectory 静音草稿文件的输出目录。
 * @return 空资源 ID 及其余编辑字段完整往返时返回 true。
 */
bool testMMMSilentSampleDraftRoundTrip(
    const std::filesystem::path& outputDirectory)
{
    // 空 audioResourceId 表示尚未选择素材的编辑草稿，而不是无效事件。
    const auto path = outputDirectory / "silent_sample_draft.mmm";

    // 保留非默认音量和明确轨道，证明空资源不会导致整个对象被过滤。
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.bgm_track_count = 1;
    source.m_audioSamples.push_back(MMM::AudioSampleEvent{
        .m_timestamp       = 1250.0,
        .m_offsetMs        = 0,
        .m_track           = 4,
        .m_audioResourceId = {},
        .m_volume          = 0.45F,
    });
    // 草稿也应可持久化，不能要求资源 ID 非空才允许保存项目。
    if ( !source.saveToFile(path) ) return false;

    // 先验证对象数量，再访问 front，避免失败场景产生越界行为。
    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    if ( loaded.m_audioSamples.size() != 1 ) {
        return check(false, "MMM v2 should reload a silent sample draft");
    }
    const auto& sample = loaded.m_audioSamples.front();
    // 空 ID、时序、轨道与音量共同构成草稿恢复所需的最小状态。
    return check(sample.m_audioResourceId.empty() &&
                     sample.m_timestamp == 1250.0 && sample.m_offsetMs == 0 &&
                     sample.m_track == 4 &&
                     std::abs(sample.m_volume - 0.45F) < 1e-6F,
                 "MMM v2 should preserve silent sample draft fields");
}

/**
 * @brief 验证无版本 MMM 文件迁移单音频字段和旧玩家采样字段。
 *
 * 这是读取兼容与规范回写的组合场景。旧 audio 会建立主音频兼容路径、
 * 歌曲提示和自动采样，旧 bound_sound 则建立玩家物件的嵌套采样绑定。
 * timing 与 note 的时间必须保持原值，证明迁移只增加音频语义而不平移谱面。
 *
 * 同目录存在原始 .mc 时还应生成可重新导入诊断；随后删除对应原始文件，
 * 再证明诊断不会无条件出现。最后检查重新保存的 JSON 只含当前规范字段，
 * 防止旧表示在每次保存后继续传播。
 *
 * @param outputDirectory 程序化旧文件和迁移结果的输出目录。
 * @return 默认值、迁移、诊断和规范回写全部通过时返回 true。
 */
bool testLegacyMMMMetadataDefaults(const std::filesystem::path& outputDirectory)
{
    // 不写 format_version，明确触发最早期 MMM 文档的迁移分支。
    const auto path = outputDirectory / "legacy_metadata_defaults.mmm";
    // 同名前缀 .mc 模拟仍可重新导入的原始 Malody 谱面。
    const auto originalMalodyPath =
        outputDirectory / "legacy_metadata_defaults.mc";
    // 单行 JSON 只保留旧 audio、旧 bound_sound、一个时间点和一个音符。
    constexpr std::string_view content =
        R"({"metadata":{"base":{"name":"Legacy","audio":"legacy.ogg","cover":"legacy.png","track_count":4}},"timing":[{"timestamp":250,"bpm":120,"beat_length":500,"effect":"bpm","param":120}],"note":[{"type":"note","timestamp":1000,"track":1,"bound_sound":"legacy-hit.wav"}]})";
    if ( !writeTextFile(path, content) ||
         !writeTextFile(originalMalodyPath, "{}") ) {
        // 两个文件共同定义诊断前置条件，任一缺失都不能继续验证。
        return false;
    }

    // 加载器需要同时完成默认值填充、音频对象迁移和诊断生成。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    const auto&  meta   = loaded.m_baseMapMetadata;
    bool         ok     = true;
    // 旧文件没有背景类型，兼容默认值必须维持传统图片语义。
    ok &= check(meta.cover_type == MMM::CoverType::IMAGE,
                "legacy MMM should default to image background");
    ok &= check(meta.video_starttime == 0,
                "legacy MMM should default video start time to zero");
    ok &= check(meta.bgxoffset == 0 && meta.bgyoffset == 0,
                "legacy MMM should default background offsets to zero");
    ok &= check(meta.main_audio_path == std::filesystem::path("legacy.ogg") &&
                    meta.song_file_hint == std::filesystem::path("legacy.ogg"),
                "legacy MMM audio should populate both compatibility fields");
    // 旧 audio 既提供历史路径，也应物化为首条 BGM 轨上的自动播放对象。
    ok &= check(meta.track_count == 4 && meta.bgm_track_count == 1,
                "legacy MMM audio should create one BGM track");
    ok &= check(
        loaded.m_loadDiagnostics.size() == 1 &&
            loaded.m_loadDiagnostics.front().m_code ==
                MMM::BeatmapLoadDiagnosticCode::
                    LEGACY_MMM_ORIGINAL_MALODY_AVAILABLE &&
            loaded.m_loadDiagnostics.front().m_severity ==
                MMM::BeatmapLoadDiagnosticSeverity::
                    BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING &&
            loaded.m_loadDiagnostics.front().m_relatedPath ==
                originalMalodyPath,
        "legacy MMM should expose a structured reimport diagnostic when the "
        "original Malody file exists");
    // 迁移出的主音频应从时间零开始、无偏移并保持默认满音量。
    ok &= check(loaded.m_audioSamples.size() == 1,
                "legacy MMM audio should migrate to one automatic sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& sample = loaded.m_audioSamples.front();
        ok &= check(sample.m_timestamp == 0.0 && sample.m_offsetMs == 0 &&
                        sample.m_track == 4 &&
                        sample.m_audioResourceId == "legacy.ogg" &&
                        std::abs(sample.m_volume - 1.0F) < 1e-6F,
                    "legacy MMM automatic sample migration should be stable");
    }
    // 玩家物件仍需保留；主音频迁移不能改变其时序或类别。
    ok &= check(loaded.m_allNotes.size() == 1,
                "legacy playable note should still load");
    if ( loaded.m_allNotes.size() == 1 ) {
        ok &= check(loaded.m_allNotes.front().get().m_timestamp == 1000.0,
                    "legacy audio migration should not move playable notes");
        const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
        // 旧字段没有独立音量时，兼容层按满音量构造规范绑定。
        ok &= check(binding.has_value() &&
                        binding->m_audioResourceId == "legacy-hit.wav" &&
                        std::abs(binding->m_volume - 1.0F) < 1e-6F,
                    "legacy bound_sound should default binding volume to one");
    }
    ok &= check(loaded.m_timings.size() == 1 &&
                    loaded.m_timings.front().m_timestamp == 250.0,
                "legacy audio migration should not move timing events");

    // 将已经迁移的统一模型再次保存，验证输出只包含当前规范字段。
    const auto migratedPath =
        outputDirectory / "legacy_metadata_migrated_v2.mmm";
    ok &= check(loaded.saveToFile(migratedPath),
                "migrated legacy MMM should save");
    if ( ok ) {
        // 读取原始保存结果，防止当前加载器再次容忍旧字段而掩盖回写问题。
        json          migrated;
        std::ifstream input(migratedPath);
        input >> migrated;
        ok &= check(migrated.value("format_version", 0) == 3,
                    "migrated legacy MMM should save as v3");
        ok &= check(!migrated["metadata"]["base"].contains("audio"),
                    "migrated MMM v2 should omit the legacy audio field");
        // 主音频迁移后应成为唯一规范 audio_samples 项。
        ok &= check(migrated.contains("audio_samples") &&
                        migrated["audio_samples"].size() == 1 &&
                        migrated["audio_samples"][0].value("offset_ms", 1) == 0,
                    "migrated legacy MMM should emit canonical audio_samples");
        ok &=
            check(migrated["note"].size() == 1 &&
                      migrated["note"][0].contains("sample") &&
                      !migrated["note"][0].contains("bound_sound"),
                  "migrated legacy note should emit canonical sample binding");
    }

    // 第二个历史文件故意不提供同名前缀 .mc，用于验证诊断不会无条件出现。
    const auto withoutOriginalPath =
        outputDirectory / "legacy_metadata_without_original.mmm";
    auto absentOriginalPath = withoutOriginalPath;
    absentOriginalPath.replace_extension(".mc");
    std::error_code removeError;
    // 清除上次测试可能留下的文件，保证“不存在”前置条件可重复成立。
    std::filesystem::remove(absentOriginalPath, removeError);
    ok &= check(writeTextFile(withoutOriginalPath, content),
                "legacy MMM fixture without original Malody should be written");
    if ( std::filesystem::exists(withoutOriginalPath) ) {
        // 只有原始谱面确实存在时，重新导入建议才对用户可执行。
        const MMM::BeatMap withoutOriginal =
            MMM::BeatMap::loadFromFile(withoutOriginalPath);
        ok &= check(withoutOriginal.m_loadDiagnostics.empty(),
                    "legacy MMM should not emit a reimport diagnostic without "
                    "a sibling Malody file");
    }
    return ok;
}

/**
 * @brief 验证带版本号的过渡 MMM 仍能读取旧 metadata.base.audio 提示。
 *
 * 该字段在无版本文件中有迁移权限，在 v2 文档中只作为早期过渡提示读取。
 * 版本判断必须先于对象生成，避免同一字段在新版项目中意外恢复播放权限。
 *
 * @param outputDirectory 过渡版本夹具的输出目录。
 * @return 字段只恢复为 song_file_hint 且不生成采样时返回 true。
 */
bool testVersion2LegacyAudioHintCompatibility(
    const std::filesystem::path& outputDirectory)
{
    // 显式 format_version 2 改变旧 audio 字段语义：它只能充当显示提示。
    const auto path = outputDirectory / "v2_legacy_audio_hint.mmm";
    constexpr std::string_view content =
        R"({"format_version":2,"metadata":{"base":{"audio":"legacy-hint.ogg","track_count":4}},"audio_samples":[],"timing":[],"note":[]})";
    if ( !writeTextFile(path, content) ) return false;

    // 版本化文档禁止把提示字段重新提升为有播放权限的主音频。
    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    bool               ok     = true;
    ok &= check(loaded.m_baseMapMetadata.main_audio_path.empty(),
                "MMM v2 legacy audio field must not regain runtime authority");
    ok &= check(loaded.m_baseMapMetadata.song_file_hint ==
                    std::filesystem::path("legacy-hint.ogg"),
                "MMM v2 should accept metadata.base.audio as a hint fallback");
    // 若生成 audio_samples，加载一次旧项目就会凭空改变实际播放内容。
    ok &= check(loaded.m_audioSamples.empty(),
                "MMM v2 hint fallback must not synthesize a playback object");
    return ok;
}

/**
 * @brief 验证 MMM v2 自动采样不会停留在玩家轨道区。
 *
 * 自动采样错误落在玩家轨时仍有可恢复内容，加载器应移动而不是删除。迁移
 * 同时补足 BGM 轨声明、保留 original_track，并向上层暴露结构化诊断。
 *
 * @param outputDirectory 非法轨道夹具的输出目录。
 * @return 迁移位置、来源元数据和诊断均正确时返回 true。
 */
bool testVersion2InvalidSampleTrackRelocation(
    const std::filesystem::path& outputDirectory)
{
    // 玩家轨数量为 4，但采样错误写在轨道 2，且文档宣称没有 BGM 轨。
    const auto path = outputDirectory / "v2_invalid_sample_track.mmm";
    constexpr std::string_view content =
        R"({"format_version":2,"metadata":{"base":{"track_count":4,"bgm_track_count":0}},"audio_samples":[{"timestamp":125,"offset_ms":-25,"track":2,"audio_ref":"effect.wav","volume":0.75}],"timing":[],"note":[]})";
    if ( !writeTextFile(path, content) ) return false;

    // 加载器不能丢弃可恢复采样，应把它搬到首条合法 BGM 轨。
    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    bool               ok     = true;
    ok &= check(loaded.m_audioSamples.size() == 1,
                "MMM v2 should retain an invalid-track sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& sample = loaded.m_audioSamples.front();
        // 首条 BGM 轨的绝对索引等于玩家轨总数，同时需要补足 BGM 轨计数。
        ok &=
            check(sample.m_track == 4 &&
                      loaded.m_baseMapMetadata.bgm_track_count == 1,
                  "MMM v2 invalid sample track should move to first BGM lane");
        ok &= check(sample.m_metadata.getValue<std::string>(
                        MMM::SampleMetadataType::MMM, "original_track") == "2",
                    "MMM v2 should retain the original invalid track");
    }
    // 修复性迁移必须可观测，界面才能告知用户文件已被规范化。
    ok &= check(std::any_of(loaded.m_loadDiagnostics.begin(),
                            loaded.m_loadDiagnostics.end(),
                            [](const MMM::BeatmapLoadDiagnostic& diagnostic) {
                                return diagnostic.m_code ==
                                       MMM::BeatmapLoadDiagnosticCode::
                                           AUDIO_SAMPLE_TRACK_RELOCATED;
                            }),
                "MMM v2 invalid sample track should emit a diagnostic");
    return ok;
}

/**
 * @brief 验证 osu! 单音频字段迁移到第一条 BGM 轨且不移动谱面事件。
 *
 * AudioFilename 在统一模型中既提供兼容路径，也形成时间零的自动采样。
 * 测试保留一条 timing 和 note，专门证明该迁移不改变已有谱面时间轴。
 *
 * @param outputDirectory osu! 输入和二次导出文件的输出目录。
 * @return 单音频规范化和二次往返均保持模型语义时返回 true。
 */
bool testOSUSingleAudioMigration(const std::filesystem::path& outputDirectory)
{
    // 使用带空格的文件名，顺便覆盖 General::AudioFilename 的原样保留。
    const auto sourcePath = outputDirectory / "osu_single_audio_source.osu";
    // 最小 osu! 夹具包含一条 timing 和一枚 note，用于观察迁移是否扰动时间轴。
    constexpr std::string_view content = R"(osu file format v14

[General]
AudioFilename: legacy audio.ogg
Mode: 3

[Difficulty]
CircleSize: 4

[TimingPoints]
0,500,4,2,0,100,1,0

[HitObjects]
64,192,1000,1,0,0:0:0:0:
)";
    if ( !writeTextFile(sourcePath, content) ) return false;

    // osu! 的单一 AudioFilename 同时映射为兼容路径提示和自动播放对象。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    bool         ok     = true;
    ok &= check(loaded.m_baseMapMetadata.main_audio_path ==
                        std::filesystem::path("legacy audio.ogg") &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("legacy audio.ogg"),
                "osu! audio should populate both compatibility fields");
    ok &= check(loaded.m_baseMapMetadata.track_count == 4 &&
                    loaded.m_baseMapMetadata.bgm_track_count >= 1,
                "osu! audio should reserve the first BGM track");
    // 统一模型需要显式的 audioSamples，不能只把路径停留在 metadata 中。
    ok &= check(loaded.m_audioSamples.size() == 1,
                "osu! audio should migrate to one automatic sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        const auto& sample = loaded.m_audioSamples.front();
        // 单音频格式固定折叠到首条 BGM 轨，并以时间零、零偏移播放。
        ok &= check(sample.m_timestamp == 0.0 && sample.m_offsetMs == 0 &&
                        sample.m_track == 4 &&
                        sample.m_audioResourceId == "legacy audio.ogg" &&
                        std::abs(sample.m_volume - 1.0F) < 1e-6F,
                    "osu! automatic sample migration should use 0/0/K");
    }
    // 音频迁移只能新增音频对象，不得整体平移原谱面的 timing 与 note。
    ok &= check(loaded.m_timings.size() == 1 &&
                    loaded.m_timings.front().m_timestamp == 0.0,
                "osu! audio migration should not move timing events");
    ok &= check(loaded.m_allNotes.size() == 1 &&
                    loaded.m_allNotes.front().get().m_timestamp == 1000.0,
                "osu! audio migration should not move playable notes");

    const auto exportedPath = outputDirectory / "osu_single_audio_exported.osu";
    // 再次导出验证统一模型能够降回 osu! 的唯一 AudioFilename 表示。
    ok &= check(loaded.saveToFile(exportedPath),
                "canonical osu! single audio should export");
    if ( ok ) {
        // 重新加载导出文件，按业务模型而非文本片段验证完整往返。
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportedPath);
        ok &= check(reloaded.m_audioSamples.size() == 1 &&
                        reloaded.m_audioSamples.front().m_timestamp == 0.0 &&
                        reloaded.m_audioSamples.front().m_offsetMs == 0 &&
                        reloaded.m_audioSamples.front().m_track == 4 &&
                        reloaded.m_audioSamples.front().m_audioResourceId ==
                            "legacy audio.ogg",
                    "canonical osu! single audio should round trip");
        ok &= check(reloaded.m_timings.size() == 1 &&
                        reloaded.m_timings.front().m_timestamp == 0.0 &&
                        reloaded.m_allNotes.size() == 1 &&
                        reloaded.m_allNotes.front().get().m_timestamp == 1000.0,
                    "osu! round trip should keep timing and note positions");
    }
    return ok;
}

/**
 * @brief 验证 RM/IMD 同名前缀音频迁移到第一条 BGM 轨。
 *
 * IMD 本体不保存完整音频路径，加载器依靠同目录、同名前缀文件发现资源。
 * 测试从规范模型导出夹具，再重载验证发现结果成为唯一自动采样。
 *
 * @param outputDirectory IMD 与外置 FLAC 占位文件的输出目录。
 * @return 路径提示、BGM 轨和自动采样均规范化时返回 true。
 */
bool testRMSingleAudioMigration(const std::filesystem::path& outputDirectory)
{
    // RM/IMD 通过同名前缀发现外置音频，因此先在输出目录建立最小占位文件。
    const auto audioPath = outputDirectory / "LegacyAudio.flac";
    if ( !writeTextFile(audioPath, "fLaC") ) return false;

    MMM::BeatMap source;
    // 构造统一模型中的规范单音频事件，再由保存器生成二进制夹具。
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = 0.0;
    sample.m_offsetMs        = 0;
    sample.m_track           = 4;
    sample.m_audioResourceId = "LegacyAudio.flac";
    sample.m_volume          = 1.0F;
    source.m_audioSamples.push_back(sample);

    // 文件名保留 RM 常见的“曲名_轨数_难度”形式，确保前缀发现得到 LegacyAudio。
    const auto mapPath = outputDirectory / "LegacyAudio_4k_Test.imd";
    bool       ok      = true;
    ok &= check(source.saveToFile(mapPath),
                "canonical RM/IMD single audio should export");
    if ( !ok ) return false;

    // 加载器应把外置资源发现结果重新规范为首条 BGM 轨上的采样。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(mapPath);
    ok &= check(loaded.m_baseMapMetadata.main_audio_path ==
                        std::filesystem::path("LegacyAudio.flac") &&
                    loaded.m_baseMapMetadata.song_file_hint ==
                        std::filesystem::path("LegacyAudio.flac"),
                "RM/IMD audio should populate both compatibility fields");
    ok &= check(loaded.m_baseMapMetadata.track_count == 4 &&
                    loaded.m_baseMapMetadata.bgm_track_count >= 1,
                "RM/IMD audio should reserve the first BGM track");
    ok &= check(loaded.m_audioSamples.size() == 1,
                "RM/IMD audio should migrate to one automatic sample");
    if ( loaded.m_audioSamples.size() == 1 ) {
        // 外部音频的默认触发时序同样是 0/0，音量保持满值。
        const auto& loadedSample = loaded.m_audioSamples.front();
        ok &= check(loadedSample.m_timestamp == 0.0 &&
                        loadedSample.m_offsetMs == 0 &&
                        loadedSample.m_track == 4 &&
                        loadedSample.m_audioResourceId == "LegacyAudio.flac" &&
                        std::abs(loadedSample.m_volume - 1.0F) < 1e-6F,
                    "RM/IMD automatic sample migration should use 0/0/K");
    }
    // 夹具没有写入游戏事件，音频发现不得伪造 timing 或 note。
    ok &= check(loaded.m_timings.empty() && loaded.m_allNotes.empty(),
                "RM/IMD audio migration should not add timing or notes");
    return ok;
}

/**
 * @brief 验证单音频格式的 BGM 轨折叠与不兼容时间线拒绝。
 *
 * osu! 和 RM/IMD 的主音频表示能力小于 MMM 统一模型。本场景分别覆盖
 * 非零时间采样、容差边界、多层采样、玩家绑定、后置 BGM 轨折叠以及空
 * BGM 轨声明，要求保存器按各自格式能力作出明确选择。
 *
 * 可以无损折叠的唯一自动采样允许导出；会丢失时间、层级或玩家采样的
 * 状态必须拒绝。拒绝不仅看返回值，还检查目标路径不存在，以保证用户的
 * 旧文件不会先被截断再收到错误提示。
 *
 * @param outputDirectory 所有能力探针和外置占位音频的输出目录。
 * @return 每种格式均只接受可表示状态且拒绝无残留时返回 true。
 */
bool testSingleAudioExporterCompatibility(
    const std::filesystem::path& outputDirectory)
{
    // 第一组构造有明确非零时间和负偏移的自动采样。
    // osu! 与 RM/IMD 的主音频字段无法表达这两个维度，应拒绝降级。
    MMM::BeatMap timedSource;
    timedSource.m_baseMapMetadata.track_count     = 4;
    timedSource.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent timedSample;
    timedSample.m_timestamp       = 500.0;
    timedSample.m_offsetMs        = -25;
    timedSample.m_track           = 4;
    timedSample.m_audioResourceId = "Reject.ogg";
    timedSource.m_audioSamples.push_back(timedSample);

    // 预先删除目标，便于区分本次失败遗留与旧测试产物。
    const auto      timedOSUPath = outputDirectory / "reject_timed_sample.osu";
    const auto      timedIMDPath = outputDirectory / "Reject_4k_Timed.imd";
    std::error_code removeError;
    std::filesystem::remove(timedOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(timedIMDPath, removeError);

    bool ok = true;
    // 两种保存器都必须在写入前完成能力检查。
    ok &= check(!timedSource.saveToFile(timedOSUPath),
                "osu! should reject a timed automatic sample");
    ok &= check(!timedSource.saveToFile(timedIMDPath),
                "RM/IMD should reject a timed automatic sample");
    ok &= check(!std::filesystem::exists(timedOSUPath) &&
                    !std::filesystem::exists(timedIMDPath),
                "rejected timed exports should not leave partial files");

    // RM/IMD 对外部音频时间允许一毫秒容差，以吸收格式换算的浮点噪声。
    MMM::BeatMap nearZeroSource;
    nearZeroSource.m_baseMapMetadata.track_count     = 4;
    nearZeroSource.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent nearZeroSample;
    nearZeroSample.m_timestamp       = 0.32700178886724274;
    nearZeroSample.m_track           = 4;
    nearZeroSample.m_audioResourceId = "NearZero.ogg";
    nearZeroSource.m_audioSamples.push_back(nearZeroSample);

    // 一个典型亚毫秒值应被视为时间零附近，而非不可表示的延迟采样。
    const auto nearZeroIMDPath = outputDirectory / "NearZero_4k_Test.imd";
    removeError.clear();
    std::filesystem::remove(nearZeroIMDPath, removeError);
    ok &= check(nearZeroSource.saveToFile(nearZeroIMDPath),
                "RM/IMD should accept a sub-millisecond audio timestamp");

    // 精确 1.0 ms 是闭区间边界，防止实现误写为严格小于。
    nearZeroSource.m_audioSamples.front().m_timestamp       = 1.0;
    nearZeroSource.m_audioSamples.front().m_audioResourceId = "Boundary.ogg";
    const auto boundaryIMDPath = outputDirectory / "Boundary_4k_Test.imd";
    removeError.clear();
    std::filesystem::remove(boundaryIMDPath, removeError);
    ok &= check(nearZeroSource.saveToFile(boundaryIMDPath),
                "RM/IMD should accept the one-millisecond audio boundary");

    // 只比边界大极小量仍必须拒绝，锁定容差上界而不是近似无限放宽。
    nearZeroSource.m_audioSamples.front().m_timestamp       = 1.000001;
    nearZeroSource.m_audioSamples.front().m_audioResourceId = "Outside.ogg";
    const auto outsideIMDPath = outputDirectory / "Outside_4k_Test.imd";
    removeError.clear();
    std::filesystem::remove(outsideIMDPath, removeError);
    ok &= check(!nearZeroSource.saveToFile(outsideIMDPath),
                "RM/IMD should reject audio beyond one millisecond");
    ok &= check(!std::filesystem::exists(outsideIMDPath),
                "rejected near-zero audio should not leave a partial file");

    // 第二组在两条不同 BGM 轨放置自动采样，构成单音频格式无法表达的分层。
    MMM::BeatMap layeredSource;
    layeredSource.m_baseMapMetadata.track_count     = 4;
    layeredSource.m_baseMapMetadata.bgm_track_count = 1;
    MMM::AudioSampleEvent first;
    first.m_track           = 4;
    first.m_audioResourceId = "Layered.ogg";
    layeredSource.m_audioSamples.push_back(first);
    MMM::AudioSampleEvent second;
    second.m_track           = 5;
    second.m_audioResourceId = "effect.wav";
    layeredSource.m_audioSamples.push_back(second);

    // 同样清理目标文件，确认拒绝路径具备事务性。
    const auto layeredOSUPath = outputDirectory / "reject_layered_audio.osu";
    const auto layeredIMDPath = outputDirectory / "Layered_4k_Test.imd";
    removeError.clear();
    std::filesystem::remove(layeredOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(layeredIMDPath, removeError);
    ok &= check(!layeredSource.saveToFile(layeredOSUPath),
                "osu! should reject multiple automatic samples");
    ok &= check(!layeredSource.saveToFile(layeredIMDPath),
                "RM/IMD should reject multiple automatic samples");
    ok &= check(!std::filesystem::exists(layeredOSUPath) &&
                    !std::filesystem::exists(layeredIMDPath),
                "rejected layered exports should not leave partial files");

    // 第三组验证玩家命中采样：它不属于自动播放层，格式能力需要分别判断。
    MMM::BeatMap boundSource;
    boundSource.m_baseMapMetadata.track_count = 4;
    MMM::Note boundNote;
    boundNote.m_track = 1;
    boundNote.setSampleBinding({ "hit.wav", 0.6F });
    boundSource.m_noteData.notes.push_back(boundNote);
    boundSource.sync();

    // osu! HitSample 能承接绑定，RM/IMD 则没有等价表示。
    const auto boundOSUPath = outputDirectory / "reject_bound_note.osu";
    const auto boundIMDPath = outputDirectory / "Bound_4k_Note.imd";
    removeError.clear();
    std::filesystem::remove(boundOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(boundIMDPath, removeError);
    ok &= check(boundSource.saveToFile(boundOSUPath),
                "osu! should preserve representable playable sample bindings");
    ok &= check(!boundSource.saveToFile(boundIMDPath),
                "RM/IMD should reject playable sample bindings");
    ok &= check(std::filesystem::exists(boundOSUPath) &&
                    !std::filesystem::exists(boundIMDPath),
                "bound-note exports should follow each format capability");
    if ( std::filesystem::exists(boundOSUPath) ) {
        // 从导出文件恢复绑定，防止仅创建文件但遗漏实际资源名。
        const MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(boundOSUPath);
        const auto         binding =
            reloaded.m_allNotes.empty()
                        ? std::optional<MMM::AudioSampleBinding>{}
                        : reloaded.m_allNotes.front().get().getSampleBinding();
        ok &= check(
            binding.has_value() && binding->m_audioResourceId == "hit.wav",
            "osu! should round-trip a playable sample file");
    }

    // 第四组只有一个采样，但它位于声明的最后一条 BGM 轨。
    // RM/IMD 应允许把单个有效音频折叠回唯一主音频，而不是按轨道号拒绝。
    const auto collapsedAudioPath = outputDirectory / "Collapsed.flac";
    ok &= check(writeTextFile(collapsedAudioPath, "fLaC"),
                "collapsed RM/IMD fixture audio should be created");
    MMM::BeatMap collapsedBgmSource;
    collapsedBgmSource.m_baseMapMetadata.track_count     = 6;
    collapsedBgmSource.m_baseMapMetadata.bgm_track_count = 5;
    MMM::AudioSampleEvent collapsedSample;
    collapsedSample.m_track           = 10;
    collapsedSample.m_audioResourceId = "Collapsed.flac";
    collapsedBgmSource.m_audioSamples.push_back(collapsedSample);

    // 保存后重载，确认折叠结果位于新的第一条 BGM 轨且计数收敛为 1。
    const auto collapsedBgmIMDPath = outputDirectory / "Collapsed_6k_Test.imd";
    removeError.clear();
    std::filesystem::remove(collapsedBgmIMDPath, removeError);
    ok &= check(collapsedBgmSource.saveToFile(collapsedBgmIMDPath),
                "RM/IMD should collapse one audio from a later BGM track");
    if ( std::filesystem::exists(collapsedBgmIMDPath) ) {
        const MMM::BeatMap reloaded =
            MMM::BeatMap::loadFromFile(collapsedBgmIMDPath);
        ok &= check(reloaded.m_audioSamples.size() == 1U &&
                        reloaded.m_audioSamples.front().m_track == 6U &&
                        reloaded.m_audioSamples.front().m_audioResourceId ==
                            "Collapsed.flac" &&
                        reloaded.m_baseMapMetadata.bgm_track_count == 1,
                    "RM/IMD should reload the collapsed audio on BGM track 1");
    }

    // 最后一组只有 BGM 轨声明而没有音频对象，用于区分“空容量”和“播放内容”。
    MMM::BeatMap emptyBgmSource;
    emptyBgmSource.m_baseMapMetadata.track_count     = 4;
    emptyBgmSource.m_baseMapMetadata.bgm_track_count = 2;

    // osu! 无法保存没有 AudioFilename 的 BGM 声明；RM/IMD 可忽略空声明。
    const auto emptyBgmOSUPath = outputDirectory / "reject_empty_bgm_lanes.osu";
    const auto emptyBgmIMDPath = outputDirectory / "Empty_4k_Bgm.imd";
    removeError.clear();
    std::filesystem::remove(emptyBgmOSUPath, removeError);
    removeError.clear();
    std::filesystem::remove(emptyBgmIMDPath, removeError);
    ok &= check(!emptyBgmSource.saveToFile(emptyBgmOSUPath),
                "osu! should reject unrepresentable empty BGM tracks");
    ok &= check(emptyBgmSource.saveToFile(emptyBgmIMDPath),
                "RM/IMD should ignore empty BGM track declarations");
    ok &= check(!std::filesystem::exists(emptyBgmOSUPath) &&
                    std::filesystem::exists(emptyBgmIMDPath),
                "empty-BGM exports should follow each format capability");
    return ok;
}

/// @brief 验证 Malody 保存器不再根据歌曲提示伪造 SOUND 对象。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testMalodySaverDoesNotSynthesizeAudioSample(
    const std::filesystem::path& outputDirectory)
{
    // song_file_hint 只帮助界面定位候选资源，不代表时间线上存在播放事件。
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count    = 4;
    source.m_baseMapMetadata.song_file_hint = "hint.ogg";

    // 保存一个完全没有 audioSamples 的 Malody 文件，观察是否伪造 SOUND。
    const auto path = outputDirectory / "malody_hint_without_sample.mc";
    if ( !source.saveToFile(path) ) {
        return false;
    }

    json saved;
    // 直接检查 JSON 节点类型，避免加载器的兼容迁移隐藏伪造对象。
    std::ifstream input(path);
    if ( !input ) return false;
    input >> saved;

    bool hasAutomaticSample = false;
    if ( saved.contains("note") && saved["note"].is_array() ) {
        // Malody type=1 表示 SOUND；普通 note 数组存在本身不构成失败。
        for ( const auto& note : saved["note"] ) {
            if ( note.is_object() && note.value("type", 0) == 1 ) {
                hasAutomaticSample = true;
                break;
            }
        }
    }
    // 提示路径不得获得播放权限，这是元数据与时间线数据的职责边界。
    return check(!hasAutomaticSample,
                 "Malody saver should serialize only explicit samples");
}

/// @brief 验证 osu! 保存器不会把仅作提示的旧音频字段重新物化为播放内容。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testOSUSaverDoesNotSynthesizeAudioSample(
    const std::filesystem::path& outputDirectory)
{
    // 同时提供三个历史/提示来源，但故意不创建任何显式自动采样。
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count     = 4;
    source.m_baseMapMetadata.main_audio_path = "legacy.ogg";
    source.m_baseMapMetadata.song_file_hint  = "hint.ogg";
    source.m_metadata
        .map_properties[MMM::MapMetadataType::OSU]["General::AudioFilename"] =
        "source.ogg";

    // 保存器只允许从 audioSamples 决定 AudioFilename，不能猜测优先级。
    const auto path = outputDirectory / "osu_hint_without_sample.osu";
    if ( !source.saveToFile(path) ) return false;

    std::ifstream input(path);
    if ( !input ) return false;
    // 检查文本是为了确认字段仍存在但值为空，而不是误用任一候选路径。
    const std::string saved((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    return check(saved.find("AudioFilename: \n") != std::string::npos,
                 "osu! saver should serialize audio only from an explicit "
                 "sample");
}

/// @brief 验证 MMM 原生格式分别保留整条折线与子音符注释。
/// @param outputDirectory 测试输出目录。
/// @return 折线结构和两级注释完整往返时返回 true。
bool testMMMPolylineAnnotationRoundTrip(
    const std::filesystem::path& outputDirectory)
{
    // 折线由一段 hold 和一个 flick 转向节点组成，两者都带独立批注。
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count = 4;
    auto& hold        = source.m_noteData.holds.emplace_back();
    hold.m_timestamp  = 1000.0;
    hold.m_duration   = 500.0;
    hold.m_track      = 1;
    hold.m_isSubNote  = true;
    hold.m_annotation = "折线起段注释";
    // flick 既是子音符又改变轨道，用于覆盖异构节点序列。
    auto& flick        = source.m_noteData.flicks.emplace_back();
    flick.m_timestamp  = 1500.0;
    flick.m_track      = 1;
    flick.m_dtrack     = 1;
    flick.m_isSubNote  = true;
    flick.m_annotation = "折线转向注释";
    // 折线容器自身拥有另一条批注，不能覆盖或替代子节点内容。
    auto& polyline        = source.m_noteData.polylines.emplace_back();
    polyline.m_timestamp  = hold.m_timestamp;
    polyline.m_track      = hold.m_track;
    polyline.m_annotation = "整条折线注释";
    polyline.m_subNotes.emplace_back(hold);
    polyline.m_subNotes.emplace_back(flick);
    polyline.m_subHolds.emplace_back(hold);
    polyline.m_subFlicks.emplace_back(flick);
    // sync 建立各具体容器与通用 subNotes 引用之间的统一关系。
    source.sync();

    // 经过正式原生格式往返后，先确认折线没有被展开或丢失。
    const auto path = outputDirectory / "polyline_annotations.mmm";
    if ( !source.saveToFile(path) ) return false;
    const auto restored = MMM::BeatMap::loadFromFile(path);
    if ( restored.m_noteData.polylines.size() != 1U ) {
        return check(false, "annotated polyline structure should be preserved");
    }
    const auto& restoredPolyline = restored.m_noteData.polylines.front();
    // 容器与两个节点的三条批注逐一比较，证明层级之间保持独立。
    return check(
        restoredPolyline.m_annotation == "整条折线注释" &&
            restoredPolyline.m_subNotes.size() == 2U &&
            restoredPolyline.m_subNotes[0].get().m_annotation ==
                "折线起段注释" &&
            restoredPolyline.m_subNotes[1].get().m_annotation == "折线转向注释",
        "MMM polyline annotations should round-trip independently");
}

/**
 * @brief 验证 MMM 原生格式保留同时间戳的多条 Markdown 批注及物件目标。
 *
 * 时间戳只描述批注出现位置，不能充当目标身份。三条批注故意共享同一时间，
 * 但分别指向纯时间点、玩家物件和自动采样；后两者必须借助 collaborationId
 * 跨保存保持引用。Markdown 内容按原文保存，不在模型层执行渲染或清洗。
 *
 * @param outputDirectory 原生往返文件的输出目录。
 * @return 批注值、作者、目标种类及稳定目标 ID 全部保持时返回 true。
 */
bool testMMMBeatmapAnnotationsRoundTrip(
    const std::filesystem::path& outputDirectory)
{
    // 所有目标都放在同一时间戳，迫使序列化依赖目标种类与稳定 ID 区分。
    MMM::BeatMap source;
    source.m_baseMapMetadata.track_count = 4;

    // 玩家物件使用 collaborationId 作为跨保存稳定目标，而非容器下标。
    auto& note             = source.m_noteData.notes.emplace_back();
    note.m_timestamp       = 1250.0;
    note.m_track           = 2;
    note.m_collaborationId = "annotation-note-target";

    // 自动采样使用不同命名空间下的稳定 ID，时间相同也不能与 note 混淆。
    auto& sample             = source.m_audioSamples.emplace_back();
    sample.m_timestamp       = 1250.0;
    sample.m_track           = 4;
    sample.m_audioResourceId = "effect";
    sample.m_collaborationId = "annotation-sample-target";

    // 三条批注分别指向时间点、玩家物件和自动采样，并保留 Markdown 内容。
    source.m_annotations = {
        MMM::BeatmapAnnotation{
            .m_id         = "annotation-at-time",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
            .m_timestamp  = 1250.0,
            .m_author     = "Creator One",
            .m_content    = "# 时间点\n- 检查节奏",
        },
        MMM::BeatmapAnnotation{
            .m_id         = "annotation-on-note",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
            .m_targetId   = "annotation-note-target",
            .m_timestamp  = 1250.0,
            .m_author     = "Creator Two",
            .m_content    = "> 音符位置待确认",
        },
        MMM::BeatmapAnnotation{
            .m_id         = "annotation-on-sample",
            .m_targetKind = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE,
            .m_targetId   = "annotation-sample-target",
            .m_timestamp  = 1250.0,
            .m_author     = "Creator Three",
            .m_content    = "`sample` 音量过大",
        },
    };
    // sync 后按编辑器实际索引状态保存，覆盖稳定身份的完整业务路径。
    source.sync();

    // 原生格式应按值完整保存批注 DTO，不进行时间戳去重或 Markdown 清洗。
    const auto path = outputDirectory / "beatmap_annotations.mmm";
    if ( !source.saveToFile(path) ) return false;
    const auto restored = MMM::BeatMap::loadFromFile(path);
    // 先比较批注集合，再确认其引用的两个稳定目标也确实完成往返。
    return check(restored.m_annotations == source.m_annotations,
                 "MMM beatmap annotations should round-trip independently") &&
           check(restored.m_audioSamples.size() == 1U &&
                     restored.m_audioSamples.front().m_collaborationId ==
                         "annotation-sample-target",
                 "MMM sample annotation target identity should round-trip") &&
           check(restored.m_noteData.notes.size() == 1U &&
                     restored.m_noteData.notes.front().m_collaborationId ==
                         "annotation-note-target",
                 "MMM player annotation target identity should round-trip");
}

}  // namespace

/// @brief 运行背景元数据格式兼容测试。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数，首个参数为测试输出目录。
/// @return 全部验证通过时返回 0，否则返回 1。
int main(int argc, char* argv[])
{
    // 输出目录由 CMake 放在 build/test_output，缺失参数时禁止回落到源码树。
    if ( argc < 2 ) {
        XERROR("Usage: MetadataCompatibilityTest <output_directory>");
        return 1;
    }

    const std::filesystem::path outputDirectory = argv[1];
    std::error_code             directoryError;
    // 使用 error_code 遵守项目禁用异常的约束，并给出可诊断的失败消息。
    std::filesystem::create_directories(outputDirectory, directoryError);
    if ( directoryError ) {
        XERROR("Failed to create metadata compatibility output directory: {}",
               directoryError.message());
        return 1;
    }

    bool ok = true;
    // 工作区和配色场景不产生文件，先验证纯 JSON 配置契约。
    ok &= testProjectAudioToolWorkspaceRoundTrip();
    ok &= testProjectColorPaletteDefaults();
    // 背景组覆盖 osu! 两种视频写法、图片回退和原生视频元数据往返。
    ok &= testPureStringVideoEvent(outputDirectory);
    ok &= testNumericVideoEventPriority(outputDirectory);
    ok &= testImageEventFallback(outputDirectory);
    ok &= testMMMVideoMetadataRoundTrip(outputDirectory);
    // 原生格式组覆盖当前结构、草稿、无版本历史文件和版本化兼容字段。
    ok &= testMMMVersion2AudioSampleRoundTrip(outputDirectory);
    ok &= testMMMSilentSampleDraftRoundTrip(outputDirectory);
    ok &= testLegacyMMMMetadataDefaults(outputDirectory);
    ok &= testVersion2LegacyAudioHintCompatibility(outputDirectory);
    ok &= testVersion2InvalidSampleTrackRelocation(outputDirectory);
    // 外部格式组验证单音频迁移、能力拒绝和提示字段无播放权限。
    ok &= testOSUSingleAudioMigration(outputDirectory);
    ok &= testRMSingleAudioMigration(outputDirectory);
    ok &= testSingleAudioExporterCompatibility(outputDirectory);
    ok &= testMalodySaverDoesNotSynthesizeAudioSample(outputDirectory);
    ok &= testOSUSaverDoesNotSynthesizeAudioSample(outputDirectory);
    // 批注组验证折线层级以及同时间戳多目标的稳定身份。
    ok &= testMMMPolylineAnnotationRoundTrip(outputDirectory);
    ok &= testMMMBeatmapAnnotationsRoundTrip(outputDirectory);

    if ( ok ) {
        // 只有全部场景通过才输出统一成功消息，便于 CTest 日志快速定位。
        XINFO("Metadata compatibility tests passed.");
        return 0;
    }
    return 1;
}
