#include "logic/ProjectController.h"
#include "config/AppConfig.h"
#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"

#include <fmt/format.h>
#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 将新建项目初始设置写入项目实例。
/// @param project 要初始化的项目。
/// @param options 新项目初始设置。
/// @param fallbackTitle 标题为空时使用的回退名称。
/// @note 本次选项按值写入项目，不保留对创建对话框数据的引用。
/// @details 只用于没有既有配置的新项目；已有项目的恢复不经过此默认值入口。
/// 初始侧栏使用内部页签标识，不能用本地化后的显示名称代替。
/// @note 只设置创建选项涉及的字段，其他项目设置继续保留类型默认值。
void applyProjectCreationOptions(
    Project& project, const ProjectController::ProjectCreationOptions& options,
    const std::string& fallbackTitle)
{
    // 标题和曲师允许省略；创建时补齐展示值，避免后续界面各自选择默认文案。
    project.m_metadata.m_title =
        options.m_title.empty() ? fallbackTitle : options.m_title;
    project.m_metadata.m_artist =
        options.m_artist.empty() ? "Unknown" : options.m_artist;
    // 谱师优先采用本次输入，其次采用规范化后的个人身份；两者均缺失才用占位值。
    const auto defaultCreator = Config::normalizeCreatorIdentity(
        Config::AppConfig::instance().getEditorSettings().defaultCreator);
    // 显式 options.m_mapper
    // 不经过本次默认身份规范化，调用方提交的值仍优先保留。
    project.m_metadata.m_mapper =
        options.m_mapper.empty()
            ? (defaultCreator.empty() ? std::string{ "Unknown" }
                                      : defaultCreator)
            : options.m_mapper;
    // 配色方案的空值保留给配置继承逻辑，不在创建阶段固定为某个全局方案。
    // 侧栏则需要可定位的页签标识，以便首次进入项目时恢复到文件浏览。
    project.m_settings.m_colorPaletteSchemeName =
        options.m_colorPaletteSchemeName;
    project.m_settings.m_workspace.m_sidebarActiveTab =
        options.m_sidebarActiveTab.empty() ? std::string{ "FileExplorer" }
                                           : options.m_sidebarActiveTab;
}

/// @brief 将 ASCII 扩展名转换为小写。
/// @param value 输入扩展名。
/// @return 小写后的扩展名。
/// @note 仅服务后缀匹配，不承担完整文件名的 Unicode 大小写折叠。
std::string toLowerAscii(std::string value)
{
    // cctype 接收 unsigned char 可表示的值，避免扩展名字节被解释为负数。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 判断文件扩展名是否是可按 zip 读取的谱面包。
/// @param path 待检查路径。
/// @return 扩展名受支持时返回 true。
/// @note 包含 .7z 后缀入口，但内容仍由 ZIP 读取器验证，不承诺通用 7z 解码。
bool isTemporaryPackagePath(const std::filesystem::path& path)
{
    // 这里只按后缀分派打开流程；是否确实可解码仍由后续归档读取判定。
    const auto extension = toLowerAscii(Config::pathToUtf8(path.extension()));
    return extension == ".zip" || extension == ".7z" || extension == ".mcz" ||
           extension == ".osz" || extension == ".mpk";
}

/// @brief 将文件名片段净化为临时目录名可用的 ASCII 字符串。
/// @param name 原始 UTF-8 名称。
/// @return 净化后的目录名片段。
/// @note 返回值不是唯一标识；调用方还需添加时间及重试后缀。
/// 名称净化也不进行文件系统保留名检查，不能单独当成任意文件的可写性验证。
/// 净化不保留 Unicode 原名，只服务内部目录选名，界面仍应显示源包路径。
std::string sanitizeTemporaryFolderName(std::string name)
{
    // 此名称只用于缓存目录，不用于还原原始包名；按字节替换也适用于 UTF-8 输入。
    // 保留的 ASCII 集合不含路径分隔符，因此不会由包名引入额外目录层级。
    for ( char& ch : name ) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        const bool          ok   = (byte >= 'a' && byte <= 'z') ||
                        (byte >= 'A' && byte <= 'Z') ||
                        (byte >= '0' && byte <= '9') || ch == '-' || ch == '_';
        if ( !ok ) ch = '_';
    }
    // 剥掉替换形成的首尾占位符；全中文等名称可能因此变空，统一使用回退前缀。
    while ( !name.empty() && name.front() == '_' ) {
        name.erase(name.begin());
    }
    while ( !name.empty() && name.back() == '_' ) {
        name.pop_back();
    }
    if ( name.empty() ) return "package";
    // 不把净化结果用作资源 ID，它可能与另一个原始包名得到相同字符串。
    return name;
}

/// @brief 读取文件全部字节。
/// @param path 文件路径。
/// @param outBytes 成功时接收完整文件；任何失败均清空。
/// @return 读取成功返回 true。
/// @pre 文件长度可由 size_t 和 streamsize 表达，读取期间源文件应保持稳定。
/// @warning 低频文件操作，内存占用随整个输入文件大小增长，不用于流式预览。
bool readFileBytes(const std::filesystem::path& path,
                   std::vector<std::uint8_t>&   outBytes)
{
    // 调用方可能复用缓冲区；打开失败也不能留下上一份谱包的字节。
    outBytes.clear();

    // 先定位末尾取得大小，再一次性分配；归档解析需要完整且连续的输入缓冲。
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;

    const std::ifstream::pos_type endPos = file.tellg();
    // 空文件不构成可打开的谱包，与获取长度失败一样交给上层报告读取失败。
    if ( endPos <= 0 ) return false;

    outBytes.resize(static_cast<std::size_t>(endPos));
    // 输出长度由文件长度决定，不在尾部附加零字节；ZIP 读取使用显式缓冲大小。
    file.seekg(0, std::ios::beg);
    // 读取起点必须回到文件头，ate 只用于取得长度而非从尾部读取归档。
    if ( !file.read(reinterpret_cast<char*>(outBytes.data()),
                    static_cast<std::streamsize>(outBytes.size())) ) {
        // 短读不交付部分数据，避免后续把不完整缓冲当成完整归档。
        outBytes.clear();
        return false;
    }
    return true;
}

/// @brief 写入文件字节并创建父目录。
/// @param path 输出文件路径。
/// @param data 文件字节指针。
/// @param size 文件字节数。
/// @return 写入成功返回 true。
/// @pre 路径已由调用方检查，data 在 size 非零时必须覆盖对应字节数。
/// @note 写入会截断已有文件，失败不回滚已写字节；临时目录清理由上层负责。
/// @warning 同步写入解压内容，不能用于渲染或逻辑每帧回调。
/// 返回前只检查流状态，析构关闭时的延迟错误不由这个返回值额外报告。
bool writeBytesToFile(const std::filesystem::path& path, const void* data,
                      std::size_t size)
{
    // 条目可以位于尚未创建的多层子目录中，写文件前先准备其父路径。
    std::error_code filesystemError;
    const auto      parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        std::filesystem::create_directories(parentPath, filesystemError);
        if ( filesystemError ) return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    // 采用字节模式，不能让平台文本换行转换改变归档中的音频或图片内容。
    if ( !file ) return false;
    // 零字节条目也必须创建文件，但不要求 data 指向可读内存。
    if ( size > 0 ) {
        // 文件字节不是文本字符串，必须使用 size 而不是搜索零结尾决定写出长度。
        file.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(file);
}

/// @brief 将 zip 包内路径归一化为通用分隔符。
/// @param archiveName 原始包内路径。
/// @return 归一化后的包内路径。
/// @note 不解析点路径或拒绝绝对路径，结果仍须经过名称范围检查。
/// 不按文件扩展名决定目录属性，去掉尾斜杠后仍需读取归档元数据。
std::string normalizeArchiveName(std::string archiveName)
{
    // 将 Windows 打包器的分隔符统一后再做相对路径检查。
    // 目录条目尾部的斜杠不属于名称；是否为目录由归档条目的元数据决定。
    std::replace(archiveName.begin(), archiveName.end(), '\\', '/');
    while ( !archiveName.empty() && archiveName.back() == '/' ) {
        archiveName.pop_back();
    }
    return archiveName;
}

/// @brief ZIP 中央目录固定头长度。
constexpr std::size_t ZIP_CENTRAL_DIRECTORY_HEADER_SIZE = 46U;

/// @brief ZIP 中央目录文件名长度字段偏移。
constexpr std::size_t ZIP_FILENAME_LENGTH_OFFSET = 28U;

/// @brief ZIP 中央目录扩展字段长度字段偏移。
constexpr std::size_t ZIP_EXTRA_FIELD_LENGTH_OFFSET = 30U;

/// @brief ZIP 中央目录头签名。
constexpr std::uint32_t ZIP_CENTRAL_DIRECTORY_SIGNATURE = 0x02014B50U;

/// @brief Info-ZIP Unicode Path 扩展字段标识。
constexpr std::uint16_t ZIP_UNICODE_PATH_EXTRA_FIELD_ID = 0x7075U;

/// @brief 从 ZIP 小端字节中读取 16 位无符号整数。
/// @param data 至少包含两个字节的输入位置。
/// @return 读取出的整数。
/// @pre 调用方先验证字段边界，本助手本身没有长度参数可检查越界。
std::uint16_t readZipLittleEndian16(const std::uint8_t* data)
{
    // 逐字节组装，不要求 ZIP 字段地址按整数对齐，也不依赖主机字节序。
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8U);
}

/// @brief 从 ZIP 小端字节中读取 32 位无符号整数。
/// @param data 至少包含四个字节的输入位置。
/// @return 读取出的整数。
/// @note 仅解码整数，不验证中央目录签名或字段含义。
std::uint32_t readZipLittleEndian32(const std::uint8_t* data)
{
    // 移位前扩大到无符号 32 位，最高字节不经有符号整数左移。
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

/// @brief 检查字符串是否是结构合法且不含 NUL 的 UTF-8。
/// @param value 待检查字节串。
/// @return 编码合法且不含 NUL 时返回 true；路径范围另行检查。
/// @note 只验证字节结构，不做 Unicode 规范化，也不限制系统保留文件名。
/// 空字符串通过编码检查，是否允许空路径由后续名称检查决定。
/// @note 检查不分配码点数组，单次遍历只移动字节偏移。
bool isValidArchiveUtf8(std::string_view value)
{
    std::size_t index = 0U;
    while ( index < value.size() ) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        // 拒绝内嵌 NUL，避免按长度保存的名字和底层零结尾路径出现不同解释。
        if ( first == 0U ) return false;
        if ( first <= 0x7FU ) {
            // 非零 ASCII 不需要续字节，路径控制字符的限制留给名称规则处理。
            ++index;
            continue;
        }

        // 首字节决定序列长度；第二字节的特殊边界进一步排除过长编码、
        // UTF-16 代理区以及超出 U+10FFFF 的码点，其余续字节使用统一范围。
        std::size_t  continuationCount = 0U;
        std::uint8_t secondMinimum     = 0x80U;
        std::uint8_t secondMaximum     = 0xBFU;
        if ( first >= 0xC2U && first <= 0xDFU ) {
            continuationCount = 1U;
        } else if ( first >= 0xE0U && first <= 0xEFU ) {
            continuationCount = 2U;
            if ( first == 0xE0U ) secondMinimum = 0xA0U;
            // E0 的较小第二字节会把可用更短序列表示的码点编码为三字节。
            if ( first == 0xEDU ) secondMaximum = 0x9FU;
        } else if ( first >= 0xF0U && first <= 0xF4U ) {
            continuationCount = 3U;
            if ( first == 0xF0U ) secondMinimum = 0x90U;
            if ( first == 0xF4U ) secondMaximum = 0x8FU;
            // F4 后超过该上界就不再是 Unicode 标量值，不能作为路径编码接收。
        } else {
            // 独立续字节、过长编码首字节及超出四字节编码范围的首字节均拒绝。
            return false;
        }

        // 在访问第二字节前验证完整序列仍在输入内，末尾截断不能作为有效名称。
        if ( continuationCount > value.size() - index - 1U ) return false;
        const auto second = static_cast<std::uint8_t>(value[index + 1U]);
        if ( second < secondMinimum || second > secondMaximum ) return false;
        for ( std::size_t continuationIndex = 2U;
              continuationIndex <= continuationCount;
              ++continuationIndex ) {
            const auto continuation =
                static_cast<std::uint8_t>(value[index + continuationIndex]);
            if ( continuation < 0x80U || continuation > 0xBFU ) return false;
        }
        index += continuationCount + 1U;
        // 完整码点通过后一次跨过全部字节，不把续字节再次当成首字节解析。
    }
    return true;
}

