#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveProgressEvent.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace MMM::Event
{
/// @brief 在 Event 模块内提供唯一的文件操作门闩，兼容动态库链接。
/// @return 进程内共享的文件操作互斥量。
std::mutex& beatmapFileOperationGate()
{
    // 函数局部静态量将实例固定在 Event 动态库内，并保证线程安全初始化。
    static std::mutex gate;
    return gate;
}

namespace detail
{
/// @brief 为首次出现的事件类型生成进程内唯一编号。
/// @return 单调递增但不具备跨进程稳定性的类型编号。
uint64_t nextStaticTypeId()
{
    // 类型初始化可能并发发生，只要求编号唯一，因此使用最弱的 relaxed 顺序。
    static std::atomic<uint64_t> nextTypeId{ 0 };
    return nextTypeId.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

struct EventBus::Impl {
    /// @brief 一条已类型擦除的订阅记录。
    struct SubscriberInfo {
        /// @brief 用于精确取消订阅的进程内编号。
        SubscriptionID id;
        /// @brief 已类型擦除的同步回调。
        std::function<void(const void*)> callback;
    };

    /// @brief 按事件类型 ID 分组的订阅表。
    std::unordered_map<uint64_t, std::vector<SubscriberInfo>> subscribers;

    /// @brief 保护订阅表结构和其中所有订阅记录的读写锁。
    /// @note 发布只持有共享锁复制快照，增删订阅需要独占锁。
    std::shared_mutex mutex;

    /// @brief 所有订阅共享的自增 ID。
    /// @note 仅要求唯一性，编号顺序不表达回调优先级。
    std::atomic<SubscriptionID> nextId{ 0 };
};

/// @brief 创建拥有独立订阅表的事件总线。
EventBus::EventBus() : m_impl(std::make_unique<Impl>()) {}

/// @brief 在实现类型完整的位置销毁私有订阅状态。
EventBus::~EventBus() = default;

/// @brief 返回业务代码共享的进程内事件总线。
/// @return 首次调用时创建的静态 EventBus 实例。
EventBus& EventBus::instance()
{
    // C++ 静态局部变量规则保证并发首次访问只构造一次。
    static EventBus evtBus;
    return evtBus;
}

/// @brief 将已类型擦除的回调登记到对应事件类型下。
/// @param typeId 具体事件类型的进程内编号。
/// @param callback 发布时同步调用的回调。
/// @return 新订阅的唯一编号。
SubscriptionID EventBus::subscribeImpl(
    uint64_t typeId, std::function<void(const void*)> callback)
{
    // 独占锁保证分组容器扩容和记录追加不会与发布快照竞争。
    std::unique_lock lock(m_impl->mutex);
    // 零值保留给无效令牌，因此首次生成的有效编号从一开始。
    const SubscriptionID id = ++m_impl->nextId;
    m_impl->subscribers[typeId].push_back({ id, std::move(callback) });
    return id;
}

/// @brief 从指定事件类型分组移除匹配订阅。
/// @param typeId 订阅登记时使用的事件类型编号。
/// @param id 需要移除的订阅编号。
void EventBus::unsubscribeImpl(uint64_t typeId, SubscriptionID id)
{
    // 查找和擦除会改变订阅表，整个操作必须持有独占锁。
    std::unique_lock lock(m_impl->mutex);
    const auto       subscriberIt = m_impl->subscribers.find(typeId);
    // 重复取消或类型不匹配视为空操作，便于 RAII 清理路径保持幂等。
    if ( subscriberIt == m_impl->subscribers.end() ) return;

    auto& subscribers = subscriberIt->second;
    // 同一编号理论上只出现一次，remove_if 同时防御重复记录。
    subscribers.erase(std::remove_if(subscribers.begin(),
                                     subscribers.end(),
                                     [id](const Impl::SubscriberInfo& info) {
                                         return info.id == id;
                                     }),
                      subscribers.end());
}

/// @brief 向精确类型及其父类订阅者同步分发事件。
/// @param relatedTypes 按具体类型到父类顺序排列的类型编号。
/// @param eventPtr 在本次调用返回前保持有效的事件对象地址。
/// @warning 输入热路径可能频繁调用；此处会复制回调快照，回调自身不得阻塞。
void EventBus::publishImpl(const std::vector<uint64_t>& relatedTypes,
                           const void*                  eventPtr)
{
    // 快照让回调可以安全地订阅或取消，而不会在持锁状态下重入订阅表。
    std::vector<Impl::SubscriberInfo> subscribersToCall;
    {
        // 共享锁允许多个发布者并发收集快照，并阻止订阅表在复制时变化。
        std::shared_lock lock(m_impl->mutex);
        for ( const uint64_t targetType : relatedTypes ) {
            const auto subscriberIt = m_impl->subscribers.find(targetType);
            // 没有订阅者的父类分支无需创建空表项。
            if ( subscriberIt == m_impl->subscribers.end() ) continue;

            // 保持类型层级与各分组内部的登记顺序，不额外排序回调。
            subscribersToCall.insert(subscribersToCall.end(),
                                     subscriberIt->second.begin(),
                                     subscriberIt->second.end());
        }
    }

    // 锁外执行用户回调，避免长回调阻塞其他线程增删订阅。
    for ( const auto& subscriber : subscribersToCall ) {
        subscriber.callback(eventPtr);
    }
}
}  // namespace MMM::Event
