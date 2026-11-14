#include "main/PGOProfileCache.h"

#include <algorithm>
#include <limits>
#include <system_error>
#include <vector>

namespace MMM::Main
{
namespace
{
/// @brief 扫描快照只保存淘汰决策需要的元数据，不读取采样文件正文。
struct CachedProfile {
    /// @brief 保留原生路径，避免为排序和删除反复转换平台字符编码。
    std::filesystem::path m_path;
    /// @brief 扫描时的逻辑长度，用于统计与删除前的并发变更检查。
    std::uintmax_t m_bytes;
    /// @brief 修改时间定义新旧顺序；相同时间再按路径稳定排序。
    std::filesystem::file_time_type m_modified;
};
}  // namespace

/// @brief 用一次低频快照清理直属缓存，失败项保留并反馈统计。
/// @param directory 应用独占的原始采样根，不应传入工程目录。
/// @param protectedProfile 尚有读取者的文件，其大小仍计入预算。
/// @param maxBytes 正数上限；初始相等不触发，超限后必须严格降低。
/// @param forceBelowLimit 延续此前未完成的超限整理，释放保护后严格降低。
/// @return 真实删除结果与已知容量，元数据失败时保留文件并反馈错误。
/// @details 先完整扫描再排序，不依赖枚举顺序，也不在遍历时修改目录。
/// 不解析 LLVM 内容，因此损坏的历史采样同样参与容量控制。
/// 时间相同按原生路径稳定排序，不依据文件名里的版本或时间戳。
/// 这是低频最佳努力整理，不宣称跨多个进程提供原子磁盘配额。
/// @warning 只在启动和退出阶段调用，内部执行文件系统访问及完整排序。
PGOProfileCacheCleanupResult trimPGOProfileCache(
    const std::filesystem::path& directory,
    const std::filesystem::path& protectedProfile, std::uintmax_t maxBytes,
    bool forceBelowLimit)
{
    PGOProfileCacheCleanupResult result;
    // 零上限不可能满足“严格低于”的契约，不能把它解释成清空所有文件。
    if ( maxBytes == 0 ) {
        result.m_errors = 1;
        return result;
    }

    // 文件系统失败使用错误码，权限、目录消失等情况不得中断应用启动。
    std::error_code error;
    // 新安装没有缓存目录属于正常状态，不为清理动作创建任何目录。
    std::filesystem::directory_iterator iterator(directory, error), end;
    if ( error ) {
        if ( error != std::errc::no_such_file_or_directory )
            result.m_errors = 1;
        return result;
    }

    // 保存完整候选快照，文件系统返回顺序不能用于判断最旧样本。
    // 路径和值归快照所有，后续目录迭代不会使已经保存的条目失效。
    std::vector<CachedProfile> profiles;
    for ( ; iterator != end; iterator.increment(error) ) {
        // 枚举失败时不使用不完整快照做删除决策，保留所有已发现文件。
        if ( error ) break;
        const auto& entry = *iterator;
        if ( entry.path().extension() != ".profraw" ) continue;
        // symlink_status 不跟随链接，名字相同的目录也不属于采样文件。
        const auto status = entry.symlink_status(error);
        if ( error ) break;
        // 拒绝设备和管道等特殊类型，绝不为统计缓存而读取其内容。
        if ( !std::filesystem::is_regular_file(status) ) continue;
        // 预算按文件逻辑长度累加，不依赖稀疏文件或压缩文件的分配块数。
        const auto bytes = entry.file_size(error);
        if ( error ) break;
        // 跨版本文件共用预算，修改时间比文件名中的历史版本更可靠。
        const auto modified = entry.last_write_time(error);
        if ( error ) break;
        // 溢出会让超大缓存看起来很小，拒绝该不可信统计而不是错误淘汰。
        if ( bytes > std::numeric_limits<std::uintmax_t>::max() -
                         result.m_bytesBefore ) {
            result.m_errors     = 1;
            result.m_bytesAfter = result.m_bytesBefore;
            return result;
        }
        result.m_bytesBefore += bytes;
        // 只有元数据齐全的项才发布到快照，避免出现没有可靠时间的候选。
        profiles.push_back({ entry.path(), bytes, modified });
    }
    result.m_bytesAfter = result.m_bytesBefore;
    // 枚举不完整时不删除任何项，避免总量和排序都基于残缺数据。
    if ( error ) {
        ++result.m_errors;
        return result;
    }
    // 只有真正超限才启动清理，单个新采样恰好达到上限仍可以保留。
    // 上传保护可能让上一轮停在等号，释放后不能遗忘此前已经触发的整理。
    if ( result.m_bytesAfter < maxBytes ||
         (result.m_bytesAfter == maxBytes && !forceBelowLimit) )
        return result;

    // 直接比较绝对修改时间，即使系统时钟曾回拨也不额外重写历史时间。
    std::sort(profiles.begin(),
              profiles.end(),
              [](const CachedProfile& left, const CachedProfile& right) {
                  if ( left.m_modified != right.m_modified )
                      return left.m_modified < right.m_modified;
                  // 粗粒度文件系统可能产生相同时间，路径决定稳定的次序。
                  return left.m_path < right.m_path;
              });
    // 路径只做词法规范化，不解析链接或把保护检查变成额外目录遍历。
    const auto protectedPath = protectedProfile.lexically_normal();
    // 保护项不能忽略统计，否则上传中的大文件会被虚报为已经释放。
    for ( const auto& profile : profiles ) {
        // 本轮已经触发超限；删除后等于上限也必须继续淘汰。
        if ( result.m_bytesAfter < maxBytes ) break;
        if ( !protectedProfile.empty() &&
             profile.m_path.lexically_normal() == protectedPath )
            continue;

        // 并发写入或文件替换会使快照过期，宁可保留也不删除新的活动采样。
        // 使用新查询而非 directory_entry 缓存，避免复核读到原来的状态。
        const auto status =
            std::filesystem::symlink_status(profile.m_path, error);
        if ( error || !std::filesystem::is_regular_file(status) ) {
            ++result.m_errors;
            continue;
        }
        // 类型复核成功后仍要验证大小，避免删除扫描之后被重写的采样。
        const auto bytes = std::filesystem::file_size(profile.m_path, error);
        if ( error || bytes != profile.m_bytes ) {
            ++result.m_errors;
            continue;
        }
        // 大小不变也可能更新了正文，修改时间是第二道并发变化保护。
        const auto modified =
            std::filesystem::last_write_time(profile.m_path, error);
        if ( error || modified != profile.m_modified ) {
            ++result.m_errors;
            continue;
        }

        // 不持有文件句柄或跨进程锁，上传线程不必等待本轮目录整理。
        // 复核只能缩小竞态窗口，调用方仍需显式保护当前上传文件。
        // 仅扣除真正成功删除的文件；失败继续尝试其他旧项，不虚报释放空间。
        // 零长度旧文件不会降低字节数，但有界的候选循环仍会继续推进。
        if ( std::filesystem::remove(profile.m_path, error) ) {
            result.m_bytesAfter -= profile.m_bytes;
            ++result.m_removedFiles;
        } else {
            // 某个平台锁定了文件时仍尝试下一份旧数据，不重复等待同一个句柄。
            ++result.m_errors;
        }
    }
    // 删除失败保留在剩余值里，调用方可明确记录为何仍未达到容量目标。
    return result;
}
}  // namespace MMM::Main
