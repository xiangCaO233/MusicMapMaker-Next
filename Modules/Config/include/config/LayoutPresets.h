#pragma once

#include "config/VisualConfig.h"
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace MMM::Config
{
/// @brief 一个包含所有键数布局及公共相机姿态的客户端方案。
/// @note visual 只是快照载体；捕获、应用和文件序列化均只处理布局字段。
/// 背景、配色、音频和谱面内容不属于方案，切换时保持实时配置。
struct LayoutPreset {
    /// @brief 库内唯一名称，文件导入发生冲突时生成后缀。
    std::string m_name{ "默认" };
    /// @brief 布局快照，包含未定制键数使用的回退模板。
    VisualConfig m_visual;
    /// @brief 判断是否为代码定义的保留方案，不信任文件中的只读标志。
    bool isBuiltIn() const;
    /// @brief 返回界面名称；内置项带标志，稳定身份与持久名称保持不变。
    /// @warning 每帧界面读取仅返回已有字符串，不分配显示名副本。
    const char* displayName() const;
};
/// @brief 只属于客户端的布局方案集合，至少保留一项。
/// @note 修改边沿立即捕获；连续拖动只排队快照，磁盘写入由后台完成。
struct LayoutPresets {
    /// @brief 构造普通布局与用户指定的斜轨预览布局。
    LayoutPresets();
    /// @brief 方案列表，名称为稳定身份；所有内置项始终保持代码基准。
    std::vector<LayoutPreset> m_items;
    /// @brief 当前方案名称，也是重启后的选择。
    std::string m_selected{ "默认" };
    /// @brief 读取当前项，失效选择回退首项。
    LayoutPreset& current();
    /// @brief 读取当前项的只读视图。
    const LayoutPreset& current() const;
    /// @brief 捕获离开的方案，再应用目标方案的全部键数布局。
    bool select(const std::string& name, VisualConfig& visual);
    /// @brief 收存实时修改；内置项被修改时自动建立并选中自定义副本。
    /// @return 布局内容或当前方案身份发生变化时为真。
    bool captureCurrent(const VisualConfig& visual);
    /// @brief 收存当前现场并切回代码默认布局，不删除已有自定义项。
    void resetToDefault(VisualConfig& visual);
    /// @brief 复制当前实时布局并选中副本，拒绝空名和重名。
    bool duplicate(const std::string& name, const VisualConfig& visual);
    /// @brief 重命名当前自定义方案，拒绝内置项、空名和重名。
    bool rename(const std::string& name);
    /// @brief 删除当前自定义方案并恢复首项，禁止删除内置项或最后一项。
    bool remove(VisualConfig& visual);
    /// @brief 导入单项并选中；失败不改变库或实时配置。
    bool importFile(const std::filesystem::path& path, VisualConfig& visual,
                    std::string& error);
};
/// @brief 捕获布局相关字段；不会保存音量、配色或播放状态。
void captureLayoutPreset(LayoutPreset& preset, const VisualConfig& visual);
/// @brief 应用布局相关字段；不会覆盖接收端的其他视觉设置。
void applyLayoutPreset(const LayoutPreset& preset, VisualConfig& visual);
/// @brief 写出单个版本化方案，完整保留所有键数布局和相机参数。
/// @return 文件写入并关闭成功时为真；错误由 error 返回。
bool exportLayoutPreset(const std::filesystem::path& path,
                        const LayoutPreset& preset, std::string& error);
/// @brief 序列化客户端方案集合。
void to_json(nlohmann::json& json, const LayoutPresets& presets);
/// @brief 验证每项结构后恢复集合，损坏项不进入配置反序列化器。
/// @note 无可用项时保留调用方回退库；默认构造库含两个内置方案。
void from_json(const nlohmann::json& json, LayoutPresets& presets);
}  // namespace MMM::Config
