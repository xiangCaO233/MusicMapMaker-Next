/// @file
/// @brief 使用独立输出目录验证备份提交、单谱面轮转与路径隔离。
/// 所有谱面和备份均由夹具生成，不修改仓库中的测试资源。
#include "logic/BeatmapBackupService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{
/// @brief 收集服务管理的正式 MMM 备份文件。
/// @param directory 单谱面备份目录。
/// @return 文件名使用 backup- 前缀的普通文件。
/// 这是测试观测入口，不复刻生产侧的完整固定宽度文件名验证。
/// 返回次序来自目录枚举，调用点只按数量观测，不把第一项误当成最旧文件。
std::vector<std::filesystem::path> collectBackups(
    const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path>  backups;
    std::error_code                     error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if ( error ) return backups;
    // 无法枚举就返回空结果，让调用点的预期数量检查失败而不是遗漏异常。

    while ( iterator != end ) {
        std::error_code itemError;
        const auto      fileName =
            MMM::Config::pathToUtf8(iterator->path().filename());
        // 忽略子目录与未提交文件，计数只针对可供恢复的正式 MMM 文件。
        if ( iterator->is_regular_file(itemError) && !itemError &&
             fileName.starts_with("backup-") &&
             iterator->path().extension() == ".mmm" ) {
            backups.push_back(iterator->path());
        }
        iterator.increment(error);
        if ( error ) {
            // 中途失败清空候选，不能让不完整计数偶然等于期望的保留数量。
            backups.clear();
            return backups;
        }
    }
    return backups;
}

/// @brief 检查目录中是否遗留自动备份临时文件。
/// @param directory 单谱面备份目录。
/// @return 发现 .pending- 前缀文件或目录枚举失败时返回 true。
/// 无法证明临时文件已清理时按失败处理，不以访问错误冒充目录干净。
/// 检查范围限于当前谱面的独立目录，不检查其他谱面的并发临时工作。
bool hasPendingBackup(const std::filesystem::path& directory)
{
    std::error_code                     error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if ( error ) return true;
    while ( iterator != end ) {
        // 名称即可识别 pending 残留，不要求它已是完整可读的谱面文件。
        const auto fileName =
            MMM::Config::pathToUtf8(iterator->path().filename());
        if ( fileName.starts_with(".pending-") ) return true;
        iterator.increment(error);
        if ( error ) return true;
    }
    return false;
}

/// @brief 使用确定性毫秒时间点创建自动备份。
/// @param beatmap 待备份谱面。
/// @param root 测试项目根目录。
/// @param source 谱面源路径。
/// @param maxCount 保留数量。
/// @param milliseconds 系统时钟毫秒值。
/// 显式时间使测试不需要等待真实时钟，也可稳定复现同刻触发与时钟回拨。
/// 保留数量直接传入服务，夹具不执行删除来帮助被测逻辑达到期望结果。
/// @return 服务执行结果。
MMM::Logic::BeatmapBackupResult createAt(const MMM::BeatMap&          beatmap,
                                         const std::filesystem::path& root,
                                         const std::filesystem::path& source,
                                         int                          maxCount,
                                         std::int64_t milliseconds)
{
    // 只转换时间表示，不在夹具中修正顺序；单调命名必须由被测服务完成。
    return MMM::Logic::BeatmapBackupService::createBackupAt(
        beatmap,
        root,
        source,
        maxCount,
        std::chrono::system_clock::time_point(
            std::chrono::milliseconds(milliseconds)));
}

