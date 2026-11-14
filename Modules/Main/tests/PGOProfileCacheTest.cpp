/// @file PGOProfileCacheTest.cpp
/// @brief 缓存容量、淘汰顺序与文件系统边界的独立回归。
/// @details 夹具正文没有 LLVM 格式要求，清理器只能依赖类型、长度和时间。
/// @note 每个场景同时核对数值统计与具体文件身份，避免以自报结果证明成功。
/// @warning 输出根由 CTest 指定，不能从默认临时目录发现或删除用户旧数据。
#include "main/PGOProfileCache.h"
#include "log/colorful-log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
namespace FS = std::filesystem;
using MMM::Main::trimPGOProfileCache;
/// @brief 缩放容量关系，模拟 1 GiB 积累时不实际占用大量磁盘。
constexpr std::uintmax_t TEST_LIMIT = 100;
/// @brief 发布上限与测试缩放互相独立，防止默认值被意外改小。
static_assert(MMM::Main::PGO_PROFILE_CACHE_MAX_BYTES == 100ULL * 1024 * 1024);

/// @brief 一份夹具及它在整理后的预期存在状态。
struct Sample {
    /// @brief 可以包含子目录，用于验证清理范围不递归。
    std::string m_name;
    /// @brief 普通文件的实际逻辑长度，不伪造真实 LLVM 采样。
    std::size_t m_bytes;
    /// @brief 分钟差拉开时间，兼容修改时间精度较低的文件系统。
    int m_age;
    /// @brief 文件必须保留还是必须淘汰，与统计结果独立断言。
    bool m_keep;
};

/// @brief 累积失败而不提前退出，使断言失败后仍能清理所有夹具。
/// @param value 由用例给出的独立业务断言，不复用被测实现推导期望。
/// @param message 表示失败的行为边界，不依赖目录名或随机时间戳。
/// @return 可累加的失败数，使后续用例与目录回收都继续执行。
int expect(bool value, const char* message)
{
    if ( value ) return 0;
    XERROR("PGO cache test failed: {}", message);
    return 1;
}

/// @brief 写入小夹具并固定新旧关系，不依赖目录枚举顺序。
/// @return 目录、文件和时间戳都成功准备后才允许测试使用该夹具。
/// @param path 文件可以位于嵌套目录，目录创建只发生在夹具准备阶段。
/// @param bytes 所有长度均按比例缩小，边界关系与真实大缓存一致。
/// @param age 明确的修改时间偏移，版本名和创建顺序都不是排序依据。
/// @warning 这是低频测试文件操作，输出流析构前不能修改最终时间戳。
bool writeFixture(const FS::path& path, std::size_t bytes, int age)
{
    std::error_code error;
    FS::create_directories(path.parent_path(), error);
    if ( error ) return false;
    // 流先关闭再改时间，避免后续写入覆盖指定的修改时间。
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << std::string(bytes, 'p');
        if ( !output ) return false;
    }
    FS::last_write_time(
        path,
        FS::file_time_type::clock::now() - std::chrono::minutes(age),
        error);
    return !error;
}

/// @brief 独立核对磁盘文件存在性，不以被测函数的返回统计代替实际状态。
/// @param path 某份夹具的准确身份，不能用目录总条目数替代该检查。
/// @return 成功确认存在时返回 true，查询错误不会伪装成已保留。
bool fileExists(const FS::path& path)
{
    std::error_code error;
    return FS::exists(path, error) && !error;
}

