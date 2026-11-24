#include "ConfigSnapshotWriter.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <list>
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
/// @brief 串行写入配置，同一路径的编辑快照可在队列中合并。
/// @details 显式保存和 flush
/// 是顺序边界。不同路径分别保留，连续编辑不会等待磁盘。
/// 文件线程独占序列化和临时文件，队列锁不覆盖耗时 I/O。
/// 每个连续编辑区间至多保留同一路径的一份待处理文档。
/// 路径采用调用者指定的原生路径，排队时不访问文件系统解析链接。
/// 领取后正在写出的文档不会被撤回；后续同路径请求会在它之后写入。
class SnapshotWriter
{
public:
    /// @brief 第一次保存时创建专用线程；同步对象均已初始化。
    /// @note 线程只消费冻结的数据，静态析构时可以在 AppConfig 释放后完成队列。
    SnapshotWriter() : m_worker([this] { run(); }) {}

    /// @brief 停止接收工作并等待队列内最后的快照写出。
    /// @warning 仅退出清理会等待文件线程。
    ~SnapshotWriter()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_ready.notify_one();
        m_worker.join();
    }

    /// @brief 异步提交已冻结的配置，不借用调用线程的实时数据。
    /// @param path 保留调用方选择的输出文件。
    /// @param json 写入线程独立拥有的配置文档。
    void enqueue(const std::filesystem::path& path, nlohmann::json json)
    {
        submit({ path, std::move(json), std::nullopt });
    }

    /// @brief 同步提交整份配置，以同一队列保证旧快照不会覆盖它。
    /// @param path 本次显式保存的文件。
    /// @param json 本次同步保存的完整配置。
    /// @return 本次目标的实际写盘结果，与其他路径的结果无关。
    /// @warning 仅低频调用允许等待文件线程，拖动时使用异步发布。
    bool save(const std::filesystem::path& path, nlohmann::json json)
    {
        std::promise<bool> completion;
        auto               result = completion.get_future();
        submit({ path, std::move(json), std::move(completion) });
        return result.get();
    }

    /// @brief 等待调用时已发布的快照耗尽，失败也视作已处理。
    /// @note 使用队列内的空路径槽作屏障，之后的发布不会延长本次等待。
    /// 保留屏障前已经排队的值，即使屏障后又收到同一路径的更新。
    void flush()
    {
        std::promise<bool> completion;
        auto               result = completion.get_future();
        submit({ std::nullopt, {}, std::move(completion) });
        result.get();
    }

private:
    /// @brief 单条配置文档或队列屏障。
    struct Request {
        /// @brief 原生目标路径；空 optional 仅用于 flush。
        std::optional<std::filesystem::path> m_path;
        /// @brief 由文件线程独占的冻结文档。
        nlohmann::json m_json;
        /// @brief 同步请求拥有完成信号，调用方只保留配对的 future。
        std::optional<std::promise<bool>> m_completion;
    };

    /// @brief 在最近的同步边界之后合并目标一致的后台请求。
    /// @param request 新请求持有文档，以及调用方尚在等待的完成信号。
    /// @details 插入位置始终在队尾，多个路径的请求可以交错提交。
    /// 跳过已经开始的写出；异步调用不会等待同步请求的 I/O。
    void submit(Request request)
    {
        std::list<Request> retired;
        {
            std::lock_guard lock(m_mutex);
            if ( request.m_path ) {
                // 只有队尾这段后台请求允许被覆盖，不能跳过已排队的同步等待者。
                auto previous =
                    std::find_if(m_pending.rbegin(),
                                 m_pending.rend(),
                                 [&](const Request& pending) {
                                     return pending.m_completion ||
                                            pending.m_path == request.m_path;
                                 });
                if ( previous != m_pending.rend() && !previous->m_completion )
                    retired.splice(
                        retired.end(), m_pending, std::prev(previous.base()));
            }
            m_pending.push_back(std::move(request));
        }
        // 被替代的 JSON 在锁外销毁，界面排队时无需等待释放整份配置。
        m_ready.notify_one();
    }

    /// @brief 按提交顺序处理同步保存、后台快照与 flush 屏障。
    /// @warning 仅此线程写盘，执行写入时不占用队列锁。
    void run()
    {
        for ( ;; ) {
            Request request{};
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock,
                             [&] { return m_stopping || !m_pending.empty(); });
                if ( m_pending.empty() ) return;
                request = std::move(m_pending.front());
                m_pending.pop_front();
            }
            // 缺省路径表示屏障，前面的文件操作此时都已返回。
            const bool result = !request.m_path ||
                                writeSnapshot(*request.m_path, request.m_json);
            // 文件线程持有 promise，唤醒等待者后仍由本线程销毁请求。
            // 异步失败只写日志，同步请求把同一写入结果交回调用方。
            if ( request.m_completion ) request.m_completion->set_value(result);
        }
    }

    /// @brief 保护待处理列表及退出标志。
    std::mutex m_mutex;
    /// @brief 新请求或退出通知，后台无工作时休眠。
    std::condition_variable m_ready;
    /// @brief 保留每个路径的最新后台请求及同步边界。
    std::list<Request> m_pending;
    /// @brief 析构设置后继续处理队列，到空再退出。
    bool m_stopping{ false };
    /// @brief 最后构造，确保线程看到有效队列和同步对象。
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
/// @brief 将布局编辑快照发布到按路径合并的文件队列。
/// @param path 后台写入使用的稳定原生路径。
/// @param snapshot 调用者冻结的完整文档，不借用 UI 状态。
/// @warning 本函数只排队，不在 UI 线程执行文件系统操作。
void queueConfigSnapshot(const std::filesystem::path& path,
                         nlohmann::json               snapshot)
{
    writer().enqueue(path, std::move(snapshot));
}
/// @brief 在测试或退出阶段等待当前队列完成。
/// @note 同步条件由队列中的屏障决定，不使用固定 sleep。
/// @warning 此阻塞接口不能从实时 UI 或逻辑循环调用。
void flushConfigSnapshots()
{
    writer().flush();
}
}  // namespace MMM::Config
