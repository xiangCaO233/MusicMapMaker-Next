#include "network/AssetSyncService.h"

#include "AssetPathSafety.h"
#include "config/StagedFile.h"
#include "config/Utf8Path.h"

#include <miniz.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <system_error>

/// @file AssetZipExtraction.cpp
/// @brief 完整资源包的受限读取、分块解压与条目发布。
/// @details
/// 中央目录缓存是随机访问条目的入口，不读取或缓存完整 ZIP 内容。
/// 初始读取使用有界分配回调，限制进入预检之前中央目录能占用的工作缓冲。
/// 预检统计所有条目，包括重复条目。每个条目的上限约束单个文件，累计
/// 上限约束一次安装的磁盘写入量，数量上限同时约束大量零字节或目录条目。
/// 解压阶段记录实际输出长度，即使声明长度不准确也不会写出超过预算的字节。
/// 文件先写入同父目录的任务专属路径，只有完整通过 miniz 和输出流检查后才替换
/// 目标。已有调用方决定整个包安装和版本发布策略，解压器负责单条文件完整性。
/// 本地包导入在外层另有临时树，整包通过后才移动到安装目录。首次下载复用
/// 同一解压器，可以留存较早校验完成的资源；后续重试仍需通过整包检查。
/// 文件写入和取消仅在资源安装线程执行，回调次序与归档项的处理顺序一致。
namespace MMM::Network
{
namespace
{
/// @brief 完整资源包允许的条目总数，目录与文件均计入。
/// @details 限制大量小文件的目录和打开成本，独立于字节预算。
constexpr mz_uint MAX_ASSET_ARCHIVE_ENTRIES = 10'000;
/// @brief 单个资源的展开上限，以字节计。
/// @note 按展开后体积限制字体、图像和音频，不能用压缩大小代替。
constexpr mz_uint64 MAX_ASSET_ENTRY_BYTES = 512ULL * 1024 * 1024;
/// @brief 一次安装允许展开的资源总量，以字节计。
constexpr mz_uint64 MAX_ASSET_ARCHIVE_OUTPUT_BYTES = 4ULL * 1024 * 1024 * 1024;
/// @brief 限制 miniz 单次分配，包含初始化时缓存的中央目录。
/// @note 此处约束单次缓冲，库可以同时保有多个未达到此上限的缓冲。
constexpr std::size_t MAX_ZIP_ALLOCATION_BYTES = 16U * 1024U * 1024U;

/// @brief 检查 miniz 的数组分配是否在预算内，避免乘法溢出。
/// @param items 请求的元素数。
/// @param itemSize 单个元素的字节数，可为零。
/// @return 两个参数的乘积可用于一次工作缓冲申请时返回真。
bool zipAllocationFits(std::size_t items, std::size_t itemSize)
{
    return itemSize == 0 || items <= MAX_ZIP_ALLOCATION_BYTES / itemSize;
}

/// @brief miniz 初次申请缓冲，超出中央目录等工作缓冲上限时返回空。
/// @param items 请求的元素数。
/// @param itemSize 单个元素的字节数。
/// @return 由库持有的缓冲，失败返回空并交由库结束读取。
void* allocateZipBuffer(void*, std::size_t items, std::size_t itemSize)
{
    // 先检验上限再相乘，工作缓冲无须由调用者预置内容。
    return zipAllocationFits(items, itemSize) ? std::malloc(items * itemSize)
                                              : nullptr;
}

/// @brief miniz 释放由同一组分配回调创建的工作缓冲。
/// @param buffer miniz 交还的地址，与上面的分配器配对。
void freeZipBuffer(void*, void* buffer)
{
    std::free(buffer);
}

/// @brief miniz 扩大工作缓冲，拒绝时旧缓冲仍由 miniz 管理。
/// @param buffer 当前仍有效的缓冲。
/// @param items 调整后的元素数。
/// @param itemSize 单个元素的字节数。
/// @return 预算内的新地址或空指针，后者保留库的旧地址。
void* reallocateZipBuffer(void*, void* buffer, std::size_t items,
                          std::size_t itemSize)
{
    // 拒绝时不能释放旧指针，miniz 还要沿失败路径归还原缓冲。
    return zipAllocationFits(items, itemSize)
               ? std::realloc(buffer, items * itemSize)
               : nullptr;
}

/// @brief 从已打开的 ZIP 读取借用缓冲区所需的区间。
/// @details ifstream 以 filesystem::path 打开，Windows 使用原生路径字符。
/// @param context 调用方在 reader 生命周期内保有的输入文件流。
/// @param offset miniz 请求的绝对文件偏移。
/// @param buffer 回调借用的输出区间。
/// @param size 输出区间可用字节数。
/// @return 读取的字节数，定位或读取失败时交由 miniz 判为短读。
std::size_t readZipBlock(void* context, mz_uint64 offset, void* buffer,
                         std::size_t size)
{
    auto& input = *static_cast<std::ifstream*>(context);
    // 防止把归档请求的无符号偏移截断成流接口的负值。
    if ( offset > static_cast<mz_uint64>(
                      std::numeric_limits<std::streamoff>::max()) ||
         size > static_cast<std::size_t>(
                    std::numeric_limits<std::streamsize>::max()) ) {
        return 0;
    }
    // 中央目录与本地头可能交替被读取，按绝对偏移重定位而非假定顺序。
    if ( !input.seekg(static_cast<std::streamoff>(offset)) ) return 0;
    input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount());
}

/// @brief 分块解压时记录输出位置、条目预算和取消状态。
/// @details 状态只在同步 miniz 回调期间使用，实际写入量始终不超过声明长度。
struct ZipEntryOutput {
    std::ofstream& m_file;  ///< 当前条目的暂存文件，生命周期覆盖解压回调。
    const std::function<bool()>& m_isCancelled;  ///< 调用者的取消查询。
    mz_uint64 m_expected;           ///< 中央目录宣称并已通过预算校验的字节数。
    mz_uint64 m_written   = 0;      ///< 本条目实际连续写出的字节数。
    bool      m_cancelled = false;  ///< 保留块内取消原因，区分普通解压错误。
};

/// @brief 写出一个解压块，短写使 miniz 停止处理当前条目。
/// @param context 当前条目暂存状态。
/// @param offset 相对于条目开头的偏移。
/// @param buffer miniz 持有的解压后字节区间。
/// @param size 当前区间大小。
/// @return 成功写入的字节数，失败或取消时返回零。
std::size_t writeZipBlock(void* context, mz_uint64 offset, const void* buffer,
                          std::size_t size)
{
    auto& out = *static_cast<ZipEntryOutput*>(context);
    // 保留首次取消信号，即使上游随后清掉标志也不能继续发布部分文件。
    if ( out.m_isCancelled && out.m_isCancelled() ) {
        out.m_cancelled = true;
        return 0;
    }

    // 实际输出必须连续，且不能超过预检计入总额的声明大小。
    if ( offset != out.m_written || size > out.m_expected - out.m_written ||
         size > static_cast<std::size_t>(
                    std::numeric_limits<std::streamsize>::max()) ) {
        return 0;
    }
    // 只向暂存对象写入，原同名文件在整条目校验完成前始终可读。
    if ( !out.m_file.write(static_cast<const char*>(buffer),
                           static_cast<std::streamsize>(size)) ) {
        return 0;
    }
    out.m_written += size;
    return size;
}

/// @brief 解压到同文件系统的独占暂存目录，校验成功后才发布到目标路径。
/// @details 保留已安装同名文件直到字节数、CRC 和关闭写入均成功；失败或取消只
/// 清理当前条目的暂存文件。归档仍按原顺序发布，版本标记由外层事务管理。
/// @param zip 已通过归档预算预检的读取器。
/// @param index 中央目录中当前条目的序号。
/// @param stat 预检时用于确定条目长度的目录记录。
/// @param destination 已经过相对路径和根路径检查的目标文件。
/// @param cancelled 上游可选取消查询。
/// @param error 接收阻止当前条目发布的错误。
/// @return 暂存文件成功移动到目标位置时返回真。
bool extractAssetZipEntry(mz_zip_archive& zip, mz_uint index,
                          const mz_zip_archive_file_stat& stat,
                          const std::filesystem::path&    destination,
                          const std::function<bool()>&    cancelled,
                          std::string&                    error)
{
    std::error_code ec;
    const auto      parent = destination.parent_path();
    // 临时文件与目标共用父目录所在文件系统，发布时只需要重命名。
    if ( !parent.empty() ) std::filesystem::create_directories(parent, ec);
    if ( ec ) {
        error = ec.message();
        return false;
    }

    // 每个 ZIP 条目独占同卷暂存，名称既不取自归档也不被其他任务接管。
    const Config::ScopedTemporaryDirectory staging(parent, ".mmm-zip-entry-");
    if ( staging.error() ) {
        error = staging.error().message();
        return false;
    }
    const auto    temporary = staging.path() / "entry";
    std::ofstream file(temporary, std::ios::binary);
    if ( !file ) {
        error = "Failed to open staging file for asset package entry";
        return false;
    }

    // 使用 callback 而不是 heap 接口，正文缓冲由 miniz 按解码工作块复用。
    ZipEntryOutput output{ file, cancelled, stat.m_uncomp_size };
    const bool     extracted = mz_zip_reader_extract_to_callback(
        &zip, index, writeZipBlock, &output, 0);
    // 显式检查关闭时的缓冲写入错误，避免把未完整落盘的文件重命名发布。
    file.close();
    // 即使最后一个回调之后才取消，也不能越过本次发布点。
    if ( output.m_cancelled || (cancelled && cancelled()) ) {
        error = "Asset sync cancelled";
        return false;
    }
    // CRC、输出长度、磁盘写入及关闭必须同时成功，任一失败都保留已装文件。
    if ( !extracted || output.m_written != output.m_expected || !file ) {
        error = std::string("Failed to extract asset package entry: ") +
                stat.m_filename;
        return false;
    }
    // 失败时不退化成覆盖式复制，避免破坏现有资源的单条发布边界。
    std::filesystem::rename(temporary, destination, ec);
    if ( ec ) {
        error = ec.message();
        return false;
    }
    return true;
}

}  // namespace