/// @brief 数据驱动回归同时核对统计和每个文件的准确保留状态。
/// @param samples 文件名、年龄和结果均由用例显式给出，不由算法推导期望。
/// @param before 只统计直属普通 profraw，因此可能小于所有夹具的合计。
/// @param after 预期真实剩余值，不能只检查删除次数或是否低于上限。
/// @param removed 预期删除次数，用于识别错误地一次清空整个缓存。
/// @param protectedName 活动样本即使最旧也必须保留，大小仍计入统计。
/// @param root 每个场景独占的目录，不继承前一个场景的历史文件。
/// @return 准备失败、统计错误与保留状态错误都计入失败总数。
/// @details 输入中可有大于预算的 ZIP 或索引，它们不应触发原始采样淘汰。
/// @note 期望集合还会发现过度删除，不能仅以最终容量足够小作为通过依据。
int runCase(const FS::path& root, const std::vector<Sample>& samples,
            std::uintmax_t before, std::uintmax_t after, std::size_t removed,
            const char* protectedName = "")
{
    int fail = 0;
    for ( const auto& sample : samples )
        fail += expect(
            writeFixture(root / sample.m_name, sample.m_bytes, sample.m_age),
            "prepare fixture");
    // 空保护路径与缓存根不同，不能把所有文件都意外排除出淘汰计划。
    // 保护项也在 samples 内，因此必须计入 before 和 after 的期望容量。
    const auto active = protectedName[0] ? root / protectedName : FS::path{};
    const auto result = trimPGOProfileCache(root, active, TEST_LIMIT);
    fail +=
        expect(result.m_bytesBefore == before && result.m_bytesAfter == after &&
                   result.m_removedFiles == removed && result.m_errors == 0,
               "cleanup statistics");
    for ( const auto& sample : samples )
        fail += expect(fileExists(root / sample.m_name) == sample.m_keep,
                       "exact survivor set");
    return fail;
}

/// @brief 保护中的大单份可暂时超限，上传读取结束后必须重新收敛。
/// @param root 同一目录用于保护和释放两个阶段，证明状态变化会重新整理。
/// @return 活动正文丢失、其他旧项未清理或释放后仍超限均视为失败。
/// @details 活动样本的时间比普通文件更早，保留不能仅是排序偶然造成的。
/// @note 单份超大样本在上传后也需要淘汰，不存在永久突破预算的白名单。
/// @par 跨阶段等号边界
/// 活动样本恰好等于上限时，保护阶段可能先删除其他文件而停在等号。
/// 后续必须继续之前的超限整理，不能误用初始等于上限的保留规则。
/// 因此大于与等于上限的活动样本都需要两阶段回归。
int testProtected(const FS::path& root)
{
    int fail = runCase(root,
                       { { "active.profraw", 150, 30, true },
                         { "other.profraw", 20, 20, false } },
                       170,
                       150,
                       1,
                       "active.profraw");
    // 第二次调用复用同一份文件，真实模拟 MIME 释放而不是重建夹具。
    // 不重置时间或重写正文，释放保护是两阶段之间唯一发生的状态变化。
    const auto released = trimPGOProfileCache(root, {}, TEST_LIMIT);
    fail += expect(released.m_removedFiles == 1 && released.m_bytesAfter == 0 &&
                       !fileExists(root / "active.profraw"),
                   "released oversized file");
    // 若保护阶段恰好停在上限，释放后仍需完成此前已经触发的淘汰。
    // 普通初始相等不删的规则不能误用于这次跨上传阶段的连续整理。
    fail += runCase(root / "equal-active",
                    { { "active.profraw", 100, 30, true },
                      { "old.profraw", 20, 20, false } },
                    120,
                    100,
                    1,
                    "active.profraw");
    // 强制续接只来自已触发的整理，不是把正常未超限缓存统一改成激进删除。
    const auto finished =
        trimPGOProfileCache(root / "equal-active", {}, TEST_LIMIT, true);
    fail += expect(finished.m_removedFiles == 1 && finished.m_bytesAfter == 0,
                   "continue strict eviction after protection ends");
    return fail;
}

/// @brief 链接不应计入预算，也不能借清理操作删除根目录外的目标。
/// @details Windows 可能没有链接权限，其余类型排除仍由独立用例覆盖。
/// @param root 链接位于缓存中，目标故意位于该目录之外。
/// @return 计入链接目标、删除目标或删除链接自身都会触发断言失败。
/// @note 无链接权限只跳过该能力检查，不跳过普通文件容量检查。
/// @warning 只在本次输出根内部建立链接，不涉及用户实际目录。
int testSymlink(const FS::path& root)
{
    const auto outside = root.parent_path() / "outside.profraw";
    int fail = expect(writeFixture(outside, 900, 40), "outside sentinel");
    fail += expect(writeFixture(root / "keep.profraw", 50, 30),
                   "link case fixture");
    std::error_code error;
    FS::create_symlink(outside, root / "linked.profraw", error);
    // 即使链接无法创建，仍须验证默认整理没有凭空产生错误或多删普通文件。
    const bool created = !error;
    const auto result  = trimPGOProfileCache(root, {}, TEST_LIMIT);
    fail += expect(result.m_bytesBefore == 50 && result.m_removedFiles == 0 &&
                       fileExists(outside),
                   "link excluded from budget");
    if ( created )
        fail += expect(FS::is_symlink(FS::symlink_status(
                           root / "linked.profraw", error)) &&
                           !error,
                       "link itself retained");
    return fail;
}

