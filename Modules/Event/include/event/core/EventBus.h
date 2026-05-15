#pragma once

#include "event/EventDef.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace MMM::Event
{
namespace detail
{
/// @brief 全局自增计数器,所有类型共享
inline std::atomic<uint64_t> g_nextStaticTypeId{ 0 };

/// @brief 模板化静态类型 ID,替代 typeid/type_index (支持 -fno-rtti)
template<typename T> inline uint64_t getStaticTypeId()
{
    static const uint64_t id =
        g_nextStaticTypeId.fetch_add(1, std::memory_order_relaxed);
    return id;
}
}  // namespace detail

///@brief 订阅令牌，用于取消订阅
using SubscriptionID = uint64_t;

class EventBus
{
public:
    EventBus()                           = default;
    EventBus(EventBus&&)                 = delete;
    EventBus(const EventBus&)            = delete;
    EventBus& operator=(EventBus&&)      = delete;
    EventBus& operator=(const EventBus&) = delete;
    ~EventBus()                          = default;

    /**
     * @brief 获取总线实例
     * @return 返回静态总线实例引用
     */
    static EventBus& instance();

    /**
     * @brief 订阅事件
     * @return 返回订阅 ID，可用于取消订阅
     */
    template<typename EventType>
    SubscriptionID subscribe(std::function<void(const EventType&)> callback)
    {
        std::unique_lock lock(m_mutex);
        auto             index = detail::getStaticTypeId<EventType>();
        SubscriptionID   id    = ++m_nextId;

        m_subscribers[index].push_back(
            { id, [callback](const void* eventPtr) {
                 callback(*static_cast<const EventType*>(eventPtr));
             } });

        return id;
    }

    /**
     * @brief 取消订阅
     * @param id 订阅时返回的 ID
     */
    template<typename EventType> void unsubscribe(SubscriptionID id)
    {
        std::unique_lock lock(m_mutex);
        auto             index = detail::getStaticTypeId<EventType>();

        if ( auto it = m_subscribers.find(index); it != m_subscribers.end() ) {
            auto& vec = it->second;
            // 擦除除移法 (Erase-Remove Idiom)
            vec.erase(std::remove_if(vec.begin(),
                                     vec.end(),
                                     [id](const SubscriberInfo& info) {
                                         return info.id == id;
                                     }),
                      vec.end());
        }
    }

    /**
     * @brief 发布事件
     * 自动分发给当前类型及其所有在 EventTraits 中注册的父类
     */
    template<typename EventType> void publish(const EventType& event)
    {
        // 1. 确保继承关系被缓存（懒加载，仅在首次发布某类型时执行）
        cache_inheritance<EventType>();

        auto index = detail::getStaticTypeId<EventType>();

        // 2. 安全地获取相关类型索引列表 (克隆一份以保证线程安全)
        std::vector<uint64_t> relatedTypes;
        {
            std::lock_guard cacheLock(m_cacheMutex);
            relatedTypes = m_inheritanceCache[index];
        }

        // 3. 收集该类型及其所有父类的所有订阅者回调
        // 关键修复：在调用回调前释放锁，防止订阅者执行 publish/subscribe
        // 导致死锁或升级锁冲突
        std::vector<SubscriberInfo> subsToCall;
        {
            std::shared_lock lock(m_mutex);
            for ( const auto& targetType : relatedTypes ) {
                if ( auto subIt = m_subscribers.find(targetType);
                     subIt != m_subscribers.end() ) {
                    // 复制订阅者信息（含 std::function 拷贝）
                    for ( const auto& subInfo : subIt->second ) {
                        subsToCall.push_back(subInfo);
                    }
                }
            }
        }

        // 4. 在锁外依次调用订阅者
        for ( const auto& subInfo : subsToCall ) {
            // 传递事件指针
            subInfo.callback(&event);
        }
    }

private:
    struct SubscriberInfo {
        SubscriptionID                   id;
        std::function<void(const void*)> callback;
    };

    std::unordered_map<uint64_t, std::vector<SubscriberInfo>> m_subscribers;
    std::shared_mutex                                         m_mutex;
    std::atomic<SubscriptionID>                               m_nextId{ 0 };

    // --- 性能优化：继承关系缓存 ---
    // 记录每种类型发生时，应该通知哪些类型（自身 + 所有父类）
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_inheritanceCache;
    std::mutex m_cacheMutex;  // 单独的锁保护缓存写入

    template<typename EventType> void cache_inheritance()
    {
        auto index = detail::getStaticTypeId<EventType>();

        // 快速检查：如果已经缓存，直接返回
        {
            std::lock_guard cacheLock(m_cacheMutex);
            if ( m_inheritanceCache.find(index) != m_inheritanceCache.end() ) {
                return;
            }
        }

        std::vector<uint64_t> hierarchy;
        hierarchy.push_back(index);  // 添加自身

        // 递归收集父类
        if constexpr ( std::tuple_size_v<
                           typename EventTraits<EventType>::Parents> > 0 ) {
            collect_parents<EventType,
                            typename EventTraits<EventType>::Parents>(
                hierarchy);
        }

        // 写入缓存
        std::lock_guard cacheLock(m_cacheMutex);
        m_inheritanceCache[index] = std::move(hierarchy);
    }

    template<typename EventType, typename Tuple, size_t Index = 0>
    void collect_parents(std::vector<uint64_t>& hierarchy)
    {
        if constexpr ( Index < std::tuple_size_v<Tuple> ) {
            using ParentType = std::tuple_element_t<Index, Tuple>;
            hierarchy.push_back(detail::getStaticTypeId<ParentType>());

            // 继续递归该父类的父类
            if constexpr ( std::tuple_size_v<
                               typename EventTraits<ParentType>::Parents> >
                           0 ) {
                collect_parents<ParentType,
                                typename EventTraits<ParentType>::Parents>(
                    hierarchy);
            }

            // 处理 Tuple 中的下一个父类
            collect_parents<EventType, Tuple, Index + 1>(hierarchy);
        }
    }
};

/**
 * @brief RAII 风格的订阅令牌，超出作用域自动取消订阅
 */
template<typename EventType> class ScopedSubscription
{
public:
    ScopedSubscription() = default;
    ScopedSubscription(SubscriptionID id) : m_id(id) {}
    ~ScopedSubscription()
    {
        if ( m_id != 0 ) {
            EventBus::instance().unsubscribe<EventType>(m_id);
        }
    }

    // 禁用拷贝，允许移动
    ScopedSubscription(const ScopedSubscription&)            = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;
    ScopedSubscription(ScopedSubscription&& other) noexcept : m_id(other.m_id)
    {
        other.m_id = 0;
    }
    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept
    {
        if ( this != &other ) {
            if ( m_id != 0 ) EventBus::instance().unsubscribe<EventType>(m_id);
            m_id       = other.m_id;
            other.m_id = 0;
        }
        return *this;
    }

    void reset()
    {
        if ( m_id != 0 ) {
            EventBus::instance().unsubscribe<EventType>(m_id);
            m_id = 0;
        }
    }

private:
    SubscriptionID m_id{ 0 };
};

}  // namespace MMM::Event
