#include "config/skin/SkinPackageService.h"

#include "config/Utf8Path.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <miniz.h>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMM::Config
{

namespace
{

/// @brief MSK 包允许的最大压缩文件大小。
/// @details 在把归档读入内存前执行此上限，避免不受控的单次分配。
constexpr std::uintmax_t kMaximumPackageSize = 256U * 1024U * 1024U;

/// @brief MSK 包允许的最大文件数量。
/// @details 目录条目也计入归档条目总数，用于限制遍历和元数据开销。
constexpr mz_uint kMaximumArchiveEntryCount = 8192U;

/// @brief MSK 包允许的单文件最大解压大小。
/// @details 在调用 miniz 堆解压前检查，限制单个条目的峰值内存。
constexpr std::uint64_t kMaximumEntrySize = 128U * 1024U * 1024U;

/// @brief MSK 包允许的全部文件最大解压大小。
/// @details 以声明的未压缩大小累计，降低压缩炸弹耗尽磁盘的风险。
constexpr std::uint64_t kMaximumExtractedSize = 512U * 1024U * 1024U;

/// @brief 导入临时目录自动清理器。
/// @details 只有成功提交安装目录后才清空 path，从而转移清理责任。
struct TemporaryDirectory {
    /// @brief 需要在离开作用域时清理的目录。
    /// @note 成功转移目录所有权后必须由提交方显式清空。
    std::filesystem::path path;

    /// @brief 清理临时目录及其全部内容。
    ~TemporaryDirectory()
    {
        // 空路径表示目录从未创建，或已经通过 rename 提交为正式安装。
        if ( path.empty() ) return;
        // 清理属于失败恢复路径，使用 error_code 避免析构期间传播异常。
        std::error_code removeError;
        // 临时目录由本服务独占创建，可递归移除其中的全部解压产物。
        std::filesystem::remove_all(path, removeError);
    }
};

/// @brief 判断扩展名是否为 MSK，匹配时忽略 ASCII 大小写。
/// @param path 待检查的包文件或输出文件路径。
/// @return 扩展名按 ASCII 小写转换后等于 .msk 时返回 true。
/// @note 这里只验证格式入口，文件存在性由导入或导出流程分别检查。
bool hasMskExtension(const std::filesystem::path& path)
{
    // 路径先按项目 UTF-8 约定转换，避免直接依赖平台 path::string 编码。
    std::string extension = pathToUtf8(path.extension());
    // 扩展名限定为 ASCII，逐字节 tolower 不改变非 ASCII 路径主体。
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        // 转成 unsigned char 后再调用 cctype，避免负 char 导致未定义行为。
        return static_cast<char>(std::tolower(c));
    });
    // 仅接受完整扩展名，诸如 .msk.zip 不会被误判为皮肤包。
    return extension == ".msk";
}

/// @brief 读取完整二进制文件并限制最大大小。
/// @param path 待读取的普通文件路径。
/// @param sizeLimit 允许分配和读取的最大字节数。
/// @param bytes 成功时接收文件完整内容，失败时保持为空。
/// @return 文件大小可表示、文件可打开且读取完整时返回 true。
/// @warning 该函数会按文件大小一次性分配内存，只能用于低频包处理路径。
bool readFileBytes(const std::filesystem::path& path, std::uintmax_t sizeLimit,
                   std::vector<std::uint8_t>& bytes)
{
    // 先清空调用方缓冲，保证任何早退都不会遗留上一次文件内容。
    bytes.clear();

    // 先读取元数据再分配，避免打开超限文件后才发现无法承载。
    std::error_code fileSizeError;
    const auto      fileSize = std::filesystem::file_size(path, fileSizeError);
    // file_size 的返回类型可能宽于进程地址空间，两个上限必须分别验证。
    // 同时检查业务上限和本机 size_t 上限，兼容 32 位地址空间。
    if ( fileSizeError || fileSize > sizeLimit ||
         fileSize > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::size_t>::max()) ) {
        return false;
    }

    // 二进制模式禁止平台换行转换，归档和资源必须逐字节保持一致。
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return false;
    // 文件打开失败时 bytes 仍为空，调用方不需要额外清理缓冲。

    // 元数据已通过范围验证，此处转换不会截断实际文件大小。
    bytes.resize(static_cast<std::size_t>(fileSize));
    if ( !bytes.empty() ) {
        // 空文件无需调用 read，避免对零长度 data 指针提出额外要求。
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    // 精确读到文件尾时 eof 可能和成功读取同时出现，因此两种状态都接受。
    return input.good() || input.eof();
}

