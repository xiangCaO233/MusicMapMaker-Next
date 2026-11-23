#pragma once

#include <ctime>
#include <filesystem>
#include <string>

namespace MMM::UI::Utils
{
/// @brief 把文件修改时间格式化为本地日期和分钟。
/// @param time 已成功读取的文件时钟时间点。
/// @return 本地日期文本；转换或格式化失败时返回空字符串，由视图翻译占位。
/// @note 仅供展示；排序仍使用原始文件时钟和字段有效位。
[[nodiscard]] std::string formatFileModifiedTime(
    std::filesystem::file_time_type time);

namespace FileModifiedTimeDetail
{
/// @brief 格式化已转换为系统纪元秒数的本地时间。
/// @param time 文件时刻对应的 C 时间值。
/// @return 格式化文本；平台不支持该时刻或缓冲不足时为空。
[[nodiscard]] std::string formatLocalMinute(std::time_t time);
}  // namespace FileModifiedTimeDetail
}  // namespace MMM::UI::Utils
