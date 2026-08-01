#pragma once

#include "event/EventDef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace MMM::Event
{
namespace detail
{
/// @brief 为首次使用的事件类型分配进程内唯一 ID。
/// @return 新的事件类型 ID。
uint64_t nextStaticTypeId();

/// @brief 获取模板化静态类型 ID，替代 typeid/type_index。
/// @return 当前事件类型的进程内唯一 ID。
template<typename T> inline uint64_t getStaticTypeId()
{
    static const uint64_t id = nextStaticTypeId();
    return id;
}
}  // namespace detail

///@brief 订阅令牌，用于取消订阅
using SubscriptionID = uint64_t;

class EventBus
{
public:
    /// @brief 创建独立的事件总线。
    EventBus();
    EventBus(EventBus&&)                 = delete;
    EventBus(const EventBus&)            = delete;
    EventBus& operator=(EventBus&&)      = delete;
    EventBus& operator=(const EventBus&) = delete;

    /// @brief 销毁事件总线及其私有订阅状态。
    ~EventBus();

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
        return subscribeImpl(
            detail::getStaticTypeId<EventType>(),
            [callback](const void* eventPtr) {
                callback(*static_cast<const EventType*>(eventPtr));
            });
    }

    /**
     * @brief 取消订阅
     * @param id 订阅时返回的 ID
     */
    template<typename EventType> void unsubscribe(SubscriptionID id)
    {
        unsubscribeImpl(detail::getStaticTypeId<EventType>(), id);
    }

    /**
     * @brief 发布事件
     * 自动分发给当前类型及其所有在 EventTraits 中注册的父类
     */
    template<typename EventType> void publish(const EventType& event)
    {
        publishImpl(relatedTypeIds<EventType>(), &event);
    }

private:
    /// @brief 隐藏订阅表、锁与订阅 ID 计数器。
    struct Impl;

    /// @brief 注册已类型擦除的订阅回调。
    /// @param typeId 事件类型 ID。
    /// @param callback 接收事件地址的回调。
    /// @return 可用于取消订阅的 ID。
    SubscriptionID subscribeImpl(uint64_t                         typeId,
                                 std::function<void(const void*)> callback);

    /// @brief 取消已类型擦除的订阅。
    /// @param typeId 事件类型 ID。
    /// @param id 订阅 ID。
    void unsubscribeImpl(uint64_t typeId, SubscriptionID id);

    /// @brief 向相关事件类型的订阅者发布对象。
    /// @param relatedTypes 当前类型及所有注册父类的 ID。
    /// @param eventPtr 发布期间有效的事件地址。
    void publishImpl(const std::vector<uint64_t>& relatedTypes,
                     const void*                  eventPtr);

    /// @brief 获取事件类型及其所有注册父类的稳定 ID 列表。
    /// @return 首次使用时构建、之后复用的 ID 列表。
    template<typename EventType>
    static const std::vector<uint64_t>& relatedTypeIds()
    {
        static const std::vector<uint64_t> relatedTypes = [] {
            std::vector<uint64_t> hierarchy;
            hierarchy.push_back(detail::getStaticTypeId<EventType>());
            if constexpr ( std::tuple_size_v<
                               typename EventTraits<EventType>::Parents> > 0 ) {
                collectParents<typename EventTraits<EventType>::Parents>(
                    hierarchy);
            }
            return hierarchy;
        }();
        return relatedTypes;
    }

    /// @brief 递归收集父类及更高层父类的类型 ID。
    /// @param hierarchy 写入的类型 ID 列表。
    template<typename Tuple, std::size_t Index = 0>
    static void collectParents(std::vector<uint64_t>& hierarchy)
    {
        if constexpr ( Index < std::tuple_size_v<Tuple> ) {
            using ParentType = std::tuple_element_t<Index, Tuple>;
            hierarchy.push_back(detail::getStaticTypeId<ParentType>());

            if constexpr ( std::tuple_size_v<
                               typename EventTraits<ParentType>::Parents> >
                           0 ) {
                collectParents<typename EventTraits<ParentType>::Parents>(
                    hierarchy);
            }

            collectParents<Tuple, Index + 1>(hierarchy);
        }
    }

    /// @brief 私有事件总线状态。
    std::unique_ptr<Impl> m_impl;
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
