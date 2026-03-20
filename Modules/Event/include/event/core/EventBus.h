#pragma once

#include "event/EventDef.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace MMM::Event
{

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
        auto             index = std::type_index(typeid(EventType));
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
        auto             index = std::type_index(typeid(EventType));

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

        std::shared_lock lock(m_mutex);
        auto             index = std::type_index(typeid(EventType));

        // 2. 获取该类型及其所有父类的类型索引列表
        const auto& relatedTypes = m_inheritanceCache[index];

        // 3. 遍历所有相关类型，并调用它们的订阅者
        for ( const auto& targetType : relatedTypes ) {
            if ( auto subIt = m_subscribers.find(targetType);
                 subIt != m_subscribers.end() ) {
                for ( const auto& subInfo : subIt->second ) {
                    // 传递事件指针。注意：底层我们要求事件具有相同的内存布局或正确的基类偏移。
                    // 对于纯粹的 struct 继承，static_cast
                    // 处理指针时会自动调整偏移。
                    subInfo.callback(&event);
                }
            }
        }
    }

private:
    struct SubscriberInfo {
        SubscriptionID                   id;
        std::function<void(const void*)> callback;
    };

    std::unordered_map<std::type_index, std::vector<SubscriberInfo>>
                                m_subscribers;
    std::shared_mutex           m_mutex;
    std::atomic<SubscriptionID> m_nextId{ 0 };

    // --- 性能优化：继承关系缓存 ---
    // 记录每种类型发生时，应该通知哪些类型（自身 + 所有父类）
    std::unordered_map<std::type_index, std::vector<std::type_index>>
               m_inheritanceCache;
    std::mutex m_cacheMutex;  // 单独的锁保护缓存写入

    template<typename EventType> void cache_inheritance()
    {
        auto index = std::type_index(typeid(EventType));

        // 快速检查：如果已经缓存，直接返回
        {
            std::lock_guard cacheLock(m_cacheMutex);
            if ( m_inheritanceCache.find(index) != m_inheritanceCache.end() ) {
                return;
            }
        }

        std::vector<std::type_index> hierarchy;
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

    template<typename DerivedType, typename Tuple, size_t Index = 0>
    void collect_parents(std::vector<std::type_index>& hierarchy)
    {
        if constexpr ( Index < std::tuple_size_v<Tuple> ) {
            using ParentType = std::tuple_element_t<Index, Tuple>;
            hierarchy.push_back(std::type_index(typeid(ParentType)));

            // 继续递归该父类的父类
            if constexpr ( std::tuple_size_v<
                               typename EventTraits<ParentType>::Parents> >
                           0 ) {
                collect_parents<ParentType,
                                typename EventTraits<ParentType>::Parents>(
                    hierarchy);
            }

            // 处理 Tuple 中的下一个父类
            collect_parents<DerivedType, Tuple, Index + 1>(hierarchy);
        }
    }
};

}  // namespace MMM::Event
