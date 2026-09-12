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

/// @brief 订阅令牌，用于精确取消一条订阅记录。
/// @note 零值保留为无效令牌，便于 RAII 包装器表达空状态。
using SubscriptionID = uint64_t;

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
        publishImpl(relatedTypeIds<EventType>(), &event);
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

    /// @brief 向相关事件类型的订阅者发布对象。
    /// @param relatedTypes 当前类型及所有注册父类的 ID。
    /// @param eventPtr 发布期间有效的事件地址。
    /// @warning 高频发布会复制匹配订阅记录；回调在释放读锁后同步执行。
    void publishImpl(const std::vector<uint64_t>& relatedTypes,
                     const void*                  eventPtr);

    /// @brief 获取事件类型及其所有注册父类的稳定 ID 列表。
    /// @return 首次使用时构建、之后复用的 ID 列表。
    template<typename EventType>
    static const std::vector<uint64_t>& relatedTypeIds()
    {
        // 静态缓存由语言规则保证线程安全初始化，避免每次发布遍历类型元数据。
        static const std::vector<uint64_t> relatedTypes = [] {
            std::vector<uint64_t> hierarchy;
            // 先加入精确类型，使具体订阅者先于父类订阅者进入快照。
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
            // 直接父类先入队，然后深度优先展开它自己的父类链。
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
