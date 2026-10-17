#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/ProjectController.h"
#include "mmm/project/PackageFileTypes.h"

#include <archive.h>
#include <archive_entry.h>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace
{
/// @brief 创建小型归档，复用正式链接的压缩库而不依赖外部命令。
/// @param path 输出位置，必须属于构建目录。
/// @param format libarchive 格式标识。
/// @param filter 压缩过滤器标识。
/// @param name 包内路径，用于正常与越界样本。
/// @param link 非空时生成符号链接，验证解包拒绝策略。
/// @return 头、数据、尾部均成功写出时返回 true。
bool makeArchive(const std::filesystem::path& path, int format, int filter,
                 const char* name, const char* link = nullptr)
{
    // 句柄只存在于当前测试，写出失败也配对释放。
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(
        archive_write_new(), archive_write_free);
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), archive_entry_free);
    // 两个句柄都必须有效，避免测试夹具生成失败被误报为解压器故障。
    if ( !writer || !entry ) return false;
    // 格式与过滤器独立设置，可以分别验证容器与外层压缩。
    // setup 返回 WARN 也不能继续，否则可能生成无压缩样本冒充压缩覆盖。
    if ( archive_write_set_format(writer.get(), format) != ARCHIVE_OK ||
         archive_write_add_filter(writer.get(), filter) != ARCHIVE_OK )
        return false;
    // 打开输出文件使用原生 Windows 路径，覆盖中文目录下的包名。
#ifdef _WIN32
    const auto result =
        archive_write_open_filename_w(writer.get(), path.c_str());
#else
    const auto result = archive_write_open_filename(writer.get(), path.c_str());
#endif
    // 测试目录应可写；打开失败直接结束，不能沿用上次残留文件。
    if ( result != ARCHIVE_OK ) return false;
    // 内容包含换行且与文件名独立，避免仅检查存在性漏过数据损坏。
    const std::string payload = "#TITLE archive regression\n#BPM 180\n";
    // 写出前测试进程已选择 UTF-8 locale，使 7z 能生成 UTF-16 名称字段。
    // 读取阶段另切换到 C locale，不能依靠夹具生成时的环境掩盖读取缺陷。
    archive_entry_set_pathname(entry.get(), name);
    archive_entry_set_filetype(entry.get(), link ? AE_IFLNK : AE_IFREG);
    // 固定权限只用于生成普通条目，生产解包不会恢复这些元数据。
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(),
                           link ? 0 : static_cast<la_int64_t>(payload.size()));
    // 链接样本的目标刻意越出根目录，不提供正常文件体。
    if ( link ) archive_entry_set_symlink(entry.get(), link);
    // 头部失败不能继续写数据，否则得到的错误归档无法验证预期边界。
    if ( archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK )
        return false;
    // 链接没有数据体，普通条目必须完整写入后再完成归档尾部。
    if ( !link &&
         archive_write_data(writer.get(), payload.data(), payload.size()) !=
             static_cast<la_ssize_t>(payload.size()) )
        return false;
    // 尾部包含索引与校验信息，必须关闭成功后才能交给读取测试。
    return archive_write_close(writer.get()) == ARCHIVE_OK;
}

/// @brief 经正式准备入口解包并核对内容，不切换用户项目。
/// @param path 待测归档路径。
/// @param relative 要核对的包内相对路径。
/// @param expected 预期完整字节内容。
/// @return 解包和清理均成功且内容一致时返回 true。
bool checkExtracted(const std::filesystem::path& path,
                    const std::filesystem::path& relative,
                    const std::string&           expected)
{
    // 所有归档均走同一生产入口，测试不直接调用库替代业务路径。
    // 使用 C locale 模拟未设置进程区域的启动环境，读取不能依赖全局 UTF-8
    // locale。
    // 保存名称副本，setlocale 返回的内部字符串会在后续切换时被覆盖。
    const std::string previousLocale = std::setlocale(LC_CTYPE, nullptr);
    std::setlocale(LC_CTYPE, "C");
    // 控制器失败时还负责回收本次临时目录；测试检查不能返回可用路径。
    auto result = MMM::Logic::ProjectController::instance()
                      .prepareTemporaryProjectPackage(path);
    // 无论准备成功与否都恢复测试原环境，使下一种容器的生成条件一致。
    std::setlocale(LC_CTYPE, previousLocale.c_str());
    if ( !result.m_success ) {
        XERROR("Archive failed: {}: {}",
               MMM::Config::pathToUtf8(path),
               result.m_errorMessage);
        return false;
    }
    const auto        root = result.m_temporaryInfo.m_cacheProjectPath;
    std::ifstream     input(root / relative, std::ios::binary);
    const std::string content{ std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>() };
    // 读取器关闭后清理本次缓存，绝不删除传入的源样本。
    // Windows 下读取句柄关闭后才能删除测试文件。
    input.close();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    // 清理失败与数据错误都使测试失败，不能让运行次数掩盖输出残留。
    if ( content != expected || ec ) {
        XERROR("Archive content mismatch: {}", MMM::Config::pathToUtf8(path));
        return false;
    }
    return true;
}

