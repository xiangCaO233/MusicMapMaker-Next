#pragma once

#include "mmm/note/Note.h"

namespace MMM
{

/// @brief 具有持续时间的玩家长条物件。
/// @details
/// 起点沿用 Note::m_timestamp，终点由起点加 `m_duration` 得到。对象本身不
/// 强制持续时间为正，以便导入层保留来源数据并由上层校验策略决定如何处理。
class Hold : public Note
{
public:
    /// @brief 构造零长度长条并固定物件类型。
    Hold() { m_type = NoteType::HOLD; }
    /// @brief 移动构造完整长条值。
    Hold(Hold&&) = default;
    /// @brief 复制构造完整长条值。
    Hold(const Hold&) = default;
    /// @brief 移动赋值完整长条值。
    Hold& operator=(Hold&&) = default;
    /// @brief 复制赋值完整长条值。
    Hold& operator=(const Hold&) = default;
    /// @brief 通过 Note 的虚析构链安全销毁。
    ~Hold() override = default;

    /// @brief 从起点到终点的持续时间，单位为毫秒。
    double m_duration{ .0 };

    /// @brief 从 osu!mania HitObject 字段加载长条。
    /// @param description 按逗号切分的 HitObject 字段。
    /// @param orbit_count 目标玩家轨道数。
    void from_osu_description(const std::vector<std::string>& description,
                              int32_t orbit_count) override;
    /// @brief 转换为 osu!mania Hold HitObject 描述。
    /// @param orbit_count 用于把内部轨道映射回 0 至 512 的轨道坐标。
    /// @return 可写入 `[HitObjects]` 的单行字段。
    std::string to_osu_description(int32_t orbit_count) override;
};

}  // namespace MMM
