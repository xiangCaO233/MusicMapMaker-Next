#include "logic/ProjectController.h"

#include "ProjectControllerState.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "runtime/AppThreadPool.h"

#include <ice/thread/ThreadPool.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

/// @file ProjectController_DirectoryRefresh.cpp
/// @brief 目录监视事件到工程资源与持久化分片的线程边界。
/// @details 文件线程检测目录和谱面引用，逻辑线程提交差异，后续文件任务
/// 持久化完整工程。后台闭包拥有独立值，不依赖工程切换前的对象生命周期。

namespace MMM::Logic
{

/// @brief 复制扫描输入并交接给文件池。
/// @return 当输入接收或无需工作时为真；有任务在途时让调用方保留通知。
/// @note 开始扫描前只复制随后会参与建索引的字段。工程及最新代次由会话锁
/// 保护，复制完便不再持有会话对象或项目引用。
/// @warning 文件线程闭包只拥有值，不允许回调会话和音频运行时。
bool ProjectController::startDirectoryScan()
{
    // 已就绪但还未应用也算在途，防止一次新通知覆盖尚待提交的扫描结果。
    if ( m_state->m_directoryScan.valid() ) return false;
    auto* worker = Runtime::AppThreadPool::instance().getFileThreadPool();
    // 关闭过程中池可能已不可用，不在调用线程退化成同步扫描。
    if ( !worker ) return true;

    Project                               snapshot;
    std::shared_ptr<const ProjectUiState> revision;
    {
        std::lock_guard lock(EditorEngine::instance().getSessionMutex());
        if ( !m_currentProject ) return true;
        // 同目录重开仍会改变发布身份，不能只记录根路径作为工作归属。
        revision = uiSnapshot();
        // 只交接资源同步读取的字段，避免为一次目录探测复制工作区与草稿谱。
        snapshot.m_projectRoot    = m_currentProject->m_projectRoot;
        snapshot.m_beatmaps       = m_currentProject->m_beatmaps;
        snapshot.m_audioResources = m_currentProject->m_audioResources;
        snapshot.m_excludedBeatmapPaths =
            m_currentProject->m_excludedBeatmapPaths;
        snapshot.m_excludedAudioPaths = m_currentProject->m_excludedAudioPaths;
    }
    // 既枚举路径也在文件线程读取谱面，后者可能比目录遍历耗时更长。
    m_state->m_directoryScan =
        worker->enqueue([snapshot = std::move(snapshot),
                         revision = std::move(revision)]() mutable {
            const auto scan =
                ProjectDirectoryScanner{}.scan(snapshot.m_projectRoot);
            // 即使扫描失败也返回已完成的结果槽；消费侧保留活动工程资源。
            // 已生成的资源描述只包含文件名和默认配置，不在此接入引擎音效图。
            auto sync =
                ProjectResourceService{}.syncDirectoryResources(snapshot, scan);
            return ProjectDirectoryScanResult{ std::move(snapshot),
                                               std::move(revision),
                                               std::move(sync) };
        });
    // 排队失败通常意味着程序正在关闭；旧项目状态始终保留，过期结果不回流。
    return true;
}

/// @brief 只检查已存在的目录任务是否完成。
/// @warning 每轮逻辑循环调用，不查询文件系统，也不等待未完成 future。
bool ProjectController::isDirectoryScanReady() const
{
    using namespace std::chrono_literals;
    // future 槽只由逻辑循环访问，文件线程只完成其共享结果，不写槽本身。
    const auto& job = m_state->m_directoryScan;
    return job.valid() && job.wait_for(0s) == std::future_status::ready;
}

/// @brief 拒绝陈旧结果，再在同一锁下交换资源表。
/// @return 只有基于当前工程修订计算的成功结果才会提交。
/// @pre 调用方在交换资源表时持有会话锁。
/// @details 只替换后台任务负责的两个资源表；设置、草稿轨与工作区继续属于
/// 逻辑线程。项目替换直接丢弃结果，同工程新修订则安排重新扫描。
ProjectController::DirectoryScanUpdate ProjectController::applyDirectoryScan()
{
    if ( !isDirectoryScanReady() ) return {};
    auto       result  = m_state->m_directoryScan.get();
    const auto current = uiSnapshot();
    if ( !m_currentProject || !current || !result.m_revision ||
         result.m_revision->m_generation != current->m_generation )
        return {};
    // 同一次打开期间，普通资源编辑可能更新了配置和排除项。完整扫描不能
    // 把旧值写回；重新读取本次工程的最新状态后再安排一轮。
    if ( result.m_revision != current ) return { .m_retry = true };
    // 只有完成全量枚举及引用检测时才允许删除消失的资源。
    // 同样只让工作者写入由它计算的字段，保留逻辑线程拥有的草稿组设置。
    if ( result.m_sync.m_scanSucceeded ) {
        m_currentProject->m_beatmaps = std::move(result.m_project.m_beatmaps);
        m_currentProject->m_audioResources =
            std::move(result.m_project.m_audioResources);
    }
    return { .m_sync = std::move(result.m_sync) };
}

namespace
{

/// @brief 将已冻结的配置写入正式项目分片，从不借用活动工程。
/// @param snapshot I/O 期间保持稳定的项目值。
/// @return 成功时的清单绝对路径，错误时返回空并记录具体失败。
/// @pre 调用方持有同一磁盘顺序锁，同一工程不能同时打开相同的分片临时文件。
/// @note 路径只来自已捕获的工程根，保存期间无需重新查询活动项目。
/// @warning 进行序列化及磁盘 I/O；监视刷新只能在文件线程调用。
std::filesystem::path writeProjectSnapshot(const Project& snapshot)
{
    const auto& root     = snapshot.m_projectRoot;
    const auto  manifest = ProjectStorage::manifestPath(root);
    XINFO("Saving split project storage to {}", Config::pathToUtf8(manifest));
    const ProjectStorage storage;
    std::string          errorMessage;
    if ( !storage.save(snapshot, root, errorMessage) ) {
        XERROR("Failed to save split project storage: {}", errorMessage);
        return {};
    }
    // 分片全部成功才删除旧格式，清理失败仍不发布保存成功。
    if ( !storage.removeLegacyProjectFile(root, errorMessage) ) {
        XERROR("Failed to remove legacy project configuration: {}",
               errorMessage);
        return {};
    }
    XINFO("Project saved successfully.");
    return manifest;
}

/// @brief 在写入锁外发出成功事件，避免回调与磁盘事务互相等待。
/// @param manifest 已完整写入且仍有效的工程清单路径。
/// @note 后台成功由逻辑循环重新校验序号后发布；显式保存沿用原调用线程。
void publishProjectSave(const std::filesystem::path& manifest)
{
    Event::EventBus::instance().publish(Event::ProjectSavedEvent{
        .m_projectFilePath = Config::pathToUtf8(manifest),
    });
}

}  // namespace

/// @brief 冻结刷新后的配置，重复的目录变更覆盖尚未提交的旧保存。
/// @pre 调用方已在会话锁内提交资源变更，并捕获本次工作区。
/// @details 按实际内存资源发布 UI 快照，不要求写入成功才显示新列表。
void ProjectController::queueDirectorySave()
{
    if ( !m_currentProject ) return;
    // 用完整值记录未来写出的一致状态。相同工程中的新保存一定以更晚的
    // 序号替代此前待写配置，不能根据最终任务提交先后推断版本先后。
    publishUiSnapshot();
    const auto revision = m_state->m_directorySaveOrder->m_latest.fetch_add(
                              1, std::memory_order_relaxed) +
                          1;
    m_state->m_pendingDirectorySave =
        ProjectDirectorySave{ *m_currentProject, revision };
}

/// @brief 轮询后台写入，过时结果不得发布保存成功。
/// @details 每轮最多一份在途和一份待写；磁盘速度不会扩大队列。
/// 文件任务相互隔离，不持有会话锁，也不从文件线程发布界面事件。
/// @warning 热路径仅检查任务状态，既不持有磁盘锁也不等待慢写入。
void ProjectController::pollDirectorySave()
{
    using namespace std::chrono_literals;
    auto&       job   = m_state->m_directorySave;
    const auto& order = m_state->m_directorySaveOrder;
    // 没有磁盘锁的就绪检查既覆盖正在运行的任务，也覆盖文件池中的排队。
    if ( job.valid() ) {
        if ( job.wait_for(0s) != std::future_status::ready ) return;
        const auto result = job.get();
        // 关闭、重开和显式保存同样推进序号，已失效的事件不能刷新新项目。
        if ( !result.m_manifest.empty() &&
             result.m_revision ==
                 order->m_latest.load(std::memory_order_relaxed) )
            publishProjectSave(result.m_manifest);
    }

    // 保存失败已有磁盘错误日志；正常退出仍由既有同步保存负责确认失败。
    auto& pending = m_state->m_pendingDirectorySave;
    // 无新内容时不进入线程池队列锁，也不接触全局路径设置。
    if ( !pending ) return;
    // 延迟到快照确定且旧写入结束后才访问文件池，避免排队多份完整工程。
    auto* worker = Runtime::AppThreadPool::instance().getFileThreadPool();
    if ( worker && pending->m_revision ==
                       order->m_latest.load(std::memory_order_relaxed) ) {
        // 写入前再在同一磁盘串行区核对顺序。较新的显式保存先完成时，
        // 已排队的旧任务不能在后台覆盖它。
        // 新通知只占一个可替换的待写槽，不阻塞已开始写入的旧配置。
        job = worker->enqueue([order, request = std::move(*pending)] {
            std::lock_guard            diskLock(order->m_diskMutex);
            ProjectDirectorySaveResult result{ .m_revision =
                                                   request.m_revision };
            if ( request.m_revision ==
                 order->m_latest.load(std::memory_order_relaxed) )
                result.m_manifest = writeProjectSnapshot(request.m_project);
            return result;
        });
    }
    // 拒绝或提交结束均释放待写槽，关闭路径不退化为同步 I/O。
    pending.reset();
}

/// @brief 同步保存当前工程，使用与后台保存相同的磁盘顺序门禁。
/// @return 分片与旧格式清理均成功时为真。
/// @warning 显式保存或关闭工程低频路径，可等待已开始写出的后台保存完成。
/// @details 先作废排队快照，再取得磁盘锁。若后台已经开始写入，当前完整
/// 配置在它之后写，避免旧后台结果覆盖更晚的显式保存。
bool ProjectController::saveProject()
{
    // 同步保存仍保留原调用方的错误处理，关闭工程可据失败保留打开状态。
    if ( !m_currentProject ) return false;
    // UI 按内存实际内容刷新，即使磁盘保存失败也不能继续使用旧资源 ID。
    publishUiSnapshot();
    const auto& order = m_state->m_directorySaveOrder;
    order->m_latest.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path manifest;
    {
        std::lock_guard diskLock(order->m_diskMutex);
        manifest = writeProjectSnapshot(*m_currentProject);
    }
    // 同步分片错误立即交给调用方，避免关闭成功掩盖最后一次保存失败。
    // 成功时才发布清单；目录刷新成功事件与持久化成功事件是独立信号。
    if ( manifest.empty() ) return false;
    publishProjectSave(manifest);
    return true;
}

}  // namespace MMM::Logic
