#pragma once

#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/EditorAction.h"

#include <cstdint>
#include <entt/entt.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MMM::Logic
{

/// @brief 单个自动采样的创建、删除或更新操作。
class SampleAction : public IEditorAction
{
public:
    /// @brief 自动采样操作类型。
    enum class Type { Update, Create, Delete };

    /// @brief 构造自动采样操作。
    /// @param type 操作类型。
    /// @param entity 关联实体。
    /// @param before 变更前数据。
    /// @param after 变更后数据。
    SampleAction(Type type, entt::entity entity,
                 std::optional<SampleComponent> before,
                 std::optional<SampleComponent> after)
        : m_type(type)
        , m_entity(entity)
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
    }

    /// @brief 执行自动采样操作。
    void execute(SessionContext& ctx) override;

    /// @brief 撤销自动采样操作。
    void undo(SessionContext& ctx) override;

    /// @brief 重做自动采样操作。
    void redo(SessionContext& ctx) override;

    /// @brief 获取用户可读操作名称。
    std::string getName() const override;
    /// @brief 返回自动采样及其实际扩展的 BGM 轨道元数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return m_beforeBgmTrackCount && m_afterBgmTrackCount &&
                       *m_beforeBgmTrackCount != *m_afterBgmTrackCount
                   ? ::MMM::BeatmapMutationFlags::AudioSamples |
                         ::MMM::BeatmapMutationFlags::Metadata
                   : ::MMM::BeatmapMutationFlags::AudioSamples;
    }

private:
    Type                           m_type;    ///< 操作类型。
    entt::entity                   m_entity;  ///< 关联实体。
    std::optional<SampleComponent> m_before;  ///< 变更前数据。
    std::optional<SampleComponent> m_after;   ///< 变更后数据。
    /// @brief 首次执行前的持久化 BGM 轨道数。
    std::optional<std::int32_t> m_beforeBgmTrackCount;
    /// @brief 首次执行后可能扩展的持久化 BGM 轨道数。
    std::optional<std::int32_t> m_afterBgmTrackCount;
};

/// @brief 批量自动采样操作，保证一次撤销恢复完整集合。
class BatchSampleAction : public IEditorAction
{
public:
    /// @brief 单个批量条目。
    struct Entry {
        entt::entity                   entity{ entt::null };  ///< 关联实体。
        std::optional<SampleComponent> before;                ///< 变更前数据。
        std::optional<SampleComponent> after;                 ///< 变更后数据。
        /// @brief 变更前的选中状态；为空时不覆盖交互组件。
        std::optional<bool> beforeSelected;
        /// @brief 变更后的选中状态；为空时不覆盖交互组件。
        std::optional<bool> afterSelected;
    };

    /// @brief 构造批量自动采样操作。
    /// @param entries 批量条目。
    /// @param name 操作名称。
    BatchSampleAction(std::vector<Entry> entries,
                      std::string        name = "批量自动采样操作")
        : m_entries(std::move(entries)), m_name(std::move(name))
    {
    }

    /// @brief 执行批量操作。
    void execute(SessionContext& ctx) override;

    /// @brief 撤销批量操作。
    void undo(SessionContext& ctx) override;

    /// @brief 重做批量操作。
    void redo(SessionContext& ctx) override;

    /// @brief 获取用户可读操作名称。
    std::string getName() const override;
    /// @brief 返回批量自动采样及其实际扩展的 BGM 轨道元数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return m_beforeBgmTrackCount && m_afterBgmTrackCount &&
                       *m_beforeBgmTrackCount != *m_afterBgmTrackCount
                   ? ::MMM::BeatmapMutationFlags::AudioSamples |
                         ::MMM::BeatmapMutationFlags::Metadata
                   : ::MMM::BeatmapMutationFlags::AudioSamples;
    }

private:
    std::vector<Entry> m_entries;  ///< 批量条目。
    std::string        m_name;     ///< 操作名称。
    /// @brief 首次执行前的持久化 BGM 轨道数。
    std::optional<std::int32_t> m_beforeBgmTrackCount;
    /// @brief 首次执行后可能扩展的持久化 BGM 轨道数。
    std::optional<std::int32_t> m_afterBgmTrackCount;
};