/// @brief 将字节写入文件，并按需创建父目录。
/// @param path 目标文件路径。
/// @param data 待写入缓冲区；size 为零时允许为空。
/// @param size 待写入字节数。
/// @return 父目录准备和完整写入均成功时返回 true。
/// @warning 采用截断写入，只允许指向临时文件或明确可覆盖的目标。
bool writeFileBytes(const std::filesystem::path& path, const void* data,
                    std::size_t size)
{
    // 根级目标可能没有父目录，此时无需尝试创建空路径。
    const auto parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        // 递归创建支持归档中的嵌套资源布局。
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) return false;
        // create_directories 对已经存在的父目录同样视为可继续状态。
    }

    // 所有生产调用都先写临时路径，截断不会直接损坏既有最终包。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    if ( size > 0 ) {
        // 调用方在非零大小时必须提供有效连续缓冲区。
        output.write(static_cast<const char*>(data),
                     static_cast<std::streamsize>(size));
    }
    // 返回流最终状态，使磁盘写入失败能够触发上层清理和恢复。
    return output.good();
}

/// @brief 判断压缩包相对路径是否安全。
/// @param path 使用正斜线分隔的归档条目名称。
/// @return 路径是非空、相对且不含点段或盘符标识时返回 true。
/// @note 调用方必须先把反斜线统一转换为正斜线。
bool isSafeArchivePath(std::string_view path)
{
    // 拒绝 POSIX 根、Windows 根和任何冒号，覆盖盘符及常见流名称。
    if ( path.empty() || path.front() == '/' || path.front() == '\\' ||
         path.find(':') != std::string_view::npos ) {
        return false;
    }

    // 逐段验证比简单搜索 ".." 更精确，不会误伤普通文件名中的连续点。
    std::size_t start = 0;
    while ( start <= path.size() ) {
        // 归档规范使用正斜线，因此无需依赖宿主平台分隔符。
        const std::size_t end = path.find('/', start);
        const auto        segment =
            path.substr(start,
                        end == std::string_view::npos ? std::string_view::npos
                                                      : end - start);
        // 空段拒绝重复或尾部分隔符；点段拒绝规范化后的目录逃逸。
        if ( segment.empty() || segment == "." || segment == ".." ) {
            return false;
        }
        // 未找到更多分隔符时，最后一个分量已经验证完毕。
        if ( end == std::string_view::npos ) break;
        start = end + 1;
    }
    // 每一段都经过验证后，路径才可与目标根组合使用。
    return true;
}

/// @brief 判断目标路径是否仍位于指定根目录内。
/// @param root 受信任的目标根目录。
/// @param target 由根目录和不受信任相对路径组合得到的候选路径。
/// @return 词法规范化后 target 是 root 的非空后代时返回 true。
/// @note 该检查不跟随符号链接，复制流程会另行拒绝符号链接条目。
bool isPathInsideRoot(const std::filesystem::path& root,
                      const std::filesystem::path& target)
{
    // 两端先做词法规范化，消除点段后再计算相对关系。
    const auto relative =
        target.lexically_normal().lexically_relative(root.lexically_normal());
    // 根本身不是可写文件目标；绝对相对结果表示无法建立包含关系。
    if ( relative.empty() || relative.is_absolute() ) return false;
    for ( const auto& part : relative ) {
        // 任一上级分量都说明候选路径逃出了目标根。
        if ( part == ".." ) return false;
    }
    // 没有发现上级分量，候选目标是根目录内部的后代。
    return true;
}

/// @brief 创建不存在的唯一临时目录。
/// @param parent 临时目录的受信任父目录。
/// @param prefix 用于诊断和区分用途的目录名前缀。
/// @param errorMessage 失败时接收面向 UI 的原因。
/// @return 成功创建且由调用方独占的目录路径，否则返回空路径。
/// @warning 会执行文件系统创建，只能用于导入导出的低频事务路径。
std::filesystem::path createUniqueDirectory(const std::filesystem::path& parent,
                                            std::string_view             prefix,
                                            std::string& errorMessage)
{
    // 稳态时钟只用于降低名称碰撞概率，不承担安全随机数或持久 ID 语义。
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // 有限重试处理同一时刻的并发操作，同时避免异常文件系统上忙等。
    for ( unsigned int attempt = 0; attempt < 32U; ++attempt ) {
        const auto candidate =
            parent / utf8ToPath(std::string(prefix) + std::to_string(stamp) +
                                "_" + std::to_string(attempt));
        // create_directories 的 true 结果证明本次调用获得了新目录所有权。
        std::error_code createError;
        if ( std::filesystem::create_directories(candidate, createError) ) {
            // 仅返回由本轮原子创建成功的候选，避免接管碰撞目录。
            return candidate;
        }
        // 权限、路径或设备错误不会通过更换后缀恢复，应立即向上层报告。
        if ( createError ) {
            errorMessage = "无法创建临时目录: " + createError.message();
            return {};
        }
    }
    // 连续名称碰撞时返回明确错误，不复用任何可能属于其他进程的目录。
    errorMessage = "无法分配唯一临时目录";
    return {};
}

