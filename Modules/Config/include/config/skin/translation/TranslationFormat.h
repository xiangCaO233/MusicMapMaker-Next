#pragma once

#include "config/skin/translation/Translation.h"

#include <fmt/format.h>

/// @brief 使用当前语言模板格式化翻译文本。
#define TR_FMT(key, ...) fmt::format(fmt::runtime(TR(key).view()), __VA_ARGS__)
