#pragma once

#include "logic/BeatmapSession.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM
{
struct BaseMapMeta;
}

namespace MMM::Logic
{

/// @brief 会话持久化组件。逻辑线程推进状态，UI 仅投递自动保存与备份意图。
struct BeatmapSession::Persistence {
    /// @brief 创建稳定的持久化状态，后台快照按需创建。
    Persistence();
    /// @brief 释放会话持有的在途操作，独立文件任务自行收尾。
    ~Persistence();

    /// @brief 跨线程请求一次由指定编辑器事件触发的自动保存与自动备份。
    /// @param trigger 要合并到待处理位集合中的编辑器事件。
    /// @note 不保证请求一定产生文件，消费方仍检查模式、脏数据与忙碌状态。
    /// @warning
    /// 多个事件来源写入位集合，逻辑线程交换消费；原子只承载事件位，不发布谱面数据。
    void requestAutoSave(AutoSaveTrigger trigger);

    /// @brief 判断后台会话是否仍需轮询自动保存或自动备份。
    /// @param saveConfig 全局保存调度策略。
    /// @param backupConfig 项目覆盖后的备份调度策略。
    /// @return 存在待处理请求或满足定时轮询条件时返回 true。
    /// @warning 调度热路径只读取请求位和会话状态；请求位由事件来源跨线程写入。
    bool needsAutoSavePolling(
        const SessionContext& context, const Config::AutoSaveConfig& saveConfig,
        const Config::AutoBackupConfig& backupConfig) const;

    /// @brief 在元数据停止变化且会话空闲后执行一次尾随自动保存。
    /// @param currentSysTime 当前单调系统时间，单位秒。
    /// @param isEditingBusy 是否仍在进行编辑手势。
    /// @note
    /// 请求计时重置优先于超时判断，刚发生的元数据编辑不能沿用旧截止点保存。
    /// @warning 逻辑热路径：普通帧只做常量级状态判断；仅空闲超时分支允许
    /// 调用同步文件保存流程。
    void flushDeferredMetadataAutoSave(SessionContext& context,
                                       double          currentSysTime,
                                       bool            isEditingBusy);

    /// @brief 消费全局配置允许的事件请求并推进定时/事件自动保存。
    /// @param currentSysTime 当前单调系统时间，供截止时间比较。
    /// @param isEditingBusy 是否需要暂缓文件保存以避免打断编辑。
    /// @param config 当前有效的全局自动保存配置。
    /// @note 事件位按种类合并，不保存同类事件次数，不能据此生成逐事件保存历史。
    /// @warning 周期检查只更新状态；到期且非编辑忙碌时可进入同步文件保存流程。
    std::optional<BeatmapSaveKind> takeConfiguredAutoSave(
        const SessionContext& context, double currentSysTime,
        bool isEditingBusy, const Config::AutoSaveConfig& config);

    /// @brief 消费有效项目配置允许的事件请求并推进谱面自动备份。
    /// @param currentSysTime 当前单调系统时间。
    /// @param isEditingBusy 是否存在尚未提交完成的编辑活动。
    /// @param config 项目覆盖后得到的有效备份策略。
    /// @note
    /// 备份与保存有各自的事件交换和截止时间，不能共用其中一方已消费的请求位。
    /// @warning 到期备份会同步完整模型并访问文件系统，仅在非编辑忙碌分支执行。
    void flushConfiguredAutoBackup(SessionContext& context,
                                   double currentSysTime, bool isEditingBusy,
                                   const Config::AutoBackupConfig& config);

    /// @brief 立即落盘尚在等待空闲期的元数据自动保存。
    /// @return 无待处理元数据或保存后所有修改已清除时返回 true。
    /// @note 此强制刷新入口允许覆盖外部修改，不能当成无副作用的状态查询。
    /// @warning 低频阻塞路径：仅允许逻辑线程在打包、项目关闭或尾随自动保存
    /// 超时时调用；可能同步谱面数据、访问文件系统并保存项目配置。
    bool flushPendingMetadataAutoSave(SessionContext& context);

    /// @brief 为打包流程立即保存当前会话中的全部未落盘修改。
    /// @return 无谱面、无修改或保存成功返回 true，否则阻止使用旧文件继续打包。
    /// @note 这里只准备当前会话的源文件，不创建谱包，也不保存其他会话。
    /// @warning
    /// 低频阻塞路径：仅允许逻辑线程在打包前调用；会同步完整谱面并访问文件系统。
    bool saveDirtyBeatmapForPackaging(SessionContext& context);

