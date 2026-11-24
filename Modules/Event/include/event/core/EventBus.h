#pragma once

#include "event/EventDef.h"
#include "event/core/SubscriptionID.h"

#include <algorithm>
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
/// @note ID 只在当前进程内有效，不得持久化或用于网络协议。
uint64_t nextStaticTypeId();

/// @brief 获取模板化静态类型 ID，替代 typeid/type_index。
/// @return 当前事件类型的进程内唯一 ID。
template<typename T> inline uint64_t getStaticTypeId()
{
    // 函数模板静态量保证每个具体类型只向全局计数器申请一次 ID。
    static const uint64_t id = nextStaticTypeId();
    return id;
}
}  // namespace detail


/// @brief 提供线程安全的类型事件订阅、取消和同步发布。
/// @note 回调在 publish 调用线程执行，事件对象仅在发布期间有效。
class EventBus
{
public:
    /// @brief 创建独立的事件总线。
    /// @note 独立实例适合测试隔离；业务代码通常使用 instance()。
    EventBus();
    EventBus(EventBus&&)                 = delete;
    EventBus(const EventBus&)            = delete;
    EventBus& operator=(EventBus&&)      = delete;
    EventBus& operator=(const EventBus&) = delete;

    /// @brief 销毁事件总线及其私有订阅状态。
    ~EventBus();

    /// @brief 获取进程内共享的事件总线实例。
    /// @return 静态事件总线引用。
    static EventBus& instance();

    /// @brief 为具体事件类型注册同步回调。
    /// @param callback 发布时调用的回调；回调接收只读事件引用。
    /// @return 可传给 unsubscribe 的订阅 ID。
    /// @warning 注册会获取写锁并分配内存，不应在每帧或输入热路径重复调用。
    template<typename EventType>
    SubscriptionID subscribe(std::function<void(const EventType&)> callback)
    {
        // 类型擦除层只保存 void 地址；模板包装器负责恢复精确事件类型。
        return subscribeImpl(
            detail::getStaticTypeId<EventType>(),
            [callback](const void* eventPtr) {
                callback(*static_cast<const EventType*>(eventPtr));
            });
    }

    /// @brief 取消具体事件类型上的一条订阅。
    /// @param id subscribe 返回的订阅 ID。
    /// @warning 取消会获取写锁并线性查找，不应在每帧热路径反复调用。
    template<typename EventType> void unsubscribe(SubscriptionID id)
    {
        unsubscribeImpl(detail::getStaticTypeId<EventType>(), id);
    }

    /// @brief 同步发布事件，并分发给当前类型及所有已注册父类。
    /// @param event 发布期间保持有效的只读事件对象。
    /// @warning
    /// 输入和画布路径可能高频调用；回调不得阻塞，当前实现还会复制订阅快照。
    template<typename EventType> void publish(const EventType& event)
    {
        // 类型层级首次使用时构建，后续发布只复用缓存结果。
        publishImpl(dispatchRoutes<EventType>(), std::addressof(event));
    }

private:
    /// @brief 隐藏订阅表、锁与订阅 ID 计数器。
    struct Impl;

    /// @brief 注册已类型擦除的订阅回调。
    /// @param typeId 事件类型 ID。
    /// @param callback 接收事件地址的回调。
    /// @return 可用于取消订阅的 ID。
    /// @warning 该低频操作获取订阅表写锁，并可能扩容容器。
    SubscriptionID subscribeImpl(uint64_t                         typeId,
                                 std::function<void(const void*)> callback);

    /// @brief 取消已类型擦除的订阅。
    /// @param typeId 事件类型 ID。
    /// @param id 订阅 ID。
    /// @warning 该低频操作获取订阅表写锁并线性擦除匹配记录。
    void unsubscribeImpl(uint64_t typeId, SubscriptionID id);

    /// @brief 保存事件类型及从原对象到对应基类子对象的转换。
    struct DispatchRoute {
        /// @brief 与订阅表中的键对应的进程内类型编号。
        uint64_t typeId;
        /// @brief 按实际继承路径调整地址，不依赖基类的内存偏移。
        const void* (*adjust)(const void*);
    };

    /// @brief 向相关事件类型的订阅者发布对象。
    /// @param routes 精确类型及父类的缓存分发次序。
    /// @param eventPtr 发布期间有效的完整事件对象地址。
    /// @warning 高频发布会复制匹配订阅记录，回调在释放读锁后同步执行。
    void publishImpl(const std::vector<DispatchRoute>& routes,
                     const void*                       eventPtr);