/// @brief 检查资源 ZIP 的条目和展开总量，将各文件分块解压到目标根下。
/// @details 读取器只按需读取 ZIP 区间。先预检所有条目大小，再处理路径和输出。
/// 条目名经过相对路径检查及真实祖先检查，单条文件经 CRC 校验后才替换同名资源。
/// @param zipPath 已下载或用户选中的 ZIP 文件。
/// @param destinationRoot 归档相对路径的唯一写入根。
/// @param errorMessage 接收取消、格式、路径、预算或写入错误。
/// @param cancellationCallback 可选取消查询，在条目之间与输出块之间调用。
/// @return 所有条目成功写入时返回 true，由调用方决定何时发布资源版本。
bool AssetSyncService::extractZipArchive(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destinationRoot, std::string& errorMessage,
    const std::function<bool()>& cancellationCallback)
{
    errorMessage.clear();
    if ( cancellationCallback && cancellationCallback() ) {
        errorMessage = "Asset sync cancelled";
        return false;
    }

    // 以二进制方式获取同一文件句柄的长度，避免 Windows 文本转换影响偏移。
    std::ifstream        input(zipPath, std::ios::binary | std::ios::ate);
    const std::streamoff size = input.tellg();
    // 打开或定位失败时不向 miniz 传递未知长度，也不创建输出目录。
    if ( !input || size < 0 ) {
        errorMessage = "Failed to read asset package";
        return false;
    }

    // 打开时会建立中央目录，先安装工作缓冲上限再允许库读取该结构。
    mz_zip_archive zipArchive{};
    zipArchive.m_pRead      = readZipBlock;
    zipArchive.m_pIO_opaque = &input;
    zipArchive.m_pAlloc     = allocateZipBuffer;
    zipArchive.m_pFree      = freeZipBuffer;
    zipArchive.m_pRealloc   = reallocateZipBuffer;
    // 只按中央目录序号访问，无需建立用于按文件名查找的排序索引。
    if ( !mz_zip_reader_init(&zipArchive,
                             static_cast<mz_uint64>(size),
                             MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY) ) {
        errorMessage = "Failed to open asset package";
        return false;
    }
    // 作用域退出时关闭读取器，input 在读取器销毁之后才关闭。
    const std::unique_ptr<mz_zip_archive, decltype(&mz_zip_reader_end)> reader(
        &zipArchive, &mz_zip_reader_end);

    // 文件数直接取自已解析目录，包括不会生成输出的空目录记录。
    const mz_uint fileCount = mz_zip_reader_get_num_files(&zipArchive);
    // 数量拒绝放在第一次遍历之前，包括只含空目录的包。
    if ( fileCount > MAX_ASSET_ARCHIVE_ENTRIES ) {
        errorMessage = "Asset package contains too many entries";
        return false;
    }
    mz_uint64 totalSize = 0;
    // 先扫中央目录，确保后部超预算条目不会让前部文件先消耗磁盘。
    for ( mz_uint index = 0; index < fileCount; ++index ) {
        if ( cancellationCallback && cancellationCallback() ) {
            errorMessage = "Asset sync cancelled";
            return false;
        }
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&zipArchive, index, &fileStat) ) {
            errorMessage = "Failed to read asset package entry";
            return false;
        }
        // 先与剩余预算比较再累加，避免异常声明发生无符号回绕。
        if ( fileStat.m_uncomp_size > MAX_ASSET_ENTRY_BYTES ||
             fileStat.m_uncomp_size >
                 MAX_ASSET_ARCHIVE_OUTPUT_BYTES - totalSize ) {
            errorMessage =
                std::string("Asset package exceeds extraction budget: ") +
                fileStat.m_filename;
            return false;
        }
        // 重复输出路径仍逐条计费，累计写磁盘与最终占用可能不同。
        totalSize += fileStat.m_uncomp_size;
    }

    std::error_code rootError;
    // 用户选定的解压根可以经由配置链接到另一个卷。只信任解析后的根，
    // 归档提供的相对条目不应再次引入重定向。
    const auto root = AssetPaths::resolveRoot(destinationRoot, rootError);
    if ( rootError ) {
        errorMessage = rootError.message();
        return false;
    }
    // 通过全部规模限制后再写入，使用原归档顺序维护同名条目的覆盖语义。
    // 逐次读取目录项避免另存一份条目向量或路径副本。
    for ( mz_uint index = 0; index < fileCount; ++index ) {
        if ( cancellationCallback && cancellationCallback() ) {
            errorMessage = "Asset sync cancelled";
            return false;
        }
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&zipArchive, index, &fileStat) ) {
            errorMessage = "Failed to read asset package entry";
            return false;
        }
        // 从与预检相同的中央目录取名称和长度，保持预算与实际条目对应。
        std::string archiveName = fileStat.m_filename;
        // 目录标记不参与相对路径分段判断；空名称不产生文件系统操作。
        while ( !archiveName.empty() && archiveName.back() == '/' )
            archiveName.pop_back();
        if ( archiveName.empty() ) continue;
        // 清单和归档复用同一套相对路径规则，拒绝平台相关的路径别名。
        if ( !AssetPaths::isSafeRelativeAssetPath(archiveName) ) {
            errorMessage = "Unsafe path in asset package: " + archiveName;
            return false;
        }
        // 相对名称已经按 ZIP 的分隔符协议验证，实际文件系统仍接收原生编码。
        const auto relative    = Config::utf8ToPath(archiveName);
        const auto destination = root / relative;
        // 完整包和增量检查同一套真实目标规则，已有链接不会成为安装入口。
        if ( !AssetPaths::isUnredirectedDestination(
                 root, relative, rootError) ) {
            errorMessage = "Unsafe asset package destination: " + archiveName +
                           ": " + rootError.message();
            return false;
        }

        // 显式目录条目仅保留层级，本身没有交给解码器的文件正文。
        if ( mz_zip_reader_is_file_a_directory(&zipArchive, index) ) {
            std::error_code ec;
            std::filesystem::create_directories(destination, ec);
            if ( ec ) {
                errorMessage = ec.message();
                return false;
            }
        } else if ( !extractAssetZipEntry(zipArchive,
                                          index,
                                          fileStat,
                                          destination,
                                          cancellationCallback,
                                          errorMessage) ) {
            return false;
        }
    }
    return true;
}

}  // namespace MMM::Network