/// @brief 输入错误必须安全返回，不能把零预算解释成清空全部文件。
/// @param root 先验证未创建目录，再建立异常输入的保留哨兵。
/// @return 错误未反馈或任何哨兵被更改、删除均应导致非零结果。
/// @details 零预算无法满足严格小于的停止条件，必须安全拒绝。
/// @note 普通文件被误作目录时，只能报告错误，不能递归操作其父目录。
int testInvalid(const FS::path& root)
{
    auto result = trimPGOProfileCache(root / "missing", {}, TEST_LIMIT);
    int  fail   = expect(result.m_errors == 0 && !fileExists(root / "missing"),
                         "missing directory not created");
    fail += expect(writeFixture(root / "keep.profraw", 150, 30),
                   "invalid sentinel");
    // 非目录和零上限都要报告失败，哨兵必须保持不变。
    result = trimPGOProfileCache(root, {}, 0);
    fail += expect(result.m_errors != 0 && fileExists(root / "keep.profraw"),
                   "zero budget");
    result = trimPGOProfileCache(root / "keep.profraw", {}, TEST_LIMIT);
    fail += expect(result.m_errors != 0 && fileExists(root / "keep.profraw"),
                   "non-directory");
    return fail;
}
}  // namespace

/// @brief 输出局限于构建树，每次运行再使用唯一子目录隔离并发。
/// @warning 测试执行磁盘操作，绝不能把用户真实临时缓存作为夹具。
/// @param argc 必须提供一个测试输出目录，避免误用进程当前目录。
/// @param argv 单调时间戳只命名唯一工作目录，不参与淘汰排序。
/// @return 聚合各项断言；夹具删除成功不代表业务边界已经通过。
/// @details 历史场景的每个版本都有独立文件，不能仅清理当前版本。
/// @note 相同用例重复或并发执行也不共享已经存在的文件集合。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 1;
    const auto root =
        FS::path(argv[1]) /
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    // 初始恰好等于预算不淘汰；ZIP 再大也不能改变原始采样的统计。
    int fail = runCase(root / "equal",
                       { { "a.profraw", 40, 30, true },
                         { "b.profraw", 60, 20, true },
                         { "assets.zip", 1000, 40, true } },
                       100,
                       100,
                       0);
    // 文件名顺序与修改时间相反，删除任意一份虽都能达标，但只有最旧项正确。
    fail += runCase(root / "oldest",
                    { { "z-old.profraw", 40, 30, false },
                      { "a-middle.profraw", 35, 20, true },
                      { "b-new.profraw", 40, 10, true } },
                    115,
                    75,
                    1);
    // 110 删除第一份只剩 100，必须继续到 50，不能在等号处提前结束。
    fail += runCase(root / "strict",
                    { { "first.profraw", 10, 30, false },
                      { "second.profraw", 50, 20, false },
                      { "last.profraw", 50, 10, true } },
                    110,
                    50,
                    2);
    // 索引不归原始缓存管理，名为 profraw 的目录也不能导致递归清理。
    fail += runCase(root / "types",
                    { { "old.profraw", 60, 30, false },
                      { "new.profraw", 60, 20, true },
                      { "merged.profdata", 900, 40, true },
                      { "nested.profraw/inside.profraw", 900, 40, true } },
                    120,
                    60,
                    1);
    fail += testProtected(root / "protected");
    fail += testSymlink(root / "symlink");
    fail += testInvalid(root / "invalid");
    // 缩放复现二十份跨版本历史：总计 1000，连续删除十九份才严格小于 100。
    std::vector<Sample> history;
    for ( int i = 0; i < 20; ++i )
        history.push_back({ "mmm_pgo_v0." + std::to_string(i) + ".profraw",
                            50,
                            40 - i,
                            i == 19 });
    fail += runCase(root / "history", history, 1000, 50, 19);
    // 不清空公共输出根，断言失败也只回收本次自己的唯一目录。
    std::error_code error;
    FS::remove_all(root, error);
    fail += expect(!error, "fixture cleanup");
    return fail == 0 ? 0 : 1;
}
