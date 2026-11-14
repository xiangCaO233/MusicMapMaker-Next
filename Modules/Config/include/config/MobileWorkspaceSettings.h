#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{
/// @brief 仅属于本机移动外壳的布局值，不参与谱面或桌面布局预设。
/// @details 保存显隐、比例和稳定标签身份，不保存屏幕像素坐标或窗口指针。
struct MobileWorkspaceSettings {
    /// @brief 首次启动显示工具栏、时间线和预览；旧配置缺字段也沿用该默认值。
    bool m_toolbar{ true }, m_timing{ true }, m_preview{ true };
    /// @brief 设置页仍由用户主动打开，不能随辅助窗口一起占用首次启动主区。
    bool m_settingsOpen{};
    /// @brief 工作台默认可用，但只在主画布选中时实际占用布局空间。
    bool m_workbench{ true };
    /// @brief 左右页耳默认贴底；独立归一化中心保留用户拖动后的停放位置。
    /// @details 归一化值一由布局夹紧到完整触摸区的最低位置，不裁切页耳。
    float m_leftCenter{ 1.0f }, m_rightCenter{ 1.0f };
    /// @brief 默认收边减少画布遮挡，左右独立保存且不影响归一化中心。
    bool m_leftHandleDocked{ true }, m_rightHandleDocked{ true };
    /// @brief 辅助分区占整个中央宿主宽度的比例，隐藏时保留上次值。
    float m_timingRatio{ .28f }, m_previewRatio{ .2f },
        m_workbenchRatio{ .17f };
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
