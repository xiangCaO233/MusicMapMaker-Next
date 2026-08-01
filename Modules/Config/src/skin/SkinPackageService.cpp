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
constexpr std::uintmax_t kMaximumPackageSize = 256U * 1024U * 1024U;

/// @brief MSK 包允许的最大文件数量。
constexpr mz_uint kMaximumArchiveEntryCount = 8192U;

/// @brief MSK 包允许的单文件最大解压大小。
constexpr std::uint64_t kMaximumEntrySize = 128U * 1024U * 1024U;

/// @brief MSK 包允许的全部文件最大解压大小。
constexpr std::uint64_t kMaximumExtractedSize = 512U * 1024U * 1024U;

/// @brief 导入临时目录自动清理器。
struct TemporaryDirectory {
    std::filesystem::path path;  ///< 需要在离开作用域时清理的目录。

    /// @brief 清理临时目录及其全部内容。
    ~TemporaryDirectory()
    {
        if ( path.empty() ) return;
        std::error_code removeError;
        std::filesystem::remove_all(path, removeError);
    }
};

/// @brief 判断扩展名是否为 MSK，匹配时忽略 ASCII 大小写。
bool hasMskExtension(const std::filesystem::path& path)
{
    std::string extension = pathToUtf8(path.extension());
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".msk";
}

/// @brief 读取完整二进制文件并限制最大大小。
bool readFileBytes(const std::filesystem::path& path, std::uintmax_t sizeLimit,
                   std::vector<std::uint8_t>& bytes)
{
    bytes.clear();

    std::error_code fileSizeError;
    const auto      fileSize = std::filesystem::file_size(path, fileSizeError);
    if ( fileSizeError || fileSize > sizeLimit ||
         fileSize > static_cast<std::uintmax_t>(
                        std::numeric_limits<std::size_t>::max()) ) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if ( !input ) return false;

    bytes.resize(static_cast<std::size_t>(fileSize));
    if ( !bytes.empty() ) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    return input.good() || input.eof();
}

/// @brief 将字节写入文件，并按需创建父目录。
bool writeFileBytes(const std::filesystem::path& path, const void* data,
                    std::size_t size)
{
    const auto parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        std::error_code createError;
        std::filesystem::create_directories(parentPath, createError);
        if ( createError ) return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    if ( size > 0 ) {
        output.write(static_cast<const char*>(data),
                     static_cast<std::streamsize>(size));
    }
    return output.good();
}

/// @brief 判断压缩包相对路径是否安全。
bool isSafeArchivePath(std::string_view path)
{
    if ( path.empty() || path.front() == '/' || path.front() == '\\' ||
         path.find(':') != std::string_view::npos ) {
        return false;
    }

    std::size_t start = 0;
    while ( start <= path.size() ) {
        const std::size_t end = path.find('/', start);
        const auto        segment =
            path.substr(start,
                        end == std::string_view::npos ? std::string_view::npos
                                                      : end - start);
        if ( segment.empty() || segment == "." || segment == ".." ) {
            return false;
        }
        if ( end == std::string_view::npos ) break;
        start = end + 1;
    }
    return true;
}

/// @brief 判断目标路径是否仍位于指定根目录内。
bool isPathInsideRoot(const std::filesystem::path& root,
                      const std::filesystem::path& target)
{
    const auto relative =
        target.lexically_normal().lexically_relative(root.lexically_normal());
    if ( relative.empty() || relative.is_absolute() ) return false;
    for ( const auto& part : relative ) {
        if ( part == ".." ) return false;
    }
    return true;
}

/// @brief 创建不存在的唯一临时目录。
std::filesystem::path createUniqueDirectory(const std::filesystem::path& parent,
                                            std::string_view             prefix,
                                            std::string& errorMessage)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for ( unsigned int attempt = 0; attempt < 32U; ++attempt ) {
        const auto candidate =
            parent / utf8ToPath(std::string(prefix) + std::to_string(stamp) +
                                "_" + std::to_string(attempt));
        std::error_code createError;
        if ( std::filesystem::create_directories(candidate, createError) ) {
            return candidate;
        }
        if ( createError ) {
            errorMessage = "无法创建临时目录: " + createError.message();
            return {};
        }
    }
    errorMessage = "无法分配唯一临时目录";
    return {};
}

