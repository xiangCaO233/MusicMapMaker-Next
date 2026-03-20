#pragma once

#include <tuple>

namespace MMM::Event
{

/**
 * @brief 事件特性基类
 * 默认情况下，事件没有父类。
 * 如果需要支持多态，请为你的事件类特化这个结构体。
 */
template<typename T> struct EventTraits {
    using Parents = std::tuple<>;
};

// 辅助宏：用于方便地定义事件的父类关系
// 使用方法：MMM_EVENT_REGISTER_PARENTS(MyDerivedEvent, MyBaseEvent1,
// MyBaseEvent2)
#define EVENT_REGISTER_PARENTS(Derived, ...)     \
    namespace MMM::Event                         \
    {                                            \
    template<> struct EventTraits<Derived> {     \
        using Parents = std::tuple<__VA_ARGS__>; \
    };                                           \
    }
}  // namespace MMM::Event