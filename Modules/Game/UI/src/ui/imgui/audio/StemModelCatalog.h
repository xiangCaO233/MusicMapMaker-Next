#pragma once

#include "StemSeparationJob.h"

#include <array>
#include <expected>
#include <stop_token>
#include <string>

namespace MMM::UI
{
/// @brief 桌面目录中可以部署或绑定的模型条目。
/// @details 说明文本随 UI 构建，模型文件与许可证仍由上游发布。
/// 不把后端模型类型当作目录键，同一架构的不同权重有独立绑定。
struct StemCatalogEntry {
    /// @brief 持久化绑定和部署使用的稳定目录名。
    const char* id;
    /// @brief 模型原名。
    const char* name;
    /// @brief 所有可分离声部的显示文本。
    /// @details 与安装器生成的计划保持一致，混音台从计划取实际通道。
    const char* stems;
    /// @brief 来自上游说明的适用范围，避免将整组鼓描述成鼓件。
    const char* strengths;
    /// @brief 窄目录表中的换行标签，空值表示直接显示模型原名。
    const char* catalogName = nullptr;
};

/// @brief 当前可自动部署的固定权重版本。
/// @details 目录 ID 与 Python 安装器的预设及原有绑定文件共用。
/// 单目标预设的伴音由混音减去模型目标，目录说明需明确训练方向。
inline constexpr std::array STEM_MODEL_CATALOG{
    StemCatalogEntry{ "htdemucs",
                      "HTDemucs",
                      "人声 · 鼓 · 贝斯 · 其他",
                      "通用四轨，适合听人声和节奏组" },
    StemCatalogEntry{ "htdemucs_6s",
                      "HTDemucs 6s",
                      "人声 · 鼓 · 贝斯 · 吉他 · 钢琴 · 其他",
                      "可独听吉他；钢琴属于实验声部，可能串音" },
    // DrumSep 需要整组鼓作为输入，部署方案会先用 HTDemucs 从主音轨提取鼓组。
    StemCatalogEntry{ "drumsep",
                      "DrumSep",
                      "底鼓 · 军鼓 · 通鼓 · 镲片（含踩镲）",
                      "先提取鼓组，再分离鼓件" },
    // SCNet 两个大小使用上游各自的配置和检查点，四轨定义保持一致。
    StemCatalogEntry{ "scnet",
                      "SCNet",
                      "人声 · 鼓 · 贝斯 · 其他",
                      "轻量四轨，适合拆分节奏组" },
    StemCatalogEntry{ "scnet_large",
                      "SCNet-large",
                      "人声 · 鼓 · 贝斯 · 其他",
                      "较大四轨模型，适合拆分节奏组",
                      "SCNet-\nlarge" },
    // 双轨模型标示实际训练目标，另一轨为与原混音的差值。
    StemCatalogEntry{ "kimberley_vocals",
                      "KimberleyJSN Mel-Band RoFormer",
                      "人声 · 伴奏",
                      "优先提取人声",
                      "Kimberley\nJSN" },
    StemCatalogEntry{ "becruily_vocals",
                      "Becruily Mel-Band RoFormer（人声）",
                      "人声 · 伴奏",
                      "优先提取人声",
                      "Becruily\n人声" },
    StemCatalogEntry{ "becruily_instrumental",
                      "Becruily Mel-Band RoFormer（伴奏）",
                      "人声 · 伴奏",
                      "优先提取伴奏",
                      "Becruily\n伴奏" },
    StemCatalogEntry{ "viperx_bs_1297",
                      "ViperX BS-RoFormer 1297",
                      "人声 · 伴奏",
                      "优先提取人声",
                      "ViperX\n1297" },
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
/// @return 按当前目录排序的路径，持久化格式以模型 ID 查找。
/// @warning 文件操作，仅首次打开工具调用。
std::array<std::string, STEM_MODEL_CATALOG.size()> readStemModelBindings();

/// @brief 原子合并一个模型绑定，保留其他控制器已保存的目录。
/// @param entry 用户确认绑定的目录条目。
/// @param directory 已校验的部署根目录。
bool saveStemModelBinding(const StemCatalogEntry& entry,
                          const std::string&      directory);

/// @brief 解析模型目录并检查解释器、权重、配置及后端。
/// @return 成功时返回冻结的运行配置，失败时给出可展示的错误。
/// @warning 后台或用户显式选择路径后调用，不运行推理。
std::expected<BoundStemModel, std::string> bindStemModel(
    const StemCatalogEntry& entry, const std::string& directory);

/// @brief 在所选父目录中部署模型，成功后返回可直接绑定的结果。
/// @param parent 用户选择的已有父目录，部署器在其中排他创建预设子目录。
/// @warning 阻塞文件任务。创建的虚拟环境为该模型目录私有。
std::expected<BoundStemModel, std::string> deployStemModel(
    const StemCatalogEntry& entry, const std::string& parent,
    std::stop_token stop);
}  // namespace MMM::UI