/// @brief 解压 MSK 包到临时目录并阻止目录穿越和过量展开。
bool extractPackage(const std::filesystem::path& packagePath,
                    const std::filesystem::path& destinationRoot,
                    std::string&                 errorMessage)
{
    std::vector<std::uint8_t> packageBytes;
    if ( !readFileBytes(packagePath, kMaximumPackageSize, packageBytes) ||
         packageBytes.empty() ) {
        errorMessage = "无法读取 MSK 文件，或文件超过 256 MiB";
        return false;
    }

    mz_zip_archive archive{};
    if ( !mz_zip_reader_init_mem(
             &archive, packageBytes.data(), packageBytes.size(), 0) ) {
        errorMessage = "MSK 文件不是有效的 ZIP 压缩包";
        return false;
    }

    bool          success        = true;
    std::uint64_t extractedTotal = 0;
    const mz_uint entryCount     = mz_zip_reader_get_num_files(&archive);
    if ( entryCount == 0 || entryCount > kMaximumArchiveEntryCount ) {
        errorMessage = "MSK 文件数量为空或超过限制";
        success      = false;
    }

    for ( mz_uint index = 0; success && index < entryCount; ++index ) {
        mz_zip_archive_file_stat fileStat{};
        if ( !mz_zip_reader_file_stat(&archive, index, &fileStat) ) {
            errorMessage = "无法读取 MSK 压缩条目";
            success      = false;
            break;
        }

        std::string archiveName = fileStat.m_filename;
        std::ranges::replace(archiveName, '\\', '/');
        while ( !archiveName.empty() && archiveName.back() == '/' ) {
            archiveName.pop_back();
        }
        if ( archiveName.empty() ) continue;

        if ( !isSafeArchivePath(archiveName) ) {
            errorMessage = "MSK 包含不安全路径: " + archiveName;
            success      = false;
            break;
        }

        const auto destinationPath =
            (destinationRoot / utf8ToPath(archiveName)).lexically_normal();
        if ( !isPathInsideRoot(destinationRoot, destinationPath) ) {
            errorMessage = "MSK 路径越过解压目录: " + archiveName;
            success      = false;
            break;
        }

        if ( mz_zip_reader_is_file_a_directory(&archive, index) ) {
            std::error_code createError;
            std::filesystem::create_directories(destinationPath, createError);
            if ( createError ) {
                errorMessage = "无法创建 MSK 目录: " + createError.message();
                success      = false;
            }
            continue;
        }

        const std::uint64_t entrySize = fileStat.m_uncomp_size;
        if ( entrySize > kMaximumEntrySize ||
             extractedTotal > kMaximumExtractedSize - entrySize ) {
            errorMessage = "MSK 解压后的文件大小超过限制";
            success      = false;
            break;
        }
        extractedTotal += entrySize;

        std::size_t extractedSize = 0;
        void*       extractedData =
            mz_zip_reader_extract_to_heap(&archive, index, &extractedSize, 0);
        if ( (!extractedData && entrySize != 0) ||
             extractedSize != entrySize ) {
            errorMessage = "无法解压 MSK 文件: " + archiveName;
            success      = false;
            break;
        }
        success = writeFileBytes(destinationPath, extractedData, extractedSize);
        if ( extractedData ) mz_free(extractedData);
        if ( !success ) {
            errorMessage = "无法写入 MSK 文件: " + archiveName;
        }
    }

    mz_zip_reader_end(&archive);
    return success;
}

/// @brief 查找解压目录中唯一的 skin.lua 文件。
std::filesystem::path findUniqueSkinLua(
    const std::filesystem::path& extractedRoot, std::string& errorMessage)
{
    std::filesystem::path                         result;
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        extractedRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if ( iteratorError ) {
        errorMessage = "无法扫描解压后的 MSK 目录: " + iteratorError.message();
        return {};
    }

    while ( iterator != end ) {
        std::error_code typeError;
        const bool      isRegular = iterator->is_regular_file(typeError);
        if ( typeError ) {
            errorMessage = "无法检查 MSK 文件类型: " + typeError.message();
            return {};
        }
        if ( isRegular && iterator->path().filename() == "skin.lua" ) {
            if ( !result.empty() ) {
                errorMessage = "MSK 中存在多个 skin.lua，无法确定皮肤根目录";
                return {};
            }
            result = iterator->path();
        }

        iterator.increment(iteratorError);
        if ( iteratorError ) {
            errorMessage = "无法继续扫描 MSK 目录: " + iteratorError.message();
            return {};
        }
    }

    if ( result.empty() ) {
        errorMessage = "MSK 中未找到 skin.lua";
    }
    return result;
}

