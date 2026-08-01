#include "event/core/EventBus.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace MMM::Event
{
namespace detail
{
uint64_t nextStaticTypeId()
{
    static std::atomic<uint64_t> nextTypeId{ 0 };
    return nextTypeId.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

struct EventBus::Impl {
    /// @brief 一条已类型擦除的订阅记录。
    struct SubscriberInfo {
        SubscriptionID                   id;
        std::function<void(const void*)> callback;
    };

    /// @brief 按事件类型 ID 分组的订阅表。
    std::unordered_map<uint64_t, std::vector<SubscriberInfo>> subscribers;

    /// @brief 保护订阅表的读写锁。
    std::shared_mutex mutex;

    /// @brief 所有订阅共享的自增 ID。
    std::atomic<SubscriptionID> nextId{ 0 };
};

EventBus::EventBus() : m_impl(std::make_unique<Impl>()) {}

EventBus::~EventBus() = default;

EventBus& EventBus::instance()
{
    static EventBus evtBus;
    return evtBus;
}

SubscriptionID EventBus::subscribeImpl(
    uint64_t typeId, std::function<void(const void*)> callback)
{
    std::unique_lock     lock(m_impl->mutex);
    const SubscriptionID id = ++m_impl->nextId;
    m_impl->subscribers[typeId].push_back({ id, std::move(callback) });
    return id;
}

void EventBus::unsubscribeImpl(uint64_t typeId, SubscriptionID id)
{
    std::unique_lock lock(m_impl->mutex);
    const auto       subscriberIt = m_impl->subscribers.find(typeId);
    if ( subscriberIt == m_impl->subscribers.end() ) return;

    auto& subscribers = subscriberIt->second;
    subscribers.erase(std::remove_if(subscribers.begin(),
                                     subscribers.end(),
                                     [id](const Impl::SubscriberInfo& info) {
                                         return info.id == id;
                                     }),
                      subscribers.end());
}

void EventBus::publishImpl(const std::vector<uint64_t>& relatedTypes,
                           const void*                  eventPtr)
{
    std::vector<Impl::SubscriberInfo> subscribersToCall;
    {
        std::shared_lock lock(m_impl->mutex);
        for ( const uint64_t targetType : relatedTypes ) {
            const auto subscriberIt = m_impl->subscribers.find(targetType);
            if ( subscriberIt == m_impl->subscribers.end() ) continue;

            subscribersToCall.insert(subscribersToCall.end(),
                                     subscriberIt->second.begin(),
                                     subscriberIt->second.end());
        }
    }

    for ( const auto& subscriber : subscribersToCall ) {
        subscriber.callback(eventPtr);
    }
}
}  // namespace MMM::Event