    /// @brief 创建一致谱面快照并提交后台写入。
    /// @param cmd 保存来源、格式与覆盖策略。
    /// @return 请求已处理或已启动时返回 true；当前会话已有任务时返回 false。
    bool beginAsyncBeatmapSave(SessionContext&       context,
                               const CmdSaveBeatmap& cmd);

    /// @brief 非阻塞轮询后台写入，并在逻辑线程提交会话与项目状态。
    /// @details future
    /// 未就绪时立即返回。成功结果按固定顺序提交：核对谱面身份、更新
    /// 会话路径、安装后台计算的文件哈希、校验动作修订、迁移项目及草稿路径，最后
    /// 保存项目清单。失败只恢复启动前已经存在的元数据尾随请求，不覆盖任务期间新建
    /// 的请求。
    ///
    /// 工作线程已经独立发布文件结果和结束进度，这里不重复发布 UI
    /// 事件。该安排保证
    /// 会话在任务完成前被关闭时气泡仍能结束，同时被关闭对象也不会再参与项目提交。
    /// 哈希读取失败不否定已成功写出的谱面，但必须删除旧基线，使下次 ForceMMM
    /// 覆盖 重新请求保守确认。
    /// @warning 每次 update 调用；未完成分支只执行 future::wait_for(0)。
    void finalizeAsyncBeatmapSave(SessionContext& context);

    /// @brief 在必须立即知道结果的生命周期事务中同步保存当前谱面。
    /// @param cmd 保存来源、格式与覆盖策略。
    /// @return 文件与会话提交成功时返回 true。
    /// @details
    /// 项目关闭和资源打包必须在返回前确认源谱面已经落盘，不能只排队后台
    /// 任务后继续释放项目或读取旧文件。因此该入口保留同步事务，但不会由普通
    /// Ctrl+S、
    /// 定时保存或事件触发保存调用。调用方已经位于明确的低频生命周期边界，并负责
    /// 向用户表现其整体等待状态。
    ///
    /// 同步路径与异步路径保持相同提交规则：冲突检查在写盘前，失败恢复尾随元数据，
    /// 成功后才更新长期路径、哈希、ActionStack 和项目入口。这里可以直接
    /// markSaved， 因为返回前同一逻辑线程不会并发消费后续会话编辑命令。
    bool saveBeatmapBlocking(SessionContext&       context,
                             const CmdSaveBeatmap& cmd);

    /// @brief 记录媒体资源改名别名，供稍后消费的元数据命令校正路径。
    /// @param oldPath 改名前的项目相对路径。
    /// @param newPath 改名后的项目相对路径。
    /// @warning 低频资源变更路径；调用方持有会话注册表锁。
    /// @note 每次改名递增会话代次，随后入队的元数据命令使用新身份。
    /// @note 别名只由逻辑线程消费；跨线程生产者仅接触原子代次。
    /// @note 这里使用 relaxed，因为代次只是顺序标记，不发布别名容器内存。
    /// @note 新旧路径都以项目相对表示传入，和元数据命令规范化结果一致。
    /// @note 同一源文件再次改名时可形成链，消费按改名记录顺序连续投影。
    /// @note 不遍历既有队列，保留并发生产者和文件操作原有的入队顺序。
    void recordResourceRename(const std::filesystem::path& oldPath,
                              const std::filesystem::path& newPath);

    /// @brief 归约一次已提交的谱面变更，更新独立的保存与备份待办。
    /// @warning 命令热路径：只检查配置及设置标志，实际保存另行调度。
    void noteMutation(const Config::AutoSaveConfig&   save,
                      const Config::AutoBackupConfig& backup);

    /// @brief 清理旧谱面专用的尾随保存和备份状态。
    void resetAfterBeatmapChange();

    /// @brief 将载入时的磁盘文件设为外部修改检测基线。
    void establishFileBaseline(const BeatMap* beatmap);

    /// @brief 元数据发生改变后重建尾随保存时钟。
    void scheduleMetadataSave()
    {
        m_metadataAutoSavePending         = true;
        m_metadataAutoSaveTimerNeedsReset = true;
    }