/// @brief 验证皮肤安装目录名是单个安全路径分量。
bool isSafeDirectoryName(const std::filesystem::path& name)
{
    return !name.empty() && name != "." && name != ".." &&
           !name.has_parent_path() && !name.has_root_path();
}

/// @brief 将目录内容复制到新建的安装暂存目录。
bool copyDirectoryContents(const std::filesystem::path& sourceRoot,
                           const std::filesystem::path& destinationRoot,
                           std::string&                 errorMessage)
{
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        sourceRoot, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if ( iteratorError ) {
        errorMessage = "无法读取皮肤目录: " + iteratorError.message();
        return false;
    }

    while ( iterator != end ) {
        const auto relativePath =
            iterator->path().lexically_relative(sourceRoot);
        const auto destinationPath =
            (destinationRoot / relativePath).lexically_normal();
        if ( relativePath.empty() ||
             !isPathInsideRoot(destinationRoot, destinationPath) ) {
            errorMessage = "皮肤目录包含无效路径";
            return false;
        }

        std::error_code statusError;
        const auto      status = iterator->symlink_status(statusError);
        if ( statusError ) {
            errorMessage = "无法读取皮肤文件状态: " + statusError.message();
            return false;
        }

        std::error_code operationError;
        if ( std::filesystem::is_directory(status) ) {
            std::filesystem::create_directories(destinationPath,
                                                operationError);
        } else if ( std::filesystem::is_regular_file(status) ) {
            std::filesystem::create_directories(destinationPath.parent_path(),
                                                operationError);
            if ( !operationError ) {
                std::filesystem::copy_file(iterator->path(),
                                           destinationPath,
                                           std::filesystem::copy_options::none,
                                           operationError);
            }
        } else {
            errorMessage = "皮肤目录包含不支持的文件类型";
            return false;
        }

        if ( operationError ) {
            errorMessage = "无法安装皮肤文件: " + operationError.message();
            return false;
        }

        iterator.increment(iteratorError);
        if ( iteratorError ) {
            errorMessage = "无法继续复制皮肤目录: " + iteratorError.message();
            return false;
        }
    }
    return true;
}

/// @brief 收集皮肤目录中可导出的普通文件。
bool collectSkinFiles(const std::filesystem::path&        skinDirectory,
                      std::vector<std::filesystem::path>& files,
                      std::string&                        errorMessage)
{
    files.clear();
    std::uint64_t                                 totalSize = 0;
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        skinDirectory, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if ( iteratorError ) {
        errorMessage = "无法读取皮肤目录: " + iteratorError.message();
        return false;
    }

    while ( iterator != end ) {
        std::error_code statusError;
        const auto      status = iterator->symlink_status(statusError);
        if ( statusError ) {
            errorMessage = "无法读取皮肤文件状态: " + statusError.message();
            return false;
        }

        if ( std::filesystem::is_regular_file(status) ) {
            std::error_code sizeError;
            const auto      fileSize =
                std::filesystem::file_size(iterator->path(), sizeError);
            if ( sizeError || fileSize > kMaximumEntrySize ||
                 totalSize > kMaximumExtractedSize - fileSize ) {
                errorMessage = "皮肤文件大小超过 MSK 限制";
                return false;
            }
            totalSize += fileSize;
            files.push_back(iterator->path());
            if ( files.size() > kMaximumArchiveEntryCount ) {
                errorMessage = "皮肤文件数量超过 MSK 限制";
                return false;
            }
        } else if ( !std::filesystem::is_directory(status) ) {
            errorMessage = "皮肤目录包含不支持的文件类型";
            return false;
        }

        iterator.increment(iteratorError);
        if ( iteratorError ) {
            errorMessage = "无法继续读取皮肤目录: " + iteratorError.message();
            return false;
        }
    }

    std::ranges::sort(files, [](const auto& left, const auto& right) {
        return pathToUtf8Generic(left) < pathToUtf8Generic(right);
    });
    return !files.empty();
}

