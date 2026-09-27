#include "mmm/project/PackageFileTypes.h"
#include "log/colorful-log.h"

#include <cstdlib>

namespace
{

/// @file PackageFileTypesTest.cpp
/// @brief 验证 MCZ 打包候选枚举与最终资源校验使用同一扩展名白名单。
///
/// 测试既遍历公开常量中的允许项，也使用明确的反例验证拒绝路径；否则候选
/// 扫描与真正写包规则发生偏离时，用户可能能选择资源却无法完成打包。
/// 图片与音频分别使用一个明确反例，大小写变体则验证扩展名规范化发生在
/// 匹配边界，而不是要求调用者预先转换文件名。
/// 此测试不访问文件系统，扩展名字符串即为完整输入域。
/// 白名单常量是测试输入，两个查询 API 的结果是被验证行为。
/// 新增 MCZ 资源格式时必须同时更新允许项与对应反例预期。

/// @brief 验证 MCZ 候选枚举和最终写包校验仅接受声明的图片格式。
/// @return PNG、JPG、JPEG、WebP 均通过且 BMP 被拒绝时返回 true。
bool testMczKeepsImageAllowlist()
{
    // 每个声明格式必须同时通过按资源类型校验和通用候选文件校验。
    const auto& types =
        MMM::getPackageSupportedFileTypes(MMM::PackageFileType::Mcz);
    for ( const auto extension : MMM::MCZ_PACKAGE_IMAGE_EXTENSIONS ) {
        // 公开白名单中的每一项都必须能被两层 API 消费。
        if ( !MMM::isPackageResourceExtensionSupported(
                 types, MMM::PackageResourceType::Image, extension) ||
             !MMM::isPackageCandidateExtensionSupported(types, extension) ) {
            XERROR("MCZ image rules rejected supported format: {}", extension);
            return false;
        }
    }

    // BMP 是常见但未被 MCZ 契约接受的图片，用作拒绝路径哨兵。
    if ( MMM::isPackageResourceExtensionSupported(
             types, MMM::PackageResourceType::Image, ".bmp") ||
         MMM::isPackageCandidateExtensionSupported(types, ".bmp") ) {
        XERROR("MCZ image rules unexpectedly accepted BMP");
        return false;
    }

    // 大写扩展名验证匹配逻辑不依赖文件系统返回的小写形式。
    return MMM::isPackageCandidateExtensionSupported(types, ".JPEG") &&
           MMM::isPackageCandidateExtensionSupported(types, ".WEBP");
}

/// @brief 验证 MCZ 音频仅允许 WAV、OGG 和 MP3。
/// @return 三种允许格式均通过且 FLAC 被拒绝时返回 true。
bool testMczKeepsAudioAllowlist()
{
    // 音频规则与图片规则独立，不能因扩展名在另一资源类别出现而被接受。
    const auto& types =
        MMM::getPackageSupportedFileTypes(MMM::PackageFileType::Mcz);
    for ( const auto extension : MMM::MCZ_PACKAGE_AUDIO_EXTENSIONS ) {
        // 枚举全部允许音频，防止新增常量后遗漏候选扫描规则。
        if ( !MMM::isPackageResourceExtensionSupported(
                 types, MMM::PackageResourceType::Audio, extension) ||
             !MMM::isPackageCandidateExtensionSupported(types, extension) ) {
            XERROR("MCZ audio rules rejected supported format: {}", extension);
            return false;
        }
    }

    // FLAC 当前不属于 MCZ 允许集合，同时检查分类入口和候选入口均拒绝。
    if ( MMM::isPackageResourceExtensionSupported(
             types, MMM::PackageResourceType::Audio, ".flac") ||
         MMM::isPackageCandidateExtensionSupported(types, ".flac") ) {
        XERROR("MCZ audio rules unexpectedly accepted FLAC");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行 MCZ 图片与音频白名单契约测试。
/// @return 两组允许项、反例和大小写规则全部正确时返回成功。
int main()
{
    return testMczKeepsImageAllowlist() && testMczKeepsAudioAllowlist()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
