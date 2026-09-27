#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace MMM
{

/// @brief 谱面打包目标格式。
enum class PackageFileType {
    /// @brief Malody MCZ 谱面包。
    Mcz,
    /// @brief osu! OSZ 谱面包。
    Osz,
    /// @brief MusicMapMaker 原生 MPK 谱面包。
    Mpk,
};

/// @brief 打包资源分类。
enum class PackageResourceType {
    /// @brief 音频资源。
    Audio,
    /// @brief 视频资源。
    Video,
    /// @brief 静态图片资源。
    Image,
    /// @brief 可转换谱面文件。
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
    // MCZ 采用 Malody 原生扩展和受限的常见媒体集合。
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
    // OSZ 采用 osu! 原生扩展，资源类型由 osu! 客户端兼容范围限定。
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
    // MPK 保留任意媒体文件，但谱面来源仍必须能由本项目转换。
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
    // 只处理格式扩展名需要的 ASCII，避免引入区域设置依赖。
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

/// @brief 忽略 ASCII 大小写比较字符串。
/// @param lhs 左侧字符串。
/// @param rhs 右侧字符串。
/// @return 两个字符串是否相等。
[[nodiscard]] constexpr bool packageAsciiEqualsIgnoreCase(std::string_view lhs,
                                                          std::string_view rhs)
{
    // 长度不同可立即拒绝，也避免后续索引越过较短视图。
    if ( lhs.size() != rhs.size() ) return false;
    for ( std::size_t index = 0; index < lhs.size(); ++index ) {
        // 扩展名按 ASCII 逐字节比较，不对 UTF-8 路径主体做大小写折叠。
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
    // 规则表和调用方都可传带点形式，比较前统一去掉一个前导点。
    if ( !lhs.empty() && lhs.front() == '.' ) lhs.remove_prefix(1);
    if ( !rhs.empty() && rhs.front() == '.' ) rhs.remove_prefix(1);
    // 空扩展名不允许与通配数组中的空 span 语义混淆。
    return !lhs.empty() && packageAsciiEqualsIgnoreCase(lhs, rhs);
}

/// @brief 判断扩展名是否在指定列表内。
/// @param extensions 可接受扩展名列表。
/// @param extension 待检查扩展名，可带或不带前导点。
/// @return 是否匹配列表中的任意扩展名。
[[nodiscard]] constexpr bool packageExtensionInList(
    std::span<const std::string_view> extensions, std::string_view extension)
{
    // 列表很小且 constexpr，线性扫描保持规则声明简单并支持编译期求值。
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
    // 分类只选择对应公共白名单，不能跨类型接受同名后缀。
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
    // 防御未来新增枚举但遗漏规则的情况。
    return false;
}

/// @brief 按打包枚举取得文件类型规则。
/// @param type 打包格式。
/// @return 对应格式的文件类型规则。
[[nodiscard]] constexpr const PackageSupportedFileTypes&
getPackageSupportedFileTypes(PackageFileType type)
{
    // 枚举与常量表显式对应，避免依赖枚举的底层顺序索引数组。
    switch ( type ) {
    case PackageFileType::Mcz: return MCZ_PACKAGE_SUPPORTED_FILE_TYPES;
    case PackageFileType::Osz: return OSZ_PACKAGE_SUPPORTED_FILE_TYPES;
    case PackageFileType::Mpk: return MPK_PACKAGE_SUPPORTED_FILE_TYPES;
    }
    // 未知枚举使用能力最宽但仍受候选资源识别限制的 MPK 规则。
    return MPK_PACKAGE_SUPPORTED_FILE_TYPES;
}

/// @brief 根据打包文件扩展名查找文件类型规则。
/// @param extension 打包文件扩展名，可带或不带前导点。
/// @return 匹配的文件类型规则，未匹配时返回 nullptr。
[[nodiscard]] constexpr const PackageSupportedFileTypes*
findPackageSupportedFileTypes(std::string_view extension)
{
    // 返回静态 constexpr 对象地址，调用方不得持有到动态配置生命周期。
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
    // 空后缀不是资源文件，即使目标格式允许对应分类的任意格式。
    if ( extension.empty() ) return false;

    // 通配开关用于明确资源选择；谱面分类始终受转换器白名单约束。
    switch ( resourceType ) {
    case PackageResourceType::Audio:
        // 显式通配用于用户选择阶段；非通配格式检查各自白名单。
        return types.m_allowAllAudioFormats ||
               packageExtensionInList(types.m_audioExtensions, extension);
    case PackageResourceType::Video:
        // 视频规则与音频独立，避免单个通配开关扩大其他类别。
        return types.m_allowAllVideoFormats ||
               packageExtensionInList(types.m_videoExtensions, extension);
    case PackageResourceType::Image:
        // 图片同样只读取自身通配位和扩展名列表。
        return types.m_allowAllImageFormats ||
               packageExtensionInList(types.m_imageExtensions, extension);
    case PackageResourceType::Beatmap:
        // 谱面从不使用媒体通配位，只接受目标包原生写出扩展名。
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
    // 当前所有包都共享可转换来源集合，保留参数以稳定公共调用签名。
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
    // 候选发现比最终打包更保守：通配格式也只扫描已知资源后缀。
    if ( extension.empty() ) return false;

    // 谱面优先识别，避免后续通配资源分类吞掉可转换来源文件。
    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        return true;
    }
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        // 通配目标的自动发现仍要求后缀属于已知音频集合。
        return true;
    }
    // 各资源分类独立判断，同一扩展名匹配任意一类即可进入候选集合。
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
    // 不认识的后缀留给用户显式选择流程，不参与自动收集。
    return false;
}

}  // namespace MMM
