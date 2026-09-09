#include "logic/BeatmapBackupService.h"

#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Logic
{
namespace
{
/// @brief 自动备份在项目隐藏配置目录中的相对根路径。
/// 与普通资源分开，避免备份谱面被目录扫描器再次导入项目。
constexpr std::string_view BACKUP_ROOT_DIRECTORY = ".mmm/backups";

/// @brief 判断项目相对谱面路径是否不会逃逸出项目目录。
/// @param relativePath 待检查的词法相对路径。
/// @return 路径非空且不包含父目录分量时返回 true。
/// 这是词法检查，符号链接带来的真实路径变化由目录准备阶段另行处理。
/// 调用方传入已规范化或计算得到的相对路径，本函数不查询磁盘。
bool isSafeProjectRelativePath(const std::filesystem::path& relativePath)
{
    if ( relativePath.empty() || relativePath.is_absolute() ) return false;
    // 按分量拒绝父级跳转，不能用字符串前缀推断是否位于项目内。
    for ( const auto& part : relativePath ) {
        if ( part == ".." ) return false;
    }
    return true;
}

/// @brief 为项目外谱面路径生成稳定的短哈希。
/// @param path 谱面路径。
/// @return FNV-1a 64 位哈希。
/// 用于目录命名而非安全认证，不依赖 std::hash 的实现或运行期随机化。
/// 路径内容改变会改变备份命名空间，文件改名后的历史不会自动迁移。
/// 不统一路径大小写，哈希输入遵循调用方提供的规范化路径文本。
std::uint64_t stablePathHash(const std::filesystem::path& path)
{
    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;
    // 使用同一固定初值和乘数，应用重启后仍能定位同一外部谱面的目录。
    std::uint64_t hash = FNV_OFFSET;
    const auto    text = Config::pathToUtf8(path.lexically_normal());
    // 规范化词法路径后按 UTF-8 字节混合，不需要为外部文件执行真实路径查询。
    for ( const unsigned char value : text ) {
        hash ^= static_cast<std::uint64_t>(value);
        // 无符号乘法按模回绕，保留 FNV-1a 固定宽度状态。
        hash *= FNV_PRIME;
    }
    return hash;
}

/// @brief 判断文件名是否属于服务生成的正式备份。
/// @param path 待检查文件路径。
/// @return 文件名严格匹配固定宽度的自动备份格式时返回 true。
/// 手工文件和未提交的 .pending 文件不参与自动保留数量计算。
/// 此检查只识别文件名，文件类型由轮转枚举处单独确认。
bool isManagedBackupFile(const std::filesystem::path& path)
{
    const auto            fileName        = Config::pathToUtf8(path.filename());
    constexpr std::size_t EXPECTED_LENGTH = 37U;
    // 固定分隔符位置使字典序与时间戳、序号的数值顺序一致。
    if ( fileName.size() != EXPECTED_LENGTH ||
         // 长度判断必须在按固定位置取字符之前，短路防止越界读取。
         !fileName.starts_with("backup-") || fileName[26] != '-' ||
         !fileName.ends_with(".mmm") ) {
        return false;
    }
    const auto allDigits = [&fileName](std::size_t begin, std::size_t end) {
        // 长度已验证，后续区间索引仅覆盖时间戳与序号，不触及扩展名。
        return std::all_of(
            fileName.begin() + static_cast<std::ptrdiff_t>(begin),
            fileName.begin() + static_cast<std::ptrdiff_t>(end),
            [](char value) { return value >= '0' && value <= '9'; });
    };
    // 两段都要求十进制数字，外观近似的用户文件不能进入删除候选列表。
    return allDigits(7U, 26U) && allDigits(27U, 33U);
}

/// @brief 不跟随符号链接读取路径状态，并把路径不存在视为正常状态。
/// @param path 待检查路径。
/// @param status 接收文件状态。
/// @param filesystemError 接收真实文件系统错误。
/// @return 成功读取或确认路径不存在时返回 true。
/// 调用者可区分缺失、普通文件和符号链接，不把不可访问误当成空闲文件名。
/// @warning 同步元数据查询，使用 error_code 返回系统错误，不负责写日志。
bool readSymlinkStatus(const std::filesystem::path&  path,
                       std::filesystem::file_status& status,
                       std::error_code&              filesystemError)
{
    filesystemError.clear();
    status = std::filesystem::symlink_status(path, filesystemError);
    // 查询链接本身，悬空链接也必须视为占用或非法目录，而不是跟随其目标。
    if ( filesystemError && filesystemError.default_error_condition() ==
                                std::errc::no_such_file_or_directory ) {
        filesystemError.clear();
        // 只归一化“确实不存在”，权限等其他错误仍原样返回给调用方。
        status = std::filesystem::file_status{
            std::filesystem::file_type::not_found
        };
    }
    return !filesystemError;
}

/// @brief 创建备份目录并拒绝任何符号链接目录分量。
/// @param projectRoot 当前项目根目录。
/// @param directory 需要创建并验证的单谱面备份目录。
/// @param errorMessage 接收失败原因。
/// @return 目录位于项目真实路径内且所有分量均为普通目录时返回 true。
/// @warning 低频同步目录创建；失败时不会回滚此前已创建的普通目录。
/// 项目根与候选目录必须使用同一基准，不能混用相对路径和另一工作目录的绝对路径。
/// 验证与后续写入是分开的文件系统操作，调用方仍须避免并发替换目录结构。
bool prepareBackupDirectory(const std::filesystem::path& projectRoot,
                            const std::filesystem::path& directory,
                            std::string&                 errorMessage)
{
    std::error_code filesystemError;
    const auto      canonicalRoot =
        // 项目根可由用户通过链接打开，后续真实路径比较以解析后的根为基准。
        std::filesystem::weakly_canonical(projectRoot, filesystemError);
    if ( filesystemError ) {
        errorMessage = "无法解析项目真实路径：" + filesystemError.message();
        // 无法确定可信根时终止准备，不把词法目录检查当作真实路径确认。
        return false;
    }
    const auto relativeDirectory =
        directory.lexically_normal().lexically_relative(
            projectRoot.lexically_normal());
    // 先检查词法范围，再逐段建目录；不能先创建后才发现路径含父级跳转。
    if ( !isSafeProjectRelativePath(relativeDirectory) ) {
        // 目录命名函数的候选仍需要在 IO 边界验证，不能直接用于逐段创建。
        errorMessage = "谱面备份目录不在项目目录内";
        return false;
    }

    auto current = projectRoot.lexically_normal();
    // 每次只追加一个已检查的分量，使父目录先于子目录准备完成。
    for ( const auto& part : relativeDirectory ) {
        current /= part;
        std::filesystem::file_status status;
        if ( !readSymlinkStatus(current, status, filesystemError) ) {
            // 报告具体出错分量，使目录权限问题不会被笼统归为谱面序列化失败。
            errorMessage = "无法检查谱面备份目录：" +
                           Config::pathToUtf8(current) + "（" +
                           filesystemError.message() + "）";
            return false;
        }
        if ( std::filesystem::is_symlink(status) ) {
            // 即使链接目标在项目内部也拒绝，避免备份子树由链接重新定向。
            errorMessage =
                "谱面备份目录包含符号链接：" + Config::pathToUtf8(current);
            return false;
        }
        if ( std::filesystem::exists(status) ) {
            // 已有同名普通文件不能被当作目录覆盖，保留它并报告失败。
            if ( !std::filesystem::is_directory(status) ) {
                errorMessage =
                    "谱面备份路径不是目录：" + Config::pathToUtf8(current);
                return false;
            }
            continue;
        }

        // 缺失分量之外的已有父目录均已验证；此处不调用递归创建跳过中间检查。
        filesystemError.clear();
        // 只创建当前缺失的一层，后面的分量在下一轮分别验证。
        if ( !std::filesystem::create_directory(current, filesystemError) ||
             filesystemError ) {
            errorMessage = "无法创建谱面备份目录：" +
                           Config::pathToUtf8(current) + "（" +
                           filesystemError.message() + "）";
            return false;
        }
        if ( !readSymlinkStatus(current, status, filesystemError) ||
             // 创建后重新读取实际类型，不仅依赖 create_directory 的返回值。
             std::filesystem::is_symlink(status) ||
             !std::filesystem::is_directory(status) ) {
            errorMessage =
                "谱面备份目录创建后验证失败：" + Config::pathToUtf8(current);
            return false;
        }
    }

    filesystemError.clear();
    const auto canonicalDirectory =
        std::filesystem::weakly_canonical(directory, filesystemError);
    // 最后比较真实路径，补足词法前缀检查无法确认的目录解析结果。
    if ( filesystemError ||
         !isSafeProjectRelativePath(
             canonicalDirectory.lexically_relative(canonicalRoot)) ) {
        // 真实路径不能回到项目根之外，即使词法上以 .mmm/backups 开头也不例外。
        errorMessage = "谱面备份目录的真实路径逃逸出项目目录";
        return false;
    }
    return true;
}

/// @brief 读取目录中最新的单调备份时间戳。
/// @param directory 单谱面备份目录。
/// @param latestMilliseconds 接收文件名中的最大毫秒值；无备份时保持 -1。
/// @param errorMessage 接收目录枚举失败信息。
/// @return 完成枚举时返回 true。
/// 以服务命名中的毫秒字段排序，不使用可能被复制工具改变的文件修改时间。
/// 序列只在当前源谱面的目录内比较，不要求所有项目备份共用全局时间顺序。
/// @warning 同步目录枚举，只在创建一个新备份前执行，不持续轮询备份目录。
bool findLatestBackupMilliseconds(const std::filesystem::path& directory,
                                  std::int64_t& latestMilliseconds,
                                  std::string&  errorMessage)
{
    std::error_code                     filesystemError;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    const std::filesystem::directory_iterator end;
    if ( filesystemError ) {
        errorMessage = "无法枚举谱面备份目录：" + filesystemError.message();
        // 构造迭代器失败也不视为空目录，否则可能生成落后于历史的时间戳。
        return false;
    }

    constexpr std::size_t TIMESTAMP_OFFSET = 7U;
    constexpr std::size_t TIMESTAMP_LENGTH = 19U;
    // 只枚举当前谱面的独立目录，不递归读取其他谱面的备份历史。
    while ( iterator != end ) {
        if ( isManagedBackupFile(iterator->path()) ) {
            // 先验证服务格式，再提取固定偏移，跳过临时文件和人工附加文件。
            const auto fileName =
                Config::pathToUtf8(iterator->path().filename());
            if ( fileName.size() >= TIMESTAMP_OFFSET + TIMESTAMP_LENGTH ) {
                std::int64_t value  = -1;
                const char*  begin  = fileName.data() + TIMESTAMP_OFFSET;
                const char*  endPtr = begin + TIMESTAMP_LENGTH;
                const auto   parsed = std::from_chars(begin, endPtr, value);
                // 数字文本可能超过 int64_t 可表示范围，转换成功后才参与最大值。
                if ( parsed.ec == std::errc{} && parsed.ptr == endPtr ) {
                    // 只提高最大值，遍历顺序不影响最终结果。
                    latestMilliseconds = std::max(latestMilliseconds, value);
                }
            }
        }
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            // 不用不完整枚举得到的最大值继续命名，避免时间序列错误回退。
            errorMessage =
                "无法完成谱面备份目录枚举：" + filesystemError.message();
            return false;
        }
    }
    return true;
}

/// @brief 删除超出保留数量的最旧备份。
/// @param directory 单个谱面的独立备份目录。
/// @param maxBackupCount 允许保留的备份数量。
/// @param removedCount 接收成功删除数量。
/// @param errorMessage 接收轮转失败信息。
/// @return 完整轮转成功时返回 true。
/// 新备份已提交后才调用此函数，删除失败不会影响已写入的恢复点。
/// @warning 同步枚举、排序和删除；只允许在低频备份完成路径执行。
/// @pre directory 已通过准备阶段验证，调用方不并发运行同一谱面的备份轮转。
bool rotateBackups(const std::filesystem::path& directory, int maxBackupCount,
                   std::size_t& removedCount, std::string& errorMessage)
{
    std::vector<std::filesystem::path> backups;
    // 先收集完整候选再删除，避免边遍历边改变目录而漏掉待保留的条目。
    std::error_code                     filesystemError;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystemError);
    const std::filesystem::directory_iterator end;
    if ( filesystemError ) {
        errorMessage = "无法枚举谱面备份目录：" + filesystemError.message();
        return false;
    }

    while ( iterator != end ) {
        std::error_code itemError;
        // 只有常规文件且名称匹配服务格式的条目才计入轮转，目录不作递归删除。
        if ( iterator->is_regular_file(itemError) && !itemError &&
             isManagedBackupFile(iterator->path()) ) {
            backups.push_back(iterator->path());
            // 保存路径副本，后续关闭目录迭代器后仍可按排序结果执行删除。
        }
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            errorMessage =
                "无法完成谱面备份目录枚举：" + filesystemError.message();
            // 未收齐候选就退出，不能凭部分列表判断最旧备份或保留数量。
            return false;
        }
    }

    std::sort(
        // 时间戳和后缀都补零为固定宽度，文件名字典序就是保留先后顺序。
        backups.begin(),
        backups.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.filename() < rhs.filename();
        });
    const auto safeCount =
        // 存储服务也钳制保留数量，不能完全依赖界面输入已经限制范围。
        static_cast<std::size_t>(std::clamp(maxBackupCount,
                                            Config::AUTO_BACKUP_COUNT_MIN,
                                            Config::AUTO_BACKUP_COUNT_MAX));
    const std::size_t removeCount =
        // 先比较再做无符号减法，小于保留数量时不产生下溢的删除次数。
        backups.size() > safeCount ? backups.size() - safeCount : 0U;
    for ( std::size_t index = 0; index < removeCount; ++index ) {
        // 从排序后的最旧条目开始删除，最新写入的备份优先保留。
        filesystemError.clear();
        if ( !std::filesystem::remove(backups[index], filesystemError) ||
             filesystemError ) {
            // 遇到首个失败停止轮转，返回已成功删除的数量而不是计划删除数。
            errorMessage =
                "无法删除最旧谱面备份：" + Config::pathToUtf8(backups[index]);
            if ( filesystemError ) {
                errorMessage += "（" + filesystemError.message() + "）";
            }
            return false;
        }
        ++removedCount;
        // 只统计实际成功的删除操作，失败时上层仍能显示已清理数量。
    }
    return true;
}
}  // namespace

