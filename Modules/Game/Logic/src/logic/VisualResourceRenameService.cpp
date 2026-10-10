#include "logic/VisualResourceRenameService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/Metadata.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMM::Logic
{
namespace
{

/// @brief 一张谱面在视觉资源改名事务中的暂存与备份路径。
struct PendingBeatmapReplacement {
    /// @brief 正式谱面文件路径。
    std::filesystem::path m_path;

    /// @brief 已完整序列化的新谱面路径。
    std::filesystem::path m_stagedPath;

    /// @brief 仅在提交期间使用的原谱面备份路径。
    std::filesystem::path m_backupPath;

    /// @brief 原文件是否已转移到备份位置。
    bool m_backupCreated{ false };
};

/// @brief 判断文件名是否是跨平台安全的单个普通路径分量。
/// @param name 用户提交的 UTF-8 文件名。
/// @return 不含目录、控制字符和 Windows 保留字符时返回 true。
/// @note 改名只改变源文件的最后一个路径分量，目录移动由其他入口负责。
/// @note 末尾空格和点在 Windows 上可能折叠，拒绝它们以保持目标唯一性。
bool validFileName(std::string_view name)
{
    if ( name.empty() || name == "." || name == ".." || name.back() == '.' ||
         name.back() == ' ' ) {
        return false;
    }
    // Windows
    // 也可能打开同一项目，因此拒绝其保留字符，不能只检查当前平台分隔符。
    return std::none_of(name.begin(), name.end(), [](unsigned char character) {
        return character < 32 || std::string_view("/\\:*?\"<>|")
                                         .find(static_cast<char>(character)) !=
                                     std::string_view::npos;
    });
}

/// @brief 获取大小写折叠后的 ASCII 扩展名。
/// @param path 文件路径。
/// @return 用于格式白名单比较的小写 UTF-8 扩展名。
std::string lowerExtension(const std::filesystem::path& path)
{
    auto extension = Config::pathToUtf8(path.extension());
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension;
}

/// @brief 判断扩展名是否是谱面设置可选的图片或视频格式。
/// @param extension 已折叠为小写的扩展名。
/// @return 当前 UI 资源列表可展示该扩展名时返回 true。
bool supportedVisualExtension(std::string_view extension)
{
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".bmp" || extension == ".mp4" || extension == ".avi" ||
           extension == ".mkv" || extension == ".webm" || extension == ".mov" ||
           extension == ".flv" || extension == ".m4v";
}

/// @brief 判断规范化路径是否位于项目根目录内部。
/// @param root 已解析的项目根目录。
/// @param path 已解析的资源或谱面路径。
/// @return path 为 root 的非空后代时返回 true。
/// @note 调用方先解析已有符号链接，链接指向项目外时也会被拒绝。
/// @note 项目根目录本身不能充当资源路径，故要求非空后代。
bool isProjectChild(const std::filesystem::path& root,
                    const std::filesystem::path& path)
{
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

/// @brief 剥离旧项目元数据误写入的首段项目目录名称。
/// @param root 已规范化项目根路径。
/// @param path 项目相对资源路径。
/// @return 首段恰为项目目录名称时返回剩余路径，否则为空。
/// @note 只剥离完整路径分量，不按字符串前缀截断相近目录名。
std::filesystem::path stripProjectFolderPrefix(
    const std::filesystem::path& root, const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) return {};
    auto it = path.begin();
    if ( it == path.end() || *it != root.filename() ) return {};
    std::filesystem::path stripped;
    for ( ++it; it != path.end(); ++it ) stripped /= *it;
    return stripped.lexically_normal();
}

/// @brief 清理本次生成的尚未提交谱面旁路文件。
/// @param replacements 仅包含本次操作创建的暂存路径。
/// @note 预先存在的同名旁路在预检时被拒绝，不会进入此列表。
/// @note 清理失败只记日志，主错误原因仍由调用方保留。
void removeStagedFiles(
    const std::vector<PendingBeatmapReplacement>& replacements)
{
    for ( const auto& replacement : replacements ) {
        std::error_code error;
        std::filesystem::remove(replacement.m_stagedPath, error);
        if ( error ) {
            XWARN("Cannot remove visual rename staging file {}: {}",
                  Config::pathToUtf8(replacement.m_stagedPath),
                  error.message());
        }
    }
}

/// @brief 提交失败时恢复谱面文件和被改名的视觉资源。
/// @param replacements 可能已部分提交的谱面列表。
/// @param oldPath 视觉资源原路径。
/// @param newPath 视觉资源目标路径。
/// @return 回滚期间发生的错误描述；为空表示恢复完整。
/// @details
/// 提交前的原谱面先移动到备份路径，恢复时必须逆序处理已提交项。
/// 新谱面只有在备份存在时才可删除，以免误删唯一可恢复的内容。
/// 文件系统在恢复阶段再次失败时，备份保留并在结果中附上具体路径。
/// 最后恢复资源文件，使失败返回时谱面引用和物理文件尽可能重新一致。
std::string rollback(std::vector<PendingBeatmapReplacement>& replacements,
                     const std::filesystem::path&            oldPath,
                     const std::filesystem::path&            newPath)
{
    std::string rollbackError;
    for ( auto it = replacements.rbegin(); it != replacements.rend(); ++it ) {
        if ( !it->m_backupCreated ) continue;
        std::error_code error;
        // 新谱面可能已占用正式路径，先删除本次提交文件再恢复原始字节。
        std::filesystem::remove(it->m_path, error);
        if ( !error ) {
            std::filesystem::rename(it->m_backupPath, it->m_path, error);
        }
        if ( error ) {
            rollbackError += "；谱面 '" + Config::pathToUtf8(it->m_path) +
                             "' 恢复失败：" + error.message();
        }
    }
    std::error_code error;
    std::filesystem::rename(newPath, oldPath, error);
    if ( error ) {
        rollbackError += "；视觉资源文件恢复失败：" + error.message();
    }
    removeStagedFiles(replacements);
    return rollbackError;
}

}  // namespace

/// @brief 将同一文件的封面和背景引用更新为新路径。
/// @details 优先按项目根目录解析现行路径，兼容历史谱面目录相对引用；匹配后
/// 保持原路径的绝对或相对形式。仅改路径，不改 cover_type 等背景语义。
/// @param metadata 待投影的基础谱面元数据。
/// @param projectRoot 资源所属项目的规范化根路径。
/// @param oldPath 改名前的规范化绝对路径。
/// @param newPath 改名后的规范化绝对路径。
/// @return 封面或背景至少一个引用变化时返回 true。
/// @note 同一文件可以同时充当封面与背景，两个字段分别检查。
/// @note 空封面字段保持为空，不因背景改名替用户选择封面。
/// @note map_path 可以是项目相对路径，兼容分支需先补项目根路径。
/// @note 返回值仅表示字段值变更，不代表媒体解码或背景尺寸探测成功。
/// @note 两种路径写法可能同时映射到同一资源，优先保留项目相对习惯。
bool VisualResourceRenameService::remapMetadata(
    BaseMapMeta& metadata, const std::filesystem::path& projectRoot,
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    bool       changed  = false;
    const auto remapOne = [&](std::filesystem::path& storedPath) {
        if ( storedPath.empty() ) return;
        // 先投影成绝对路径再判断身份，避免同名的其他项目文件被误改。
        const auto projectCandidate =
            (storedPath.is_absolute() ? storedPath : projectRoot / storedPath)
                .lexically_normal();
        const auto stripped = stripProjectFolderPrefix(projectRoot, storedPath);
        const auto legacyCandidate =
            stripped.empty() ? std::filesystem::path{}
                             : (projectRoot / stripped).lexically_normal();
        const auto mapDirectory =
            metadata.map_path.is_absolute()
                ? metadata.map_path.parent_path()
                : projectRoot / metadata.map_path.parent_path();
        const auto mapCandidate =
            (mapDirectory / storedPath).lexically_normal();
        // 项目相对写法是当前规范；旧谱面目录相对写法仅在前者不匹配时采用。
        // 不能只比较文件名：项目内不同目录可合法拥有同名图片。
        if ( projectCandidate == oldPath ) {
            // 原来是绝对路径就保持绝对形式，项目相对路径仍相对根目录。
            storedPath = storedPath.is_absolute()
                             ? newPath
                             : newPath.lexically_relative(projectRoot);
        } else if ( !legacyCandidate.empty() && legacyCandidate == oldPath ) {
            // 旧项目路径多出工程文件夹名时，提交统一项目相对路径。
            storedPath = newPath.lexically_relative(projectRoot);
        } else if ( !storedPath.is_absolute() && mapCandidate == oldPath ) {
            // 历史相对谱面目录的写法保留同一基准，以免格式重新加载时漂移。
            storedPath = newPath.lexically_relative(mapDirectory);
        } else {
            return;
        }
        changed = true;
    };
    remapOne(metadata.cover_path);
    remapOne(metadata.main_cover_path);
    return changed;
}

/// @brief 物理改名并事务写回项目谱面中的视觉资源路径。
/// @details 全部谱面先写到独立旁路，提交时每张保留原始字节备份。中途失败则
/// 逆序恢复已替换谱面，再恢复资源文件；打开会话由调用方在成功后原地同步。
/// @param project 当前正式项目，资源列表和谱面入口只读。
/// @param oldPath 项目相对路径或绝对路径，必须解析到项目内普通文件。
/// @param newFileName 新的单个文件名；无扩展名时复用原文件扩展名。
/// @return 成功时包含新旧路径且错误为空，失败时包含可展示的原因。
/// @warning 显式低频操作，调用方必须阻止并行后台谱面保存。
/// @note 谱面以磁盘版本为序列化来源；会话中未保存编辑由调用方保持。
/// @note IMD 背景路径由谱面名称隐式决定，不能保存任意改名目标。
/// @note 仅引用目标资源的谱面被序列化，无关谱面不被重写。
/// @note 资源与谱面均在同一项目根目录下，目标只改变文件名。
/// @note 同目录改名通常是原子元数据操作，多张谱面提交仍需逐项备份。
/// @note 谱面暂存使用原格式写出器，外部格式可能规范化原有文本表示。
/// @note 失败返回中的回滚诊断若非空，用户应保留旁路备份供人工恢复。
/// @note 改名成功后只返回路径；项目会话、撤销栈和尺寸缓存由调用方同步。
/// @note 不修改音频资源表，图片与视频由项目目录及谱面元数据直接引用。
/// @note 同一文件被多张谱面引用时，所有可持久化引用共同提交或共同恢复。
/// @note 已存在的目标或旁路均属于用户数据，本操作从不覆盖它们。
/// @note 临时只读项目即使缓存目录可写，也不得改动其解压内容。
VisualResourceRenameResult VisualResourceRenameService::rename(
    const Project& project, const std::filesystem::path& oldPath,
    const std::string& newFileName)
{
    VisualResourceRenameResult result;
    if ( project.m_isTemporaryProject ) {
        // 临时谱面包的缓存不代表可持久化项目，禁止在缓存里直接改资源名。
        result.m_errorMessage = "临时只读项目不能重命名视觉资源";
        return result;
    }
    if ( !validFileName(newFileName) ) {
        // 禁止 ../ 和平台保留字符，在接触文件系统前固定本次事务的目标目录。
        result.m_errorMessage = "文件名不能为空、包含目录或平台保留字符";
        return result;
    }
    std::error_code error;
    // 根路径先做符号链接归一化，下面的源路径包含关系才有同一比较基准。
    // 此处不创建目录；项目根不存在时资源操作必须作为普通失败返回。
    const auto root =
        std::filesystem::weakly_canonical(project.m_projectRoot, error);
    if ( error || root.empty() ) {
        // 不依赖当前工作目录兜底，以免把相对路径误解释为仓库文件。
        result.m_errorMessage = "项目目录不可访问";
        return result;
    }
    auto inputPath = oldPath.is_absolute() ? oldPath : root / oldPath;
    if ( oldPath.is_relative() && !std::filesystem::exists(inputPath, error) &&
         !error ) {
        const auto stripped = stripProjectFolderPrefix(root, oldPath);
        if ( !stripped.empty() ) inputPath = root / stripped;
    }
    // 根目录本身可以通过符号链接打开；先投影到规范化根目录，随后
    // 对资源路径本身的链接保持拒绝，避免改名目标文件而留下悬空链接。
    if ( oldPath.is_absolute() ) {
        const auto relative = oldPath.lexically_relative(project.m_projectRoot);
        if ( !relative.empty() && *relative.begin() != ".." ) {
            inputPath = root / relative;
        }
    }
    const auto source = std::filesystem::weakly_canonical(inputPath, error);
    if ( !error && source != inputPath.lexically_normal() ) {
        result.m_errorMessage = "符号链接视觉资源不能直接重命名";
        return result;
    }
    // 路径可以由 UI 传绝对值或项目相对值，但不能借助符号链接越出项目。
    // 普通文件检查还排除目录、设备文件和断开的链接。
    if ( error || !isProjectChild(root, source) ||
         !std::filesystem::is_regular_file(source, error) || error ) {
        // 无论源路径由外部拖拽还是项目列表传入，都必须满足相同边界。
        result.m_errorMessage = "源文件不存在或不属于当前项目";
        return result;
    }
    if ( !supportedVisualExtension(lowerExtension(source)) ) {
        // 白名单与谱面设置的封面及图片/视频背景候选一致。
        // 只检查扩展名，不尝试在改名时转码或解析媒体内容。
        result.m_errorMessage = "只支持重命名谱面设置中的图片和视频资源";
        return result;
    }
    auto requestedName = Config::utf8ToPath(newFileName);
    // 允许只输入基础名以沿用原扩展名；显式写出扩展名必须字节相同。
    // 大小写变化可能影响区分大小写的平台资源定位，因此也拒绝。
    if ( requestedName.extension().empty() ) {
        requestedName += source.extension();
    }
    if ( requestedName.extension() != source.extension() ||
         !validFileName(Config::pathToUtf8(requestedName)) ) {
        // 目标扩展名和源扩展名逐字一致，避免仅大小写差异引起格式识别变化。
        result.m_errorMessage = "重命名不能改变资源文件扩展名";
        return result;
    }
    const auto target =
        (source.parent_path() / requestedName).lexically_normal();
    result.m_oldPath = source;
    result.m_newPath = target;
    // 完全相同的文件名是幂等成功，避免重写谱面和污染已有恢复旁路。
    if ( source == target ) return result;
    error.clear();
    // POSIX rename 对现存目标会覆盖，因此必须在提交前先拒绝冲突。
    // 项目内部操作由上层串行化，外部进程并发创建目标不受该锁保护。
    if ( std::filesystem::exists(target, error) || error ) {
        result.m_errorMessage = "目标文件已存在或不可访问";
        return result;
    }

    std::vector<PendingBeatmapReplacement> replacements;
    // 候选仅来自项目登记的谱面，不递归改写工作区中的任意未知文件。
    // 全部谱面先校验并暂存，然后才改动物理资源文件。
    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPath = std::filesystem::weakly_canonical(
            root / Config::utf8ToPath(entry.m_filePath), error);
        if ( error || !isProjectChild(root, mapPath) ||
             !std::filesystem::is_regular_file(mapPath, error) || error ) {
            // 无法读取的受管谱面可能仍包含旧引用，保守中止整批事务。
            // 已产生的暂存文件在返回前清理，正式谱面保持原样。
            result.m_errorMessage = "项目谱面不可读取：" + entry.m_filePath;
            removeStagedFiles(replacements);
            return result;
        }
        const auto extension = lowerExtension(mapPath);
        // 已知谱面格式具有可判断的视觉资源元数据；未知类型不碰内容。
        // IMD 仍需加载，以发现由文件名隐式关联的背景。
        if ( extension != ".mmm" && extension != ".mc" && extension != ".osu" &&
             extension != ".imd" ) {
            continue;
        }
        auto beatmap = BeatMap::loadFromFile(mapPath);
        // 加载成功才可以安全判断是否引用源文件；空 map_path 代表无效结果。
        // 此时仍未改名，故加载器看到的项目文件结构与用户原状态一致。
        if ( beatmap.m_baseMapMetadata.map_path.empty() ) {
            result.m_errorMessage = "无法读取项目谱面：" + entry.m_filePath;
            removeStagedFiles(replacements);
            return result;
        }
        const bool changed =
            remapMetadata(beatmap.m_baseMapMetadata, root, source, target);
        // 不相关谱面不序列化，避免其格式化、时间线或扩展属性产生旁支变化。
        if ( !changed ) continue;
        // 只有确实引用源资源的谱面进入事务，既保性能也减少外部格式回写面。
        if ( extension == ".imd" ) {
            // IMD 的背景从谱面文件名推导，无法编码一个任意的新背景名。
            // 继续改名会在重新加载后悄悄丢引用，因此直接拒绝。
            // IMD
            // 的背景名称按谱面名隐式推导，改名后无法在原格式中持久化任意路径。
            result.m_errorMessage =
                "IMD 谱面的背景文件名由谱面格式固定，不能单独重命名";
            removeStagedFiles(replacements);
            return result;
        }
        PendingBeatmapReplacement replacement;
        // 暂存文件保留原扩展名，让保存器使用原谱面格式写出。
        // 它和备份都位于谱面同目录，以便最终 rename 不跨文件系统。
        replacement.m_path       = mapPath;
        replacement.m_stagedPath = mapPath;
        replacement.m_stagedPath += ".mmm-visual-rename";
        replacement.m_stagedPath += mapPath.extension();
        replacement.m_backupPath = mapPath;
        replacement.m_backupPath += ".mmm-visual-rename.bak";
        error.clear();
        // 两个固定旁路名若已存在，可能是上次异常退出后的恢复证据。
        // 本操作拒绝覆盖，也不清理属于其他运行的文件。
        if ( std::filesystem::exists(replacement.m_stagedPath, error) ||
             error ||
             std::filesystem::exists(replacement.m_backupPath, error) ||
             error ) {
            result.m_errorMessage =
                "谱面重命名临时文件已存在：" + entry.m_filePath;
            removeStagedFiles(replacements);
            return result;
        }
        // 保留磁盘现有内容作为序列化来源，打开会话的未保存编辑不被覆盖。
        // 保存器失败时可能留下部分旁路，仅删除本次刚创建的文件。
        if ( !beatmap.saveToFile(replacement.m_stagedPath) ) {
            std::filesystem::remove(replacement.m_stagedPath, error);
            result.m_errorMessage = "无法暂存谱面：" + entry.m_filePath;
            removeStagedFiles(replacements);
            return result;
        }
        replacements.push_back(std::move(replacement));
        // 当前项完整暂存后才能入列表，清理函数不会误删尚未创建的旁路。
    }

    error.clear();
    // 完成所有谱面的可写性预检后，资源改名是第一个正式副作用。
    // 若本步失败，谱面旁路仍可直接清理而无需恢复任何正式文件。
    std::filesystem::rename(source, target, error);
    if ( error ) {
        removeStagedFiles(replacements);
        result.m_errorMessage = "重命名视觉资源文件失败：" + error.message();
        return result;
    }
    for ( auto& replacement : replacements ) {
        // 每张谱面先留原始字节备份，再以完整暂存内容覆盖正式路径。
        // 两步之间的错误由统一回滚函数恢复此前已提交的全部谱面。
        error.clear();
        std::filesystem::rename(
            replacement.m_path, replacement.m_backupPath, error);
        if ( error ) {
            result.m_errorMessage = "无法备份谱面：" + error.message();
            result.m_errorMessage += rollback(replacements, source, target);
            return result;
        }
        replacement.m_backupCreated = true;
        // 标志先于第二次 rename 更新，第二步失败时回滚仍能找回原字节。
        std::filesystem::rename(
            replacement.m_stagedPath, replacement.m_path, error);
        if ( error ) {
            result.m_errorMessage = "无法提交谱面：" + error.message();
            result.m_errorMessage += rollback(replacements, source, target);
            return result;
        }
    }
    // 原字节备份只在全部谱面成功提交后清理；删除失败不影响已经生效的改名。
    // 删除备份失败只记日志：此时正式谱面和资源均已使用新路径。
    // 把已成功的事务报告为失败会诱导用户再次改名，反而制造冲突。
    for ( const auto& replacement : replacements ) {
        error.clear();
        // 不重新解析谱面内容；提交成功由所有 rename 操作无错误共同证明。
        std::filesystem::remove(replacement.m_backupPath, error);
        if ( error ) {
            XWARN("Cannot remove visual rename backup {}: {}",
                  Config::pathToUtf8(replacement.m_backupPath),
                  error.message());
        }
    }
    return result;
}

}  // namespace MMM::Logic
