#pragma once

#include "../EventDef.h"
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace MMM::Event
{


class EventBus
{
public:
    EventBus()                           = default;
    EventBus(EventBus&&)                 = delete;
    EventBus(const EventBus&)            = delete;
    EventBus& operator=(EventBus&&)      = delete;
    EventBus& operator=(const EventBus&) = delete;
    ~EventBus()                          = default;

    static EventBus& instance()
    {
        static EventBus evtBus;
        return evtBus;
    }

    /**
     * @brief 订阅事件
     */
    template<typename EventType>
    void subscribe(std::function<void(const EventType&)> callback)
    {
        std::unique_lock lock(m_mutex);
        auto             index = std::type_index(typeid(EventType));
        m_subscribers[index].push_back([callback](const void* eventPtr) {
            callback(*static_cast<const EventType*>(eventPtr));
        });
    }

    /**
     * @brief 发布事件
     * 会自动递归发布给在 EventTraits 中注册过的父类
     */
    template<typename EventType> void publish(const EventType& event)
    {
        // 我们使用一个内部函数来处理递归，避免多次重复获取锁
        std::shared_lock lock(m_mutex);
        internal_publish<EventType>(event);
    }

private:
    // 递归发布逻辑
    template<typename EventType> void internal_publish(const EventType& event)
    {
        auto index = std::type_index(typeid(EventType));

        // 1. 通知当前类型的订阅者
        if ( auto it = m_subscribers.find(index); it != m_subscribers.end() ) {
            for ( auto& callback : it->second ) {
                callback(&event);
            }
        }

        // 2. 编译时检查是否有父类，并递归发布
        if constexpr ( std::tuple_size_v<
                           typename EventTraits<EventType>::Parents> > 0 ) {
            publish_to_parents<typename EventTraits<EventType>::Parents>(event);
        }
    }

    // 遍历 Tuple 中的每一个父类并发布
    template<typename Tuple, size_t Index = 0>
    void publish_to_parents(const auto& event)
    {
        if constexpr ( Index < std::tuple_size_v<Tuple> ) {
            using ParentType = std::tuple_element_t<Index, Tuple>;

            // 将当前事件向上转型为父类引用，递归调用
            // 注意：这里需要 static_cast 确保类型转换正确
            internal_publish<ParentType>(static_cast<const ParentType&>(event));

            // 处理 Tuple 中的下一个父类（针对多继承）
            publish_to_parents<Tuple, Index + 1>(event);
        }
    }

private:
    std::unordered_map<std::type_index,
                       std::vector<std::function<void(const void*)>>>
        m_subscribers;

    std::shared_mutex m_mutex;  // 增加线程安全支持
};

}  // namespace MMM::Event