/// @brief 解压 MSK 包到临时目录并阻止目录穿越和过量展开。
/// @param packagePath 待读取的 .msk 文件路径。
/// @param destinationRoot 已由调用方独占创建的空临时目录。
/// @param errorMessage 失败时接收具体归档或文件系统原因。
/// @return 全部条目通过预定限制并成功写入时返回 true。
/// @warning 该函数会读取完整压缩包并逐条目分配解压缓冲，只可低频调用。
bool extractPackage(const std::filesystem::path& packagePath,
                    const std::filesystem::path& destinationRoot,
                    std::string&                 errorMessage)
{
    // 压缩包先整体受限读取，阻止超大输入直接进入 miniz 解析器。
    std::vector<std::uint8_t> packageBytes;
    if ( !readFileBytes(packagePath, kMaximumPackageSize, packageBytes) ||
         packageBytes.empty() ) {
        errorMessage = "无法读取 MSK 文件，或文件超过 256 MiB";
        return false;
    }

    // archive 零初始化是 miniz reader 初始化契约的一部分。
    mz_zip_archive archive{};
    if ( !mz_zip_reader_init_mem(
             &archive, packageBytes.data(), packageBytes.size(), 0) ) {
        errorMessage = "MSK 文件不是有效的 ZIP 压缩包";
        return false;
    }
    // 从此处开始的所有返回都汇聚到 reader_end，不在循环中直接早退。

    // 后续采用单一 success 状态，保证无论失败位置都统一释放 reader。
    bool          success        = true;
    std::uint64_t extractedTotal = 0;
    const mz_uint entryCount     = mz_zip_reader_get_num_files(&archive);
    // 空包不可能包含皮肤入口，过多条目则可能造成遍历资源耗尽。
    if ( entryCount == 0 || entryCount > kMaximumArchiveEntryCount ) {
        errorMessage = "MSK 文件数量为空或超过限制";
        success      = false;
    }

    // 只在前序条目成功时继续，避免首个安全错误后仍写入更多数据。
    for ( mz_uint index = 0; success && index < entryCount; ++index ) {
        // 每轮重新初始化统计结构，避免 miniz 未填充字段残留旧值。
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&archive, index, &fileStat) ) {
            errorMessage = "无法读取 MSK 压缩条目";
            success      = false;
            break;
        }

        // ZIP 中可能出现 Windows 分隔符，先统一后再执行平台无关验证。
        std::string archiveName = fileStat.m_filename;
        std::ranges::replace(archiveName, '\\', '/');
        // 目录条目通常以斜线结尾；移除后按普通安全分量验证。
        while ( !archiveName.empty() && archiveName.back() == '/' ) {
            archiveName.pop_back();
        }
        // 归档根目录占位本身无需创建或计入文件总量。
        if ( archiveName.empty() ) continue;
        // 规范化后的空目录占位不影响唯一入口扫描结果。

        // 第一层拒绝绝对路径、盘符、空段及任何点段。
        if ( !isSafeArchivePath(archiveName) ) {
            errorMessage = "MSK 包含不安全路径: " + archiveName;
            success      = false;
            break;
        }

        // 第二层在 filesystem 语义下验证最终路径仍位于解压根。
        const auto destinationPath =
            (destinationRoot / utf8ToPath(archiveName)).lexically_normal();
        if ( !isPathInsideRoot(destinationRoot, destinationPath) ) {
            errorMessage = "MSK 路径越过解压目录: " + archiveName;
            success      = false;
            break;
        }
        // 两层校验都成功后，destinationPath 才能参与任何文件系统操作。

        // 显式目录只负责创建层级，不参与解压大小累计。
        if ( mz_zip_reader_is_file_a_directory(&archive, index) ) {
            std::error_code createError;
            std::filesystem::create_directories(destinationPath, createError);
            // 目录创建失败后停止处理，临时目录守卫会清理已写内容。
            if ( createError ) {
                errorMessage = "无法创建 MSK 目录: " + createError.message();
                success      = false;
            }
            continue;
        }

        // 在请求堆解压前读取声明大小，并同时检查单项和累计上限。
        const std::uint64_t entrySize = fileStat.m_uncomp_size;
        if ( entrySize > kMaximumEntrySize ||
             extractedTotal > kMaximumExtractedSize - entrySize ) {
            errorMessage = "MSK 解压后的文件大小超过限制";
            success      = false;
            break;
        }
        // 减法形式的上限判断已防止加法溢出，此处累计是安全的。
        extractedTotal += entrySize;
        // 目录条目不计入总量，普通文件包括零字节文件均纳入条目遍历。

        // miniz 为每个文件分配独立缓冲，写入后必须立即释放。
        std::size_t extractedSize = 0;
        void*       extractedData =
            mz_zip_reader_extract_to_heap(&archive, index, &extractedSize, 0);
        // 非空条目必须有缓冲，实际字节数也必须与归档元数据一致。
        if ( (!extractedData && entrySize != 0) ||
             extractedSize != entrySize ) {
            errorMessage = "无法解压 MSK 文件: " + archiveName;
            success      = false;
            break;
        }
        // 仅把已经通过两层路径校验的数据写入临时解压根。
        success = writeFileBytes(destinationPath, extractedData, extractedSize);
        // 空文件可能返回空指针；mz_free 只接收实际分配的缓冲。
        if ( extractedData ) mz_free(extractedData);
        if ( !success ) {
            errorMessage = "无法写入 MSK 文件: " + archiveName;
        }
    }

    // reader 初始化成功后无论循环结果如何都必须配对结束。
    // 临时目录的最终清理由调用方守卫负责，不在本函数内递归删除。
    mz_zip_reader_end(&archive);
    return success;
}