/// @brief 按 ZIP 中央目录和 Info-ZIP Unicode Path 扩展解析条目路径。
/// @param packageBytes 完整谱面包字节。
/// @param zipArchive 已由 miniz 初始化的 ZIP 读取器。
/// @param fileStat 当前中央目录条目信息。
/// @param archiveName 接收 UTF-8 条目路径。
/// @param errorMessage 接收解析失败原因。
/// @note 成功不清空旧错误文本，调用方应以返回值而非错误字符串判断本次结果。
/// @return 得到编码合法的 UTF-8 名称时返回 true；调用方仍需检查路径范围。
/// @pre fileStat 与 zipArchive 来自同一份 packageBytes，读取器仍然有效。
/// @details 匹配的 Unicode Path 优先于原始名字；不匹配的扩展被跳过。
/// 对匹配但编码损坏的扩展报告失败，不静默改用可能指向另一文件的原始名称。
/// @note 读取中央目录原字节，避免固定长度展示字段截断文件名后产生错误路径。
bool resolveZipArchiveName(const std::vector<std::uint8_t>& packageBytes,
                           const mz_zip_archive&            zipArchive,
                           const mz_zip_archive_file_stat&  fileStat,
                           std::string& archiveName, std::string& errorMessage)
{
    // 只有完成解析才发布名称；失败时不让调用方沿用上一条目的路径。
    archiveName.clear();
    // 输入缓冲只读，得到的名称独立持有字节，不把指向归档内部的视图交给调用方。

    // 条目偏移相对中央目录起点。先验证各自剩余长度，再求绝对位置，
    // 避免先相加后才发现位置已越过整个谱包缓冲。
    const auto centralDirectoryOffset = zipArchive.m_central_directory_file_ofs;
    if ( centralDirectoryOffset > packageBytes.size() ||
         fileStat.m_central_dir_ofs >
             packageBytes.size() - centralDirectoryOffset ) {
        errorMessage = "谱面包中央目录偏移无效";
        return false;
    }

    const auto headerOffset = static_cast<std::size_t>(
        centralDirectoryOffset + fileStat.m_central_dir_ofs);
    if ( headerOffset > packageBytes.size() ||
         ZIP_CENTRAL_DIRECTORY_HEADER_SIZE >
             packageBytes.size() - headerOffset ) {
        errorMessage = "谱面包中央目录条目不完整";
        return false;
    }

    // 固定头完整不代表它就是目录条目，还需用签名确认字段解释的起点。
    const auto* header = packageBytes.data() + headerOffset;
    // 指针只在完整固定头已验证后形成，后续字段读取仍受各自长度检查约束。
    if ( readZipLittleEndian32(header) != ZIP_CENTRAL_DIRECTORY_SIGNATURE ) {
        errorMessage = "谱面包中央目录条目签名无效";
        return false;
    }

    // 文件名和扩展字段是固定头后连续的变长区域，逐段扣除长度后才允许读取。
    const auto filenameLength = static_cast<std::size_t>(
        readZipLittleEndian16(header + ZIP_FILENAME_LENGTH_OFFSET));
    const auto extraFieldLength = static_cast<std::size_t>(
        readZipLittleEndian16(header + ZIP_EXTRA_FIELD_LENGTH_OFFSET));
    const auto payloadOffset = headerOffset + ZIP_CENTRAL_DIRECTORY_HEADER_SIZE;
    // 使用剩余长度检查变长字段，不能先计算所有长度之和再判断整数是否越界。
    if ( filenameLength > packageBytes.size() - payloadOffset ||
         extraFieldLength >
             packageBytes.size() - payloadOffset - filenameLength ) {
        errorMessage = "谱面包中央目录文件名或扩展字段不完整";
        return false;
    }

    const auto*       rawFilenameBytes = packageBytes.data() + payloadOffset;
    const std::string rawFilename(
        reinterpret_cast<const char*>(rawFilenameBytes), filenameLength);
    // 名称长度来自中央目录字段，不使用 strlen，内嵌 NUL 会在编码检查中被拒绝。
    // Unicode 扩展绑定的是原始名字字节；CRC 不能对归一化后的名字计算。
    const auto rawFilenameCrc = static_cast<std::uint32_t>(
        mz_crc32(0U, rawFilenameBytes, filenameLength));

    const auto* extraField                = rawFilenameBytes + filenameLength;
    std::size_t remainingExtraFieldLength = extraFieldLength;
    // 遍历只限本条目声明的扩展区，不能把下一条目录记录当成附加字段。
    // 扩展区由多个带长度的字段组成；不认识的字段只跳过其负载，不按内容猜测。
    while ( remainingExtraFieldLength > 0U ) {
        if ( remainingExtraFieldLength < 4U ) {
            errorMessage = "谱面包中央目录扩展字段不完整";
            return false;
        }

        const auto fieldId = readZipLittleEndian16(extraField);
        // 小端读取助手不做边界检查，此时扩展区已确认至少剩余四字节字段头。
        const auto fieldSize =
            static_cast<std::size_t>(readZipLittleEndian16(extraField + 2U));
        extraField += 4U;
        remainingExtraFieldLength -= 4U;
        // 字段头与负载分别消耗，未知字段也必须有完整声明长度才能安全跳过。
        if ( fieldSize > remainingExtraFieldLength ) {
            errorMessage = "谱面包中央目录扩展字段长度无效";
            return false;
        }

        // 只接受版本 1 且 CRC 匹配的 Unicode
        // Path，避免使用不属于当前名称的扩展。 五字节前缀由版本和 CRC
        // 构成，其后的全部字节才是 UTF-8 文件名。
        if ( fieldId == ZIP_UNICODE_PATH_EXTRA_FIELD_ID && fieldSize >= 5U &&
             extraField[0] == 1U &&
             readZipLittleEndian32(extraField + 1U) == rawFilenameCrc ) {
            const std::string unicodeFilename(
                reinterpret_cast<const char*>(extraField + 5U), fieldSize - 5U);
            // CRC 校验绑定原始名字，不是对 Unicode 名称内容做完整性校验的替代。
            if ( !isValidArchiveUtf8(unicodeFilename) ) {
                errorMessage = "谱面包 Unicode 路径不是合法 UTF-8";
                return false;
            }
            archiveName = normalizeArchiveName(unicodeFilename);
            // 首个版本和 CRC 均匹配的扩展即决定名称，不继续拼接其他扩展内容。
            return true;
        }

        extraField += fieldSize;
        // 不适用的 Unicode 字段也按声明长度跳过，再尝试后续字段或原始名称。
        remainingExtraFieldLength -= fieldSize;
    }

    // 没有可用扩展时只接受原始
    // UTF-8，不猜测本机代码页，以免解包后资源引用失配。
    if ( !isValidArchiveUtf8(rawFilename) ) {
        // 原始名称解析失败则停止解包，不按替换字符生成无法匹配谱面引用的文件。
        errorMessage = "谱面包条目文件名不是 UTF-8，且缺少有效的 Unicode 路径";
        return false;
    }
    archiveName = normalizeArchiveName(rawFilename);
    // 编码成功只交付文本，绝对路径、上级目录和设备名仍需另行拒绝。
    return true;
}

/// @brief 判断 Windows 文件名片段是否包含不兼容字符或形式。
/// @param name 单个文件名片段，不包含路径分隔符。
/// @return 不兼容时返回原因，否则返回空。
/// @note 此函数检查单个片段，不负责分隔符、根路径或父目录跳转。
/// @note 返回首个发现的问题，不对名称进行替换修复，以免解压后谱面引用失配。
std::optional<std::string> describeWindowsIncompatibleFileName(
    const std::string& name)
{
    if ( name.empty() ) return "空文件名片段";
    // 控制字节逐个检查，非 ASCII 字节的编码合法性已由上一阶段统一验证。

    for ( const unsigned char ch : name ) {
        if ( ch < 32 ) return "控制字符";
        switch ( ch ) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '|':
        case '?':
        case '*':
            return std::string("Windows 不支持的字符 '") +
                   static_cast<char>(ch) + "'";
        default: break;
        }
    }

    if ( name.back() == ' ' || name.back() == '.' ) {
        // 不自动裁掉末尾字符，避免两个不同归档名称在落盘时合并成同一文件。
        return "文件名不能以空格或点结尾";
    }

    // 设备名即使带扩展名仍受限制，因此先取首个点之前的部分，再忽略大小写比较。
    std::string baseName = name;
    // 原名称用于落盘和错误展示，设备名比较副本的大小写折叠不修改它。
    if ( const auto dotPos = baseName.find('.'); dotPos != std::string::npos ) {
        baseName.resize(dotPos);
    }
    std::transform(
        baseName.begin(),
        baseName.end(),
        baseName.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    if ( baseName == "CON" || baseName == "PRN" || baseName == "AUX" ||
         baseName == "NUL" ) {
        return "Windows 保留设备名";
    }
    // 这里只匹配单个设备序号，不把普通的较长前缀名称一并拒绝。
    if ( baseName.size() == 4 &&
         (baseName.starts_with("COM") || baseName.starts_with("LPT")) &&
         baseName[3] >= '1' && baseName[3] <= '9' ) {
        return "Windows 保留设备名";
    }

    return std::nullopt;
}

/// @brief 检查 zip 包内路径是否安全且可在 Windows 上落盘。
/// @param archiveName 归一化后的包内路径。
/// @return 不安全或不兼容时返回原因，否则返回空。
/// @pre archiveName 已统一使用正斜杠，且已通过 UTF-8 字节校验。
/// @note 这是词法名称检查，不查询目标目录中的符号链接或文件权限。
/// 检查采用 Windows 命名约束，即使当前运行在其他平台也不放宽谱包名称。
std::optional<std::string> describeUnsafeArchiveName(
    const std::string& archiveName)
{
    if ( archiveName.empty() ) {
        return "谱面包包含空路径";
    }
    if ( archiveName.front() == '/' ) {
        // 显式排除通用绝对路径，不能仅依赖宿主平台对另一平台路径格式的解释。
        return "谱面包包含绝对路径";
    }

    const auto path = Config::utf8ToPath(archiveName);
    // 名称校验使用归一化后的所有片段，不以目标目录是否已经创建为前提。
    if ( path.empty() || path.is_absolute() ) {
        return "谱面包包含绝对路径";
    }
    // 检查词法归一化后仍残留的上级目录，不以目标文件是否已经存在为前提。
    // 每个实际名称片段还要遵循 Windows 限制，保持谱包跨平台落盘的一致性。
    for ( const auto& part : path.lexically_normal() ) {
        if ( part == ".." ) {
            return "谱面包路径越出目标目录";
        }
        if ( part == "." ) {
            // 点片段不代表实际文件名，但不能因此跳过其他仍存在的目录片段检查。
            continue;
        }
        const auto partText = Config::pathToUtf8(part);
        if ( auto reason = describeWindowsIncompatibleFileName(partText) ) {
            return *reason;
        }
    }

    return std::nullopt;
}

/// @brief 发布项目或谱面包打开失败事件。
/// @param path 尝试打开的路径。
/// @param message 失败原因。
/// @param isPackage 是否为谱面包打开失败。
/// @note 此事件只承载失败展示信息，不改变当前项目或待处理请求。
/// @note 发布失败不负责删除临时目录，缓存拥有者仍需执行对应清理。
void publishProjectOpenFailed(const std::filesystem::path& path,
                              const std::string& message, bool isPackage)
{
    Event::ProjectOpenFailedEvent event;
    event.m_projectPath  = Config::pathToUtf8(path);
    event.m_errorMessage = message;
    event.m_isPackage    = isPackage;
    // 保留来源类型，使界面能区分普通项目和临时谱包失败，不从扩展名再次猜测。
    Event::EventBus::instance().publish(event);
}

