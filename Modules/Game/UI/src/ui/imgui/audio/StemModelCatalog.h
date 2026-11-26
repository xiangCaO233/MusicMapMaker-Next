#pragma once

#include "StemSeparationJob.h"

#include <array>
#include <expected>
#include <stop_token>
#include <string>

namespace MMM::UI
{
/// @brief 桌面目录中可以部署或绑定的模型条目。
struct StemCatalogEntry {
    /// @brief 持久化绑定和部署使用的稳定目录名。
    const char* id;
    /// @brief 模型原名。
    const char* name;
    /// @brief 所有可分离声部的显示文本。
    const char* stems;
    /// @brief 来自上游说明的适用范围。
    const char* strengths;
};

/// @brief 当前可自动部署的官方 Demucs 权重。
inline constexpr std::array STEM_MODEL_CATALOG{
    StemCatalogEntry{ "htdemucs",
                      "HTDemucs",
                      "人声 · 鼓 · 贝斯 · 其他",
                      "通用四轨，适合听人声和节奏组" },
    StemCatalogEntry{ "htdemucs_6s",
                      "HTDemucs 6s",
                      "人声 · 鼓 · 贝斯 · 吉他 · 钢琴 · 其他",
                      "可独听吉他；钢琴属于实验声部，可能串音" },
};

/// @brief 绑定成功后交给窗口的已解析模型、解释器和目录。
struct BoundStemModel {
    /// @brief 供分离任务复用的冻结模型包。
    StemSeparationModel model;
    /// @brief 每模型独立的 Python 和推理入口。
    StemSeparationEnvironment environment;
    /// @brief 模型根目录，用于持久化关联。
    std::string directory;
};

/// @brief 读取每个模型的目录绑定，缺失条目为空。
/// @warning 文件操作，仅首次打开工具调用。
std::array<std::string, STEM_MODEL_CATALOG.size()> readStemModelBindings();

/// @brief 原子合并一个模型绑定，保留其他控制器已保存的目录。
bool saveStemModelBinding(const StemCatalogEntry& entry,
                          const std::string&      directory);

/// @brief 解析模型目录并检查解释器、权重、配置及后端。
/// @warning 后台或用户显式选择路径后调用，不运行推理。
std::expected<BoundStemModel, std::string> bindStemModel(
    const StemCatalogEntry& entry, const std::string& directory);

/// @brief 在所选父目录中部署模型，成功后返回可直接绑定的结果。
/// @warning 阻塞文件任务。创建的虚拟环境为该模型目录私有。
std::expected<BoundStemModel, std::string> deployStemModel(
    const StemCatalogEntry& entry, const std::string& parent,
    std::stop_token stop);
}  // namespace MMM::UI