/// @brief 以可恢复方式将临时文件替换为最终输出文件。
bool replaceOutputFile(const std::filesystem::path& temporaryPath,
                       const std::filesystem::path& outputPath,
                       std::string&                 errorMessage)
{
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto backupPath = outputPath.parent_path() /
                            utf8ToPath(pathToUtf8(outputPath.filename()) +
                                       ".backup-" + std::to_string(stamp));

    std::error_code existsError;
    const bool outputExists = std::filesystem::exists(outputPath, existsError);
    if ( existsError ) {
        errorMessage = "无法检查输出文件: " + existsError.message();
        return false;
    }

    if ( outputExists ) {
        std::error_code typeError;
        if ( !std::filesystem::is_regular_file(outputPath, typeError) ||
             typeError ) {
            errorMessage = "输出路径不是普通文件";
            return false;
        }

        std::error_code backupError;
        std::filesystem::rename(outputPath, backupPath, backupError);
        if ( backupError ) {
            errorMessage = "无法暂存已有 MSK 文件: " + backupError.message();
            return false;
        }
    }

    std::error_code renameError;
    std::filesystem::rename(temporaryPath, outputPath, renameError);
    if ( renameError ) {
        if ( outputExists ) {
            std::error_code restoreError;
            std::filesystem::rename(backupPath, outputPath, restoreError);
        }
        errorMessage = "无法写入 MSK 文件: " + renameError.message();
        return false;
    }

    if ( outputExists ) {
        std::error_code removeError;
        std::filesystem::remove(backupPath, removeError);
    }
    return true;
}

}  // namespace

SkinPackageImportResult SkinPackageService::importPackage(
    const std::filesystem::path& packagePath,
    const std::filesystem::path& skinsRoot)
{
    SkinPackageImportResult result;
    if ( !hasMskExtension(packagePath) ) {
        result.errorMessage = "请选择 .msk 皮肤包";
        return result;
    }

    std::error_code packageTypeError;
    if ( !std::filesystem::is_regular_file(packagePath, packageTypeError) ||
         packageTypeError ) {
        result.errorMessage = "MSK 文件不存在或不可读取";
        return result;
    }

    std::error_code createRootError;
    std::filesystem::create_directories(skinsRoot, createRootError);
    if ( createRootError ) {
        result.errorMessage =
            "无法创建用户 skins 目录: " + createRootError.message();
        return result;
    }

    std::error_code tempPathError;
    auto tempParent = std::filesystem::temp_directory_path(tempPathError);
    if ( tempPathError ) {
        result.errorMessage =
            "无法访问系统临时目录: " + tempPathError.message();
        return result;
    }

    TemporaryDirectory extraction;
    extraction.path = createUniqueDirectory(
        tempParent, "mmm_skin_import_", result.errorMessage);
    if ( extraction.path.empty() ||
         !extractPackage(packagePath, extraction.path, result.errorMessage) ) {
        return result;
    }

    const auto skinLua =
        findUniqueSkinLua(extraction.path, result.errorMessage);
    if ( skinLua.empty() ) return result;

    const auto sourceDirectory = skinLua.parent_path();
    auto       directoryName   = sourceDirectory.filename();
    if ( sourceDirectory == extraction.path || directoryName.empty() ) {
        directoryName = packagePath.stem();
    }
    if ( !isSafeDirectoryName(directoryName) ) {
        result.errorMessage = "无法从 MSK 确定安全的皮肤目录名";
        return result;
    }

    const auto      destination = skinsRoot / directoryName;
    std::error_code destinationExistsError;
    if ( std::filesystem::exists(destination, destinationExistsError) ||
         destinationExistsError ) {
        result.errorMessage =
            destinationExistsError
                ? "无法检查皮肤安装目录: " + destinationExistsError.message()
                : "同名皮肤目录已存在";
        return result;
    }

    const std::string stagingPrefix =
        ".mmm-skin-import-" + pathToUtf8(directoryName) + "-";
    TemporaryDirectory staging;
    staging.path =
        createUniqueDirectory(skinsRoot, stagingPrefix, result.errorMessage);
    if ( staging.path.empty() ||
         !copyDirectoryContents(
             sourceDirectory, staging.path, result.errorMessage) ) {
        return result;
    }

    std::error_code installedSkinError;
    if ( !std::filesystem::is_regular_file(staging.path / "skin.lua",
                                           installedSkinError) ||
         installedSkinError ) {
        result.errorMessage = "导入后的皮肤根目录缺少 skin.lua";
        return result;
    }

    std::error_code renameError;
    std::filesystem::rename(staging.path, destination, renameError);
    if ( renameError ) {
        result.errorMessage = "无法完成皮肤安装: " + renameError.message();
        return result;
    }
    staging.path.clear();

    result.success            = true;
    result.skinDirectoryName  = pathToUtf8(directoryName);
    result.installedDirectory = destination;
    return result;
}

