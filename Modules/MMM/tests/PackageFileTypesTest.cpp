#include "mmm/project/PackageFileTypes.h"
#include "log/colorful-log.h"

#include <cstdlib>

namespace
{

/// @brief 验证 MCZ 候选枚举和最终写包校验仅接受声明的图片格式。
/// @return PNG、JPG、JPEG、WebP 均通过且 BMP 被拒绝时返回 true。
bool testMczKeepsImageAllowlist()
{
    const auto& types =
        MMM::getPackageSupportedFileTypes(MMM::PackageFileType::Mcz);
    for ( const auto extension : MMM::MCZ_PACKAGE_IMAGE_EXTENSIONS ) {
        if ( !MMM::isPackageResourceExtensionSupported(
                 types, MMM::PackageResourceType::Image, extension) ||
             !MMM::isPackageCandidateExtensionSupported(types, extension) ) {
            XERROR("MCZ image rules rejected supported format: {}", extension);
            return false;
        }
    }

    if ( MMM::isPackageResourceExtensionSupported(
             types, MMM::PackageResourceType::Image, ".bmp") ||
         MMM::isPackageCandidateExtensionSupported(types, ".bmp") ) {
        XERROR("MCZ image rules unexpectedly accepted BMP");
        return false;
    }

    return MMM::isPackageCandidateExtensionSupported(types, ".JPEG") &&
           MMM::isPackageCandidateExtensionSupported(types, ".WEBP");
}

/// @brief 验证 MCZ 音频仅允许 WAV、OGG 和 MP3。
/// @return 三种允许格式均通过且 FLAC 被拒绝时返回 true。
bool testMczKeepsAudioAllowlist()
{
    const auto& types =
        MMM::getPackageSupportedFileTypes(MMM::PackageFileType::Mcz);
    for ( const auto extension : MMM::MCZ_PACKAGE_AUDIO_EXTENSIONS ) {
        if ( !MMM::isPackageResourceExtensionSupported(
                 types, MMM::PackageResourceType::Audio, extension) ||
             !MMM::isPackageCandidateExtensionSupported(types, extension) ) {
            XERROR("MCZ audio rules rejected supported format: {}", extension);
            return false;
        }
    }

    if ( MMM::isPackageResourceExtensionSupported(
             types, MMM::PackageResourceType::Audio, ".flac") ||
         MMM::isPackageCandidateExtensionSupported(types, ".flac") ) {
        XERROR("MCZ audio rules unexpectedly accepted FLAC");
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    return testMczKeepsImageAllowlist() && testMczKeepsAudioAllowlist()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