/// @brief 发布协作访客在线期间的本机项目打开拦截事件。
void publishCollaborationProjectOpenBlocked()
{
    // 只通知界面显示拦截原因，不在事件发布器里断开协作或自动切换项目。
    Event::EventBus::instance().publish(
        Event::CollaborationProjectOpenBlockedEvent{});
}

/// @brief 取得适合状态栏展示的项目加载路径名称。
/// @param path 项目、谱面包或资源路径。
/// @return 优先返回文件名，无法取得时返回完整路径。
/// @note 仅缩短展示文字，不改变实际项目定位路径，也不访问磁盘。
std::string projectOpenProgressPathDetail(const std::filesystem::path& path)
{
    const auto fileName = path.filename();
    // 根目录或尾部无文件名的路径仍返回完整表示，避免进度详情变成空文字。
    return Config::pathToUtf8(fileName.empty() ? path : fileName);
}

/// @brief 发布项目打开流程的分阶段进度。
/// @param stage 当前加载阶段。
/// @param fraction 当前总进度，范围为 0 到 1。
/// @param detail 当前处理对象名称。
/// @note 进度值由各阶段调用方给出，不在此根据文件数量重新推算。
/// 不在此钳制进度或保证单调性；阶段调用方负责提供与流程一致的数值。
void publishProjectOpenProgress(Event::ProjectOpenProgressStage stage,
                                float fraction, std::string detail)
{
    Event::ProjectOpenProgressEvent event;
    event.m_stage = stage;
    // 阶段与进度一起发布，接收方无需根据数值区间反推当前工作类型。
    event.m_fraction = fraction;
    event.m_detail   = std::move(detail);
    // 事件拥有详情文本，调用方的局部字符串结束后仍不依赖其内存。
    Event::EventBus::instance().publish(event);
}

/// @brief 尽量取得路径的绝对规范形式。
/// @param path 待规范化路径。
/// @return 成功时返回 weakly_canonical/absolute
/// 路径，失败时退回词法规范化路径。
/// @note 非空结果不保证目录存在、权限可用或全部符号链接已解析。
std::filesystem::path makeAbsoluteNormalizedPath(
    const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    // 优先消除已存在路径中的符号链接；目标尚未建立时也保留可规范化的前缀。
    std::error_code filesystemError;
    auto normalized = std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    // 文件系统规范化失败不直接丢弃用户输入，仍尝试消除相对工作目录的歧义。
    filesystemError.clear();
    normalized = std::filesystem::absolute(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    // 最后只折叠词法片段，是否存在及能否访问仍由打开流程判定。
    return path.lexically_normal();
}

/// @brief 将 zip 兼容谱面包安全解压到目标目录。
/// @param packagePath 谱面包路径。
/// @param destinationRoot 解压目标目录。
/// @note 目标根由准备阶段创建；此函数不能用于向已有正式项目合并任意谱包。
/// @param errorMessage 失败时写入错误信息。
/// @return 解压成功返回 true。
/// @pre destinationRoot 是本次准备流程专用的目标根，不与正式项目混用。
/// @details 按归档顺序解压并在首个失败处停止；此前写入的条目可能仍存在。
/// 调用方必须依据返回值决定保留或清理整个目标目录，不能只清理最后一个条目。
/// @warning 内存同时持有压缩包和当前条目的完整解压结果，仅用于低频打开流程。
bool extractZipPackageToDirectory(const std::filesystem::path& packagePath,
                                  const std::filesystem::path& destinationRoot,
                                  std::string&                 errorMessage)
{
    errorMessage.clear();

    std::vector<std::uint8_t> packageBytes;
    // 错误由当前准备尝试重新生成，不让复用的错误字符串残留上次失败信息。
    if ( !readFileBytes(packagePath, packageBytes) ) {
        // 尚未初始化归档读取器，此分支无库内状态需要 reader_end 清理。
        errorMessage = "无法读取谱面包文件";
        return false;
    }

    // 读取器引用 packageBytes 的内存；该缓冲必须存活到 reader_end 之后。
    mz_zip_archive zipArchive{};
    if ( !mz_zip_reader_init_mem(
             &zipArchive, packageBytes.data(), packageBytes.size(), 0) ) {
        errorMessage = "谱面包不是可读取的 zip 兼容格式";
        return false;
    }

    // 初始化成功后的所有条目失败都汇合到循环后释放读取器，不在循环中提前返回。
    bool success = true;
    // 条目失败通过 success 汇合到统一释放点，不能在库状态有效时直接 return。
    const mz_uint fileCount = mz_zip_reader_get_num_files(&zipArchive);
    // 空归档可成功解压，是否含可阅览谱面由项目打开阶段继续判断。
    for ( mz_uint index = 0; index < fileCount; ++index ) {
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&zipArchive, index, &fileStat) ) {
            errorMessage = "读取谱面包条目失败";
            success      = false;
            break;
        }

        std::string archiveName;
        if ( !resolveZipArchiveName(packageBytes,
                                    zipArchive,
                                    fileStat,
                                    archiveName,
                                    errorMessage) ) {
            success = false;
            break;
        }
        // 归一化后的空名称没有落盘对象；其余名称必须先通过路径检查再拼接目标。
        if ( archiveName.empty() ) continue;
        // 名称检查在创建文件和目录之前进行，不能写出后才拒绝越界名称。
        if ( auto unsafeReason = describeUnsafeArchiveName(archiveName) ) {
            errorMessage = fmt::format("{}：{}", *unsafeReason, archiveName);
            success      = false;
            break;
        }

        const auto destinationPath =
            (destinationRoot / Config::utf8ToPath(archiveName))
                .lexically_normal();

        // 目录条目只创建结构，不送入文件解压；这样空目录也能保留下来。
        if ( mz_zip_reader_is_file_a_directory(&zipArchive, index) ) {
            // 目录条目不能进入堆解压路径，否则会混淆空目录与空文件的落盘语义。
            std::error_code filesystemError;
            std::filesystem::create_directories(destinationPath,
                                                filesystemError);
            if ( filesystemError ) {
                errorMessage = filesystemError.message();
                success      = false;
                break;
            }
            continue;
        }

        std::size_t extractedSize = 0;
        // 按条目释放解压缓冲，不累计所有已解压文件的内存副本。
        void* extractedData = mz_zip_reader_extract_to_heap(
            &zipArchive, index, &extractedSize, 0);
        if ( !extractedData ) {
            // 此分支没有可写入缓冲，不将解压失败替换成一个空文件继续打开。
            errorMessage = "解压谱面包条目失败：" + archiveName;
            success      = false;
            break;
        }

        // 解压内存由 miniz 分配，写入是否成功都需配对释放。
        // 此处只返回失败原因，部分落盘内容由拥有临时根目录的上层清理。
        success =
            writeBytesToFile(destinationPath, extractedData, extractedSize);
        // 单个文件写出失败即停止整包，不把部分可读谱面当成完整解包成功。
        mz_free(extractedData);
        // 释放完成后才处理写入错误分支，避免失败路径漏掉本条目的归档分配。
        if ( !success ) {
            errorMessage = "写入临时项目文件失败：" + archiveName;
            break;
        }
    }

    mz_zip_reader_end(&zipArchive);
    // 读取器结束后 packageBytes 才可随函数退出释放，库不能继续借用输入缓冲。
    return success;
}

/// @brief 创建唯一的临时项目目录。
/// @note 通过时间和重试后缀避让已存在缓存，不提供跨进程排他占用保证。
/// @param packagePath 原始谱面包路径。
/// @param errorMessage 失败时写入错误信息。
/// @return 成功时返回临时项目目录。
/// @note 目录创建成功即交付给调用方，后续准备失败的清理由调用方执行。
/// 不回收旧批次目录；清理范围只能是本次返回的具体缓存路径。
std::optional<std::filesystem::path> createTemporaryProjectRoot(
    const std::filesystem::path& packagePath, std::string& errorMessage)
{
    std::error_code filesystemError;
    const auto      tempRoot =
        std::filesystem::temp_directory_path(filesystemError) /
        "MusicMapMaker-Next" / "temporary_projects";
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return std::nullopt;
    }

    std::filesystem::create_directories(tempRoot, filesystemError);
    // 共享缓存父目录可已存在；失败只返回原因，不清理其他项目的临时数据。
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return std::nullopt;
    }

    // 时间戳区分不同批次，重试后缀处理同一批次或已有缓存的名称冲突。
    // 原始包名只参与可读前缀，不直接用作整个临时路径。
    const auto tick = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    const std::string baseName =
        sanitizeTemporaryFolderName(Config::pathToUtf8(packagePath.stem()));
    // 时间戳只用于区分名称，不记录项目的逻辑创建时间，也不影响谱面时间轴。

    for ( int attempt = 0; attempt < 64; ++attempt ) {
        // 重试只是尝试其他目录名，不 sleep 等待时钟变化或阻塞等待旧缓存消失。
        const auto candidate =
            tempRoot / fmt::format("{}_{}_{}", baseName, tick, attempt);
        if ( std::filesystem::exists(candidate, filesystemError) &&
             !filesystemError ) {
            continue;
        }
        filesystemError.clear();
        std::filesystem::create_directories(candidate, filesystemError);
        if ( !filesystemError ) return candidate;
    }

    errorMessage = "无法创建唯一的临时项目目录";
    // 达到重试上限后交还失败，不无限阻塞等待某个候选目录变得可用。
    return std::nullopt;
}

/// @brief 判断目录是否为空。
/// @param path 待检查目录。
/// @return 空目录返回 true。
/// @note 此处不判断目录是否属于本程序，空目录不是删除或覆盖授权。
/// @note 只判断是否有首个条目，隐藏文件同样使目录非空；不递归统计文件数量。
bool isDirectoryEmpty(const std::filesystem::path& path)
{
    // 无法枚举不能视为空目录，避免另存为把权限错误误判为可直接写入。
    std::error_code                     filesystemError;
    std::filesystem::directory_iterator iterator(path, filesystemError);
    if ( filesystemError ) return false;
    const std::filesystem::directory_iterator endIterator;
    return iterator == endIterator;
}

/// @brief 为保存临时项目选择不会覆盖用户文件的最终目录。
/// @param selectedPath 用户选择的路径。
/// @param project 当前临时项目。
/// @return 实际用于保存的项目目录。
/// @note 空返回值表示无法选择目标；非空返回值仍需由复制操作验证可写性。
/// 名称选择不是目录占用锁，选择与复制之间的文件系统变化不由此函数阻止。
std::filesystem::path resolveTemporaryProjectSaveRoot(
    const std::filesystem::path& selectedPath, const Project& project)
{
    std::error_code filesystemError;
    if ( selectedPath.empty() ) return {};
    if ( !std::filesystem::exists(selectedPath, filesystemError) ||
         filesystemError ) {
        // 未存在或查询失败均交给后续创建验证，不在此声明路径一定可写。
        return selectedPath;
    }
    filesystemError.clear();
    if ( !std::filesystem::is_directory(selectedPath, filesystemError) ||
         filesystemError ) {
        return {};
    }
    // 用户指定的空目录直接使用；非空目录仅作父目录，另选子目录隔离导出内容。
    if ( isDirectoryEmpty(selectedPath) ) return selectedPath;
    // 非空父目录原有内容保留，仅在它下方选择本项目专用的新子目录。

    std::string baseName =
        Config::pathToUtf8(project.m_temporarySourcePackagePath.stem());
    if ( baseName.empty() ) {
        // 来源包缺少名称时才使用项目标题，目录净化不反向修改项目展示标题。
        baseName = project.m_metadata.m_title;
    }
    baseName = sanitizeTemporaryFolderName(baseName);
    // 正式目录候选也限制名称字符，不把项目标题中的分隔符变成额外路径层级。

    // 这里只选择尚不存在的名称，不创建目录；后续复制步骤负责实际落盘。
    for ( int attempt = 0; attempt < 128; ++attempt ) {
        const auto candidate =
            selectedPath /
            (attempt == 0 ? baseName : fmt::format("{}_{}", baseName, attempt));
        if ( !std::filesystem::exists(candidate, filesystemError) &&
             !filesystemError ) {
            // 后缀只避让已发现的候选，不覆盖非空父目录里任何已有同名项目。
            return candidate;
        }
        filesystemError.clear();
    }
    return {};
}

