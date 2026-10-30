#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{
/// @brief 仅属于本机移动外壳的布局值，不参与谱面或桌面布局预设。
/// @details 保存显隐、比例和稳定标签身份，不保存屏幕像素坐标或窗口指针。
struct MobileWorkspaceSettings {
    /// @brief 辅助窗口默认收起；首次启动仍只有主画布与欢迎两个标签。
    bool m_toolbar{}, m_timing{}, m_preview{}, m_settingsOpen{};
    /// @brief 左右把手独立归一化中心，屏幕尺寸变化后仍可重新定位。
    float m_leftCenter{ .5f }, m_rightCenter{ .5f };
    /// @brief 辅助分区占整个中央宿主宽度的比例，隐藏时保留上次值。
    float m_timingRatio{ .28f }, m_previewRatio{ .2f };
    /// @brief 主区选中标签的稳定哈希；找不到旧窗口时由原生停靠选择回退。
    std::uint32_t m_selectedTab{};
    /// @brief 左资源抽屉选中类别，零表示尚未选择，不恢复抽屉的展开动画。
    int m_resourceTab{};
    /// @brief 标量值比较用于提交边沿去重，空闲帧不生成 JSON 快照。
    bool operator==(const MobileWorkspaceSettings&) const = default;
};

/// @brief 序列化客户端移动布局，定义与轻量值类型保持同一 ADL 命名空间。
void to_json(nlohmann::json& json, const MobileWorkspaceSettings& value);
/// @brief 旧配置缺字段时保留默认值，损坏或非有限数值不传播到布局计算。
void from_json(const nlohmann::json& json, MobileWorkspaceSettings& value);
}  // namespace MMM::Config