/// @brief 改变玩家轨道数并保持全部自动采样 BGM 相对索引不变。
class TrackCountAction : public IEditorAction
{
public:
    /// @brief 单个自动采样的轨道迁移记录。
    struct SampleTrackChange {
        entt::entity  entity{ entt::null };  ///< 自动采样实体。
        std::uint32_t beforeTrack{ 0 };      ///< 原绝对轨道。
        std::uint32_t afterTrack{ 0 };       ///< 新绝对轨道。
    };

    /// @brief 构造轨道数迁移操作。
    /// @param beforeTrackCount 原玩家轨道数。
    /// @param afterTrackCount 新玩家轨道数。
    /// @param sampleChanges 全部自动采样的绝对轨道迁移。
    TrackCountAction(std::int32_t                   beforeTrackCount,
                     std::int32_t                   afterTrackCount,
                     std::vector<SampleTrackChange> sampleChanges)
        : m_beforeTrackCount(beforeTrackCount)
        , m_afterTrackCount(afterTrackCount)
        , m_sampleChanges(std::move(sampleChanges))
    {
    }

    /// @brief 执行玩家轨道数和自动采样轨道迁移。
    void execute(SessionContext& ctx) override;

    /// @brief 撤销玩家轨道数和自动采样轨道迁移。
    void undo(SessionContext& ctx) override;

    /// @brief 重做玩家轨道数和自动采样轨道迁移。
    void redo(SessionContext& ctx) override;

    /// @brief 获取用户可读操作名称。
    std::string getName() const override;
    /// @brief 轨道数操作修改元数据，并在存在自动采样时迁移其绝对轨道。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return m_sampleChanges.empty()
                   ? ::MMM::BeatmapMutationFlags::Metadata
                   : ::MMM::BeatmapMutationFlags::Metadata |
                         ::MMM::BeatmapMutationFlags::AudioSamples;
    }

private:
    /// @brief 应用轨道数及对应自动采样绝对轨道。
    /// @param ctx 会话上下文。
    /// @param trackCount 目标玩家轨道数。
    /// @param useAfterTrack 是否应用迁移后的自动采样轨道。
    void apply(SessionContext& ctx, std::int32_t trackCount,
               bool useAfterTrack);

    std::int32_t                   m_beforeTrackCount{ 0 };  ///< 原玩家轨道数。
    std::int32_t                   m_afterTrackCount{ 0 };   ///< 新玩家轨道数。
    std::vector<SampleTrackChange> m_sampleChanges;  ///< 自动采样轨道迁移表。
};

/// @brief 改变持久化 BGM 轨道数量。
class BgmTrackCountAction : public IEditorAction
{
public:
    /// @brief 构造 BGM 轨道数量操作。
    /// @param beforeBgmTrackCount 原持久化 BGM 轨道数量。
    /// @param afterBgmTrackCount 新持久化 BGM 轨道数量。
    BgmTrackCountAction(std::int32_t beforeBgmTrackCount,
                        std::int32_t afterBgmTrackCount)
        : m_beforeBgmTrackCount(beforeBgmTrackCount)
        , m_afterBgmTrackCount(afterBgmTrackCount)
    {
    }

    /// @brief 应用新 BGM 轨道数量。
    void execute(SessionContext& ctx) override;

    /// @brief 恢复原 BGM 轨道数量。
    void undo(SessionContext& ctx) override;

    /// @brief 重新应用新 BGM 轨道数量。
    void redo(SessionContext& ctx) override;

    /// @brief 获取用户可读操作名称。
    std::string getName() const override;
    /// @brief BGM 轨道数量属于谱面元数据。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Metadata;
    }

private:
    /// @brief 应用持久化 BGM 轨道数量并同步谱面元数据。
    /// @param ctx 会话上下文。
    /// @param bgmTrackCount 目标持久化 BGM 轨道数量。
    void apply(SessionContext& ctx, std::int32_t bgmTrackCount);

    std::int32_t m_beforeBgmTrackCount{ 0 };  ///< 原持久化 BGM 轨道数量。
    std::int32_t m_afterBgmTrackCount{ 0 };   ///< 新持久化 BGM 轨道数量。
};

}  // namespace MMM::Logic