/// @brief 递归复制目录内容。
/// @param sourceRoot 源目录。
/// @pre 目标不能是源目录的后代，避免递归枚举把本次输出再次当成输入。
/// @param destinationRoot 目标目录。
/// @param errorMessage 失败时写入错误信息。
/// @return 复制成功返回 true。
/// @pre 目标由另存为目录选择逻辑确定，不应指向正在使用的源缓存。
/// @details 复制普通文件与目录结构，不处理其他文件类型；失败保留已复制部分。
/// 此函数不修改项目根和临时标志，内存状态只在后续配置保存成功后切换。
/// @warning 同步递归枚举和文件复制，只用于显式另存为，不能用于每帧资源同步。
/// @note 枚举启用
/// skip_permission_denied，成功不证明权限拒绝的子目录内容已复制。
/// 复制选项允许覆盖目标同名文件，目标隔离必须由调用方保证。
bool copyDirectoryContents(const std::filesystem::path& sourceRoot,
                           const std::filesystem::path& destinationRoot,
                           std::string&                 errorMessage)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(destinationRoot, filesystemError);
    // 先建立根目录，即使源目录为空也可以得到可保存项目配置的目标位置。
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return false;
    }

    constexpr auto directoryOptions =
        std::filesystem::directory_options::skip_permission_denied;
    // 没有启用 follow_directory_symlink，不把符号链接目录作为普通子树递归枚举。
    std::filesystem::recursive_directory_iterator iterator(
        sourceRoot, directoryOptions, filesystemError);
    const std::filesystem::recursive_directory_iterator endIterator;
    // 目标已创建后枚举仍可能失败；返回失败不自动删除这个目标目录。
    if ( filesystemError ) {
        errorMessage = filesystemError.message();
        return false;
    }

    while ( iterator != endIterator ) {
        const auto entryPath = iterator->path();
        auto       relativePath =
            std::filesystem::relative(entryPath, sourceRoot, filesystemError);
        if ( filesystemError ) {
            errorMessage = filesystemError.message();
            return false;
        }

        // 保持相对层级，谱面内对音频和图片的相对引用无需在复制时重写。
        const auto destinationPath =
            (destinationRoot / relativePath).lexically_normal();
        filesystemError.clear();
        if ( iterator->is_directory(filesystemError) && !filesystemError ) {
            // 目录先创建可保留空目录，同时为后续子项提供父路径。
            std::filesystem::create_directories(destinationPath,
                                                filesystemError);
        } else if ( iterator->is_regular_file(filesystemError) &&
                    !filesystemError ) {
            std::filesystem::create_directories(destinationPath.parent_path(),
                                                filesystemError);
            if ( !filesystemError ) {
                // 按字节复制资源，不重编码图片、音频或谱面，不修改源缓存内容。
                std::filesystem::copy_file(
                    entryPath,
                    destinationPath,
                    std::filesystem::copy_options::overwrite_existing,
                    filesystemError);
            }
        }
        if ( filesystemError ) {
            errorMessage = filesystemError.message();
            return false;
        }

        // 类型检查未归为普通文件或目录且无错误时跳过，不在此转换特殊文件。
        // 枚举途中也可能失败，不能只检查初始化便把部分复制报告成成功。
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            errorMessage = filesystemError.message();
            return false;
        }
    }
    return true;
}

}  // namespace

/// @brief 获取项目控制器全局实例。
/// @return 项目控制器全局实例引用。
/// @note 静态实例的创建不等于已打开项目；当前项目仍可能为空。
/// 静态初始化安全不替代当前项目读写和请求队列各自的同步要求。
ProjectController& ProjectController::instance()
{
    static ProjectController instance;
    return instance;
}

/// @brief 构造项目控制器并订阅项目请求事件。
/// @note 订阅句柄由析构函数逐项解除，事件回调不拥有控制器实例。
/// 构造不打开默认项目，启动时是否发起打开请求由调用方决定。
ProjectController::ProjectController()
{
    // 事件入口只排入请求，不在回调里直接切换项目或销毁旧画布。
    // 保存确认和画布关闭完成后，再由逻辑线程消费可执行动作。
    /// @brief 项目控制器订阅项目事件使用的全局事件总线。
    auto& eventBus = Event::EventBus::instance();
    // 每种事件单独保存订阅句柄，析构时可以精确解除本控制器注册的回调。
    m_openProjectSubscription = eventBus.subscribe<Event::OpenProjectEvent>(
        [this](const Event::OpenProjectEvent& event) {
            requestOpenProject(event.m_projectPath, event.m_origin);
        });
    m_openTemporaryProjectSubscription =
        eventBus.subscribe<Event::OpenTemporaryProjectPackageEvent>(
            [this](const Event::OpenTemporaryProjectPackageEvent& event) {
                requestOpenTemporaryProjectPackage(event.m_packagePath,
                                                   event.m_origin);
            });
    m_closeProjectSubscription =
        eventBus.subscribe<Event::ProjectCloseRequestedEvent>(
            [this](const Event::ProjectCloseRequestedEvent&) {
                requestCloseProject();
            });
    m_createProjectSubscription =
        eventBus.subscribe<Event::ProjectCreateRequestedEvent>(
            [this](const Event::ProjectCreateRequestedEvent& event) {
                // 复制必要创建参数再排队，不保留可能在发布结束后失效的事件引用。
                ProjectCreationOptions options;
                options.m_title  = event.m_title;
                options.m_artist = event.m_artist;
                options.m_mapper = event.m_mapper;
                options.m_colorPaletteSchemeName =
                    event.m_colorPaletteSchemeName;
                options.m_sidebarActiveTab = event.m_sidebarActiveTab;
                requestCreateProject(event.m_projectPath, options);
            });
    // 完成与取消只推进请求状态，不在回调中重复执行目录复制或项目存储。
    m_projectSwitchCompletedSubscription =
        eventBus.subscribe<Event::ProjectSwitchCompletedEvent>(
            [this](const Event::ProjectSwitchCompletedEvent&) {
                completePendingProjectSwitch();
            });
    m_projectSwitchCancelledSubscription =
        eventBus.subscribe<Event::ProjectSwitchCancelledEvent>(
            [this](const Event::ProjectSwitchCancelledEvent&) {
                cancelPendingProjectSwitch();
            });
}

/// @brief 析构项目控制器并取消项目事件订阅。
/// @pre 应在外部停止发起新的项目操作后销毁，退订不能替代调用方的退出协调。
ProjectController::~ProjectController()
{
    // 回调捕获 this，先解除所有订阅再停止监听，避免控制器析构后被事件访问。
    /// @brief 项目控制器取消项目事件订阅使用的全局事件总线。
    auto& eventBus = Event::EventBus::instance();
    if ( m_openProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::OpenProjectEvent>(
            m_openProjectSubscription);
    }
    if ( m_openTemporaryProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::OpenTemporaryProjectPackageEvent>(
            m_openTemporaryProjectSubscription);
    }
    if ( m_closeProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectCloseRequestedEvent>(
            m_closeProjectSubscription);
    }
    if ( m_createProjectSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectCreateRequestedEvent>(
            m_createProjectSubscription);
    }
    if ( m_projectSwitchCompletedSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectSwitchCompletedEvent>(
            m_projectSwitchCompletedSubscription);
    }
    if ( m_projectSwitchCancelledSubscription != 0 ) {
        eventBus.unsubscribe<Event::ProjectSwitchCancelledEvent>(
            m_projectSwitchCancelledSubscription);
    }
    stopDirectoryWatcher();
}

/// @brief 发布项目切换需要关闭旧画布的事件。
/// @param projectPathToOpen 旧画布关闭后需要打开的项目路径。
/// @param closeOnly 是否只关闭当前项目而不打开新项目。
/// @note 空目标配合 closeOnly 表达单纯关闭；不能只靠路径是否为空推断事件用途。
void ProjectController::publishProjectSwitchNeedsCanvasClose(
    const std::filesystem::path& projectPathToOpen, bool closeOnly) const
{
    /// @brief 项目切换等待旧画布关闭的事件载荷。
    Event::ProjectSwitchNeedsCanvasCloseEvent event;
    event.m_projectPathToOpen = projectPathToOpen;
    // 载荷路径是后继目标，关闭的仍是当前旧画布，不是该路径下的新谱面。
    event.m_closeOnly = closeOnly;
    Event::EventBus::instance().publish(event);
}

/// @brief 获取当前项目。
/// @return 当前项目指针；未打开项目时返回 nullptr。
/// @note 返回非拥有观察指针，项目切换后应重新取得，不跨切换保存。
/// @pre 调用方与项目替换、关闭串行访问，不能依赖此 getter 延长对象生命期。
Project* ProjectController::currentProject()
{
    return m_currentProject.get();
}

/// @brief 获取当前项目。
/// @return 当前项目只读指针；未打开项目时返回 nullptr。
/// @note const 只限制本次访问，不为项目生命周期或并发访问额外加锁。
/// @pre 不与 openProject、closeProject 或正式目录接管并发读取项目字段。
const Project* ProjectController::currentProject() const
{
    return m_currentProject.get();
}

