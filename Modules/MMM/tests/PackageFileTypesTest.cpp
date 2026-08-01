#include "mmm/project/PackageFileTypes.h"
#include "log/colorful-log.h"

#include <cstdlib>

namespace
{

/// @brief 验证 MCZ 候选枚举和最终写包校验均接受 WAV 音频。
/// @return 两条共享规则均接受 WAV 时返回 true。
bool testMczAcceptsWavAudio()
{
    const auto& types =
        MMM::getPackageSupportedFileTypes(MMM::PackageFileType::Mcz);
    if ( !MMM::isPackageResourceExtensionSupported(
             types, MMM::PackageResourceType::Audio, ".wav") ) {
        XERROR("MCZ audio rules rejected WAV");
        return false;
    }
    if ( !MMM::isPackageCandidateExtensionSupported(types, ".WAV") ) {
        XERROR("MCZ candidate rules rejected uppercase WAV");
        return false;
    }
    return true;
}

/// @brief 验证 MCZ 仍会拒绝未声明支持的音频格式。
/// @return FLAC 未被误纳入 MCZ 时返回 true。
bool testMczRejectsUnsupportedAudio()
{
    const auto& types =
        MMM::getPackageSupportedFileTypes(MMM::PackageFileType::Mcz);
    if ( MMM::isPackageResourceExtensionSupported(
             types, MMM::PackageResourceType::Audio, ".flac") ) {
        XERROR("MCZ audio rules unexpectedly accepted FLAC");
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    return testMczAcceptsWavAudio() && testMczRejectsUnsupportedAudio()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
