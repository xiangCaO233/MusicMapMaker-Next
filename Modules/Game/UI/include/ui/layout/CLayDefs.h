#pragma once
extern "C" {
#include <clay.h>
}
#include <string.h>
#include <string>
#include <string_view>

namespace MMM::UI
{

/// @brief 将连续字符借用为 Clay_String。
/// @param s 字符数据与长度；生命周期必须覆盖本次 Clay 布局生成。
/// @param isStatic 字符内存是否具有静态生命周期。
/// @return 不拥有字符数据的 Clay 字符串描述。
/// @warning 布局热路径：函数不复制字符，调用方不得在 Clay 消费前修改存储。
inline Clay_String ToCS(std::string_view s, bool isStatic = false)
{
    return { .isStaticallyAllocated = isStatic,
             .length                = (int32_t)s.length(),
             .chars                 = s.data() };
}

/// @brief 将静态字符串字面量借用为 Clay_String。
/// @param s 指向静态存储期字符的视图。
/// @return 标记为静态分配的 Clay 字符串描述。
inline Clay_String ToStaticCS(std::string_view s)
{
    return ToCS(s, true);
}

/// @brief 同时保存 Clay 横向与纵向对齐方式的便捷值类型。
struct Alignment {
    /// @brief 横向布局对齐方式。
    Clay_LayoutAlignmentX x;
    /// @brief 纵向布局对齐方式。
    Clay_LayoutAlignmentY y;
    /// @brief 创建双轴居中对齐。
    /// @return 横向和纵向均居中的配置。
    static Alignment Center()
    {
        return { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    }
    /// @brief 创建左上起始对齐。
    /// @return 横向靠左且纵向靠上的配置。
    static Alignment Start() { return { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP }; }
    /// @brief 创建右下结束对齐。
    /// @return 横向靠右且纵向靠下的配置。
    static Alignment End()
    {
        return { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_BOTTOM };
    }
};

/// @brief 构造与 Clay v0.14 内部尺寸宏等价的单轴策略。
struct Sizing {
    /// @brief 实际传递给 Clay 布局声明的单轴配置。
    Clay_SizingAxis axis;
    /// @brief 创建固定像素尺寸。
    /// @param px 最小值与最大值共同使用的像素数。
    /// @return 固定尺寸策略。
    static Sizing Fixed(float px)
    {
        return { .axis = { .size = { .minMax = { px, px } },
                           .type = CLAY__SIZING_TYPE_FIXED } };
    }
    /// @brief 创建占用剩余空间的增长尺寸。
    /// @param min 允许的最小像素尺寸。
    /// @param max 允许的最大像素尺寸；零表示使用包装层的大上限。
    /// @return 增长尺寸策略。
    static Sizing Grow(float min = 0, float max = 0)
    {
        // Clay 需要有限最大值，零上限在包装层语义中表示近似无限增长。
        return { .axis = {
                     .size = { .minMax = { min, max == 0 ? 100000.0f : max } },
                     .type = CLAY__SIZING_TYPE_GROW } };
    }
    /// @brief 创建相对父容器的比例尺寸。
    /// @param p 父轴尺寸比例。
    /// @return 百分比尺寸策略。
    static Sizing Percent(float p)
    {
        return { .axis = { .size = { .percent = p },
                           .type = CLAY__SIZING_TYPE_PERCENT } };
    }
    /// @brief 创建适应内容的尺寸策略。
    /// @param min 空内容时仍保留的最小像素尺寸。
    /// @param max 当前为兼容接口保留，Clay 使用统一大上限。
    /// @return 内容适配尺寸策略。
    static Sizing Fit(float min = 0, float max = 0)
    {
        // 最小值防止空节点完全塌陷，大上限允许内容决定实际尺寸。
        return { .axis = { .size = { .minMax = { min, 100000.0f } },
                           .type = CLAY__SIZING_TYPE_FIT } };
    }
};

/// @brief Clay 文本配置与皮肤 ImGui 字体角色之间的稳定映射 ID。
/// @note 数值存入 Clay_TextElementConfig::fontId，修改顺序会影响既有映射。
enum class FontID : uint16_t {
    /// @brief 正文内容字体。
    Content = 0,
    /// @brief 标题字体。
    Title,
    /// @brief 菜单字体。
    Menu,
    /// @brief 文件管理器字体。
    FileManager,
    /// @brief 侧栏字体。
    SideBar,
    /// @brief 设置页内部控件字体。
    SettingInternal,
    /// @brief 只包含图标字形的字体。
    PureIcons,
    /// @brief 默认字体角色保持与 Content 同值。
    Default = Content
};

}  // namespace MMM::UI
