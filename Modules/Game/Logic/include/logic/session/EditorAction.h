#pragma once

#include <memory>
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

    /// @brief 执行重做
    /// @param ctx 会话上下文引用
    void redo(SessionContext& ctx);

    /// @brief 清空所有栈
    void clear();

private:
    std::vector<std::unique_ptr<IEditorAction>> m_undoStack;  ///< 撤销栈
    std::vector<std::unique_ptr<IEditorAction>> m_redoStack;  ///< 重做栈
};

}  // namespace MMM::Logic
