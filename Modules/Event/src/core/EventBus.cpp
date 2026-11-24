#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveProgressEvent.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
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
    /// @brief 一条发布期间保持存活的类型擦除订阅。
    struct Subscription {
        /// @brief 用于精确取消订阅的进程内编号。
        SubscriptionID id;
        /// @brief 已调整类型地址的同步回调。
        std::function<void(const void*)> callback;
        /// @brief 已擦除或可变的旧回调继续在调用前复制。
        bool copyOnPublish;
    };

    /// @brief 表与分组都是只读快照，修改时只复制受影响的分组。
    /// @details 分组元素独立共享，写者复制分组时不触碰正在执行的函子。
    /// 同一事件的多个父类均从同一张表查询，避免在分发途中混入后发布的订阅状态。
    using Group = std::vector<std::shared_ptr<const Subscription>>;
    using Table = std::unordered_map<uint64_t, std::shared_ptr<const Group>>;

    /// @brief 仅在低频订阅增删阶段串行安排快照修改。
    std::mutex writerMutex;
    /// @brief 只保护顶层指针交换，不覆盖表查找、复制、销毁或回调。
    std::mutex snapshotMutex;
    /// @brief 当前已公开的所有订阅分组。
    std::shared_ptr<const Table> subscribers = std::make_shared<const Table>();
    /// @brief 只由持有 writerMutex 的线程推进，无需原子。
    SubscriptionID nextId{ 0 };

    /// @brief 获取同步发布所需的唯一快照所有权。
    /// @warning 逐输入事件路径；复制一次所有权让任意嵌套分发保活旧表。
    /// 锁内只有固定大小的指针复制，不会等待低频表构造。
    std::shared_ptr<const Table> snapshot()
    {
        std::lock_guard lock(snapshotMutex);
        return subscribers;
    }

    /// @brief 交换整个已完成的表，旧表在锁外释放。
    /// @param next 写者在发布前已经构建完成的不可变快照。
    /// @note 发布锁从不覆盖容器构造，也不覆盖用户捕获对象的析构。
    void replace(std::shared_ptr<const Table> next)
    {
        std::lock_guard lock(snapshotMutex);
        subscribers.swap(next);
    }
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

/// @brief 创建订阅并发布含新增记录的快照。
/// @warning 低频操作，分配与表构造不占用发布快照锁。
SubscriptionID EventBus::subscribeImpl(
    uint64_t typeId, std::function<void(const void*)> callback,
    bool copyOnPublish)
{
    // 旧表在 writerMutex 解锁后才释放，捕获析构可以安全重入总线。
    std::shared_ptr<const Impl::Table> oldTable;
    std::unique_lock                   writer(m_impl->writerMutex);
    oldTable            = m_impl->snapshot();
    auto  next          = std::make_shared<Impl::Table>(*oldTable);
    auto& previousGroup = (*next)[typeId];
    auto  group = previousGroup ? std::make_shared<Impl::Group>(*previousGroup)
                                : std::make_shared<Impl::Group>();
    // 保留同组登记次序，零继续作为无效令牌。
    const auto id = ++m_impl->nextId;
    group->push_back(std::make_shared<const Impl::Subscription>(
        Impl::Subscription{ id, std::move(callback), copyOnPublish }));
    previousGroup = std::move(group);
    m_impl->replace(std::move(next));
    return id;
}

/// @brief 从后续发布的快照移除此订阅，在途发布继续保留旧快照。
/// @param typeId 订阅登记时使用的类型编号。
/// @param id 类型内要移除的订阅令牌，重复注销没有副作用。
/// @note 支持回调注销自己或之后的回调。外层发布保持整批顺序，
/// 从回调再次发布时则读取已经提交的新表。
void EventBus::unsubscribeImpl(uint64_t typeId, SubscriptionID id)
{
    std::shared_ptr<const Impl::Table> oldTable;
    std::unique_lock                   writer(m_impl->writerMutex);
    oldTable         = m_impl->snapshot();
    const auto entry = oldTable->find(typeId);
    if ( entry == oldTable->end() ) return;
    auto group = std::make_shared<Impl::Group>(*entry->second);
    if ( std::erase_if(*group, [id](const auto& subscription) {
             return subscription->id == id;
         }) == 0 )
        return;

    // 只复制目标分组，旧快照中回调的所有权不会被取消操作提前释放。
    // 顶层只复制分组句柄。现有发布者可以继续读自己的表，直到返回。
    auto next = std::make_shared<Impl::Table>(*oldTable);
    // 空组从表中移除，使无订阅者的活跃事件只承担一次查找。
    if ( group->empty() )
        next->erase(typeId);
    else
        next->at(typeId) = std::move(group);
    m_impl->replace(std::move(next));
}

/// @brief 按路线在锁外同步执行同一份不可变快照。
/// @param routes 缓存的具体类型与父类次序。
/// @param eventPtr 本轮发布借用的完整事件对象。
/// @details 只在分组有匹配项时计算一次子对象地址，分享给该组的所有订阅。
/// @warning 输入热路径：只获取一次表。常量处理器无堆分配与回调复制。
void EventBus::publishImpl(const std::vector<DispatchRoute>& routes,
                           const void*                       eventPtr)
{
    // 顶层所有权保活所有分组，不再为每个订阅另建临时调用记录。
    const auto table = m_impl->snapshot();
    for ( const auto& route : routes ) {
        const auto group = table->find(route.typeId);
        if ( group == table->end() ) continue;
        const auto* address = route.adjust(eventPtr);
        for ( const auto& subscriber : *group->second ) {
            // 兼容显式擦除及可变捕获，保持每次分发独立的私有状态。
            if ( subscriber->copyOnPublish ) {
                auto invocation = subscriber->callback;
                invocation(address);
            } else {
                subscriber->callback(address);
            }
        }
    }
}
}  // namespace MMM::Event