/// @brief 查找解压目录中唯一的 skin.lua 文件。
/// @param extractedRoot 已完成安全解压的临时根目录。
/// @param errorMessage 未找到、发现多个入口或遍历失败时接收原因。
/// @return 唯一入口的完整路径；无法确定时返回空路径。
/// @note 唯一入口约束同时确定后续复制使用的皮肤根目录。
std::filesystem::path findUniqueSkinLua(
    const std::filesystem::path& extractedRoot, std::string& errorMessage)
{
    // result 在首次命中时记录路径，第二次命中即可判定包结构歧义。
    std::filesystem::path result;
    // 递归遍历使用 error_code 重载，避免损坏包通过异常中断导入流程。
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        extractedRoot,
        // 权限不足条目由迭代器跳过，但初始化本身失败仍需明确报告。
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if ( iteratorError ) {
        errorMessage = "无法扫描解压后的 MSK 目录: " + iteratorError.message();
        return {};
    }

    // 不假定 skin.lua 位于首层，允许皮肤包携带一个外层目录。
    while ( iterator != end ) {
        // 每个条目单独查询普通文件状态，目录同名不应被当作入口。
        std::error_code typeError;
        const bool      isRegular = iterator->is_regular_file(typeError);
        if ( typeError ) {
            errorMessage = "无法检查 MSK 文件类型: " + typeError.message();
            return {};
        }
        // 文件名采用精确大小写契约，保持跨平台包结构一致。
        if ( isRegular && iterator->path().filename() == "skin.lua" ) {
            if ( !result.empty() ) {
                // 多入口无法安全决定要安装哪个父目录，因此整体拒绝。
                errorMessage = "MSK 中存在多个 skin.lua，无法确定皮肤根目录";
                return {};
            }
            // 暂存首次入口，继续遍历以证明它在整个归档中唯一。
            result = iterator->path();
            // 保存绝对遍历路径，后续 parent_path 可直接作为复制源根。
        }

        // 显式 increment 暴露延迟发生的文件系统错误。
        iterator.increment(iteratorError);
        if ( iteratorError ) {
            errorMessage = "无法继续扫描 MSK 目录: " + iteratorError.message();
            return {};
        }
    }

    // 完整遍历后仍无入口时给出结构错误，而不是一般读取失败。
    if ( result.empty() ) {
        errorMessage = "MSK 中未找到 skin.lua";
    }
    // 非空结果已通过完整遍历证明唯一，不需要依赖条目顺序。
    return result;
}

/// @brief 验证皮肤安装目录名是单个安全路径分量。
/// @param name 从包顶层目录或包文件名推导的候选名称。
/// @return 名称非空、非点段且不携带根或父路径时返回 true。
/// @note 该约束保证 skinsRoot / name 只生成一个直接子目录。
bool isSafeDirectoryName(const std::filesystem::path& name)
{
    // filename 仍可能是特殊点段，必须和路径层级属性一起验证。
    return !name.empty() && name != "." && name != ".." &&
           !name.has_parent_path() && !name.has_root_path();
}

