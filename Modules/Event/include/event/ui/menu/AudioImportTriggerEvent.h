#pragma once
#include <string>

namespace MMM::Event
{
/**
 * @brief 触发音频导入类型选择弹窗的事件
 */
struct AudioImportTriggerEvent {
    /// @brief 待导入音频文件的 UTF-8 路径。
    std::string path;
};
}  // namespace MMM::Event