    /// @brief 是否仍有等待空闲的元数据保存。
    bool hasPendingMetadataAutoSave() const
    {
        return m_metadataAutoSavePending;
    }

    /// @brief 是否尚未提交后台谱面保存的结果。
    bool hasInFlightBeatmapSave() const
    {
        return static_cast<bool>(m_asyncSaveOperation);
    }

    /// @brief 取得命令入队时的资源代次，别名容器仅由逻辑线程读取。
    /// @warning 元数据命令入队时以 relaxed 读取，只用于身份排序。
    std::uint64_t resourceRenameGeneration() const
    {
        return m_resourceRenameGeneration.load(std::memory_order_relaxed);
    }

    /// @brief 依据命令入队时的代次，依次迁移过时的媒体路径。
    void applyResourceRenames(BaseMapMeta&  metadata,
                              std::uint64_t generation) const;

private:
    /// @brief 文件任务及其逻辑侧提交信息。
    struct AsyncSaveOperation;

    /// @brief 当前会话唯一的后台谱面保存任务。
    /// @warning 逻辑线程独占；后台任务只持有独立谱面副本和 future 共享状态。
    std::unique_ptr<AsyncSaveOperation> m_asyncSaveOperation;

    /// @brief 一次文件改名及提交它时的会话代次。
    struct ResourceRenameAlias {
        /// @brief 改名前的项目相对路径。
        std::filesystem::path m_oldPath;

        /// @brief 改名后的项目相对路径。
        std::filesystem::path m_newPath;

        /// @brief 改名提交后的单调代次。
        std::uint64_t m_generation{ 0U };
    };

    /// @brief 供入队生产者标记元数据命令所属资源身份代次。
    /// @warning UI 线程仅对元数据命令读取，逻辑线程只在显式改名时写入；
    /// relaxed 足够，因为两端只传递数字身份，别名数据仍由会话锁保护。
    std::atomic_uint64_t m_resourceRenameGeneration{ 0U };

    /// @brief 改名后的旧路径映射，仅迁移早于对应代次入队的元数据命令。
    std::vector<ResourceRenameAlias> m_resourceRenameAliases;

    /// @brief UI 与谱面切换路径提交、逻辑线程消费的自动保存事件位。
    /// @warning 多个低频生产者执行 relaxed fetch_or，逻辑线程每 update
    /// 执行一次 relaxed exchange；事件仅用于唤醒保存调度，不承载其它数据。
    std::atomic_uint8_t m_requestedAutoSaveTriggers{ 0U };

    /// @brief UI 与谱面切换路径提交、逻辑线程消费的自动备份事件位。
    /// @warning 多个低频生产者执行 relaxed fetch_or，逻辑线程每 update
    /// 执行一次 relaxed exchange；事件不承载谱面数据或生命周期。
    std::atomic_uint8_t m_requestedAutoBackupTriggers{ 0U };

    /// @brief 是否有元数据编辑正在等待尾随自动保存。
    bool m_metadataAutoSavePending{ false };

    /// @brief 下一次更新是否需要重置元数据自动保存空闲计时点。
    bool m_metadataAutoSaveTimerNeedsReset{ false };

    /// @brief 最近一次元数据编辑后的自动保存计时点（秒）。
    double m_lastMetadataUpdateTime{ 0.0 };

    /// @brief 是否有已启用的编辑器事件正在等待会话空闲后自动保存。
    bool m_triggeredAutoSavePending{ false };

    /// @brief 当前定时自动保存周期的下一次截止时间（单调秒）。
    double m_timedAutoSaveDeadline{ 0.0 };

    /// @brief 最近一次用于生成截止时间的定时间隔秒数。
    double m_timedAutoSaveIntervalSeconds{ 0.0 };

    /// @brief 是否有已启用的编辑器事件正在等待会话空闲后自动备份。
    bool m_triggeredAutoBackupPending{ false };

    /// @brief 谱面自上次成功备份后是否又产生了持久化内容变化。
    bool m_autoBackupDirty{ false };

    /// @brief 当前定时自动备份周期的下一次截止时间（单调秒）。
    double m_timedAutoBackupDeadline{ 0.0 };

    /// @brief 最近一次用于生成备份截止时间的定时间隔秒数。
    double m_timedAutoBackupIntervalSeconds{ 0.0 };