bool SkinPackageService::exportPackage(
    const std::filesystem::path& skinDirectory,
    const std::filesystem::path& outputPath, std::string& errorMessage)
{
    errorMessage.clear();
    if ( !hasMskExtension(outputPath) ) {
        errorMessage = "导出文件必须使用 .msk 扩展名";
        return false;
    }

    std::error_code skinLuaError;
    if ( !std::filesystem::is_regular_file(skinDirectory / "skin.lua",
                                           skinLuaError) ||
         skinLuaError ) {
        errorMessage = "皮肤目录中未找到 skin.lua";
        return false;
    }

    const auto directoryName = skinDirectory.lexically_normal().filename();
    if ( !isSafeDirectoryName(directoryName) ) {
        errorMessage = "皮肤目录名无效";
        return false;
    }

    std::vector<std::filesystem::path> files;
    if ( !collectSkinFiles(skinDirectory, files, errorMessage) ) {
        if ( errorMessage.empty() ) errorMessage = "皮肤目录中没有可导出文件";
        return false;
    }

    mz_zip_archive archive{};
    if ( !mz_zip_writer_init_heap(&archive, 0, 0) ) {
        errorMessage = "无法初始化 MSK 压缩器";
        return false;
    }

    bool                      success = true;
    std::vector<std::uint8_t> fileBytes;
    for ( const auto& file : files ) {
        if ( !readFileBytes(file, kMaximumEntrySize, fileBytes) ) {
            errorMessage = "无法读取皮肤文件: " + pathToUtf8(file);
            success      = false;
            break;
        }

        const auto        relativePath = file.lexically_relative(skinDirectory);
        const auto        archivePath  = directoryName / relativePath;
        const std::string archiveName  = pathToUtf8Generic(archivePath);
        if ( !isSafeArchivePath(archiveName) ||
             !mz_zip_writer_add_mem(&archive,
                                    archiveName.c_str(),
                                    fileBytes.data(),
                                    fileBytes.size(),
                                    MZ_BEST_COMPRESSION) ) {
            errorMessage = "无法压缩皮肤文件: " + archiveName;
            success      = false;
            break;
        }
    }

    void*       archiveBuffer = nullptr;
    std::size_t archiveSize   = 0;
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &archive, &archiveBuffer, &archiveSize) ) {
        errorMessage = "无法完成 MSK 压缩";
        success      = false;
    }
    mz_zip_writer_end(&archive);
    if ( success && archiveSize > kMaximumPackageSize ) {
        errorMessage = "导出的 MSK 文件超过 256 MiB";
        success      = false;
    }

    std::filesystem::path temporaryOutput;
    if ( success ) {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        temporaryOutput = outputPath.parent_path() /
                          utf8ToPath(pathToUtf8(outputPath.filename()) +
                                     ".writing-" + std::to_string(stamp));
        success = writeFileBytes(temporaryOutput, archiveBuffer, archiveSize);
        if ( !success ) errorMessage = "无法写入临时 MSK 文件";
    }
    if ( archiveBuffer ) mz_free(archiveBuffer);

    if ( success ) {
        success = replaceOutputFile(temporaryOutput, outputPath, errorMessage);
    }
    if ( !success && !temporaryOutput.empty() ) {
        std::error_code removeError;
        std::filesystem::remove(temporaryOutput, removeError);
    }
    return success;
}

}  // namespace MMM::Config
