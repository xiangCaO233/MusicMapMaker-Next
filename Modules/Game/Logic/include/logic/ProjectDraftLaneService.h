#pragma once

#include <cstdint>

namespace MMM
{
class Project;
}

namespace MMM::Logic
{

struct SessionContext;

/// @brief 管理按主音频资源共享的项目级草稿轨物件。
class ProjectDraftLaneService
{
public:
    /// @brief 为刚载入谱面的会话解析共享组并载入草稿物件。
    /// @param ctx 目标会话上下文。
    /// @param project 谱面所属项目；为空时禁用草稿共享。
    /// @warning 低频谱面加载路径：会解析完整草稿载荷并重建对应 ECS 实体。
    static void load(SessionContext& ctx, Project* project);

    /// @brief 在共享组版本变化时增量刷新当前会话的草稿物件。
    /// @param ctx 待检查会话上下文。
    /// @warning 每 update 调用；正常路径只查找小型项目组列表并比较版本，
    /// 只有其他画布提交草稿编辑后才解析载荷并重建草稿实体。
    static void refreshIfChanged(SessionContext& ctx);

    /// @brief 将当前会话的草稿物件写回项目共享组。
    /// @param ctx 草稿物件来源会话。
    /// @warning 用户提交、撤销或重做草稿编辑时调用；会完整收集草稿物件并
    /// 仅更新内存项目，持久化由显式保存或项目关闭流程负责。
    static void sync(SessionContext& ctx);
};

}  // namespace MMM::Logic
