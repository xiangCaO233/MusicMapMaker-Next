#pragma once

#include "mmm/beatmap/BeatmapMutationObserver.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace MMM::Logic
{

struct SessionContext;

/// @brief 编辑操作接口，所有撤销/重做操作的基类。
class IEditorAction
{
public:
    virtual ~IEditorAction() = default;

    /// @brief 执行操作 (初次执行)
    /// @param ctx 会话上下文引用
    virtual void execute(SessionContext& ctx) = 0;

    /// @brief 撤销操作
    /// @param ctx 会话上下文引用
    virtual void undo(SessionContext& ctx) = 0;

    /// @brief 重做操作
    /// @param ctx 会话上下文引用
    virtual void redo(SessionContext& ctx) = 0;

    /// @brief 获取操作描述名称
    virtual std::string getName() const = 0;

    /// @brief 返回该操作执行、撤销或重做会修改的谱面数据类别。
    [[nodiscard]] virtual ::MMM::BeatmapMutationFlags mutationFlags() const = 0;

    /// @brief 教学创建所属步骤；普通动作保持零，不受引导回退影响。
    std::uint64_t m_walkthroughToken{ 0 };
    /// @brief 构造只移除本动作教学产物的补偿动作；默认不支持。
    /// @note 不重放普通 undo，以免恢复布局或覆盖其他后续编辑。
    virtual std::unique_ptr<IEditorAction> walkthroughRollback(SessionContext&)
    {
        return {};
    }
};

/// @brief 操作栈管理器，维护撤销栈和重做栈。
class EditorActionStack
{
public:
    /// @brief 执行并推送新操作到栈中，同时清空重做栈
    /// @param action 要执行的操作
    /// @param ctx 会话上下文引用
    void pushAndExecute(std::unique_ptr<IEditorAction> action,
                        SessionContext&                ctx);

    /// @brief 执行撤销
    /// @param ctx 会话上下文引用
    void undo(SessionContext& ctx);

    /// @brief 精确回滚指定教学步骤创建的物件，保留其余历史和内容。
    /// @warning 仅用户点击返回时扫描历史，不允许逐帧调用。
    void rollbackWalkthrough(std::uint64_t token, SessionContext& ctx);

    /// @brief 执行重做
    /// @param ctx 会话上下文引用
    void redo(SessionContext& ctx);

    /// @brief 清空所有栈
    void clear();

    /// @brief 是否有未保存的修改
    bool isDirty() const;

    /// @brief 标记当前状态为已保存
    void markSaved();

    /// @brief 捕获当前编辑状态的单调修订号。
    /// @return 后续异步保存完成时用于核对内容是否仍未变化的修订号。
    [[nodiscard]] std::uint64_t captureSaveRevision() const;

    /// @brief 仅在内容未越过指定修订时确认异步保存点。
    /// @param revision 保存快照创建时捕获的编辑修订号。
    /// @return 当前内容仍与保存快照一致并已清除脏状态时返回 true。
    /// @note 保存期间若发生编辑、撤销或重做，则保守保留未保存状态，避免把
    /// 后续内容误标为已经写入磁盘。
    bool markSavedIfUnchanged(std::uint64_t revision);

    /// @brief 标记一次未进入撤销栈的编辑为未保存。
    void markDirty();

    /// @brief 取出尚未由 BeatmapSession 发布的操作变化类别。
    /// @return 自上次取出后执行、撤销或重做所修改的谱面数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags takePendingMutationFlags();

    /// @brief 获取下一次撤销将修改的数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags undoMutationFlags() const;

    /// @brief 获取下一次重做将修改的数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags redoMutationFlags() const;

    /// @brief 获取撤销栈深度
    size_t getUndoStackSize() const { return m_undoStack.size(); }

    /// @brief 获取重做栈深度
    size_t getRedoStackSize() const { return m_redoStack.size(); }

private:
    std::vector<std::unique_ptr<IEditorAction>> m_undoStack;  ///< 撤销栈
    std::vector<std::unique_ptr<IEditorAction>> m_redoStack;  ///< 重做栈
    size_t m_saveIndex{ 0 };  ///< 上次保存时的撤销栈深度

    /// @brief 是否存在未进入撤销栈且尚未保存的编辑。
    bool m_hasNonUndoableChanges{ false };

    /// @brief 每次内容状态变化时递增的异步保存校验修订号。
    std::uint64_t m_changeRevision{ 0 };

    /// @brief 等待 BeatmapSession 合并并发布的操作变化类别。
    ::MMM::BeatmapMutationFlags m_pendingMutationFlags{
        ::MMM::BeatmapMutationFlags::None
    };
};

/// @brief 将多个领域操作合并为一次原子撤销记录。
class CompositeEditorAction : public IEditorAction
{
public:
    /// @brief 构造复合操作。
    /// @param actions 按执行顺序排列的子操作。
    /// @param name 用户可读操作名称。
    CompositeEditorAction(std::vector<std::unique_ptr<IEditorAction>> actions,
                          std::string                                 name)
        : m_actions(std::move(actions)), m_name(std::move(name))
    {
    }

    /// @brief 按顺序执行全部子操作。
    void execute(SessionContext& ctx) override;

    /// @brief 按逆序撤销全部子操作。
    void undo(SessionContext& ctx) override;

    /// @brief 按顺序重做全部子操作。
    void redo(SessionContext& ctx) override;

    /// @brief 获取复合操作名称。
    std::string getName() const override;

    /// @brief 合并全部子操作的数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override;

private:
    std::vector<std::unique_ptr<IEditorAction>> m_actions;  ///< 子操作。
    std::string                                 m_name;  ///< 用户可读操作名称。
};

}  // namespace MMM::Logic
