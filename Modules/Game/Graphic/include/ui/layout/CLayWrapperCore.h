#pragma once

extern "C" {
#include <clay.h>
}
#include <string>

namespace MMM::Graphic::UI
{

// 将 std::string 转为 Clay_String
inline Clay_String ToClayString(const std::string& s)
{
    return { .isStaticallyAllocated = false,
             .length                = (int32_t)s.length(),
             .chars                 = s.c_str() };
}

// 布局项类型
enum class CLayItemType { Element, Spring };

struct CLayElementConfig {
    std::string id;
    Clay_Sizing sizing;
    // 可以添加更多属性，如对齐等
};

class CLayWrapperCore
{
public:
    static CLayWrapperCore& instance();
    CLayWrapperCore(CLayWrapperCore&&)                 = delete;
    CLayWrapperCore(const CLayWrapperCore&)            = delete;
    CLayWrapperCore& operator=(CLayWrapperCore&&)      = delete;
    CLayWrapperCore& operator=(const CLayWrapperCore&) = delete;
    ~CLayWrapperCore();

    void setupClayTextMeasurement();

private:
    CLayWrapperCore();
    // 准备 Clay 所需的内存竞技场 (Arena)
    uint64_t   clayMemorySize;
    Clay_Arena clayArena;
};

}  // namespace MMM::Graphic::UI
