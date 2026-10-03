#include "ui/utils/ProjectResourceImport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

/// @file ProjectResourceImportTest.cpp
/// @brief 项目资源分类、项目内复用、同名保护和文件错误回归测试。
/// @details 所有文件都写入调用方提供的测试输出目录，不接触真实项目资源。
///
/// 断言重点：
/// - 常见音频、图片和视频扩展名不区分大小写；
/// - 未知及伪装复合后缀保持不支持；
/// - 外部同名资源追加递增数字后缀；
/// - 已存在项目文件内容不会被覆盖；
/// - 项目内资源直接复用相对路径；
/// - 缺失路径和目录不会被当作普通文件导入。

/// @brief 验证资源导入的同名保护、项目内复用、子目录及失败处理。
/// @param argc 必须包含隔离测试输出根目录。
/// @param argv argv[1] 为本次测试输出根目录。
/// @return 0 表示全部通过，非零值标识具体分类或导入失败场景。
int main(int argc, char** argv)
{
    using MMM::UI::Utils::classifyProjectResource;
    using MMM::UI::Utils::ProjectResourceType;
    // 分类必须忽略扩展名大小写，并只接受明确白名单格式。
    if ( classifyProjectResource("song.MP3") != ProjectResourceType::Audio ||
         classifyProjectResource("cover.JpEg") != ProjectResourceType::Image ||
         classifyProjectResource("background.MP4") !=
             ProjectResourceType::Video ||
         classifyProjectResource("background.WebM") !=
             ProjectResourceType::Video ||
         classifyProjectResource("chart.mmm") !=
             ProjectResourceType::Unsupported ||
         classifyProjectResource("cover.png.exe") !=
             ProjectResourceType::Unsupported )
        // 伪装复合后缀按最后扩展名判定为不支持。
        return 11;
    // 文件导入场景需要 CTest 提供隔离目录。
    if ( argc != 2 ) return 1;
    namespace fs = std::filesystem;
    // 单调时间戳避免并发或重复运行使用同一目录。
    const auto root =
        fs::path(argv[1]) /
        ("resource-import-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    // 建立项目子目录和外部来源目录两类路径。
    fs::create_directories(root / "project" / "nested", error);
    if ( error ) return 2;
    fs::create_directories(root / "external", error);
    if ( error ) return 3;
    // 同名文件内容不同，用于验证导入不会覆盖原项目文件。
    std::ofstream(root / "external" / "cover.png") << "new image";
    std::ofstream(root / "project" / "cover.png") << "original image";
    std::ofstream(root / "project" / "nested" / "audio.ogg") << "audio";
    using MMM::UI::Utils::importProjectResource;
    const auto project = root / "project";
    // 首次外部同名导入应选择 cover_1.png。
    const auto copied =
        importProjectResource(project, root / "external" / "cover.png");
    if ( !copied || *copied != "cover_1.png" ) return 4;
    // 原 cover.png 内容必须保持不变。
    std::string original;
    std::getline(std::ifstream(project / "cover.png"), original);
    if ( original != "original image" ) return 5;
    // 已位于项目根内的资源直接返回相对路径，不产生第二份复制。
    const auto reused = importProjectResource(project, project / *copied);
    if ( !reused || *reused != *copied ) return 6;
    // 项目子目录资源应保留完整嵌套相对路径。
    const auto nested =
        importProjectResource(project, project / "nested" / "audio.ogg");
    if ( !nested || *nested != fs::path("nested/audio.ogg") ) return 7;
    // 缺失源和目录源都必须返回失败 expected。
    if ( importProjectResource(project, root / "absent.png") ) return 8;
    if ( importProjectResource(project, root / "external") ) return 9;
    // 再次导入同一外部源应继续选择 cover_2.png。
    const auto second =
        importProjectResource(project, root / "external" / "cover.png");
    if ( !second || *second != "cover_2.png" ) return 10;
    // 视频文件覆盖大小写分类、复制和内容完整性。
    std::ofstream(root / "external" / "background.MP4") << "video payload";
    const auto video =
        importProjectResource(project, root / "external" / "background.MP4");
    if ( !video ||
         classifyProjectResource(*video) != ProjectResourceType::Video )
        return 12;
    // 复制后的目标内容必须与外部源一致。
    std::string videoContent;
    std::getline(std::ifstream(project / *video), videoContent);
    if ( videoContent != "video payload" ) return 13;
    // 原始编码与配套数据以同一音频文件名绑定，改名导入后必须仍能读取。
    // 简短音频占位文件足以验证配套路径；此测试不宣称验证音频解码器。
    std::ofstream(root / "external" / "chapter.aac") << "audio";
    std::ofstream(root / "external" / "chapter.aac.mmm-timing.json")
        << R"({"version":1,"audio_size":5,"chapters":[{"seconds":1.25,"title":"verse"}],"bpm_segments":[{"seconds":0.25,"bpm":150.5}]})";
    const auto chapterAudio =
        importProjectResource(project, root / "external" / "chapter.aac");
    if ( !chapterAudio ) return 14;
    // 此处必须读取项目副本，不能误从仍存在的外部来源读取。
    // 精确数值断言验证配套文件已迁移，而非只复制一个同名空文件。
    // 不调用音频解码器，避免把路径事务测试变成声卡或编码器测试。
    const auto restored =
        MMM::Audio::AudioMarkerService::read(project / *chapterAudio);
    if ( !restored.success || restored.data.chapters.size() != 1 ||
         restored.data.bpmSegments.size() != 1 ||
         restored.data.chapters.front().title != "verse" ||
         restored.data.bpmSegments.front().bpm != 150.5 )
        return 15;
    // 标记损坏的外部资源导入必须回滚新音频，避免项目登记一个失去章节的副本。
    std::ofstream(root / "external" / "bad.aac") << "audio";
    std::ofstream(root / "external" / "bad.aac.mmm-timing.json") << "bad json";
    if ( importProjectResource(project, root / "external" / "bad.aac") ||
         fs::exists(project / "bad.aac", error) )
        return 16;
    // 音频本身没有同名冲突，但已有配套文件时必须另选新名称。
    std::ofstream(project / "chapter_1.aac.mmm-timing.json")
        << "existing metadata";
    // 预先留下的配套文件属于项目原数据，即使没有同名音频也要保护。
    // 新候选必须跳过该名称并保留绑定关系。
    // 成功回读新路径能够发现只改音频名而未改配套名的错误。
    const auto renamed =
        importProjectResource(project, root / "external" / "chapter.aac");
    if ( !renamed || *renamed != "chapter_2.aac" ||
         !MMM::Audio::AudioMarkerService::read(project / *renamed).success )
        return 17;
    // 全部断言通过后保留输出夹具供检查，不清理来源之外的文件。
    // 测试目录由单调时间戳隔离，重复测试不会覆盖前一轮产物。
    return 0;
}
