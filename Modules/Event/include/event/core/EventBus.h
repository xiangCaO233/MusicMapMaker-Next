#pragma once

#include <functional>
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

    // 订阅事件
    template<typename EventType>
    void subscribe(std::function<void(const EventType&)> callback)
    {
        auto index = std::type_index(typeid(EventType));
        m_subscribers[index].push_back([callback](const void* eventPtr) {
            callback(*static_cast<const EventType*>(eventPtr));
        });
    }

    // 发布事件
    template<typename EventType> void publish(const EventType& event)
    {
        auto index = std::type_index(typeid(EventType));
        if ( m_subscribers.find(index) != m_subscribers.end() ) {
            for ( auto& callback : m_subscribers[index] ) {
                callback(&event);
            }
        }
    }

private:
    // 存储类型擦除的回调函数
    std::unordered_map<std::type_index,
                       std::vector<std::function<void(const void*)>>>
        m_subscribers;
};

}  // namespace MMM::Event
