#pragma once

#include <string>
#include <string_view>

namespace MMM::Canvas
{

/// @brief 构造主画布标签的可见标题。
/// @param fallbackTitle 未加载谱面时使用的回退标题。
/// @param hasBeatmap 当前快照是否包含谱面。
/// @param beatmapName 当前谱面名称。
/// @param isDirty 当前谱面是否存在未保存修改。
/// @return 脏谱面以固定前缀 `* ` 开头的完整标题。
/// @warning UI 热路径：主画布每帧调用，只允许进行一次短字符串构造。
inline std::string makeCanvasTabTitle(std::string_view fallbackTitle,
                                      bool             hasBeatmap,
                                      std::string_view beatmapName,
                                      bool             isDirty)
{
    std::string title = hasBeatmap && !beatmapName.empty()
                            ? std::string(beatmapName)
                            : std::string(fallbackTitle);
    if ( hasBeatmap && isDirty ) {
        title.insert(0, "* ");
    }
    return title;
}

}  // namespace MMM::Canvas
