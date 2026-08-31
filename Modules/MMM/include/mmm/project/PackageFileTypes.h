#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace MMM
{

/// @brief 谱面打包目标格式。
enum class PackageFileType {
    Mcz,
    Osz,
    Mpk,
};

/// @brief 打包资源分类。
enum class PackageResourceType {
    Audio,
    Video,
    Image,
    Beatmap,
};

/// @brief 单个打包格式支持的资源扩展名规则。
struct PackageSupportedFileTypes {
    /// @brief 打包文件本身的扩展名。
    std::string_view m_packageExtension;

    /// @brief 支持打包的音频文件扩展名。
    std::span<const std::string_view> m_audioExtensions;

    /// @brief 支持打包的视频文件扩展名。
    std::span<const std::string_view> m_videoExtensions;

    /// @brief 支持打包的图片文件扩展名。
    std::span<const std::string_view> m_imageExtensions;

    /// @brief 支持打包的谱面文件扩展名。
    std::span<const std::string_view> m_beatmapExtensions;

    /// @brief 是否允许任意音频文件扩展名。
    bool m_allowAllAudioFormats{ false };

    /// @brief 是否允许任意视频文件扩展名。
    bool m_allowAllVideoFormats{ false };

    /// @brief 是否允许任意图片文件扩展名。
    bool m_allowAllImageFormats{ false };
};

/// @brief MCZ 支持的音频扩展名。
inline constexpr std::array<std::string_view, 3> MCZ_PACKAGE_AUDIO_EXTENSIONS{
    ".ogg", ".mp3", ".wav"
};

/// @brief MCZ 支持的视频扩展名。
inline constexpr std::array<std::string_view, 1> MCZ_PACKAGE_VIDEO_EXTENSIONS{
    ".mp4"
};

/// @brief MCZ 支持的图片扩展名。
inline constexpr std::array<std::string_view, 4> MCZ_PACKAGE_IMAGE_EXTENSIONS{
    ".png", ".jpg", ".jpeg", ".webp"
};

/// @brief MCZ 支持的谱面扩展名。
inline constexpr std::array<std::string_view, 1> MCZ_PACKAGE_BEATMAP_EXTENSIONS{
    ".mc"
};

/// @brief OSZ 支持的音频扩展名。
inline constexpr std::array<std::string_view, 3> OSZ_PACKAGE_AUDIO_EXTENSIONS{
    ".ogg", ".mp3", ".wav"
};

/// @brief OSZ 支持的视频扩展名。
inline constexpr std::array<std::string_view, 1> OSZ_PACKAGE_VIDEO_EXTENSIONS{
    ".mp4"
};

/// @brief OSZ 支持的图片扩展名。
inline constexpr std::array<std::string_view, 2> OSZ_PACKAGE_IMAGE_EXTENSIONS{
    ".png", ".jpg"
};

/// @brief OSZ 支持的谱面扩展名。
inline constexpr std::array<std::string_view, 1> OSZ_PACKAGE_BEATMAP_EXTENSIONS{
    ".osu"
};

/// @brief MPK 通配资源分类使用的空扩展名列表。
inline constexpr std::array<std::string_view, 0>
    MPK_PACKAGE_WILDCARD_EXTENSIONS{};

/// @brief MPK 支持的谱面扩展名。
inline constexpr std::array<std::string_view, 1> MPK_PACKAGE_BEATMAP_EXTENSIONS{
    ".mmm"
};

/// @brief 可被打包流程加载并转换的谱面源扩展名。
inline constexpr std::array<std::string_view, 4>
    PACKAGE_BEATMAP_SOURCE_EXTENSIONS{ ".mmm", ".mc", ".osu", ".imd" };

/// @brief 通配音频资源发现时使用的常见音频扩展名。
inline constexpr std::array<std::string_view, 15>
    PACKAGE_COMMON_AUDIO_EXTENSIONS{ ".ogg", ".mp3",  ".wav", ".flac", ".opus",
                                     ".aac", ".m4a",  ".wma", ".ape",  ".alac",
                                     ".aif", ".aiff", ".mid", ".midi", ".xm" };

/// @brief 通配视频资源发现时使用的常见视频扩展名。
inline constexpr std::array<std::string_view, 10>
    PACKAGE_COMMON_VIDEO_EXTENSIONS{ ".mp4", ".mkv", ".webm", ".avi", ".mov",
                                     ".wmv", ".flv", ".m4v",  ".mpg", ".mpeg" };

/// @brief 通配图片资源发现时使用的常见图片扩展名。
inline constexpr std::array<std::string_view, 10>
    PACKAGE_COMMON_IMAGE_EXTENSIONS{ ".png", ".jpg", ".jpeg", ".bmp", ".webp",
                                     ".gif", ".tga", ".dds",  ".ktx", ".svg" };

/// @brief MCZ 打包格式支持的完整文件类型规则。
inline constexpr PackageSupportedFileTypes MCZ_PACKAGE_SUPPORTED_FILE_TYPES{
    ".mcz",
    std::span<const std::string_view>{ MCZ_PACKAGE_AUDIO_EXTENSIONS },
    std::span<const std::string_view>{ MCZ_PACKAGE_VIDEO_EXTENSIONS },
    std::span<const std::string_view>{ MCZ_PACKAGE_IMAGE_EXTENSIONS },
    std::span<const std::string_view>{ MCZ_PACKAGE_BEATMAP_EXTENSIONS },
    false,
    false,
    false,
};

/// @brief OSZ 打包格式支持的完整文件类型规则。
inline constexpr PackageSupportedFileTypes OSZ_PACKAGE_SUPPORTED_FILE_TYPES{
    ".osz",
    std::span<const std::string_view>{ OSZ_PACKAGE_AUDIO_EXTENSIONS },
    std::span<const std::string_view>{ OSZ_PACKAGE_VIDEO_EXTENSIONS },
    std::span<const std::string_view>{ OSZ_PACKAGE_IMAGE_EXTENSIONS },
    std::span<const std::string_view>{ OSZ_PACKAGE_BEATMAP_EXTENSIONS },
    false,
    false,
    false,
};

/// @brief MPK 打包格式支持的完整文件类型规则。
inline constexpr PackageSupportedFileTypes MPK_PACKAGE_SUPPORTED_FILE_TYPES{
    ".mpk",
    std::span<const std::string_view>{ MPK_PACKAGE_WILDCARD_EXTENSIONS },
    std::span<const std::string_view>{ MPK_PACKAGE_WILDCARD_EXTENSIONS },
    std::span<const std::string_view>{ MPK_PACKAGE_WILDCARD_EXTENSIONS },
    std::span<const std::string_view>{ MPK_PACKAGE_BEATMAP_EXTENSIONS },
    true,
    true,
    true,
};

/// @brief 当前支持的所有谱面打包格式规则。
inline constexpr std::array<PackageSupportedFileTypes, 3>
    PACKAGE_SUPPORTED_FILE_TYPES{ MCZ_PACKAGE_SUPPORTED_FILE_TYPES,
                                  OSZ_PACKAGE_SUPPORTED_FILE_TYPES,
                                  MPK_PACKAGE_SUPPORTED_FILE_TYPES };

/// @brief 将 ASCII 字符转为小写。
/// @param ch 输入字符。
/// @return 小写后的 ASCII 字符，非大写 ASCII 字符保持不变。
[[nodiscard]] constexpr char toPackageLowerAscii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// @brief 忽略 ASCII 大小写比较字符串。
/// @param lhs 左侧字符串。
/// @param rhs 右侧字符串。
/// @return 两个字符串是否相等。
[[nodiscard]] constexpr bool packageAsciiEqualsIgnoreCase(std::string_view lhs,
                                                          std::string_view rhs)
{
    if ( lhs.size() != rhs.size() ) return false;
    for ( std::size_t index = 0; index < lhs.size(); ++index ) {
        if ( toPackageLowerAscii(lhs[index]) !=
             toPackageLowerAscii(rhs[index]) ) {
            return false;
        }
    }
    return true;
}

/// @brief 忽略前导点并比较扩展名。
/// @param lhs 左侧扩展名。
/// @param rhs 右侧扩展名。
/// @return 两个扩展名是否相等。
[[nodiscard]] constexpr bool packageExtensionEquals(std::string_view lhs,
                                                    std::string_view rhs)
{
    if ( !lhs.empty() && lhs.front() == '.' ) lhs.remove_prefix(1);
    if ( !rhs.empty() && rhs.front() == '.' ) rhs.remove_prefix(1);
    return !lhs.empty() && packageAsciiEqualsIgnoreCase(lhs, rhs);
}

/// @brief 判断扩展名是否在指定列表内。
/// @param extensions 可接受扩展名列表。
/// @param extension 待检查扩展名，可带或不带前导点。
/// @return 是否匹配列表中的任意扩展名。
[[nodiscard]] constexpr bool packageExtensionInList(
    std::span<const std::string_view> extensions, std::string_view extension)
{
    for ( const std::string_view item : extensions ) {
        if ( packageExtensionEquals(item, extension) ) return true;
    }
    return false;
}

/// @brief 判断扩展名是否属于常见资源类型。
/// @param resourceType 资源分类。
/// @param extension 待检查资源扩展名，可带或不带前导点。
/// @return 扩展名是否属于该资源分类的常见格式。
[[nodiscard]] constexpr bool isKnownPackageResourceExtension(
    PackageResourceType resourceType, std::string_view extension)
{
    switch ( resourceType ) {
    case PackageResourceType::Audio:
        return packageExtensionInList(PACKAGE_COMMON_AUDIO_EXTENSIONS,
                                      extension);
    case PackageResourceType::Video:
        return packageExtensionInList(PACKAGE_COMMON_VIDEO_EXTENSIONS,
                                      extension);
    case PackageResourceType::Image:
        return packageExtensionInList(PACKAGE_COMMON_IMAGE_EXTENSIONS,
                                      extension);
    case PackageResourceType::Beatmap:
        return packageExtensionInList(PACKAGE_BEATMAP_SOURCE_EXTENSIONS,
                                      extension);
    }
    return false;
}

/// @brief 按打包枚举取得文件类型规则。
/// @param type 打包格式。
/// @return 对应格式的文件类型规则。
[[nodiscard]] constexpr const PackageSupportedFileTypes&
getPackageSupportedFileTypes(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return MCZ_PACKAGE_SUPPORTED_FILE_TYPES;
    case PackageFileType::Osz: return OSZ_PACKAGE_SUPPORTED_FILE_TYPES;
    case PackageFileType::Mpk: return MPK_PACKAGE_SUPPORTED_FILE_TYPES;
    }
    return MPK_PACKAGE_SUPPORTED_FILE_TYPES;
}

