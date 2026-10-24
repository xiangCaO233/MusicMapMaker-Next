#pragma once

#include <cstdint>

namespace MMM::Event
{
/// @brief 订阅令牌，用于精确取消一条订阅记录。
/// @note 零值保留为无效令牌，保持总线与窗口适配器的既有约定。
/// 令牌本身不拥有订阅，不依赖回调模板或事件总线实现。
using SubscriptionID = uint64_t;
}  // namespace MMM::Event