/// @brief 校验失败必须带原因，不能留下一个假成功的临时项目。
/// @param path 不安全或损坏的归档。
/// @return 被明确拒绝时返回 true。
bool checkRejected(const std::filesystem::path& path)
{
    auto result = MMM::Logic::ProjectController::instance()
                      .prepareTemporaryProjectPackage(path);
    if ( result.m_success ) {
        // 意外成功仍需清理，避免失败测试持续堆积输出。
        std::error_code ec;
        std::filesystem::remove_all(result.m_temporaryInfo.m_cacheProjectPath,
                                    ec);
        XERROR("Unsafe archive accepted: {}", MMM::Config::pathToUtf8(path));
        return false;
    }
    // 空错误会重现用户看到的无反应，因此错误消息本身也是验收条件。
    return !result.m_errorMessage.empty() &&
           result.m_temporaryInfo.m_cacheProjectPath.empty();
}
}  // namespace

/// @brief 验证真实归档解码、数据完整性和失败反馈。
/// @param argc 必须提供资源根及构建输出根。
/// @param argv 不读取用户项目，仅操作测试私有输出目录。
/// @return 所有子项通过时返回零。
int main(int argc, char** argv)
{
    // 资源根和输出根是不同职责，必须由 CTest 显式传入。
    if ( argc != 3 ) return 1;
    // libarchive 写入器依赖 LC_CTYPE 转换 UTF-16 文件名；仅影响独立测试进程。
    // 夹具生成固定使用 UTF-8，不依赖 Windows 用户默认 ANSI 代码页。
// 环境缺少所需 locale 时应明确失败，不能生成乱码夹具后误报解码问题。
#ifdef _WIN32
    if ( !std::setlocale(LC_CTYPE, ".UTF8") ) return 1;
#else
    // macOS 通常使用 en_US.UTF-8，Linux 优先使用不依赖地区包的 C.UTF-8。
    if ( !std::setlocale(LC_CTYPE, "C.UTF-8") &&
         !std::setlocale(LC_CTYPE, "en_US.UTF-8") )
        return 1;
#endif
    // 上游 RAR 文件通过 LFS 保存，运行中不改写资源文件。
    const auto fixtures = std::filesystem::path(argv[1]) / "archives";
    const auto output   = std::filesystem::path(argv[2]) / "archive_packages";
    std::error_code ec;
    // 不清理整个构建树，只创建本测试所需目录。
    std::filesystem::create_directories(output, ec);
    if ( ec ) return 1;
    const auto        name    = MMM::Config::utf8ToPath("子目录/测试.bms");
    const std::string payload = "#TITLE archive regression\n#BPM 180\n";
    // 所有主要容器均检验实际文件内容；大写后缀同时验证公共识别规则。
    // 大写扩展名必须进入同一读取流程，而不是被 UI 当成普通文件。
    const auto sevenZip = output / MMM::Config::utf8ToPath("中文归档.7Z");
    // 谱面文件本身不能被归档路由抢走，BMS 保留原先直接打开行为。
    if ( !MMM::isProjectArchiveExtension(".RAR") ||
         MMM::isProjectArchiveExtension(".bms") )
        return 1;
    // 使用真正的 7z 容器，不能只把 ZIP 文件改后缀作为样本。
    if ( !makeArchive(sevenZip,
                      ARCHIVE_FORMAT_7ZIP,
                      ARCHIVE_FILTER_NONE,
                      "子目录/测试.bms") ||
         !checkExtracted(sevenZip, name, payload) )
        return 1;
    // 每种 TAR 过滤器分别生成样本，不能只用无压缩归档冒充格式覆盖。
    for ( const auto& [suffix, filter] :
          { std::pair{ ".tar", ARCHIVE_FILTER_NONE },
            { ".tar.gz", ARCHIVE_FILTER_GZIP },
            { ".tar.bz2", ARCHIVE_FILTER_BZIP2 },
            { ".tar.xz", ARCHIVE_FILTER_XZ },
            { ".tar.zst", ARCHIVE_FILTER_ZSTD } } ) {
        // 每个过滤器使用独立文件名，防止某次失败误读前一格式的成功产物。
        const auto path = output / (std::string("sample") + suffix);
        if ( !makeArchive(path,
                          ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
                          filter,
                          "子目录/测试.bms") ||
             !checkExtracted(path, name, payload) )
            return 1;
    }
    // 固定样本测试独立读取能力，避免测试生成器与读取器共享同一种错误。
    // 测试资源由 LFS 提供，测试过程中不会改写原始样本。
    // 解包输出始终使用生产入口分配的缓存根。
    // 每次检查后清理该次缓存，防止旧文件让后续断言误通过。
    // RAR 没有开放写入器，读取上游固定版本的真实样本。
    // Windows RAR 样本还包含空目录，生产入口应保留目录层次。
    if ( !checkExtracted(fixtures / "test_read_format_rar_windows.rar",
                         "test.txt",
                         "test text file\r\n") )
        return 1;
    // RAR5 的样本包含 1200 个字节；校验完整内容而非只校验文件长度。
    std::ifstream     expectedFile(fixtures / "rar5-expected.bin",
                                   std::ios::binary);
    const std::string expected{ std::istreambuf_iterator<char>(expectedFile),
                                std::istreambuf_iterator<char>() };
    // 缺少 LFS 对象或期望文件被替换为文本指针时必须明确失败。
    if ( expected.size() != 1200 ||
         !checkExtracted(fixtures / "test_read_format_rar5_compressed.rar",
                         "test.bin",
                         expected) )
        return 1;
    // TAR 的 traversal 和 symlink 是不同攻击路径，分别拒绝。
    // 越界样本即使可以被 libarchive 正常读取，也必须被业务路径校验拒绝。
    const auto traversal = output / "traversal.tar";
    const auto symlink   = output / "symlink.tar";
    // 该条目不能在清理缓存前或后逃逸到缓存外部。
    if ( !makeArchive(traversal,
                      ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
                      ARCHIVE_FILTER_NONE,
                      "../escaped.bms") ||
         !checkRejected(traversal) )
        return 1;
    // 不依赖宿主平台能否创建 symlink，拒绝发生在创建文件之前。
    if ( !makeArchive(symlink,
                      ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
                      ARCHIVE_FILTER_NONE,
                      "link",
                      "../outside") ||
         !checkRejected(symlink) )
        return 1;
    // RAR4 中夹带链接和加密条目同样不能绕过通用检查。
    // 加密与链接分别验证库错误传播和业务安全策略。
    if ( !checkRejected(fixtures /
                        "test_read_format_rar_compress_normal.rar") ||
         !checkRejected(fixtures / "test_read_format_rar4_encrypted.rar") )
        return 1;
    // 普通文本带 RAR 后缀用于验证不能只凭扩展名宣称打开成功。
    const auto broken = output / "broken.rar";
    std::ofstream(broken, std::ios::binary) << "not an archive";
    // 这个错误应到达已有 ProjectOpenFailed 反馈流程，而非静默跳过。
    if ( !checkRejected(broken) ) return 1;
    // 最后确认 traversal 没有在输出根外留下文件。
    // 存在性查询同样使用 error_code，不引入测试异常路径。
    if ( std::filesystem::exists(output.parent_path() / "escaped.bms", ec) ||
         ec )
        return 1;
    return 0;
}
