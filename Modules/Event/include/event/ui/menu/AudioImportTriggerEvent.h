#pragma once
#include <string>

namespace MMM::Event
{
/**
 * @brief 触发音频导入类型选择弹窗的事件
 */
struct AudioImportTriggerEvent {
    std::string path;
};
}  // namespace MMM::Event
