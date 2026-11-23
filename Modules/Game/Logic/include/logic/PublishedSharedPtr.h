#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace MMM::Logic
{

/// @brief 跨线程发布一个共享拥有的值，读取者可在下一次替换后继续访问旧值。
/// @note 访问状态由调用方自行同步，此类型只保护发布槽及其对象的生命周期。
/// @note 写者在发布前完成构造，读取者在使用底层对象期间持有返回的句柄。
template<typename T> class PublishedSharedPtr
{
public:
    /// @brief 获取当前共享句柄，首次发布前可能返回空值。
    /// @warning 快照热路径单次调用只复制一个句柄。兼容实现的锁仅保护指针复制，
    /// 不会等待写者构造快照、执行回调或销毁旧对象。
    [[nodiscard]] std::shared_ptr<T> load() const noexcept
    {
#ifdef __cpp_lib_atomic_shared_ptr
        // 与写侧 release 配对，使完整构造的快照对读取者可见。
        return m_value.load(std::memory_order_acquire);
#else
        // 复制期间阻止写侧替换，返回后由局部拥有权保证对象存活。
        const std::lock_guard lock(m_mutex);
        return m_value;
#endif
    }

    /// @brief 替换当前句柄，空值表示撤回发布。
    /// @param value 已完整构造的新值，调用后发布槽持有该值。
    /// @note 无其他拥有者时，替换者释放旧值；否则由最后释放句柄的线程回收。
    void store(std::shared_ptr<T> value) noexcept
    {
#ifdef __cpp_lib_atomic_shared_ptr
        // release 发布指针及其构造写入，读者无需获取调用方的业务锁。
        m_value.store(std::move(value), std::memory_order_release);
#else
        // 借形参带出旧值。形参析构发生在锁守卫析构之后，避免回收阻塞读取。
        const std::lock_guard lock(m_mutex);
        m_value.swap(value);
#endif
    }

private:
#ifdef __cpp_lib_atomic_shared_ptr
    /// @brief 由标准库提供的共享拥有权发布槽，具体同步由标准库实现。
    std::atomic<std::shared_ptr<T>> m_value;
#else
    /// @brief libc++ 等未提供原子特化时，专门保护一次指针复制或交换。
    mutable std::mutex m_mutex;

    /// @brief 与指针锁配合保存当前句柄；禁止借锁访问或释放底层对象。
    std::shared_ptr<T> m_value;
#endif
};

}  // namespace MMM::Logic