/// @brief 请求打开项目，必要时等待 UI 完成旧画布关闭。
/// @param projectPath 要打开的项目目录或谱面文件路径。
/// @param origin 本次入口来源，随延迟请求保留给演练等结果消费者。
/// @details 空路径忽略；协作限制开启时发布拦截事件，不覆盖已存的请求状态。
/// 成功排入请求不代表项目已打开，实际结果由后续消费与打开流程决定。
/// @note 请求槽不是 FIFO，多次打开保留最新目标；来源信息与路径一起替换。
/// 请求不会立即清理当前项目，UI 仍可在保存确认中取消切换。
void ProjectController::requestOpenProject(
    const std::filesystem::path& projectPath, Event::ProjectOpenOrigin origin)
{
    if ( projectPath.empty() ) {
        return;
    }
    if ( isLocalProjectOpeningBlockedByCollaboration() ) {
        publishCollaborationProjectOpenBlocked();
        return;
    }

    // 请求槽只保留最新意图；普通打开要同时撤销前一次创建选项和关闭状态。
    /// @brief 保护本次打开请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectCreationOptions.reset();
    m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    m_requestedProjectClose    = false;
    m_pendingProjectClose      = false;
    m_projectCloseReady        = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        // UI 正在关闭的是旧画布，替换目标不改变那批画布的关闭对象。
        // 已进入关闭旧画布阶段时只替换目标，不另起一次画布关闭流程。
        m_switchOrigin                 = origin;
        m_pendingProjectSwitchPath     = projectPath;
        m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
        m_pendingProjectSwitchCreationOptions.reset();
    } else {
        m_requestedOrigin          = origin;
        m_requestedProjectPath     = projectPath;
        m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    }
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 请求打开谱面包为临时只读项目。
/// @param packagePath 要解压阅览的谱面包路径。
/// @param origin 发起打开的入口来源。
/// @details 本层只排入路径和打开模式，不在请求线程读取或解压谱包。
/// 是否为可解码文件由准备阶段判断，请求返回不表示解压成功。
/// @note 排队时保留源谱包路径，不能提前把它替换为尚不存在的缓存目录。
void ProjectController::requestOpenTemporaryProjectPackage(
    const std::filesystem::path& packagePath, Event::ProjectOpenOrigin origin)
{
    if ( packagePath.empty() ) {
        return;
    }
    if ( isLocalProjectOpeningBlockedByCollaboration() ) {
        publishCollaborationProjectOpenBlocked();
        return;
    }

    // 路径与模式必须在同一锁内替换；谱包路径不能被消费为普通项目目录。
    /// @brief 保护本次临时谱面包打开请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectCreationOptions.reset();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        m_switchOrigin                 = origin;
        m_pendingProjectSwitchPath     = packagePath;
        m_pendingProjectSwitchOpenMode = ProjectOpenMode::TemporaryPackage;
        m_pendingProjectSwitchCreationOptions.reset();
    } else {
        m_requestedOrigin          = origin;
        m_requestedProjectPath     = packagePath;
        m_requestedProjectOpenMode = ProjectOpenMode::TemporaryPackage;
    }
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 请求创建并打开项目，必要时等待 UI 完成旧画布关闭。
/// @param projectPath 要创建的项目根目录。
/// @param options 新项目初始设置。
/// @note 创建与普通打开共用模式，通过独立的可选创建参数区分；不立即创建目录。
void ProjectController::requestCreateProject(
    const std::filesystem::path&  projectPath,
    const ProjectCreationOptions& options)
{
    if ( projectPath.empty() ) {
        return;
    }
    if ( isLocalProjectOpeningBlockedByCollaboration() ) {
        publishCollaborationProjectOpenBlocked();
        return;
    }

    // 创建选项与目标路径一起排队，避免等待保存确认时被后续界面编辑影响。
    /// @brief 保护本次新建项目请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        m_requestedProjectOpenMode = ProjectOpenMode::Normal;
        m_requestedProjectCreationOptions.reset();
        m_switchOrigin                 = Event::ProjectOpenOrigin::Unknown;
        m_pendingProjectSwitchPath     = projectPath;
        m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
        m_pendingProjectSwitchCreationOptions = options;
    } else {
        m_requestedOrigin                 = Event::ProjectOpenOrigin::Unknown;
        m_requestedProjectPath            = projectPath;
        m_requestedProjectOpenMode        = ProjectOpenMode::Normal;
        m_requestedProjectCreationOptions = options;
    }
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 请求关闭当前项目，必要时等待 UI 完成旧画布关闭。
/// @details 请求只是最新的关闭意图，不在调用线程保存或释放项目。
/// 等待 UI 期间仍可取消，完成通知之后才会产生实际关闭动作。
/// 关闭请求不经过本机打开限制检查，限制本机打开不应阻止退出当前项目。
void ProjectController::requestCloseProject()
{
    // 显式关闭取代所有尚未执行的打开请求，包括等待旧画布关闭的目标。
    /// @brief 保护本次关闭请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectPath.clear();
    m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    m_requestedProjectCreationOptions.reset();
    m_pendingProjectSwitchPath.clear();
    m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectSwitchCreationOptions.reset();
    // 路径已清空时残留来源枚举不构成有效动作，关闭动作无需伪造打开来源。
    m_requestedProjectClose = true;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 更新协作期间本机项目打开限制，并在首次进入限制时取消待处理切换。
/// @param blocked 是否禁止打开本机项目。
/// @note 禁止打开不会强制关闭当前项目，也不撤销已经消费到局部快照中的动作。
void ProjectController::setLocalProjectOpeningBlockedByCollaboration(
    bool blocked)
{
    const bool previous = m_localProjectOpeningBlockedByCollaboration.exchange(
        blocked, std::memory_order_acq_rel);
    // 只处理未限制到限制的边沿；重复通知和解除限制都不应取消新排入的请求。
    if ( previous == blocked || !blocked ) return;
    // 解禁只允许未来请求，不自动重放进入协作时已经取消的本机目标。
    cancelPendingProjectSwitch();
}

/// @brief 读取协作层发布的项目打开限制。
/// @return 当前禁止本机项目打开时返回 true。
/// @note 只读取限制状态，不消费该标志；重复读取不能解除协作层发布的限制。
bool ProjectController::isLocalProjectOpeningBlockedByCollaboration() const
{
    return m_localProjectOpeningBlockedByCollaboration.load(
        std::memory_order_acquire);
}

/// @brief 是否存在等待旧谱面画布关闭后的项目打开或关闭流程。
/// @return 有挂起项目切换流程时返回 true。
/// @note 不包含刚排入的请求或已经就绪的动作；不能替代待处理门闩查询。
/// 挂起关闭没有项目路径，必须同时检查 m_pendingProjectClose。
bool ProjectController::hasPendingProjectSwitch() const
{
    /// @brief 保护挂起项目切换状态读取的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return !m_pendingProjectSwitchPath.empty() || m_pendingProjectClose;
}

/// @brief 完成 UI 侧逐个关闭旧谱面画布后的项目切换流程。
/// @pre 调用方已完成旧画布关闭及其保存确认；本函数不重复验证画布状态。
/// @note 没有挂起切换时忽略通知，不能凭完成事件创建新的项目请求。
/// @note 只推进等待阶段；真正打开或关闭仍由逻辑线程消费动作后执行。
void ProjectController::completePendingProjectSwitch()
{
    // UI 完成通知仅推进待处理状态，不在事件回调中执行文件读取或目录扫描。
    /// @brief 保护挂起项目切换状态推进的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if ( m_pendingProjectClose ) {
        // 仅关闭没有后继路径，用独立 ready 标记而非空路径表示其可执行状态。
        m_pendingProjectClose = false;
        m_projectCloseReady   = true;
        m_hasPendingProjectAction.store(true, std::memory_order_release);
        return;
    }

    if ( m_pendingProjectSwitchPath.empty() ) return;
    // 重复完成通知没有对应挂起路径时无效，防止已交付目标再次进入就绪槽。

    // 将等待 UI 的槽转交给逻辑线程就绪槽，并清空前者，防止重复完成通知重放。
    m_pendingProjectPath            = m_pendingProjectSwitchPath;
    m_pendingOrigin                 = m_switchOrigin;
    m_pendingProjectOpenMode        = m_pendingProjectSwitchOpenMode;
    m_pendingProjectCreationOptions = m_pendingProjectSwitchCreationOptions;
    // 路径、模式、创建参数和来源属于同一请求，不能只转移路径留下旧选项。
    m_pendingProjectSwitchPath.clear();
    m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectSwitchCreationOptions.reset();
    m_hasPendingProjectAction.store(true, std::memory_order_release);
}

/// @brief 取消所有挂起项目切换流程。
/// @note 只撤销尚未消费的请求，不撤销已经执行的文件写入或已交接的项目。
/// 请求门闩清零不是取消正在执行的 I/O；本控制器没有在此中断文件读写。
/// 取消不会恢复 UI 已经关闭的画布，也不重建其编辑会话。
void ProjectController::cancelPendingProjectSwitch()
{
    // 取消涵盖请求、等待 UI 和已就绪三个阶段，不能只清掉当前对话框对应的路径。
    /// @brief 保护挂起项目切换状态清理的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_pendingProjectOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectCreationOptions.reset();
    m_requestedProjectPath.clear();
    m_requestedProjectOpenMode = ProjectOpenMode::Normal;
    m_requestedProjectCreationOptions.reset();
    m_pendingProjectSwitchPath.clear();
    m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
    m_pendingProjectSwitchCreationOptions.reset();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    m_hasPendingProjectAction.store(false, std::memory_order_release);
    // 清门闩与清槽都在同一锁内，后续新请求可重新置位，不永久禁用消费入口。
}

/// @brief 消费逻辑线程本轮需要处理的项目切换动作。
/// @param needsCanvasClose 当前是否需要先关闭旧谱面画布。
/// @note 参数描述消费时的会话状态，不由待处理请求的路径是否存在决定。
/// @return 本轮需要执行的关闭或打开动作。
/// @details 返回值是动作快照，文件操作由逻辑线程调用方执行。
/// @pre 只由逻辑侧单一消费者调用，不能让多个线程竞争执行取出的项目动作。
/// 等待 UI 时允许返回空动作，空动作不等同于用户已经取消请求。
/// @pre needsCanvasClose 来自当前旧会话状态，而不是待打开项目的谱面数量。
/// @warning 逻辑轮询入口；无动作时走原子门闩快路，不在此扫描目录或等待 UI。
/// @note UI/事件线程写请求和完成状态，逻辑线程消费；原子位只提供快速通知。
/// 路径和选项仍由互斥锁保护，不能把 acquire 当成无锁读取复杂状态的许可。
ProjectController::PendingProjectAction
ProjectController::consumePendingProjectAction(bool needsCanvasClose)
{
    // 门闩只表示状态可能需要处理，不代替互斥锁对路径及选项的成组保护。
    // exchange 清掉本轮通知；后续请求仍可再次置位，交由后续轮次消费。
    if ( !m_hasPendingProjectAction.exchange(false,
                                             std::memory_order_acq_rel) ) {
        return {};
    }

    /// @brief 本轮要返回给逻辑线程的项目动作。
    PendingProjectAction action;
    // 返回值可同时携带关闭和打开意图，调用方须按关闭旧项目再打开目标的顺序处理。
    /// @brief 本轮消费到的项目打开请求。
    std::filesystem::path requestedPath;
    /// @brief 本轮消费到的项目打开模式。
    ProjectOpenMode requestedOpenMode = ProjectOpenMode::Normal;
    /// @brief 本轮请求的用户入口，不受后续请求覆盖。
    Event::ProjectOpenOrigin requestedOrigin{
        Event::ProjectOpenOrigin::Unknown
    };
    /// @brief 本轮消费到的项目创建初始设置。
    std::optional<ProjectCreationOptions> requestedCreationOptions;
    /// @brief 本轮是否消费到项目关闭请求。
    bool requestedClose = false;
    /// @brief 本轮是否需要通知 UI 先关闭旧画布。
    bool shouldPublishCanvasClose = false;
    /// @brief 通知 UI 关闭旧画布后要打开的项目路径。
    std::filesystem::path canvasCloseProjectPath;
    /// @brief 通知 UI 关闭旧画布后是否只关闭项目。
    bool canvasCloseOnly = false;
    // 发布载荷先在局部准备，持锁状态只管理请求，不直接调用外部事件处理者。

    {
        /// @brief 保护从待处理请求队列取出请求的锁。
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if ( m_requestedProjectClose ) {
            // 取出后立即清请求位，等待关闭由另外的 pending 状态接续。
            requestedClose          = true;
            m_requestedProjectClose = false;
        }
        if ( !m_requestedProjectPath.empty() ) {
            requestedPath            = m_requestedProjectPath;
            requestedOrigin          = m_requestedOrigin;
            requestedOpenMode        = m_requestedProjectOpenMode;
            requestedCreationOptions = m_requestedProjectCreationOptions;
            // 局部值副本在解锁后继续使用，不借用可能被新请求替换的字符串与选项。
            m_requestedProjectPath.clear();
            m_requestedProjectOpenMode = ProjectOpenMode::Normal;
            m_requestedProjectCreationOptions.reset();
        }
    }

    // 将请求快照转换为立即动作或等待 UI 的动作；此处不实际释放当前项目。
    if ( requestedClose ) {
        if ( needsCanvasClose ) {
            // 延迟阶段不返回实际关闭动作，UI 仍有机会保存或取消旧会话。
            /// @brief 保护关闭请求转入 UI 等待状态的锁。
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingProjectPath.clear();
            m_pendingProjectOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectCreationOptions.reset();
            m_pendingProjectSwitchPath.clear();
            m_pendingProjectSwitchOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectSwitchCreationOptions.reset();
            m_pendingProjectClose    = true;
            shouldPublishCanvasClose = true;
            canvasCloseOnly          = true;
            XINFO(
                "Project close deferred until current beatmap canvases close.");
        } else {
            action.m_closeProject = true;
            // 无画布需要确认时直接交付动作，不额外发布一轮 UI 关闭事件。
        }
    }

    if ( !requestedPath.empty() ) {
        if ( needsCanvasClose ) {
            /// @brief 保护打开请求转入 UI 等待状态的锁。
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingProjectPath.clear();
            m_pendingProjectOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectCreationOptions.reset();
            m_pendingProjectClose                 = false;
            m_pendingProjectSwitchPath            = requestedPath;
            m_switchOrigin                        = requestedOrigin;
            m_pendingProjectSwitchOpenMode        = requestedOpenMode;
            m_pendingProjectSwitchCreationOptions = requestedCreationOptions;
            // 挂起请求等待完成事件；不在此重新置门闩形成逐帧重复关闭通知。
            shouldPublishCanvasClose = true;
            canvasCloseProjectPath   = requestedPath;
            canvasCloseOnly          = false;
            XINFO(
                "Project open deferred until current beatmap canvases close: "
                "{}",
                Config::pathToUtf8(requestedPath));
        } else {
            action.m_projectPathToOpen = requestedPath;
            // 这里只交付打开意图，不在消费函数中校验目录或执行谱包解压。
            action.m_origin                 = requestedOrigin;
            action.m_projectCreationOptions = requestedCreationOptions;
            action.m_projectOpenMode        = requestedOpenMode;
        }
    }

    // 发布事件放在状态锁之外，允许事件消费者回调完成/取消接口而不重入持锁区。
    if ( shouldPublishCanvasClose ) {
        // 事件载荷按本轮快照构造，发布期间发生的新请求由其各自的槽和门闩接续。
        publishProjectSwitchNeedsCanvasClose(canvasCloseProjectPath,
                                             canvasCloseOnly);
    }

    {
        // 事件消费者可能已同步完成关闭；同轮检查就绪槽，避免额外等待一轮。
        /// @brief 保护 UI 完成后的延迟动作消费锁。
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if ( m_projectCloseReady ) {
            // 消费完成标志后清空，避免后一轮重复关闭已经释放的项目。
            action.m_closeProject = true;
            m_projectCloseReady   = false;
        }
        // 已选定的立即打开动作优先，不让旧的就绪路径覆盖本轮目标。
        if ( action.m_projectPathToOpen.empty() &&
             !m_pendingProjectPath.empty() ) {
            action.m_projectPathToOpen      = m_pendingProjectPath;
            action.m_origin                 = m_pendingOrigin;
            action.m_projectOpenMode        = m_pendingProjectOpenMode;
            action.m_projectCreationOptions = m_pendingProjectCreationOptions;
            m_pendingProjectPath.clear();
            m_pendingProjectOpenMode = ProjectOpenMode::Normal;
            m_pendingProjectCreationOptions.reset();
            // 就绪动作被取走后清空参数，下一次普通打开不会继承旧创建设置。
        }
    }

    return action;
}