/// @brief 计算按源谱面隔离的备份子目录。
/// @param projectRoot 当前项目根，用作相对谱面路径的解析基准。
/// @param sourceBeatmapPath 源谱面路径，可以是项目相对路径或绝对路径。
/// @return 项目隐藏备份根下的候选目录，不访问文件系统。
/// 项目内保留相对路径层级；项目外路径仅作为稳定命名输入，不决定磁盘父目录。
std::filesystem::path BeatmapBackupService::backupDirectory(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceBeatmapPath)
{
    const auto normalizedRoot = projectRoot.lexically_normal();
    // 相对源路径基于项目根而不是进程工作目录，避免从不同启动目录得到不同位置。
    const auto sourceAbsolute =
        sourceBeatmapPath.is_absolute()
            ? sourceBeatmapPath.lexically_normal()
            : (normalizedRoot / sourceBeatmapPath).lexically_normal();
    const auto relativePath = sourceAbsolute.lexically_relative(normalizedRoot);
    // 不用路径字符串截断构造相对位置，避免前缀相似的相邻目录被混为项目内部。

    auto directory = normalizedRoot / BACKUP_ROOT_DIRECTORY;
    // 根谱面文件名本身成为一个目录名，备份文件放在其下而非覆盖同名源文件。
    if ( isSafeProjectRelativePath(relativePath) ) {
        // 保留完整相对路径与扩展名，同名但不同目录或格式的谱面各自隔离。
        directory /= relativePath;
        return directory;
    }

    auto fallbackName = sourceBeatmapPath.filename();
    // 项目外路径不拼接其父目录，使用文件名加稳定哈希收纳到项目内。
    if ( fallbackName.empty() ) fallbackName = "unnamed-beatmap";
    // 无文件名时仍给候选目录一个可读前缀，哈希部分继续参与外部路径隔离。
    directory /= fallbackName;
    directory +=
        // 后缀区分不同外部目录下的同名谱面，不把它们的保留数量混在一起。
        fmt::format("-external-{:016x}", stablePathHash(sourceAbsolute));
    return directory;
}

