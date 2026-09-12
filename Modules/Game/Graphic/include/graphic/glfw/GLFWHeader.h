#pragma once

// 统一约束 GLFW 不引入任何客户端图形 API 头，Vulkan 类型由 GLFW 按需暴露。
// 所有直接使用 GLFW 的图形模块实现都应包含此入口，避免各翻译单元的宏配置分歧。
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace MMM
{
namespace Graphic
{
/// @brief 将 GLFW 全局错误转发到项目日志系统。
/// @param error GLFW 定义的错误码。
/// @param description 由 GLFW
/// 管理生命周期的错误说明；回调内不得持久保存该指针。
void glfw_error_callback(int error, const char* description);
}  // namespace Graphic
}  // namespace MMM