/// @brief 判断是否存在待逻辑线程消费的项目切换动作。
/// @return 存在待检查的状态更新时返回 true，不承诺立即得到非空动作。
/// @note 等待 UI 时可能为 false；应使用挂起切换查询区分“等待中”与“无请求”。
/// @warning 每 update 的只读快路；不应为查询请求状态引入项目扫描或磁盘访问。
bool ProjectController::hasPendingProjectAction() const
{
    return m_hasPendingProjectAction.load(std::memory_order_acquire);
}

/// @brief 打开项目并启动项目目录监听。
/// @param projectPath 要打开的项目目录或谱面文件路径。
/// @param creationOptions 有值时允许创建目录，仅无既有配置时应用初始设置。
/// @param temporaryInfo 临时项目的源谱包信息；projectPath 此时指向解压缓存。
/// @note 临时来源只改变项目属性与显示来源，不把文件读写重定向到压缩包内部。
/// @details 成功结果包含目标谱面与音效注册请求，调用方仍需完成会话和音频接入。
/// 配置读取或回写失败可退化为扫描结果；路径无效及空临时谱包则返回未打开。
/// @note 进度事件标示处理阶段，不是磁盘写入成功或项目切换完成的独立凭证。
/// @pre 旧画布关闭和旧项目副作用清理由调用方协调，本函数不弹出保存确认。
/// @return 打开项目后的结果信息。
/// @warning
/// 低频项目切换入口，包含目录扫描、配置读写和监听器启动，不用于逐帧调用。
ProjectController::OpenProjectResult ProjectController::openProject(
    const std::filesystem::path&                 projectPath,
    const std::optional<ProjectCreationOptions>& creationOptions,
    const std::optional<TemporaryProjectInfo>&   temporaryInfo)
{
    /// @brief 本次打开项目的返回结果。
    OpenProjectResult result;
    // 请求排队后协作状态仍可能变化，真正执行时再次检查，而非只信任入口校验。
    if ( isLocalProjectOpeningBlockedByCollaboration() ) {
        publishCollaborationProjectOpenBlocked();
        return result;
    }
    /// @brief 调用方传入路径的绝对规范形式。
    const std::filesystem::path requestedProjectPath =
        makeAbsoluteNormalizedPath(projectPath);
    // 拖入文件时实际项目根将另行改为父目录，不改变保存的目标谱面路径。
    /// @brief 实际打开的项目目录路径。
    std::filesystem::path actualProjectPath = requestedProjectPath;
    /// @brief 若传入谱面文件，则记录需要自动打开的谱面路径。
    std::filesystem::path targetBeatmapPath;
    /// @brief 本次打开是否来自临时谱面包。
    const bool isPackageOpen = temporaryInfo && temporaryInfo->m_isTemporary;
    // 临时缓存是实际读取位置，源谱包是诊断展示位置，两者不能混用作写入目标。
    /// @brief 失败提示中展示给用户的源路径。
    const std::filesystem::path failureDisplayPath =
        isPackageOpen ? temporaryInfo->m_sourcePackagePath : projectPath;
    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::Validating,
        0.10F,
        projectOpenProgressPathDetail(failureDisplayPath));

    // 创建和打开共享资源恢复流程，区别在于创建允许建立尚不存在的目录。
    if ( creationOptions ) {
        // 新建选项也允许用于已有目录，不在这里清空该目录的既有资源。
        std::error_code filesystemError;
        if ( !std::filesystem::exists(actualProjectPath, filesystemError) ) {
            std::filesystem::create_directories(actualProjectPath,
                                                filesystemError);
            if ( filesystemError ) {
                const std::string message =
                    "无法创建项目目录：" +
                    Config::pathToUtf8(actualProjectPath);
                XERROR("Failed to create project directory: {}",
                       Config::pathToUtf8(actualProjectPath));
                publishProjectOpenFailed(
                    failureDisplayPath, message, isPackageOpen);
                return result;
            }
        }

        filesystemError.clear();
        if ( !std::filesystem::is_directory(actualProjectPath,
                                            filesystemError) ||
             filesystemError ) {
            const std::string message = "创建项目失败，目标不是文件夹：" +
                                        Config::pathToUtf8(actualProjectPath);
            XERROR("Failed to create project: Target is not a directory: {}",
                   Config::pathToUtf8(actualProjectPath));
            publishProjectOpenFailed(
                failureDisplayPath, message, isPackageOpen);
            return result;
        }
        actualProjectPath = makeAbsoluteNormalizedPath(actualProjectPath);
    } else {
        std::error_code requestedPathError;
        const bool      requestedPathIsFile =
            std::filesystem::exists(requestedProjectPath, requestedPathError) &&
            !requestedPathError &&
            std::filesystem::is_regular_file(requestedProjectPath,
                                             requestedPathError) &&
            !requestedPathError;
        // 拖入谱面时项目根取其父目录，同时保留文件作为打开项目后的目标谱面。
        if ( requestedPathIsFile ) {
            // 此处仅识别普通文件，不验证它能否加载成谱面；目标会话由后续流程打开。
            targetBeatmapPath = requestedProjectPath;
            actualProjectPath =
                makeAbsoluteNormalizedPath(requestedProjectPath.parent_path());
        }
    }

    std::error_code actualPathError;
    const bool      actualProjectPathIsDirectory =
        std::filesystem::exists(actualProjectPath, actualPathError) &&
        !actualPathError &&
        std::filesystem::is_directory(actualProjectPath, actualPathError) &&
        !actualPathError;
    if ( !actualProjectPathIsDirectory ) {
        // 无效项目根在候选对象分配前拒绝，不把当前项目换成一个空对象。
        const std::string message =
            "路径不存在或不是文件夹：" + Config::pathToUtf8(actualProjectPath);
        XERROR(
            "Failed to open project: Path does not exist or is not a "
            "directory: {}",
            Config::pathToUtf8(actualProjectPath));
        publishProjectOpenFailed(failureDisplayPath, message, isPackageOpen);
        return result;
    }

    XINFO("Opening project at: {}", Config::pathToUtf8(actualProjectPath));

    // 候选项目先局部组装，前置校验失败不会把当前项目替换成半成品。
    /// @brief 新创建并等待接管为当前项目的项目实例。
    auto newProject = std::make_unique<Project>();
    // 当前项目的所有权暂不变更，后面的目录和临时谱包检查仍可能提前返回。
    newProject->m_projectRoot = actualProjectPath;
    if ( temporaryInfo && temporaryInfo->m_isTemporary ) {
        newProject->m_isTemporaryProject = true;
        newProject->m_temporarySourcePackagePath =
            temporaryInfo->m_sourcePackagePath;
    }
    newProject->m_metadata.m_title =
        Config::pathToUtf8(actualProjectPath.filename());
    // 目录名只是初始展示值，成功读取的项目配置稍后可以恢复用户标题。

    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::ScanningDirectory,
        0.18F,
        projectOpenProgressPathDetail(actualProjectPath));
    /// @brief 当前项目目录扫描结果。
    auto directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
    if ( !directoryScan.m_success ) {
        // 当前实现记录扫描失败后仍继续恢复配置，不将其视为整个打开操作立即失败。
        XERROR("Error while scanning project directory: {}",
               Config::pathToUtf8(actualProjectPath));
    }

    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::BuildingResources,
        0.34F,
        projectOpenProgressPathDetail(actualProjectPath));
    // 文件系统扫描提供当前资源基线，持久化配置随后只恢复其设置与引用信息。
    m_projectResourceService.buildInitialResources(*newProject, directoryScan);
    // 资源基线来自扫描，不直接信任描述文件中可能已失效的文件清单。

    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::LoadingConfiguration, 0.46F, ".mmm");
    /// @brief 是否存在新分片或旧单文件项目配置。
    const bool projectConfigurationExists =
        ProjectStorage::hasProjectConfiguration(actualProjectPath);
    // 保留打开时的存在性快照，后面首次保存创建配置不能改变默认值应用判断。
    /// @brief 从分片或旧单文件读取出的持久化项目配置。
    auto persistedProject = m_projectStorage.load(actualProjectPath);
    // 配置是否存在与读取是否成功分别保留，创建默认值不能掩盖已存在但损坏的配置。
    // 先恢复排除列表及工作区等用户状态，再合并音频配置；不直接用旧列表替代扫描。
    if ( persistedProject.m_success ) {
        auto& loadedProject = persistedProject.m_project;
        /// @brief 需要从旧版 m_volume 迁移且不信任持久化类型的资源。
        const auto legacyAudioResourceKeys =
            ProjectResourceService::collectLegacyAudioResourceKeys(
                persistedProject.m_serializedProject);
        newProject->m_metadata = loadedProject.m_metadata;
        // 只迁移用户状态，项目根与临时来源继续由本次实际打开位置决定。
        newProject->m_settings        = loadedProject.m_settings;
        newProject->m_draftLaneGroups = loadedProject.m_draftLaneGroups;
        newProject->m_excludedBeatmapPaths =
            loadedProject.m_excludedBeatmapPaths;
        newProject->m_excludedAudioPaths = loadedProject.m_excludedAudioPaths;

        m_projectResourceService.mergePersistedAudioResources(
            *newProject, loadedProject, legacyAudioResourceKeys);
        if ( !legacyAudioResourceKeys.empty() ) {
            XINFO("Migrated {} legacy audio resource configurations.",
                  legacyAudioResourceKeys.size());
        }

        // 旧音频引用迁移按谱面记录结果，单个失败仅记录，不阻止其余资源恢复。
        // 迁移可能写回谱面文件，不是单纯恢复项目描述的内存操作。
        const auto legacyBeatmapAudioMigration =
            m_projectResourceService.migrateLegacyBeatmapAudioTracks(
                *newProject, loadedProject);
        if ( legacyBeatmapAudioMigration.m_migratedBeatmapCount > 0 ) {
            XINFO(
                "Migrated {} legacy beatmap audio track references to MMM v2 "
                "samples.",
                legacyBeatmapAudioMigration.m_migratedBeatmapCount);
        }
        for ( const auto& failedBeatmapPath :
              legacyBeatmapAudioMigration.m_failedBeatmapPaths ) {
            XWARN("Failed to migrate legacy beatmap audio reference: {}",
                  failedBeatmapPath);
        }

        XINFO("Project configuration loaded from {} storage.",
              persistedProject.m_source == ProjectStorage::Source::Split
                  ? "split"
                  : "legacy");
    } else if ( projectConfigurationExists ) {
        XWARN("Failed to load project configuration, using scanned results: {}",
              persistedProject.m_errorMessage);
    }

    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::MigratingConfiguration,
        0.58F,
        projectOpenProgressPathDetail(actualProjectPath));
    // 以配置是否存在而非加载成功判定新建，避免配置损坏时套用创建参数覆盖用户信息。
    if ( creationOptions && !projectConfigurationExists ) {
        // 即使本次请求称为创建，已存在配置的目录仍按恢复项目处理。
        applyProjectCreationOptions(
            *newProject,
            *creationOptions,
            Config::pathToUtf8(actualProjectPath.filename()));
    }
    if ( temporaryInfo && temporaryInfo->m_isTemporary ) {
        // 标题采用包名而非带时间后缀的缓存目录名，保持只读阅览的来源可辨识。
        const auto packageTitle =
            Config::pathToUtf8(temporaryInfo->m_sourcePackagePath.stem());
        if ( !packageTitle.empty() ) {
            newProject->m_metadata.m_title = packageTitle;
        }
    }

    // 恢复后的排除项作用于合并结果，防止扫描把用户移除但仍在磁盘上的资源重新加入。
    m_projectResourceService.applyExcludedResources(*newProject);
    // 排除过滤发生在配置合并和旧音轨迁移之后，不能假定前面的探测已跳过排除项。

    // 普通项目允许为空；临时谱包必须含可阅览谱面，否则不交付一个空的只读项目。
    if ( temporaryInfo && temporaryInfo->m_isTemporary &&
         newProject->m_beatmaps.empty() ) {
        // 这里检查收录列表非空，不承诺列表中每份谱面都已成功加载成编辑会话。
        const std::string message = "谱面包内没有可打开的谱面文件";
        XERROR("Temporary project package contains no supported beatmaps: {}",
               Config::pathToUtf8(temporaryInfo->m_sourcePackagePath));
        publishProjectOpenFailed(
            temporaryInfo->m_sourcePackagePath, message, true);
        return result;
    }

    // 谱包入口没有指定单个谱面时默认打开资源列表首项，不覆盖已有的明确目标。
    if ( temporaryInfo && temporaryInfo->m_isTemporary &&
         targetBeatmapPath.empty() && !newProject->m_beatmaps.empty() ) {
        targetBeatmapPath =
            actualProjectPath /
            Config::utf8ToPath(newProject->m_beatmaps.front().m_filePath);
    }

    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::SavingConfiguration, 0.68F, ".mmm");
    std::string storageError;
    // 存储错误独立于扫描结果，写回失败不会撤销已恢复的候选内存内容。
    // 新分片成功写入后才清理旧配置；写入失败仍允许阅览已恢复到内存的项目。
    if ( m_projectStorage.save(*newProject, actualProjectPath, storageError) ) {
        // 打开临时谱包时同样保存描述文件，但写入目录是缓存，不是原始谱包。
        if ( persistedProject.m_success &&
             !m_projectStorage.removeLegacyProjectFile(actualProjectPath,
                                                       storageError) ) {
            XWARN(
                "Project split storage saved but legacy file could not be "
                "removed: {}",
                storageError);
        } else if ( persistedProject.m_source ==
                    ProjectStorage::Source::Legacy ) {
            XINFO("Legacy mmm_project.json migrated to split .mmm storage.");
        }
    } else {
        XWARN("Failed to save split project storage while opening project: {}",
              storageError);
    }

    publishProjectOpenProgress(Event::ProjectOpenProgressStage::PreparingAudio,
                               0.76F,
                               newProject->m_metadata.m_title);
    std::size_t audioResourceIndex = 0;
    // 进度按全部资源位置推进，仅 Effect 产生注册事件；这不是已解码字节比例。
    const std::size_t audioResourceCount = newProject->m_audioResources.size();
    // 分母在遍历前固定，注册请求列表增长不改变本阶段的进度基准。
    for ( const auto& resource : newProject->m_audioResources ) {
        const float resourceProgress =
            audioResourceCount == 0
                ? 0.80F
                : 0.76F + 0.04F * static_cast<float>(audioResourceIndex + 1) /
                              static_cast<float>(audioResourceCount);
        ++audioResourceIndex;
        // 此处只汇集音效注册请求，主音轨由后续会话加载；不在控制器里解码全部音频。
        if ( resource.m_type != AudioTrackType::Effect ) {
            continue;
        }

        publishProjectOpenProgress(
            Event::ProjectOpenProgressStage::PreparingAudio,
            resourceProgress,
            resource.m_id);

        /// @brief 音效资源在项目目录中的绝对路径。
        auto absolutePath =
            actualProjectPath / Config::utf8ToPath(resource.m_path);
        std::error_code resourcePathError;
        if ( !std::filesystem::exists(absolutePath, resourcePathError) ||
             resourcePathError ) {
            // 缺失音效不阻止项目打开，只是不向后续加载器交付不可定位的登记请求。
            continue;
        }

        /// @brief 需要调用方登记的按需加载音效请求。
        ProjectCommandService::AudioRegistrationRequest registrationRequest;
        registrationRequest.m_resource     = resource;
        registrationRequest.m_absolutePath = absolutePath;
        result.m_effectRegistrations.push_back(registrationRequest);
        // 返回值拥有资源配置副本，不借用即将接管为当前项目的列表元素。
    }

    // 候选状态完整后才交接所有权并启动监听；临时缓存路径不写入最近项目。
    m_currentProject = std::move(newProject);
    // 交接后旧的 currentProject 观察地址不再有效，上层不能跨打开流程缓存它。
    m_projectDirectoryWatcher.start(actualProjectPath);
    // 监听器启停不作为 m_opened 的独立验收条件；结果表示项目已接管到控制器。
    if ( !temporaryInfo || !temporaryInfo->m_isTemporary ) {
        Config::AppConfig::instance().addRecentProject(
            Config::pathToUtf8(actualProjectPath));
    }

    result.m_opened            = true;
    result.m_actualProjectPath = actualProjectPath;
    result.m_targetBeatmapPath = targetBeatmapPath;
    // 目标路径供上层继续打开谱面；项目打开成功与目标谱面加载成功是两个事件。
    result.m_projectTitle = m_currentProject->m_metadata.m_title;
    result.m_beatmapCount = m_currentProject->m_beatmaps.size();
    // 这是项目入口数量，不是已成功加载到画布的谱面数量。

    return result;
}