    /// @brief 当前会话已加载或成功保存过的谱面文件哈希，键为规范化路径。
    std::unordered_map<std::string, std::uint64_t> m_savedBeatmapFileHashes;
};

/// @brief 维护只读表格的请求、投影版本与发布寿命，供 UI 无锁读取。
struct BeatmapSession::TableProjection {
    /// @brief 通知逻辑线程在安全边界投影，并保活最后一份完整快照。
    /// @return 该会话最近一次完整发布，首次逻辑处理前返回空句柄。
    /// @note 调用不保证读到本次请求的结果；逻辑线程会独立推进。
    /// @warning UI 低频调用。请求只传递标志，数据寿命由发布槽的共享句柄承担。
    std::shared_ptr<const AnnotationTableSnapshot>
    requestAnnotationTableSnapshot();

    /// @brief 在渲染缓存完成后按需发布批注表状态。
    /// @pre 会话逻辑线程拥有写入权，调用点已经完成本轮命令或空闲边界。
    /// @note 行和拍位数据各自有修订；播放变化只替换顶部状态。
    /// @note 表格未请求时不为额外窗口构造字符串，也不要求有可见主画布。
    /// @warning 每轮逻辑更新可调用。无请求时只读一个 relaxed 原子；
    /// 修订未变时不复制整表或 BPM。已发布数据由 UI 共享保活以跨过会话关闭。
    void publishAnnotationTableSnapshot(const SessionContext& context);

    /// @brief 合并 UI 请求，并返回逻辑线程上次发布的表格。
    /// @warning 表格可见时每帧读取。共享句柄保证编辑或切换谱面后旧行仍可使用。
    std::shared_ptr<const TimingTableSnapshot> requestTimingTableSnapshot();

    /// @brief 在逻辑线程投影完整 Timing 表格，在修订不变时复用已有发布。
    /// @note 读模型仅由逻辑线程构造。它不携带画布几何或向模型回写的访问权。
    /// @note 使用已完成的滚动缓存修订，确保表格和常规渲染使用同一轮 Timing。
    /// @note 请求可合并；UI 不等待逻辑任务完成，载入时由命令撤回旧发布。
    /// @note 活动谱面不需要打开主画布才能响应请求。空闲分支也会到达此入口。
    /// @warning
    /// 活动会话每轮只检查待处理标志。仅有请求且谱面或修订变化时遍历与排序。
    void publishTimingTableSnapshot(const SessionContext& context);

    /// @brief 谱面载入时撤回上一实例的时间点表，允许下一次轮询重建。
    void invalidateTimingTable()
    {
        m_timingTableRevision.reset();
        m_timingTableSnapshot.store({});
    }

private:
    /// @brief UI 写、逻辑线程读写。连续请求合并，relaxed 只传递意图。
    /// @warning 活动会话每轮检查，关表后不启动新的全表投影。
    std::atomic<bool> m_timingTableRequested{ false };

    /// @brief 逻辑线程发布、UI 读取，保活已排序的时间点表格数据。
    /// @warning 请求或发布时复制句柄，旧表跨编辑或会话销毁继续有效。
    PublishedSharedPtr<const TimingTableSnapshot> m_timingTableSnapshot;

    /// @brief 逻辑侧已经投影的滚动缓存修订，载入时清空以撤回已有发布。
    /// @note 由唯一写者直接查询，避免每次比较时复制已发布的共享句柄。
    std::optional<std::uint64_t> m_timingTableRevision;

    /// @brief UI 轮询设置标志，逻辑更新消费合并的待处理请求。
    /// @warning 每次活动逻辑更新可能读取一次，relaxed 仅传递请求，不传数据。
    std::atomic<bool> m_annotationTableRequested{ false };
    /// @brief 逻辑写、UI 读的快照发布槽，对象独立于会话生命周期。
    /// @warning 只在请求时读取或发布共享句柄，不持有会话更新锁供 UI 访问。
    PublishedSharedPtr<const AnnotationTableSnapshot> m_annotationTableSnapshot;
    /// @brief 逻辑侧记录最后投影的谱面身份，仅比较地址，不借用或解引用旧实例。
    const ::MMM::BeatMap* m_annotationTableBeatmap{};
    /// @brief 已发布批注行的逻辑缓存修订。
    std::uint64_t m_annotationTableRevision{};
    /// @brief 已发布拍位锚点的滚动缓存修订。
    std::uint64_t m_annotationTableTimingRevision{};
};

}  // namespace MMM::Logic