/// @brief 验证单谱面轮转、同时间戳去重及不同谱面隔离。
/// @param outputRoot 测试输出目录。
/// @return 全部备份行为符合预期时返回 true。
/// @pre outputRoot 是本测试独占的可清理目录，不得传入项目或测试资源根。
/// 本例会创建同级 backup_escape 目录以检查符号链接隔离。
/// 若平台不允许创建符号链接，仅跳过链接场景，数量轮转与正常提交仍须通过。
bool testBackupRotation(const std::filesystem::path& outputRoot)
{
    std::error_code error;
    // 重建测试目录以去除上一次运行的历史文件，保证轮转数量有确定起点。
    std::filesystem::remove_all(outputRoot, error);
    error.clear();
    std::filesystem::create_directories(outputRoot / "charts", error);
    // 仅预建源谱面父目录，隐藏备份目录必须由服务首次写入时创建。
    if ( error ) {
        XERROR("Failed to create backup test output: {}", error.message());
        return false;
    }
    if ( std::filesystem::exists(outputRoot / ".mmm", error) || error ) {
        // 已有隐藏目录会掩盖目录准备流程的退化，因此在调用服务前检查。
        XERROR(
            "Backup test unexpectedly started with an existing .mmm directory");
        return false;
    }

    MMM::BeatMap beatmap;
    // 使用最小内存谱面，名称是恢复后验证内容确实被序列化的观测字段。
    beatmap.m_baseMapMetadata.name     = "Backup Test";
    beatmap.m_baseMapMetadata.version  = "Hard";
    beatmap.m_baseMapMetadata.map_path = "charts/song.osu";
    // 源路径使用 osu 扩展名，但恢复点仍须能按原生 MMM 读取。

    const auto source = outputRoot / "charts/song.osu";
    // 连续三个确定时刻写入，保留两份时恰好应轮转掉最早的一份。
    const auto first = createAt(beatmap, outputRoot, source, 2, 1000);
    // 保存每次返回路径，后面以真实路径检查删除，而不是在测试中重算文件名。
    const auto second = createAt(beatmap, outputRoot, source, 2, 2000);
    const auto third  = createAt(beatmap, outputRoot, source, 2, 3000);
    // 用服务的目录映射读取结果，源文件无需在磁盘上存在才能备份内存快照。
    const auto directory =
        MMM::Logic::BeatmapBackupService::backupDirectory(outputRoot, source);
    auto restored = MMM::BeatMap::loadFromFile(third.m_backupPath);
    // 回读选择最新恢复点；此前的轮转不应影响它的完整可读性。
    // 既检查数量与被删除路径，也回读内容，避免只创建占位空文件就算成功。
    if ( !first.m_success || !second.m_success || !third.m_success ||
         third.m_removedBackupCount != 1U ||
         collectBackups(directory).size() != 2U ||
         std::filesystem::exists(first.m_backupPath, error) ||
         restored.m_baseMapMetadata.name != "Backup Test" ) {
        // 删除数量是实际轮转结果，不能仅凭目录最终大小推测服务返回值正确。
        XERROR("Per-beatmap backup rotation did not retain the newest files");
        return false;
    }

    // 同一时间再次备份不能覆盖上一恢复点，即使调用者没有推进时钟。
    const auto sameTimestamp = createAt(beatmap, outputRoot, source, 2, 3000);
    // 不限定必须增加后缀还是提升毫秒值，只约束不覆盖与保留上限两个结果。
    if ( !sameTimestamp.m_success ||
         sameTimestamp.m_backupPath == third.m_backupPath ||
         collectBackups(directory).size() != 2U ) {
        // 名称必须变化且数量保持上限，不能通过无限累积文件逃避覆盖问题。
        XERROR("Backup name collision was not resolved safely");
        return false;
    }

    // 回拨到比所有历史都早的时间，最新创建的备份仍应排在最后而被保留。
    const auto clockRollback = createAt(beatmap, outputRoot, source, 2, 500);
    // 回拨输入比首份备份还早，测试不依赖机器时区或实际当天日期。
    if ( !clockRollback.m_success ||
         // 文件名字典序直接体现服务的固定宽度单调命名约定。
         clockRollback.m_backupPath.filename() <=
             sameTimestamp.m_backupPath.filename() ||
         !std::filesystem::exists(clockRollback.m_backupPath, error) ||
         collectBackups(directory).size() != 2U ) {
        // 路径存在性防止“返回成功却在随后的轮转中把自己删掉”。
        XERROR("Clock rollback did not preserve monotonic backup rotation");
        return false;
    }

    // 第二个源谱面采用不同保留上限，确认轮转不是对项目所有备份统一计数。
    const auto otherSource = outputRoot / "charts/other.mmm";
    // 两个源路径共用项目根，能覆盖同项目不同谱面之间的目录隔离。
    const auto other = createAt(beatmap, outputRoot, otherSource, 1, 4000);
    // 复用同一内存内容，只改变源路径；隔离键应由源路径而非谱面标题决定。
    const auto otherDirectory =
        MMM::Logic::BeatmapBackupService::backupDirectory(outputRoot,
                                                          otherSource);
    if ( !other.m_success || collectBackups(otherDirectory).size() != 1U ||
         collectBackups(directory).size() != 2U ) {
        // 两边数量同时断言，避免检查新目录时漏掉旧目录被误删的情况。
        XERROR("Backup rotation crossed beatmap directory boundaries");
        return false;
    }

    const auto escapeDirectory = outputRoot.parent_path() / "backup_escape";
    // 逃逸目标是测试专用同级目录，保持为空以观察服务是否向项目外写入。
    std::filesystem::remove_all(escapeDirectory, error);
    error.clear();
    std::filesystem::create_directories(escapeDirectory, error);
    if ( error ) return false;
    const auto linkedDirectory = outputRoot / ".mmm/backups/linked";
    // 将备份树的一个中间分量替换成链接，而不是直接给服务一个外部输出路径。
    std::filesystem::create_directory_symlink(
        // 链接创建仅服务于本次夹具，不修改源谱面目录或用户项目配置。
        escapeDirectory,
        linkedDirectory,
        error);
    if ( !error ) {
        // 部分平台无创建链接权限；只有链接实际建立成功才执行此场景。
        const auto linkedSource = outputRoot / "linked/chart.mmm";
        const auto linkedResult =
            createAt(beatmap, outputRoot, linkedSource, 2, 5000);
        // 返回失败与外部目录未写入必须同时成立，不能仅拒绝最后的轮转步骤。
        if ( linkedResult.m_success ||
             !std::filesystem::is_empty(escapeDirectory, error) || error ) {
            // 外部路径查询失败也不能据此断定隔离成功。
            XERROR("Backup service followed a symlink outside the project");
            return false;
        }
    }

    // 正常提交结束后只留下正式备份，临时文件不应随每次备份积攒。
    if ( hasPendingBackup(directory) ) {
        // 检查在所有正常写入与轮转之后执行，包含相同时间与回拨两种提交路径。
        XERROR("Committed backup left a pending file behind");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行谱面自动备份写入与数量轮转测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为构建目录下的测试输出路径。
/// @return 全部测试通过时返回 0。
/// 输出目录由测试运行器传入，不能隐式使用当前工作目录作为清理范围。
/// 以进程退出码报告结果，测试框架无需解析日志里的错误文字。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        // 缺少隔离目录时直接拒绝，避免以空路径进入递归清理。
        XERROR("BeatmapBackupServiceTest requires an output directory");
        return 1;
    }
    // 路径按 UTF-8 转换后传给 filesystem，支持非 ASCII 构建目录。
    // 清理发生在夹具内部，入口不额外删除输出根的其他同级测试目录。
    return testBackupRotation(MMM::Config::utf8ToPath(argv[1])) ? 0 : 1;
}