/// @brief 解压谱面包并作为临时只读项目打开。
/// @details
/// 准备与打开是两个失败边界：前者失败无可用项目根，后者失败清理已备缓存。
/// 原谱包只作为输入，项目配置写入发生在解压缓存中，不重写源包。
/// @param packagePath 要打开的谱面包路径。
/// @return 打开项目后的结果信息。
ProjectController::OpenProjectResult
ProjectController::openTemporaryProjectPackage(
    const std::filesystem::path& packagePath)
{
    OpenProjectResult result;
    auto              prepared = prepareTemporaryProjectPackage(packagePath);
    // 先取得独立缓存再进入通用打开流程，避免把压缩文件本身当作项目目录扫描。
    if ( !prepared.m_success ) {
        XERROR("Temporary package open failed: {}", prepared.m_errorMessage);
        publishProjectOpenFailed(packagePath, prepared.m_errorMessage, true);
        return result;
    }

    result = openProject(prepared.m_temporaryInfo.m_cacheProjectPath,
                         std::nullopt,
                         prepared.m_temporaryInfo);
    // 准备阶段成功后由本层接管缓存清理责任，项目打开失败不能遗留这次解压目录。
    if ( !result.m_opened ) {
        // 清理只针对本次准备结果，不删除源谱包；删除错误不改变已经失败的打开结果。
        std::error_code filesystemError;
        std::filesystem::remove_all(prepared.m_temporaryInfo.m_cacheProjectPath,
                                    filesystemError);
    }
    return result;
}

/// @brief 仅解压谱面包并准备临时项目目录，不切换当前项目。
/// @param packagePath 要准备的谱面包路径。
/// @return 临时项目准备结果。
/// @details
/// 成功只说明解压完成，不保证内部已有受支持谱面；内容判定留给打开阶段。
/// @note 成功结果交付缓存目录，调用方若不再打开，需负责后续缓存生命周期。
/// 准备结果不包含项目资源列表或编辑会话，不能直接用于替换当前工作区。
/// @warning
/// 同步读取与解压整包，调用方应安排在低频项目准备流程，不用于逐帧更新。
ProjectController::PreparedTemporaryProjectResult
ProjectController::prepareTemporaryProjectPackage(
    const std::filesystem::path& packagePath) const
{
    PreparedTemporaryProjectResult result;
    // 默认失败状态，只有完整解压后才交付可使用的缓存信息。
    std::error_code filesystemError;
    if ( packagePath.empty() ||
         !std::filesystem::is_regular_file(packagePath, filesystemError) ||
         filesystemError ) {
        result.m_errorMessage = "谱面包文件不存在";
        return result;
    }
    if ( !isTemporaryPackagePath(packagePath) ) {
        // 后缀通过只是进入归档读取的条件，不能作为 ZIP 内容有效性的证明。
        result.m_errorMessage = "不支持的谱面包扩展名";
        return result;
    }

    std::string errorMessage;
    auto        tempProjectRoot =
        createTemporaryProjectRoot(packagePath, errorMessage);
    if ( !tempProjectRoot ) {
        // 未取得目录前没有本批解压内容，不扫描或清理共享缓存根。
        result.m_errorMessage = errorMessage;
        return result;
    }

    // 提前准备不替换当前项目；解压途中失败只清理本次创建的缓存根。
    if ( !extractZipPackageToDirectory(
             packagePath, *tempProjectRoot, errorMessage) ) {
        std::filesystem::remove_all(*tempProjectRoot, filesystemError);
        result.m_errorMessage = errorMessage;
        return result;
    }

    result.m_success = true;
    // 来源路径与缓存路径分别返回，提示来源和访问解压内容应使用不同字段。
    result.m_temporaryInfo.m_isTemporary       = true;
    result.m_temporaryInfo.m_sourcePackagePath = packagePath;
    result.m_temporaryInfo.m_cacheProjectPath  = *tempProjectRoot;
    return result;
}

/// @brief 当前是否打开了临时项目。
/// @return 当前项目是临时项目时返回 true。
/// @note 读取项目状态位，不检测文件系统只读权限；编辑入口仍需按业务规则拦截。
bool ProjectController::isCurrentProjectTemporary() const
{
    return m_currentProject && m_currentProject->m_isTemporaryProject;
}

/// @brief 获取当前临时项目信息。
/// @return 当前临时项目源文件与缓存路径；非临时项目时返回默认值。
/// @note 返回值按值复制路径，不延长当前项目生命周期，也不保留临时目录的所有权。
/// 非临时状态返回默认结果，调用方先检查标志再使用缓存路径。
ProjectController::TemporaryProjectInfo
ProjectController::currentTemporaryProjectInfo() const
{
    TemporaryProjectInfo info;
    // 快照反映查询时状态，转为正式项目不会自动更新已经返回的副本。
    if ( !m_currentProject || !m_currentProject->m_isTemporaryProject ) {
        return info;
    }

    info.m_isTemporary       = true;
    info.m_sourcePackagePath = m_currentProject->m_temporarySourcePackagePath;
    info.m_cacheProjectPath  = m_currentProject->m_projectRoot;
    return info;
}