/// @brief 将目录内容复制到新建的安装暂存目录。
/// @param sourceRoot 解压后由唯一 skin.lua 确定的皮肤根。
/// @param destinationRoot 已独占创建的安装暂存目录。
/// @param errorMessage 失败时接收遍历、类型或复制原因。
/// @return 所有目录和普通文件完整复制时返回 true。
/// @warning 会递归遍历和复制文件，只能用于用户触发的低频导入操作。
bool copyDirectoryContents(const std::filesystem::path& sourceRoot,
                           const std::filesystem::path& destinationRoot,
                           std::string&                 errorMessage)
{
    // 不跳过权限错误，任何不可验证条目都应使完整安装事务失败。
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        sourceRoot, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if ( iteratorError ) {
        errorMessage = "无法读取皮肤目录: " + iteratorError.message();
        return false;
    }
    // 初始化成功后，源目录至少可进入一致的递归遍历状态。

    // 每个源条目都通过相对路径投影到独立的暂存根。
    while ( iterator != end ) {
        // 来源已在安全临时目录内，仍重新验证目标路径以维持局部不变量。
        const auto relativePath =
            iterator->path().lexically_relative(sourceRoot);
        const auto destinationPath =
            (destinationRoot / relativePath).lexically_normal();
        // 空关系或逃逸关系不能作为安装目标，防止实现变化削弱前置检查。
        if ( relativePath.empty() ||
             !isPathInsideRoot(destinationRoot, destinationPath) ) {
            errorMessage = "皮肤目录包含无效路径";
            return false;
        }
        // relativePath 只用于暂存根投影，不会原样解释为绝对目标。

        // symlink_status 刻意不跟随链接，导入不会复制包外部指向的内容。
        std::error_code statusError;
        const auto      status = iterator->symlink_status(statusError);
        if ( statusError ) {
            errorMessage = "无法读取皮肤文件状态: " + statusError.message();
            return false;
        }

        // 目录和普通文件分别处理；其他类型在下方统一拒绝。
        std::error_code operationError;
        if ( std::filesystem::is_directory(status) ) {
            // 保留空目录，使导入后的皮肤布局与包内容一致。
            std::filesystem::create_directories(destinationPath,
                                                operationError);
        } else if ( std::filesystem::is_regular_file(status) ) {
            // 某些归档省略显式目录条目，复制文件前补齐父目录。
            std::filesystem::create_directories(destinationPath.parent_path(),
                                                operationError);
            if ( !operationError ) {
                // 暂存目录是新建的，禁止覆盖可暴露意外重名或遍历问题。
                std::filesystem::copy_file(iterator->path(),
                                           destinationPath,
                                           std::filesystem::copy_options::none,
                                           operationError);
            }
        } else {
            // 符号链接、设备和套接字均不属于可移植皮肤包内容。
            errorMessage = "皮肤目录包含不支持的文件类型";
            return false;
        }

        // 任一文件失败都会让守卫回滚整个暂存目录，不留下部分安装。
        if ( operationError ) {
            errorMessage = "无法安装皮肤文件: " + operationError.message();
            return false;
        }

        // 显式递增确保迭代期间的新错误可以转换为服务错误消息。
        iterator.increment(iteratorError);
        if ( iteratorError ) {
            errorMessage = "无法继续复制皮肤目录: " + iteratorError.message();
            return false;
        }
    }
    // 成功返回表示暂存目录已经具备源皮肤的完整普通文件树。
    return true;
}

/// @brief 收集皮肤目录中可导出的普通文件。
/// @param skinDirectory 待导出的已安装皮肤根目录。
/// @param files 成功时接收按通用 UTF-8 路径排序的普通文件列表。
/// @param errorMessage 失败时接收类型、大小或遍历原因。
/// @return 至少存在一个合规普通文件且完整收集时返回 true。
/// @warning 会递归扫描并读取文件元数据，只能用于低频导出操作。
bool collectSkinFiles(const std::filesystem::path&        skinDirectory,
                      std::vector<std::filesystem::path>& files,
                      std::string&                        errorMessage)
{
    // 清空输出，确保失败时调用方不会误用上一次收集结果。
    files.clear();
    // 累计未压缩大小，与导入端的展开总量限制保持对称。
    std::uint64_t                                 totalSize = 0;
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        skinDirectory, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if ( iteratorError ) {
        errorMessage = "无法读取皮肤目录: " + iteratorError.message();
        return false;
    }
    // 在收集结束前不初始化归档，验证失败不会产生任何输出副作用。

    // 只收集普通文件；目录用于遍历但不写入显式归档条目。
    while ( iterator != end ) {
        // 不跟随符号链接，防止导出皮肤目录之外的文件内容。
        std::error_code statusError;
        const auto      status = iterator->symlink_status(statusError);
        if ( statusError ) {
            errorMessage = "无法读取皮肤文件状态: " + statusError.message();
            return false;
        }

        if ( std::filesystem::is_regular_file(status) ) {
            // 每个文件在加入列表前验证单项大小和剩余累计容量。
            std::error_code sizeError;
            const auto      fileSize =
                std::filesystem::file_size(iterator->path(), sizeError);
            // 使用减法形式避免 totalSize + fileSize 的无符号溢出。
            if ( sizeError || fileSize > kMaximumEntrySize ||
                 totalSize > kMaximumExtractedSize - fileSize ) {
                errorMessage = "皮肤文件大小超过 MSK 限制";
                return false;
            }
            // 只有通过全部大小约束后才更新累计量和结果列表。
            totalSize += fileSize;
            files.push_back(iterator->path());
            // 保存源绝对路径，生成条目名时再统一相对化和编码转换。
            // 导出端条目数与导入端读取限制一致，保证自产包一定可导入。
            if ( files.size() > kMaximumArchiveEntryCount ) {
                errorMessage = "皮肤文件数量超过 MSK 限制";
                return false;
            }
        } else if ( !std::filesystem::is_directory(status) ) {
            // 非普通文件可能逃逸皮肤根或缺乏跨平台归档语义，统一拒绝。
            errorMessage = "皮肤目录包含不支持的文件类型";
            return false;
        }

        // 遍历错误必须终止收集，不能生成缺少部分资源的表面成功包。
        iterator.increment(iteratorError);
        if ( iteratorError ) {
            errorMessage = "无法继续读取皮肤目录: " + iteratorError.message();
            return false;
        }
    }

    // 稳定排序使相同输入得到确定的归档条目顺序，便于比较和诊断。
    std::ranges::sort(files, [](const auto& left, const auto& right) {
        // 通用 UTF-8 路径不受宿主分隔符影响，跨平台排序结果一致。
        return pathToUtf8Generic(left) < pathToUtf8Generic(right);
    });
    // 空皮肤目录没有可导出价值，具体错误文本由上层补充。
    // 排序后的非空列表是导出阶段唯一允许消费的输入状态。
    return !files.empty();
}

