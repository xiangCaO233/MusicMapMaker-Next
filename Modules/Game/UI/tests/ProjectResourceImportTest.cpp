#include "ui/utils/ProjectResourceImport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

/// @brief 验证资源导入的同名保护、项目内复用、子目录及失败处理。
int main(int argc, char** argv)
{
    using MMM::UI::Utils::classifyProjectResource;
    using MMM::UI::Utils::ProjectResourceType;
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
        return 11;
    if ( argc != 2 ) return 1;
    namespace fs = std::filesystem;
    const auto root =
        fs::path(argv[1]) /
        ("resource-import-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    fs::create_directories(root / "project" / "nested", error);
    if ( error ) return 2;
    fs::create_directories(root / "external", error);
    if ( error ) return 3;
    std::ofstream(root / "external" / "cover.png") << "new image";
    std::ofstream(root / "project" / "cover.png") << "original image";
    std::ofstream(root / "project" / "nested" / "audio.ogg") << "audio";
    using MMM::UI::Utils::importProjectResource;
    const auto project = root / "project";
    const auto copied =
        importProjectResource(project, root / "external" / "cover.png");
    if ( !copied || *copied != "cover_1.png" ) return 4;
    std::string original;
    std::getline(std::ifstream(project / "cover.png"), original);
    if ( original != "original image" ) return 5;
    const auto reused = importProjectResource(project, project / *copied);
    if ( !reused || *reused != *copied ) return 6;
    const auto nested =
        importProjectResource(project, project / "nested" / "audio.ogg");
    if ( !nested || *nested != fs::path("nested/audio.ogg") ) return 7;
    if ( importProjectResource(project, root / "absent.png") ) return 8;
    if ( importProjectResource(project, root / "external") ) return 9;
    const auto second =
        importProjectResource(project, root / "external" / "cover.png");
    if ( !second || *second != "cover_2.png" ) return 10;
    std::ofstream(root / "external" / "background.MP4") << "video payload";
    const auto video =
        importProjectResource(project, root / "external" / "background.MP4");
    if ( !video ||
         classifyProjectResource(*video) != ProjectResourceType::Video )
        return 12;
    std::string videoContent;
    std::getline(std::ifstream(project / *video), videoContent);
    if ( videoContent != "video payload" ) return 13;
    return 0;
}
