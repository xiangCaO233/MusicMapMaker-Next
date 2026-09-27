#pragma once

#include "config/skin/translation/Translation.h"

#include <fmt/format.h>

/// @brief 使用当前语言模板格式化翻译文本。
/// @details 运行时格式串来自语言资源，参数在调用点按 fmt 规则展开。
#define TR_FMT(key, ...) fmt::format(fmt::runtime(TR(key).view()), __VA_ARGS__)