/// @brief 以可恢复方式将临时文件替换为最终输出文件。
/// @param temporaryPath 已完整写入并关闭的临时 MSK 文件。
/// @param outputPath 用户选择的最终输出路径。
/// @param errorMessage 失败时接收检查、暂存或提交原因。
/// @return 新文件成功提交到最终路径时返回 true。
/// @warning 会重命名既有输出，但提交失败时尽力恢复原文件。
bool replaceOutputFile(const std::filesystem::path& temporaryPath,
                       const std::filesystem::path& outputPath,
                       std::string&                 errorMessage)
{
    // 备份名位于同一目录，rename 通常可保持同一文件系统内的原子语义。
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // 原文件名保留在前缀中，便于异常中断后人工识别残留备份。
    const auto backupPath = outputPath.parent_path() /
                            utf8ToPath(pathToUtf8(outputPath.filename()) +
                                       ".backup-" + std::to_string(stamp));

    // 先确定是否需要备份；无法查询时不能冒险覆盖未知目标。
    std::error_code existsError;
    const bool outputExists = std::filesystem::exists(outputPath, existsError);
    if ( existsError ) {
        errorMessage = "无法检查输出文件: " + existsError.message();
        return false;
    }

    if ( outputExists ) {
        // 只允许替换普通文件，目录或特殊对象绝不参与重命名事务。
        std::error_code typeError;
        if ( !std::filesystem::is_regular_file(outputPath, typeError) ||
             typeError ) {
            errorMessage = "输出路径不是普通文件";
            return false;
        }

        // 先把旧文件移到唯一备份名，提交失败时仍有恢复来源。
        std::error_code backupError;
        std::filesystem::rename(outputPath, backupPath, backupError);
        if ( backupError ) {
            errorMessage = "无法暂存已有 MSK 文件: " + backupError.message();
            return false;
        }
        // 旧文件成功移走后，最终路径必须保持空闲直到新文件提交。
    }

    // 临时文件已完成全部内容验证，此次 rename 是最终提交点。
    std::error_code renameError;
    std::filesystem::rename(temporaryPath, outputPath, renameError);
    if ( renameError ) {
        if ( outputExists ) {
            // 提交失败后尽力恢复旧输出；恢复错误不覆盖首要失败原因。
            std::error_code restoreError;
            std::filesystem::rename(backupPath, outputPath, restoreError);
        }
        errorMessage = "无法写入 MSK 文件: " + renameError.message();
        return false;
    }
    // rename 成功意味着调用方提供的临时路径已不再存在。

    if ( outputExists ) {
        // 新输出就位后旧备份不再需要；清理失败不影响已提交结果。
        std::error_code removeError;
        std::filesystem::remove(backupPath, removeError);
    }
    return true;
}

}  // namespace

