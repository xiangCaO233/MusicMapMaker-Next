#pragma once

#include "audio/StemAudition.h"

#include <expected>
#include <string>
#include <vector>

namespace MMM::UI
{
/// @brief 外部分离模型包经检查后可展示的声部和已解析配置。
struct StemSeparationModel {
    /// @brief 模型包显示名，来自文件名。
    std::string name;
    /// @brief 发布名按固定顺序排列，可直接用于工作进程产物查找。
    std::vector<std::string> stems;
    /// @brief 已解析相对路径的原始模型包快照。
    std::string plan;
};

/// @brief 用户选择的可复用模型运行环境。
/// @details 只有本机配置和原生选择器可以指定可执行入口。
struct StemSeparationEnvironment {
    /// @brief PATH 中的解释器，或模型虚拟环境的绝对路径。
#ifdef _WIN32
    std::string python = "python";
#else
    std::string python = "python3";
#endif
    /// @brief 用户从已安装的 MSST 环境选择的推理脚本。
    std::string backend;
    /// @brief 禁用加速设备，供 CPU 机器或对照测试使用。
    bool cpu = false;
};

/// @brief 加载本机运行设置，缺失时返回默认值。
/// @warning 低频文件操作。
StemSeparationEnvironment loadStemSeparationEnvironment();

/// @brief 原子保存本机会话可复用的运行设置。
/// @warning 低频文件操作。
bool saveStemSeparationEnvironment(
    const StemSeparationEnvironment& environment);

/// @brief 读取本地模型包并列出其中声明的输出，不启动推理。
/// @warning 用户选择文件后的低频路径。
std::expected<StemSeparationModel, std::string> readStemSeparationModel(
    const std::string& path);

/// @brief 在外部环境内执行分离并将校验通过的结果读入试听目录。
/// @warning 阻塞调用，只能交给后台文件池。decodePool 必须为独立线程池。
std::expected<Audio::StemAudition, std::string> runStemSeparation(
    const StemSeparationModel& model, const std::string& source,
    const std::string& interpreter, const std::string& backend, bool cpu,
    ice::ThreadPool& decodePool);
}  // namespace MMM::UI
