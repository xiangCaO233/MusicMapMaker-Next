#include "network/collaboration/CollaborationRoom.h"

#include <cstdint>

/// @brief 验证房间层视野同步频率的默认值和可调边界。
/// @return 全部频率约束符合预期时返回 0。
int main()
{
    MMM::Network::Collaboration::CollaborationRoom room;
    if ( room.viewportPublishRateHz() != 10U ) return 1;

    room.setViewportPublishRateHz(1U);
    if ( room.viewportPublishRateHz() != 5U ) return 2;

    room.setViewportPublishRateHz(37U);
    if ( room.viewportPublishRateHz() != 37U ) return 3;

    room.setViewportPublishRateHz(120U);
    if ( room.viewportPublishRateHz() != 60U ) return 4;
    return 0;
}