/// @brief 安全导入一个 MSK 皮肤包到用户皮肤根目录。
/// @param packagePath 用户选择的 .msk 压缩包。
/// @param skinsRoot 应用管理的皮肤安装根目录。
/// @return 包含成功状态、目录名、安装位置或失败原因的结果。
/// @warning 用户触发的低频文件系统事务，会完整读取归档并复制全部资源。
SkinPackageImportResult SkinPackageService::importPackage(
    const std::filesystem::path& packagePath,
    const std::filesystem::path& skinsRoot)
{
    // 结果默认失败，只有最终目录 rename 成功后才显式发布成功状态。
    SkinPackageImportResult result;
    // 在任何文件访问前限制格式入口，给用户更直接的选择错误提示。
    if ( !hasMskExtension(packagePath) ) {
        result.errorMessage = "请选择 .msk 皮肤包";
        return result;
    }

    // 只接受现有普通文件，目录、链接目标错误和不可访问路径均被拒绝。
    std::error_code packageTypeError;
    if ( !std::filesystem::is_regular_file(packagePath, packageTypeError) ||
         packageTypeError ) {
        result.errorMessage = "MSK 文件不存在或不可读取";
        return result;
    }
    // 类型验证完成后再创建目标目录，非法输入不会产生安装侧副作用。

    // 用户 skins 根可在首次导入时不存在，由服务负责递归创建。
    std::error_code createRootError;
    std::filesystem::create_directories(skinsRoot, createRootError);
    if ( createRootError ) {
        result.errorMessage =
            "无法创建用户 skins 目录: " + createRootError.message();
        return result;
    }

    // 初始解压放在系统临时目录，未通过验证的内容不接触用户 skins 根。
    std::error_code tempPathError;
    auto tempParent = std::filesystem::temp_directory_path(tempPathError);
    if ( tempPathError ) {
        result.errorMessage =
            "无法访问系统临时目录: " + tempPathError.message();
        return result;
    }
    // 系统临时根与用户 skins 根可以跨文件系统，因为中间采用显式复制。

    // RAII 守卫覆盖解压、入口扫描和后续安装的所有失败出口。
    TemporaryDirectory extraction;
    extraction.path = createUniqueDirectory(
        tempParent, "mmm_skin_import_", result.errorMessage);
    // 唯一目录创建和完整安全解压必须同时成功，才进入结构识别阶段。
    if ( extraction.path.empty() ||
         !extractPackage(packagePath, extraction.path, result.errorMessage) ) {
        return result;
    }

    // 扫描整个解压树，要求恰好一个入口以确定唯一皮肤根。
    const auto skinLua =
        findUniqueSkinLua(extraction.path, result.errorMessage);
    if ( skinLua.empty() ) return result;
    // findUniqueSkinLua 已设置具体结构错误，调用点保持原消息返回。

    // 有外层目录时采用入口父目录名；根级入口则由包文件名派生。
    const auto sourceDirectory = skinLua.parent_path();
    auto       directoryName   = sourceDirectory.filename();
    if ( sourceDirectory == extraction.path || directoryName.empty() ) {
        // stem 去除 .msk 后缀，同时保留用户选择的可读包名称。
        directoryName = packagePath.stem();
    }
    // 无论名称来源是否受信任，都再次约束为单个直接子目录分量。
    if ( !isSafeDirectoryName(directoryName) ) {
        result.errorMessage = "无法从 MSK 确定安全的皮肤目录名";
        return result;
    }
    // 安全名称此后同时用于暂存前缀、最终路径和返回给 UI 的标识。

    // 最终目的地固定为 skinsRoot 的直接子目录，不允许自动生成后缀覆盖。
    const auto      destination = skinsRoot / directoryName;
    std::error_code destinationExistsError;
    // 查询错误和同名存在都阻止导入，保护用户已有皮肤不被替换。
    if ( std::filesystem::exists(destination, destinationExistsError) ||
         destinationExistsError ) {
        result.errorMessage =
            destinationExistsError
                ? "无法检查皮肤安装目录: " + destinationExistsError.message()
                : "同名皮肤目录已存在";
        return result;
    }

    // 暂存目录位于最终 skins 根，使最终 rename 保持同文件系统提交语义。
    const std::string stagingPrefix =
        ".mmm-skin-import-" + pathToUtf8(directoryName) + "-";
    // 第二个守卫负责安装暂存内容；原始解压目录仍独立保留到函数结束。
    TemporaryDirectory staging;
    staging.path =
        createUniqueDirectory(skinsRoot, stagingPrefix, result.errorMessage);
    // 复制完整源皮肤到空暂存目录，任何失败都由守卫整体回滚。
    if ( staging.path.empty() ||
         !copyDirectoryContents(
             sourceDirectory, staging.path, result.errorMessage) ) {
        return result;
    }

    // 提交前在暂存根再次确认入口存在，防止复制或路径投影出现偏差。
    std::error_code installedSkinError;
    if ( !std::filesystem::is_regular_file(staging.path / "skin.lua",
                                           installedSkinError) ||
         installedSkinError ) {
        result.errorMessage = "导入后的皮肤根目录缺少 skin.lua";
        return result;
    }
    // 此检查针对复制后的根级入口，避免把嵌套源目录层级复制错误。

    // 从暂存目录重命名到此前确认不存在的最终目录，形成单一提交点。
    std::error_code renameError;
    std::filesystem::rename(staging.path, destination, renameError);
    if ( renameError ) {
        result.errorMessage = "无法完成皮肤安装: " + renameError.message();
        return result;
    }
    // rename 成功后清空守卫路径，把新目录所有权转交给用户 skins 根。
    staging.path.clear();

    // 只有已提交目录才对调用方发布成功及可用于立即加载的路径。
    result.success            = true;
    result.skinDirectoryName  = pathToUtf8(directoryName);
    result.installedDirectory = destination;
    return result;
}

