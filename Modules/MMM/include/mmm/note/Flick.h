#pragma once

#include "mmm/note/Note.h"

namespace MMM
{

/// @brief 带横向滑动方向的瞬时玩家物件。
/// @details
/// Flick 复用 Note 的时间、轨道、采样和元数据；`m_dtrack` 仅描述相对终点，
/// 不改变起始轨道的归属。格式转换器负责把该增量映射到各自坐标体系。
class Flick : public Note
{
public:
    /// @brief 构造默认向右一轨的滑键并固定物件类型。
    Flick() { m_type = NoteType::FLICK; }
    /// @brief 保留值语义，移动时一并转移基类元数据与采样绑定。
    Flick(Flick&&) = default;
    /// @brief 复制完整滑键值。
    Flick(const Flick&) = default;
    /// @brief 移动赋值完整滑键值。
    Flick& operator=(Flick&&) = default;
    /// @brief 复制赋值完整滑键值。
    Flick& operator=(const Flick&) = default;
    /// @brief 通过 Note 的虚析构链安全销毁。
    ~Flick() override = default;

    /// @brief 相对起始轨道的有符号滑动增量。
    /// @details 终点为 `m_track + m_dtrack`；负值表示向低编号轨道滑动。
    int32_t m_dtrack{ 1 };
};

}  // namespace MMM