    /// @brief 沿显式父类路径逐级转换，支持多继承与虚基类。
    /// @details 逐级转换也适用于根类型上有多个同名基类子对象的事件。
    template<typename Current, typename... Parents>
    static const void* adjustEvent(const Current* event)
    {
        if constexpr ( sizeof...(Parents) == 0 ) {
            // 所有转换已完成。回调恢复的类型与当前子对象类型相同。
            return event;
        } else {
            return adjustParent<Parents...>(event);
        }
    }

    /// @brief 先转为直接父类，再继续到本次订阅类型。
    template<typename Parent, typename... Rest, typename Current>
    static const void* adjustParent(const Current* event)
    {
        return adjustEvent<Parent, Rest...>(static_cast<const Parent*>(event));
    }

    /// @brief 按声明顺序展开直接父类，空元组结束当前路径。
    template<typename Root, typename... Path, typename... Parents>
    static void collectParents(std::vector<DispatchRoute>& routes,
                               std::tuple<Parents...>*)
    {
        (appendRoute<Parents, Root, Path..., Parents>(routes), ...);
    }

    /// @brief 添加一个类型首次出现的路径，保留原深度优先顺序。
    template<typename Current, typename Root, typename... Path>
    static void appendRoute(std::vector<DispatchRoute>& routes)
    {
        const auto typeId = detail::getStaticTypeId<Current>();
        if ( std::ranges::any_of(routes, [typeId](const auto& route) {
                 return route.typeId == typeId;
             }) )
            return;

        // 一个祖先类型只投递一次，非虚菱形时使用声明顺序中的第一个子对象。
        routes.push_back({ typeId, [](const void* event) {
                              return adjustEvent<Root, Path...>(
                                  static_cast<const Root*>(event));
                          } });
        // 仅以指针推导父类元组类型，不构造事件对象。
        using Parents = typename EventTraits<Current>::Parents;
        collectParents<Root, Path...>(routes, static_cast<Parents*>(nullptr));
    }

    /// @brief 首次发布时构造只读路由，后续直接借用缓存。
    template<typename EventType>
    static const std::vector<DispatchRoute>& dispatchRoutes()
    {
        static const auto routes = [] {
            std::vector<DispatchRoute> result;
            appendRoute<EventType, EventType>(result);
            return result;
        }();
        return routes;
    }

    /// @brief 私有事件总线状态。
    std::unique_ptr<Impl> m_impl;
};

/// @brief RAII 风格的订阅令牌，超出作用域自动取消订阅。
/// @note 令牌固定关联全局 EventBus::instance()，不适用于独立测试总线。
template<typename EventType> class ScopedSubscription
{
public:
    ScopedSubscription() = default;
    /// @brief 接管一条已有订阅的取消责任。
    /// @param id EventBus::instance() 返回的订阅 ID。
    ScopedSubscription(SubscriptionID id) : m_id(id) {}
    /// @brief 若仍持有有效令牌，则自动从全局总线取消订阅。
    ~ScopedSubscription()
    {
        if ( m_id != 0 ) {
            EventBus::instance().unsubscribe<EventType>(m_id);
        }
    }

    // 令牌不可复制，避免同一订阅被多个对象重复取消。
    ScopedSubscription(const ScopedSubscription&)            = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;
    ScopedSubscription(ScopedSubscription&& other) noexcept : m_id(other.m_id)
    {
        // 清空源对象，将唯一的取消责任转移给新对象。
        other.m_id = 0;
    }
    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept
    {
        if ( this != &other ) {
            // 覆盖已有令牌前先取消旧订阅，保持单一资源语义。
            if ( m_id != 0 ) EventBus::instance().unsubscribe<EventType>(m_id);
            m_id       = other.m_id;
            other.m_id = 0;
        }
        return *this;
    }

    /// @brief 提前取消当前订阅并将令牌恢复为空状态。
    void reset()
    {
        if ( m_id != 0 ) {
            EventBus::instance().unsubscribe<EventType>(m_id);
            m_id = 0;
        }
    }

private:
    /// @brief 当前负责取消的订阅 ID；零表示不持有订阅。
    SubscriptionID m_id{ 0 };
};

}  // namespace MMM::Event