/// @brief 把一个已安装皮肤目录导出为可重新导入的 MSK 包。
/// @param skinDirectory 包含根级 skin.lua 的源目录。
/// @param outputPath 用户选择的 .msk 输出文件。
/// @param errorMessage 失败时接收可展示的具体原因。
/// @return 完整归档以可恢复替换方式提交成功时返回 true。
/// @warning 用户触发的低频文件系统事务，会扫描并读取全部皮肤资源。
bool SkinPackageService::exportPackage(
    const std::filesystem::path& skinDirectory,
    const std::filesystem::path& outputPath, std::string& errorMessage)
{
    // 清除调用方旧错误，成功返回时保证错误消息为空。
    errorMessage.clear();
    // 输出必须使用公开包扩展名，避免生成无法被导入对话框识别的文件。
    if ( !hasMskExtension(outputPath) ) {
        errorMessage = "导出文件必须使用 .msk 扩展名";
        return false;
    }

    // 根级 skin.lua 是可导出皮肤目录的最小结构契约。
    std::error_code skinLuaError;
    if ( !std::filesystem::is_regular_file(skinDirectory / "skin.lua",
                                           skinLuaError) ||
         skinLuaError ) {
        errorMessage = "皮肤目录中未找到 skin.lua";
        return false;
    }

    // 归档统一包含顶层皮肤目录，名称来自规范化后的源目录末段。
    const auto directoryName = skinDirectory.lexically_normal().filename();
    if ( !isSafeDirectoryName(directoryName) ) {
        errorMessage = "皮肤目录名无效";
        return false;
    }
    // 目录名验证也保证拼入 ZIP 后不会形成绝对或上级条目。

    // 预先完整收集和校验，避免写到一半才发现不支持的链接或超限资源。
    std::vector<std::filesystem::path> files;
    if ( !collectSkinFiles(skinDirectory, files, errorMessage) ) {
        // 空目录是唯一可能未设置具体错误的失败形态，由此处补充消息。
        if ( errorMessage.empty() ) errorMessage = "皮肤目录中没有可导出文件";
        return false;
    }

    // 先在内存中完成整个归档，最终文件不会暴露半写入 ZIP。
    mz_zip_archive archive{};
    if ( !mz_zip_writer_init_heap(&archive, 0, 0) ) {
        errorMessage = "无法初始化 MSK 压缩器";
        return false;
    }
    // writer 成功初始化后，所有循环失败均通过统一尾部完成资源释放。

    // 单一状态保证任何条目失败后仍执行 writer 和缓冲区的配对清理。
    bool success = true;
    // 缓冲区在循环中复用，峰值只由已限制的最大单文件大小决定。
    std::vector<std::uint8_t> fileBytes;
    // collectSkinFiles 已给出确定顺序，使归档条目顺序跨执行保持稳定。
    for ( const auto& file : files ) {
        // 写入前重新受限读取，可捕获收集后文件增长或消失的竞争变化。
        if ( !readFileBytes(file, kMaximumEntrySize, fileBytes) ) {
            errorMessage = "无法读取皮肤文件: " + pathToUtf8(file);
            success      = false;
            break;
        }

        // 每个条目都放在皮肤目录名下，保证导入时可恢复原安装名称。
        const auto relativePath = file.lexically_relative(skinDirectory);
        const auto archivePath  = directoryName / relativePath;
        // ZIP 条目必须使用正斜线通用格式，不能写入宿主平台分隔符。
        const std::string archiveName = pathToUtf8Generic(archivePath);
        // 导出端再次执行与导入端相同的路径约束，保证自产包结构合规。
        if ( !isSafeArchivePath(archiveName) ||
             !mz_zip_writer_add_mem(&archive,
                                    archiveName.c_str(),
                                    fileBytes.data(),
                                    fileBytes.size(),
                                    MZ_BEST_COMPRESSION) ) {
            // 压缩失败立即停止，内存归档不会被写入任何用户目标路径。
            errorMessage = "无法压缩皮肤文件: " + archiveName;
            success      = false;
            break;
        }
    }

    // finalize 由 miniz 分配连续归档缓冲，所有条目成功后才调用。
    void*       archiveBuffer = nullptr;
    std::size_t archiveSize   = 0;
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &archive, &archiveBuffer, &archiveSize) ) {
        errorMessage = "无法完成 MSK 压缩";
        success      = false;
    }
    // writer 初始化成功后始终配对结束；归档缓冲由调用方另行释放。
    mz_zip_writer_end(&archive);
    // 压缩后仍执行包大小上限，确保自产文件满足导入端输入限制。
    if ( success && archiveSize > kMaximumPackageSize ) {
        errorMessage = "导出的 MSK 文件超过 256 MiB";
        success      = false;
    }
    // 超限缓冲仍由本函数持有，后续通用释放路径不会泄漏内存。

    // 输出先落到同目录唯一临时文件，避免失败直接截断既有包。
    std::filesystem::path temporaryOutput;
    if ( success ) {
        // 稳态时钟后缀降低并发导出的临时文件碰撞概率。
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        temporaryOutput = outputPath.parent_path() /
                          utf8ToPath(pathToUtf8(outputPath.filename()) +
                                     ".writing-" + std::to_string(stamp));
        // 此时 archiveBuffer 内容完整，临时写入失败仍保留原输出不变。
        success = writeFileBytes(temporaryOutput, archiveBuffer, archiveSize);
        if ( !success ) errorMessage = "无法写入临时 MSK 文件";
    }
    // 临时文件完成写入后即可释放 miniz 内存，不跨文件替换阶段持有。
    if ( archiveBuffer ) mz_free(archiveBuffer);

    if ( success ) {
        // 可恢复替换负责暂存旧文件、提交新文件以及清理备份。
        success = replaceOutputFile(temporaryOutput, outputPath, errorMessage);
    }
    if ( !success && !temporaryOutput.empty() ) {
        // 写入或提交失败时尽力删除临时文件，不覆盖首要错误消息。
        std::error_code removeError;
        std::filesystem::remove(temporaryOutput, removeError);
    }
    return success;
}

}  // namespace MMM::Config
