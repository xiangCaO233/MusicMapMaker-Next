#include "ConfigSnapshotWriter.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <utility>

namespace MMM::Config
{
namespace
{
/// @brief 原子替换同目录配置文件，保持既有跨平台失败反馈。
/// @param path 已解析的目标路径，不读取全局配置。
/// @param j 调用线程冻结的完整文档。
/// @return 目录准备、写出与替换均成功时为真。
/// @warning 只在低频同步保存或后台写盘线程执行文件系统操作。
bool writeSnapshot(const std::filesystem::path& path, const nlohmann::json& j)
{
    // 在打开文件前准备父目录；当前目录中的显式文件名没有父目录可创建。
    if ( auto parent = path.parent_path(); !parent.empty() ) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if ( createDirectoryError ) {
            // 不继续创建临时文件，避免把输出意外写到其他工作目录。
            XERROR("Failed to create config directory: {}. Error: {}",
                   pathToUtf8(parent),
                   createDirectoryError.message());
            return false;
        }
    }

    // 同目录临时文件使常见文件系统上的 rename 保持原子替换语义。
    std::filesystem::path tempPath = path;
    tempPath += ".tmp";
    {
        // trunc 确保上次失败残留的更长临时内容不会污染本轮 JSON。
        std::ofstream file(tempPath);
        if ( !file.is_open() ) {
            // 打开失败前没有修改正式目标文件，旧配置仍可恢复。
            XERROR("Failed to open config temp file for writing: {}",
                   pathToUtf8(tempPath));
            return false;
        }

        // 四空格缩进便于用户检查配置，末尾换行保持文本工具兼容。
        file << std::setw(4) << j << '\n';
        file.close();
        if ( !file.good() ) {
            // 流失败时临时文件可能不完整，绝不进入替换正式配置步骤。
            XERROR("Failed to write config temp file: {}",
                   pathToUtf8(tempPath));
            return false;
        }
    }