/// @brief 使用当前系统时间进入公共备份实现。
/// @param beatmap 已同步的领域快照，函数不会回写源谱面。
/// @param projectRoot 当前项目目录，备份全部存放在其隐藏子目录内。
/// @param sourceBeatmapPath 用于选择备份命名空间的源文件路径。
/// @param maxBackupCount 每个源谱面独立使用的保留数量。
/// @return 新恢复点的提交结果及轮转诊断。
/// @warning 自动备份或用户命令的低频 IO 路径，不能逐帧调用。
/// 与显式时间入口共用实现，实际调度时间和测试注入时间遵循相同的命名规则。
BeatmapBackupResult BeatmapBackupService::createBackup(
    const BeatMap& beatmap, const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceBeatmapPath, int maxBackupCount)
{
    return createBackupAt(beatmap,
                          projectRoot,
                          sourceBeatmapPath,
                          maxBackupCount,
                          std::chrono::system_clock::now());
}

/// @brief 按指定时间提交备份，再轮转该谱面的历史恢复点。
/// @param beatmap 稳定的谱面快照，调用方应避免保存期间并发修改。
/// @param projectRoot 非空项目根目录。
/// @param sourceBeatmapPath 非空源谱面路径，只参与目录计算，不覆盖原文件。
/// @param maxBackupCount 保留数量，轮转时按配置允许范围钳制。
/// @param timestamp 文件命名的参考时间，必要时提高到历史最新值之后。
/// @return m_success 表示新备份已提交，轮转错误可与成功状态同时存在。
/// @warning 目录查询、序列化与文件轮转均为同步操作，应由低频入口串行调用。
/// 此服务不选择备份间隔，也不判断谱面是否脏；触发条件由外层调度维护。
/// 输出是谱面恢复点，不是项目资源包，音频与图片等外部资源不在这里打包。
BeatmapBackupResult BeatmapBackupService::createBackupAt(
    const BeatMap& beatmap, const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourceBeatmapPath, int maxBackupCount,
    std::chrono::system_clock::time_point timestamp)
{
    BeatmapBackupResult result;
    // 默认失败且路径为空，任意提交前错误都可直接返回尚未完成的结果。
    if ( projectRoot.empty() || sourceBeatmapPath.empty() ) {
        // 不以当前工作目录替代缺失路径，避免把未绑定谱面写入错误项目。
        result.m_errorMessage = "项目路径或谱面路径为空，无法创建自动备份";
        return result;
    }

    const auto directory = backupDirectory(projectRoot, sourceBeatmapPath);
    // 路径命名与真实目录检查分开；通过检查之前不创建临时备份文件。
    if ( !prepareBackupDirectory(
             projectRoot, directory, result.m_errorMessage) ) {
        return result;
    }

    // 写入路径准备好之后再读历史，目录首次创建的空状态也走同一枚举流程。
    std::int64_t latestMilliseconds = -1;
    // 读取已有序列，使系统时钟回拨或同一毫秒触发多次也不打乱轮转次序。
    if ( !findLatestBackupMilliseconds(
             directory, latestMilliseconds, result.m_errorMessage) ) {
        return result;
    }
    // 毫秒字段来自系统时钟，但历史最大值才决定最低可用命名时间。
    auto milliseconds = std::max<std::int64_t>(
        // 负纪元时间归零，生成的正式备份名称始终只有十进制数字。
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch())
            .count());
    if ( milliseconds <= latestMilliseconds ) {
        // 文件名时间代表备份顺序而非精确墙钟，至少比最新历史值大一。
        if ( latestMilliseconds == std::numeric_limits<std::int64_t>::max() ) {
            // 先检测边界再加一，不能让有符号溢出破坏排序或产生负文件名。
            result.m_errorMessage = "谱面备份时间戳已达到上限";
            return result;
        }
        milliseconds = latestMilliseconds + 1;
        // 补偿只影响本次文件名，不调整系统时钟，也不修改已有备份名称。
    }

    std::error_code       filesystemError;
    std::filesystem::path backupPath;
    std::filesystem::path pendingPath;
    // 候选检查不是文件名锁；同目录写入仍应串行，而非依赖后缀解决并发覆盖。
    // 同一次备份只选择一个正式文件与对应临时文件，后续阶段不再重新选名。
    for ( std::uint32_t suffix = 0; suffix < 1'000'000U; ++suffix ) {
        // 后缀空间与六位格式相同，避免超宽数字破坏受管文件名识别规则。
        const auto fileName =
            fmt::format("backup-{:019d}-{:06d}.mmm", milliseconds, suffix);
        auto                         candidate = directory / fileName;
        std::filesystem::file_status candidateStatus;
        if ( !readSymlinkStatus(candidate, candidateStatus, filesystemError) ) {
            // 查询失败不能当作名字可用；否则可能覆盖不可访问的现存条目。
            result.m_errorMessage =
                "无法检查谱面备份文件：" + filesystemError.message();
            return result;
        }
        if ( std::filesystem::exists(candidateStatus) ) continue;
        // 已存在的文件、目录或链接都不覆盖，继续寻找空闲候选名。

        backupPath = std::move(candidate);
        // 正式候选为空闲后才确定 pending 名称，已占用正式路径不会被清理。
        pendingPath = directory / (".pending-" + fileName);
        // 临时文件与正式文件同目录，提交时可用 rename；轮转不识别该前缀。
        break;
    }
    if ( backupPath.empty() ) {
        // 所有后缀都已占用时明确失败，不回绕后缀或删除文件腾出命名空间。
        result.m_errorMessage = "同一时间戳下的谱面备份数量过多";
        return result;
    }

    filesystemError.clear();
    std::filesystem::remove(pendingPath, filesystemError);
    // remove 的正常“不存在”不算错误，首次保存无需额外创建空占位文件。
    // 先清理上一次中断留下的同名临时条目，再把完整快照写入新的临时文件。
    if ( filesystemError ) {
        result.m_errorMessage =
            "无法清理谱面备份临时文件：" + filesystemError.message();
        // 临时路径无法清理时不尝试继续写入，避免沿用不可控的旧内容。
        return result;
    }
    if ( !beatmap.saveToFile(pendingPath) ) {
        // 原生 MMM 保存保留领域快照内容，不按源文件的扩展名执行有损格式导出。
        // 写出失败只清临时结果，已有正式恢复点和源谱面保持不变。
        std::error_code removeError;
        std::filesystem::remove(pendingPath, removeError);
        // 清理失败不覆盖最初的写入错误，正式历史仍不进入轮转。
        result.m_errorMessage = "无法写入谱面自动备份";
        return result;
    }

    filesystemError.clear();
    std::filesystem::rename(pendingPath, backupPath, filesystemError);
    // 只有提交成功才对外返回正式路径，未提交的候选不算作一次成功备份。
    if ( filesystemError ) {
        // 提交失败不执行轮转，避免新恢复点尚未建立就删去可用历史。
        // 保留 rename 的错误码，删除临时文件使用单独的 error_code。
        std::error_code removeError;
        std::filesystem::remove(pendingPath, removeError);
        result.m_errorMessage =
            "无法提交谱面自动备份：" + filesystemError.message();
        return result;
    }

    result.m_success = true;
    // 先记录成功写入，再执行保留策略；轮转不是新备份有效性的前置条件。
    result.m_backupPath = backupPath;
    // 返回实际选定的名称，不能由调用方仅凭输入时间重新推导备份路径。
    // 错误字符串在成功写入时仍为空，后续非空值只表示轮转未完全完成。
    static_cast<void>(rotateBackups(
        directory,
        // 删除计数与错误直接累积到结果，调用方可提示部分轮转失败。
        maxBackupCount,
        result.m_removedBackupCount,
        result.m_errorMessage));
    // 调用者依据成功标志确认恢复点可用，依据删除数量与诊断展示维护结果。
    return result;
}

}  // namespace MMM::Logic