/// @brief 将当前临时项目复制保存到正式目录，并原地转为正式项目。
/// @param destinationPath 用户选择的保存目录。
/// @return 保存结果。
/// @details 非空目标目录会选用新子目录，结果路径可能不同于用户选择的父目录。
/// 复制或配置写入失败时保持当前临时项目不变，但目标上可能已留下部分文件。
/// @note 不重新解压源谱包，而是复制当前缓存，因此保留缓存中已有的项目配置。
/// @warning 完整目录复制和配置写入均为同步操作，只用于显式另存为流程。
/// @pre 项目状态在复制与接管期间稳定，上层负责暂停会改变待保存内容的操作。
ProjectController::SaveTemporaryProjectResult
ProjectController::saveTemporaryProjectTo(
    const std::filesystem::path& destinationPath)
{
    SaveTemporaryProjectResult result;
    // 非临时项目不走这一整目录复制入口，普通保存使用配置保存流程。
    if ( !m_currentProject || !m_currentProject->m_isTemporaryProject ) {
        result.m_errorMessage = "当前没有临时项目";
        return result;
    }

    const auto saveRoot =
        resolveTemporaryProjectSaveRoot(destinationPath, *m_currentProject);
    // 使用解析后的实际目录作为后续所有写入基准，不能再用用户选择的父目录。
    if ( saveRoot.empty() ) {
        // 目标选择失败时尚未复制文件，当前缓存与项目根保持原位置。
        result.m_errorMessage = "保存目录不可用";
        return result;
    }

    std::string errorMessage;
    if ( !copyDirectoryContents(
             m_currentProject->m_projectRoot, saveRoot, errorMessage) ) {
        result.m_errorMessage = errorMessage;
        return result;
    }

    // 先修改副本并写入正式目录，失败时当前项目仍保持临时状态和原缓存根。
    Project savedProject = *m_currentProject;
    // 清除临时标记只作用于候选，失败时不能提前把当前缓存暴露成可编辑正式项目。
    savedProject.m_isTemporaryProject = false;
    savedProject.m_temporarySourcePackagePath.clear();
    // 正式项目不再依赖原谱包位置，之后源谱包移动不应改变已保存项目身份。
    savedProject.m_projectRoot = saveRoot;
    // 配置必须按新根保存，不能复制完成后仍把清单写回临时缓存目录。
    if ( !m_projectStorage.save(savedProject, saveRoot, errorMessage) ||
         !m_projectStorage.removeLegacyProjectFile(saveRoot, errorMessage) ) {
        // 新配置成功但旧格式清理失败也返回失败；目标副本并不因此自动删除。
        result.m_errorMessage = errorMessage;
        return result;
    }

    // 原地更新保留 Project
    // 对象地址；监听根与最近项目记录在持久化成功后一起切换。
    *m_currentProject = savedProject;
    // Project
    // 地址不变不代表其内部容器元素地址不变，观察资源的调用方需要重新取得。
    m_projectDirectoryWatcher.start(saveRoot);
    // 从此监听正式目录，旧缓存的文件变化不应继续驱动当前项目同步。
    Config::AppConfig::instance().addRecentProject(
        Config::pathToUtf8(saveRoot));

    result.m_success          = true;
    result.m_savedProjectPath = saveRoot;
    // 调用方用实际返回目录更新会话路径，而非最初选中的非空父目录。
    // 返回正式根目录，原缓存不在此删除，不能把成功理解成缓存已自动回收。
    return result;
}

/// @brief 关闭当前项目并停止项目目录监听。
/// @return 被关闭项目的信息。
/// @details
/// 返回结果接管项目所有权；控制器不再提供该项目，但调用方仍可读取它完成清理。
/// @note 不隐式保存、不删除临时缓存，相关动作由上层关闭流程决定。
/// 没有当前项目时返回未关闭结果，不重复发布 ProjectClosedEvent。
/// @warning 停止目录监听可能等待后台任务退出，仅用于低频关闭阶段。
ProjectController::CloseProjectResult ProjectController::closeProject()
{
    /// @brief 本次关闭项目的返回结果。
    CloseProjectResult result;
    if ( !m_currentProject ) {
        return result;
    }

    result.m_projectTitle = m_currentProject->m_metadata.m_title;
    /// @brief 被关闭项目的根目录路径快照。
    std::filesystem::path projectPath = m_currentProject->m_projectRoot;
    // 将所有权交给返回结果，后续保存或释放由调用方协调，不在关闭事件前直接销毁。
    result.m_project = std::move(m_currentProject);
    // unique_ptr 转移不拷贝项目资源，返回结果成为后续释放项目的所有者。
    result.m_closed = true;
    // 先从控制器脱离再通知外部，关闭事件处理者查询 currentProject 将得到空值。
    m_projectDirectoryWatcher.stop();

    /// @brief 项目关闭完成后向 UI 和其它监听者发布的生命周期事件。
    Event::ProjectClosedEvent closedEvent;
    closedEvent.m_projectTitle = result.m_projectTitle;
    closedEvent.m_projectPath  = projectPath;
    Event::EventBus::instance().publish(closedEvent);
    // 事件只携带身份信息，项目所有权仍由返回结果持有，不通过总线转移。

    return result;
}

/// @brief 停止项目目录监听。
/// @note 只停止变更来源，不关闭当前项目，也不触发资源列表重建。
/// @warning 用于项目切换和退出等低频路径，不用作每帧状态探测。
void ProjectController::stopDirectoryWatcher()
{
    m_projectDirectoryWatcher.stop();
}

/// @brief 消费项目目录监听器捕获到的变更标记。
/// @return 有待处理目录变更时返回 true。
/// @note 返回聚合标记而非变更文件清单，调用方自行安排低频扫描。
/// 消费只取得本次待处理状态，不代表目录已扫描或资源列表已更新。
bool ProjectController::consumeDirectoryChangePending()
{
    return m_projectDirectoryWatcher.consumeChangePending();
}

/// @brief 扫描当前项目目录并同步项目资源列表。
/// @return 目录同步结果，包含是否改变项目和需要预加载的音效。
/// @details 无项目或扫描失败返回默认结果，不以空扫描覆盖现有资源。
/// @note 成功扫描为空可以移除全部资源，这与无法扫描有意采用不同结果。
/// @warning 涉及文件系统枚举；应由目录变更等低频事件触发，不逐帧执行。
ProjectResourceService::DirectorySyncResult
ProjectController::scanProjectDirectory()
{
    /// @brief 本次目录扫描同步结果。
    ProjectResourceService::DirectorySyncResult result;
    // 默认结果不宣称扫描成功，未扫描不能等同于成功的空目录。
    if ( !m_currentProject ) {
        return result;
    }

    /// @brief 当前项目根目录路径。
    auto actualProjectPath = m_currentProject->m_projectRoot;
    /// @brief 当前项目目录扫描结果。
    ProjectDirectoryScanner::ScanResult directoryScan;

    directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
    // 根目录在扫描前按值保存，不依赖扫描期间复用当前项目的路径引用。
    if ( !directoryScan.m_success ) {
        return result;
    }

    // 只有完整扫描成功才做差量同步，枚举失败不能把暂时不可见的文件当成已删除。
    return m_projectResourceService.syncDirectoryResources(*m_currentProject,
                                                           directoryScan);
}

/// @brief 保存当前项目配置。
/// @return 保存成功时返回 true。
/// @note 保存的是项目配置分片，不代替打开谱面的内容保存，也不重新打包源谱包。
/// 成功事件使用分片清单路径，消费者不应将它视为旧版单文件 JSON。
/// @warning 同步保存与旧格式删除只适用于显式保存或低频自动保存。
/// @note 此入口不拒绝临时项目；保存临时项目时写入缓存，不会转成正式项目。
/// 成功事件只携带配置位置，不携带项目对象所有权或打开谱面的保存状态。
bool ProjectController::saveProject()
{
    if ( !m_currentProject ) return false;
    // 保存必须有项目上下文，不能因没有待写内容就发布成功事件。

    const auto manifest =
        ProjectStorage::manifestPath(m_currentProject->m_projectRoot);
    XINFO("Saving split project storage to {}", Config::pathToUtf8(manifest));

    std::string errorMessage;
    if ( !m_projectStorage.save(*m_currentProject,
                                m_currentProject->m_projectRoot,
                                errorMessage) ) {
        XERROR("Failed to save split project storage: {}", errorMessage);
        return false;
    }
    // 分片保存完成后再删除旧格式；任一步失败都不发布保存成功事件。
    if ( !m_projectStorage.removeLegacyProjectFile(
             m_currentProject->m_projectRoot, errorMessage) ) {
        // 分片已经写入，返回 false 不表示磁盘完全未变化，也不尝试撤销分片保存。
        XERROR("Failed to remove legacy project configuration: {}",
               errorMessage);
        return false;
    }

    XINFO("Project saved successfully.");
    Event::EventBus::instance().publish(Event::ProjectSavedEvent{
        .m_projectFilePath = Config::pathToUtf8(manifest),
    });
    return true;
}

/// @brief 创建谱面文件并登记到当前项目。
/// @param cmd 新建谱面命令。
/// @return 新建谱面的处理结果。
/// @note 控制器仅选择当前项目上下文；创建文件和资源登记由命令服务负责。
/// @pre 上层已检查临时只读及编辑权限；本转发入口只检查当前项目存在性。
ProjectCommandService::CreateBeatmapResult ProjectController::createBeatmap(
    const CmdCreateBeatmap& cmd)
{
    if ( !m_currentProject ) {
        XERROR("Cannot create beatmap: No project opened.");
        return {};
    }
    return m_projectCommandService.createBeatmap(*m_currentProject, cmd);
}

/// @brief 导入音频文件并登记到当前项目。
/// @param cmd 导入音频命令。
/// @return 导入音频的处理结果。
/// @note 返回命令服务的副作用请求供调用方接入音频系统，此处不直接操作播放器。
/// @note 导入返回不自动保存项目配置，列表变更后的持久化由上层协调。
/// @pre 音轨类型与源路径来自已确认命令，导入权限和临时只读限制由上层检查。
ProjectCommandService::ImportAudioResult ProjectController::importAudio(
    const CmdImportAudio& cmd)
{
    if ( !m_currentProject ) {
        XERROR("Cannot import audio: No project opened.");
        return {};
    }
    return m_projectCommandService.importAudio(*m_currentProject, cmd);
}

/// @brief 将单个谱面文件同步到当前项目谱面列表。
/// @param mapPath 需要同步的谱面文件路径。
/// @return 当前项目是否发生变化。
/// @note 不切换项目；没有当前项目时返回默认未变化结果，不按文件路径自动开项目。
/// 变化可能只来自排除项撤销，不必然表示新增了谱面入口。
ProjectCommandService::ProjectMutationResult
ProjectController::syncProjectWithFile(const std::filesystem::path& mapPath)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.syncProjectWithFile(*m_currentProject,
                                                       mapPath);
}

/// @brief 更新当前项目内谱面条目的文件路径关联。
/// @param oldPath 旧谱面路径。
/// @param newPath 新谱面路径。
/// @return 当前项目是否发生变化。
/// @pre 实际文件移动由上层完成，此入口只更新已有项目的路径关联。
/// 结果不代表打开会话的文件路径也已同步，需由会话层继续协调。
ProjectCommandService::ProjectMutationResult
ProjectController::updateBeatmapFilePath(const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.updateBeatmapFilePath(
        *m_currentProject, oldPath, newPath);
}

/// @brief 更新当前项目的音频资源类型。
/// @param cmd 更新音频资源命令。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @return 更新音频资源的处理结果。
/// @note 转交调用方提供的引用快照，不在控制器里遍历会话获取另一份状态。
/// @note 返回的加载或卸载请求仍需调用方执行，类型更新不代表引擎状态已同步。
/// @pre 引用快照应覆盖未保存会话，空快照并不表示磁盘之外没有任何资源依赖。
/// 同类型请求可能仍返回音效登记信息，不能仅凭类型相同忽略处理结果。
ProjectCommandService::UpdateAudioResourceResult
ProjectController::updateAudioResource(
    const CmdUpdateAudioResource&             cmd,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.updateAudioResource(
        *m_currentProject, cmd, openBeatmapReferences);
}

/// @brief 从当前项目中删除音频资源。
/// @param cmd 删除音频资源命令。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @return 删除音频资源的处理结果。
/// @note 内存引用快照与磁盘处理均交给命令服务，控制器不自行判断删除是否可行。
/// @pre 上层负责源文件删除确认，调用方不能将此转发视为只读引用检查。
/// 返回未移除时应继续查看阻止谱面列表及错误信息，不能仅靠一个布尔值选择提示。
/// 成功移除也不代表项目配置已保存，持久化与音频卸载仍是后续步骤。
ProjectCommandService::RemoveAudioResourceResult
ProjectController::removeAudioResource(
    const CmdRemoveAudioResource&             cmd,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.removeAudioResource(
        *m_currentProject, cmd, openBeatmapReferences);
}

/// @brief 从当前项目谱面列表中删除谱面。
/// @note 只移除项目入口，不在这里关闭会话或删除谱面文件。
/// 排除列表也属于项目状态，入口已不存在时仍可能报告配置变化。
/// @param cmd 删除谱面命令。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult ProjectController::removeBeatmap(
    const CmdRemoveBeatmap& cmd)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.removeBeatmap(*m_currentProject, cmd);
}

}  // namespace MMM::Logic
