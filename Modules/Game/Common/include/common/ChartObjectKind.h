#pragma once

#include <cstdint>

namespace MMM::Logic
{

/// @brief 标识独立 ECS 注册表中的谱面物件领域。
/// @note 该枚举用于选择数据域，不能代替具体物件类型或实体 ID。
/// @note 显式底层类型便于在轻量命令载荷中稳定传值。
enum class ChartObjectKind : std::uint8_t {
    // 玩家音符与项目草稿音符位于不同注册表，必须保留来源差异。
    PlayerNote = 0,  ///< 需要玩家操作的 Note。
    DraftNote,       ///< 项目级草稿轨中的 Note。
    // 自动采样属于音频时间线对象，不参与玩家音符判定。
    AudioSample,  ///< 无需玩家操作的自动采样。
};

}  // namespace MMM::Logic
