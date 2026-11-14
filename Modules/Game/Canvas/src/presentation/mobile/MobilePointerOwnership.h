#pragma once
#include "graphic/mobile/MobileTouchInput.h"
#include <cstdint>

namespace MMM::Canvas
{
/// @brief 单次物理手势只归首次落点画布，双指后不能退回单指编辑。
/// @details 使用原始代际而非合成鼠标边沿，防止后端排队的释放重新落笔。
/// 不缓存会话或窗口指针；调用者仅提供这次落点与遮罩的判定结果。
class MobilePointerOwnership
{
public:
    /// @brief 发布仲裁边沿；返回 true 表示此次需要取消已有编辑预览。
    /// @warning 每帧固定标量运算，无分配、阻塞或线程同步。
    /// @param touch 同 UI 线程发布的值快照，不跨线程借用。
    /// @param acceptsOrigin 只在首次遇到代际时采样，途中进入画布不生效。
    /// @param interrupted 播放或遮罩接管后，不因遮罩消失恢复旧编辑。
    /// @return 仅第一次接管返回取消边沿，其余帧持续屏蔽而不重复命令。
    /// @note 即使两个触点在一帧内都抬起，峰值仍能阻止合成鼠标起笔。
    /// @note 系统取消与双指接管共享门闩，下次真实 Down 才重新归属。
    bool update(const Graphic::MobileTouchSnapshot& touch, bool acceptsOrigin,
                bool interrupted)
    {
        if ( touch.m_epoch == 0 ) return false;
        if ( m_epoch != touch.m_epoch ) {
            // 新代际才允许重新归属，剩下的一根手指仍属于旧代际。
            m_epoch        = touch.m_epoch;
            m_owned        = acceptsOrigin;
            m_blocked      = !m_owned;
            m_cancelIssued = false;
        }
        if ( touch.m_peak >= 2 || touch.m_cancelled || interrupted )
            m_blocked = true;
        if ( m_owned && m_blocked && !m_cancelIssued ) {
            // 取消仅发送一次，随后所有残留鼠标帧继续被屏蔽。
            m_cancelIssued = true;
            return true;
        }
        return false;
    }
    /// @brief 原始设备未发布过输入时允许 CPU 夹具使用原有鼠标路径。
    bool blocksSingle() const { return m_epoch != 0 && m_blocked; }
    /// @brief 导航只能使用最初落点归本画布的手势。
    bool ownsGesture() const { return m_owned; }
    /// @brief 只在触点仍按住时持有画布，结束代际不妨碍方案切换。
    bool active(const Graphic::MobileTouchSnapshot& touch) const
    {
        return m_owned && touch.m_count != 0 && !touch.m_cancelled;
    }

private:
    std::uint64_t m_epoch{};  ///< SDL 原始手势代际，不复用鼠标双击编号。
    bool m_owned{}, m_blocked{}, m_cancelIssued{};  ///< 单向接管的本地门闩。
};
}  // namespace MMM::Canvas
