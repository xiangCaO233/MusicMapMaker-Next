#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace
{

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

/// @brief 验证 osu! 字符串 Video 事件能够作为唯一背景载入。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testPureStringVideoEvent(const std::filesystem::path& outputDirectory)
{
    const auto                 path = outputDirectory / "pure_string_video.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
Video,1234,"video.mp4"
)";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "string Video event should select video background");
    ok &= check(meta.video_starttime == 1234,
                "string Video event should keep start time");
    ok &= check(meta.main_cover_path == std::filesystem::path("video.mp4"),
                "string Video event should keep video path");
    return ok;
}

/// @brief 验证数字 1 视频事件优先于同文件中的图片背景事件。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testNumericVideoEventPriority(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "numeric_video_priority.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
0,0,"image.jpg",3,4
1,5678,"numeric.mp4"
)";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap map  = MMM::BeatMap::loadFromFile(path);
    const auto&        meta = map.m_baseMapMetadata;
    bool               ok   = true;
    ok &= check(meta.cover_type == MMM::CoverType::VIDEO,
                "numeric video event should override image background");
    ok &= check(meta.video_starttime == 5678,
                "numeric video event should keep start time");
    ok &= check(meta.main_cover_path == std::filesystem::path("numeric.mp4"),
                "numeric video event should keep video path");
    return ok;
}

/// @brief 验证没有视频事件时继续读取普通图片背景。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testImageEventFallback(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "image_background_fallback.osu";
    constexpr std::string_view content = R"(osu file format v14

[Events]
0,0,"image.jpg",12,-8
)";
    if ( !writeTextFile(path, content) ) return false;

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

/// @brief 验证 MMM 原生格式完整往返视频背景元数据。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testMMMVideoMetadataRoundTrip(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "video_metadata_round_trip.mmm";

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

    if ( !source.saveToFile(path) ) {
        XERROR("Failed to save MMM metadata round-trip fixture: {}",
               path.string());
        return false;
    }

    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    const auto&        meta   = loaded.m_baseMapMetadata;
    bool               ok     = true;
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

/// @brief 验证旧版 MMM 文件缺少新增字段时保持默认图片配置。
/// @param outputDirectory 测试输出目录。
/// @return 验证是否通过。
bool testLegacyMMMMetadataDefaults(const std::filesystem::path& outputDirectory)
{
    const auto path = outputDirectory / "legacy_metadata_defaults.mmm";
    constexpr std::string_view content =
        R"({"metadata":{"base":{"name":"Legacy","cover":"legacy.png"}},"timing":[],"note":[]})";
    if ( !writeTextFile(path, content) ) return false;

    const MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(path);
    const auto&        meta   = loaded.m_baseMapMetadata;
    bool               ok     = true;
    ok &= check(meta.cover_type == MMM::CoverType::IMAGE,
                "legacy MMM should default to image background");
    ok &= check(meta.video_starttime == 0,
                "legacy MMM should default video start time to zero");
    ok &= check(meta.bgxoffset == 0 && meta.bgyoffset == 0,
                "legacy MMM should default background offsets to zero");
    return ok;
}

}  // namespace

/// @brief 运行背景元数据格式兼容测试。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数，首个参数为测试输出目录。
/// @return 全部验证通过时返回 0，否则返回 1。
int main(int argc, char* argv[])
{
    if ( argc < 2 ) {
        XERROR("Usage: MetadataCompatibilityTest <output_directory>");
        return 1;
    }

    const std::filesystem::path outputDirectory = argv[1];
    std::error_code             directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);
    if ( directoryError ) {
        XERROR("Failed to create metadata compatibility output directory: {}",
               directoryError.message());
        return 1;
    }

    bool ok = true;
    ok &= testPureStringVideoEvent(outputDirectory);
    ok &= testNumericVideoEventPriority(outputDirectory);
    ok &= testImageEventFallback(outputDirectory);
    ok &= testMMMVideoMetadataRoundTrip(outputDirectory);
    ok &= testLegacyMMMMetadataDefaults(outputDirectory);

    if ( ok ) {
        XINFO("Metadata compatibility tests passed.");
        return 0;
    }
    return 1;
}
