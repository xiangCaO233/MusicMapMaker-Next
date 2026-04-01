#pragma once
extern "C" {
#include <clay.h>
}
#include <string.h>
#include <string>

namespace MMM::UI
{

// 增加对 char* 的重载，确保指针直接透传，不产生临时变量
inline Clay_String ToCS(const char* s)
{
    return { .isStaticallyAllocated = true,
             .length                = (int32_t)strlen(s),
             .chars                 = s };
}
// 保留对 string 的支持
inline Clay_String ToCS(const std::string& s)
{
    return { .isStaticallyAllocated = false,
             .length                = (int32_t)s.length(),
             .chars                 = s.c_str() };
}

// 快速创建对齐配置
struct Alignment {
    Clay_LayoutAlignmentX x;
    Clay_LayoutAlignmentY y;
    static Alignment      Center()
    {
        return { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    }
    static Alignment Start() { return { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP }; }
    static Alignment End()
    {
        return { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_BOTTOM };
    }
};

// 快速创建尺寸策略 (对齐 Clay v0.14 内部宏)
struct Sizing {
    Clay_SizingAxis axis;
    static Sizing   Fixed(float px)
    {
        return { .axis = { .size = { .minMax = { px, px } },
                           .type = CLAY__SIZING_TYPE_FIXED } };
    }
    static Sizing Grow(float min = 0, float max = 0)
    {
        return { .axis = {
                     .size = { .minMax = { min, max == 0 ? 100000.0f : max } },
                     .type = CLAY__SIZING_TYPE_GROW } };
    }
    static Sizing Percent(float p)
    {
        return { .axis = { .size = { .percent = p },
                           .type = CLAY__SIZING_TYPE_PERCENT } };
    }
    static Sizing Fit(float min = 0, float max = 0)
    {
        // 建议给 Fit 一个保底值，防止空节点导致布局塌陷
        return { .axis = { .size = { .minMax = { min, 100000.0f } },
                           .type = CLAY__SIZING_TYPE_FIT } };
    }
};

}  // namespace MMM::UI