/// @brief 根据打包文件扩展名查找文件类型规则。
/// @param extension 打包文件扩展名，可带或不带前导点。
/// @return 匹配的文件类型规则，未匹配时返回 nullptr。
[[nodiscard]] constexpr const PackageSupportedFileTypes*
findPackageSupportedFileTypes(std::string_view extension)
{
    for ( const auto& types : PACKAGE_SUPPORTED_FILE_TYPES ) {
        if ( packageExtensionEquals(types.m_packageExtension, extension) ) {
            return &types;
        }
    }
    return nullptr;
}

/// @brief 判断资源扩展名是否被指定打包格式支持。
/// @param types 打包格式文件类型规则。
/// @param resourceType 资源分类。
/// @param extension 待检查资源扩展名，可带或不带前导点。
/// @return 资源扩展名是否被支持。
[[nodiscard]] constexpr bool isPackageResourceExtensionSupported(
    const PackageSupportedFileTypes& types, PackageResourceType resourceType,
    std::string_view extension)
{
    if ( extension.empty() ) return false;

    switch ( resourceType ) {
    case PackageResourceType::Audio:
        return types.m_allowAllAudioFormats ||
               packageExtensionInList(types.m_audioExtensions, extension);
    case PackageResourceType::Video:
        return types.m_allowAllVideoFormats ||
               packageExtensionInList(types.m_videoExtensions, extension);
    case PackageResourceType::Image:
        return types.m_allowAllImageFormats ||
               packageExtensionInList(types.m_imageExtensions, extension);
    case PackageResourceType::Beatmap:
        return packageExtensionInList(types.m_beatmapExtensions, extension);
    }
    return false;
}

/// @brief 判断扩展名是否可作为指定打包格式的谱面来源文件。
/// @param types 打包格式文件类型规则。
/// @param extension 待检查的谱面文件扩展名，可带或不带前导点。
/// @return 扩展名是否可作为打包谱面来源。
[[nodiscard]] constexpr bool isPackageBeatmapSourceExtensionSupported(
    const PackageSupportedFileTypes& types, std::string_view extension)
{
    (void)types;
    return packageExtensionInList(PACKAGE_BEATMAP_SOURCE_EXTENSIONS, extension);
}

/// @brief 判断文件扩展名是否可作为指定打包格式的候选资源。
/// @param types 打包格式文件类型规则。
/// @param extension 待检查资源扩展名，可带或不带前导点。
/// @return 是否符合任意资源分类的规则。
[[nodiscard]] constexpr bool isPackageCandidateExtensionSupported(
    const PackageSupportedFileTypes& types, std::string_view extension)
{
    if ( extension.empty() ) return false;

    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        return true;
    }
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        return true;
    }
    if ( types.m_allowAllVideoFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Video,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Video, extension) ) {
        return true;
    }
    if ( types.m_allowAllImageFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Image,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Image, extension) ) {
        return true;
    }
    return false;
}

}  // namespace MMM
