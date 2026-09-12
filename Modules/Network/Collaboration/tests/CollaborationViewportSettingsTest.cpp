#include "network/collaboration/CollaborationRoom.h"

#include <cstdint>

/// @brief 验证房间层视野同步频率的默认值和可调边界。
/// @return 全部频率约束符合预期时返回 0。
int main()
{
    // 新房间应采用兼顾流畅度与网络负载的 10 Hz 默认值。
    MMM::Network::Collaboration::CollaborationRoom room;
    if ( room.viewportPublishRateHz() != 10U ) return 1;

    // 低于允许范围的输入应钳制到 5 Hz 下限。
    room.setViewportPublishRateHz(1U);
    if ( room.viewportPublishRateHz() != 5U ) return 2;

    // 合法区间内的非边界值必须原样保留。
    room.setViewportPublishRateHz(37U);
    if ( room.viewportPublishRateHz() != 37U ) return 3;

    // 高于允许范围的输入应钳制到 60 Hz 上限。
    room.setViewportPublishRateHz(120U);
    if ( room.viewportPublishRateHz() != 60U ) return 4;
    // 默认值、区间内值和两侧钳制规则全部符合协议约束。
    return 0;
}
