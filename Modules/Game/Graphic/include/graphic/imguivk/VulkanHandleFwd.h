#pragma once

#include <vulkan/vulkan_hpp_macros.hpp>

/// @file
/// @brief 图形回调接口使用的 Vulkan-Hpp 句柄前向声明。
/// @details 类型种类与命名空间已核对 Khronos Vulkan-Hpp 的句柄声明。
/// 官方 vulkan_handles.hpp 是内部定义头，不能当作独立前置声明头包含。
/// 本头只用于指针和引用；构造句柄、调用 API 或保存值的文件必须包含 vulkan.hpp。
/// 使用官方命名空间宏，避免声明脱离当前 SDK 的命名空间配置。
/// 普通头文件的前向声明不导出 C++ 模块，不能依赖较新 SDK 的导出宏。
/// Vulkan-Hpp 1.4.309 没有 VULKAN_HPP_EXPORT；补定义还会污染后续 SDK 头。
namespace VULKAN_HPP_NAMESPACE
{
/// @brief 物理设备句柄，仅供接口引用。
class PhysicalDevice;
/// @brief 逻辑设备句柄，仅供接口引用。
class Device;
/// @brief 命令池句柄，仅供接口引用。
class CommandPool;
/// @brief 队列句柄，仅供接口引用。
class Queue;
/// @brief 命令缓冲句柄，仅供接口引用。
class CommandBuffer;
}  // namespace VULKAN_HPP_NAMESPACE