    // 首选同文件系统原子重命名，成功时旧配置不会出现部分写入。
    std::error_code replaceError;
    std::filesystem::rename(tempPath, path, replaceError);
    if ( replaceError ) {
        // Windows 等平台可能拒绝覆盖现存目标，退化为显式覆盖复制。
        std::error_code copyError;
        std::filesystem::copy_file(
            tempPath,
            path,
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        // 无论复制结果如何都尝试清理本轮临时文件，避免持续积累残留。
        std::error_code removeTempError;
        std::filesystem::remove(tempPath, removeTempError);
        // 临时清理错误不覆盖 copyError；正式文件成功优先决定保存结果。
        if ( copyError ) {
            // 复制失败保留原目标文件状态，并向调用方报告保存失败。
            XERROR("Failed to replace config file: {}. Error: {}",
                   pathToUtf8(path),
                   copyError.message());
            return false;
        }
    }

    return true;
}
/// @brief 串行保存最新配置，防止拖动快照积压与旧请求覆盖新状态。
/// @note 队列只保留一个未消费请求，内存占用不随拖动时间增长。
/// 写盘锁与队列锁分开，连续编辑发布请求不会等待磁盘完成。
/// 每个请求自带不可变 JSON，工作线程不借用 AppConfig 或 UI 元素。
class SnapshotWriter
{
public:
    /// @brief 第一次保存时创建专用文件线程，常态 UI 不再创建线程。
    /// @note 成员声明顺序保证队列、条件变量和停止状态先于线程构造。
    /// 线程入口仅等待请求，不在构造期间读取应用配置单例。
    SnapshotWriter() : m_worker([this] { run(); }) {}
    /// @brief 退出时唤醒线程并完成最后一个请求，不丢弃最终现场。
    /// @warning 仅进程退出会等待文件线程，不在交互调用链触发。
    ~SnapshotWriter()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_ready.notify_one();
        m_worker.join();
    }
    /// @brief 发布一个请求；替换尚未写出的中间拖动状态。
    /// @note 持锁范围仅覆盖移动 JSON 所有权，不做文件系统操作。
    /// @param path 目标路径在发布时复制，后台不调用应用路径解析器。
    /// @param json 已冻结的文档，调用者发布后不能再修改它。
    /// @details 序号与队列替换处于同一短锁，消费者不会取得不匹配状态。
    void enqueue(const std::filesystem::path& path, nlohmann::json json)
    {
        std::optional<Request> request{ Request{ path, std::move(json), 0 } };
        {
            std::lock_guard lock(m_mutex);
            request->m_generation = ++m_generation;
            // 旧文档移出锁外再销毁，队列锁不承担 JSON 树释放成本。
            m_pending.swap(request);
        }
        m_ready.notify_one();
    }
    /// @brief 同步保存也使用同一序号，废弃此前未完成的旧请求。
    /// @return 实际写盘结果；失败保留原有配置并输出错误日志。
    /// @warning 仅显式保存允许等待文件锁，连续编辑必须使用 enqueue。
    /// @param path 显式目标路径，测试与客户端保存共用相同顺序规则。
    /// @param json 本次同步调用的最终文档，不重新读取实时对象。
    /// @details 先取得磁盘锁，再使此前待处理请求失效。
    /// 同步写入期间收到的新请求具有更大序号，会在本次完成后继续写出。
    bool save(const std::filesystem::path& path, nlohmann::json json)
    {
        std::lock_guard diskLock(m_diskMutex);
        std::uint64_t   generation;
        {
            std::lock_guard lock(m_mutex);
            generation = ++m_generation;
            m_pending.reset();
        }
        const bool result = writeSnapshot(path, json);
        complete(generation);
        return result;
    }
    /// @brief 测试或退出阶段等待调用时已经发布的请求完成。
    /// @warning 条件变量只在此低频等待，UI 修改路径不会调用。
    /// @note 只等待进入时已发布的序号，不阻止调用后继续发布请求。
    /// 后台失败也必须标记完成，避免磁盘错误导致退出永久等待。
    void flush()
    {
        std::unique_lock lock(m_mutex);
        const auto       target = m_generation;
        m_done.wait(lock, [&] { return m_completed >= target; });
    }

private:
    /// @brief 请求独占路径和文档，不引用可变应用配置。
    struct Request {
        /// @brief 使用原生路径编码，支持 Windows Unicode 目录。
        std::filesystem::path m_path;
        /// @brief 完整冻结文档，后台只序列化此对象。
        nlohmann::json m_json;
        /// @brief 单调序号，旧请求不得覆盖新请求。
        std::uint64_t m_generation;
    };
    /// @brief 标记已消费的序号，成功与失败都必须唤醒收尾等待者。
    /// @param generation 本轮已写入或被更新请求取代的序号。
    /// @note 完成序号只向前推进，被跳过旧请求不能倒退已完成状态。
    void complete(std::uint64_t generation)
    {
        {
            std::lock_guard lock(m_mutex);
            m_completed = std::max(m_completed, generation);
        }
        m_done.notify_all();
    }
    /// @brief 无请求时睡眠等待通知，有请求立即开始写入，不设延时窗口。
    /// @warning 文件线程可阻塞；它不持有会话、渲染或配置状态锁。
    /// @details 队列锁只保护领取和版本检查，写盘期间始终释放。
    /// 磁盘锁保证同步保存与后台请求不会同时修改同一个临时文件。
    /// 退出通知不清空队列，最后的用户状态仍按正常写入流程提交。
    /// 旧请求被丢弃也通知完成，收尾等待不依赖它是否实际写盘。
    void run()
    {
        for ( ;; ) {
            std::optional<Request> request;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(
                    lock, [&] { return m_stopping || m_pending.has_value(); });
                if ( !m_pending ) return;
                request = std::move(m_pending);
                m_pending.reset();
            }
            // 磁盘锁不参与 enqueue，因此慢磁盘不会阻塞拖动反馈。
            std::lock_guard diskLock(m_diskMutex);
            bool            current;
            {
                std::lock_guard lock(m_mutex);
                current = request->m_generation == m_generation;
            }
            // 更新的请求已入队或同步写入时，丢弃这个过时请求。
            if ( current ) writeSnapshot(request->m_path, request->m_json);
            complete(request->m_generation);
        }
    }
    /// @brief 队列与序号锁，发布路径只进行常量级所有权移动。
    std::mutex m_mutex;
    /// @brief 独立磁盘锁，串行化同目录临时文件的创建和替换。
    std::mutex m_diskMutex;
    /// @brief 请求或退出通知，不以固定时间轮询。
    std::condition_variable m_ready;
    /// @brief 测试和收尾同步通知，与实时 UI 无关。
    std::condition_variable m_done;
    /// @brief 最新待处理请求，覆盖中间状态但不丢最终状态。
    std::optional<Request> m_pending;
    /// @brief 仅由队列锁保护的单调请求序号。
    std::uint64_t m_generation{ 0 };
    /// @brief 已处理的最高请求序号，包括被新快照取代的请求。
    std::uint64_t m_completed{ 0 };
    /// @brief 析构设置后先处理最终请求，再退出线程。
    bool m_stopping{ false };
    /// @brief 最后构造成员，确保线程启动时所有同步状态已经可用。
    std::thread m_worker;
};
/// @brief 整个客户端共用单个写入队列，避免同路径保存相互竞争。
/// @return 进程级唯一后台写入器，生命周期覆盖最后一次排队请求。
/// @note 静态析构会完成最终快照，工作线程不依赖 AppConfig 的存活。
SnapshotWriter& writer()
{
    static SnapshotWriter instance;
    return instance;
}
}  // namespace
/// @brief 将同步保存委托给共用写入器。
/// @param path 本次目标路径，调用者已完成默认路径解析。
/// @param snapshot 完整不可变文档，所有权移交给写入器。
/// @return 写入器实际文件替换结果。
bool saveConfigSnapshot(const std::filesystem::path& path,
                        nlohmann::json               snapshot)
{
    return writer().save(path, std::move(snapshot));
}
/// @brief 将布局编辑快照发布到单槽文件队列。
/// @param path 后台写入使用的稳定原生路径。
/// @param snapshot 调用者冻结的完整文档，不借用 UI 状态。
/// @warning 本函数只排队，不在 UI 线程执行文件系统操作。
void queueConfigSnapshot(const std::filesystem::path& path,
                         nlohmann::json               snapshot)
{
    writer().enqueue(path, std::move(snapshot));
}
/// @brief 在测试或退出阶段等待当前队列完成。
/// @note 同步条件由真实完成序号决定，不使用固定 sleep。
/// @warning 此阻塞接口不能从实时 UI 或逻辑循环调用。
void flushConfigSnapshots()
{
    writer().flush();
}
}  // namespace MMM::Config
