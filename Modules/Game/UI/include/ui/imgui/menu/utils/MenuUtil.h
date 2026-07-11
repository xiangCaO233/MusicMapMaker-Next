#pragma once

#include "common/LogicCommands.h"
#include "ui/imgui/menu/MainMenuTypes.h"

#include <filesystem>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI
{

/// @brief 主菜单 action 共享的无状态工具函数集合。
class MenuUtil
{
public:
    /// @brief 发布逻辑命令事件。
    /// @param cmd 需要分发给逻辑层的命令。
    static void dispatchCommand(const Logic::LogicCommand& cmd);

    /// @brief 打开项目目录选择器并发布打开项目事件。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞。
    static void openProjectFolderPicker();

    /// @brief 打开音频导入选择器并发布导入事件。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞。
    static void openAudioImportPicker();

    /// @brief 当前是否存在活跃谱面。
    /// @param requireProject 是否同时要求当前项目存在。
    /// @return 存在活跃谱面时返回 true。
    /// @warning UI 热路径低频分支：仅在菜单展开或 action
    /// 判定时读取当前会话状态。
    static bool hasActiveBeatmap(bool requireProject);

    /// @brief 当前是否允许触发画布编辑类快捷键。
    /// @return 允许触发时返回 true。
    /// @warning UI 热路径：每帧快捷键判断调用；只读取 ImGui 输入阻断状态。
    static bool canTriggerCanvasEditingShortcut();

    /// @brief 将 ASCII 字符串转换为小写。
    /// @param value 原始字符串。
    /// @return 转换后的字符串。
    static std::string toLowerAscii(std::string value);

    /// @brief 获取 UTF-8 路径的小写扩展名。
    /// @param path UTF-8 路径字符串。
    /// @return 小写扩展名。
    static std::string lowerExtension(const std::string& path);

    /// @brief 根据导出格式生成推荐文件名。
    /// @param extension 目标扩展名。
    /// @param currentFileName 当前文件名，用于保留非 RM/IMD 格式的主文件名。
    /// @return 推荐文件名。
    static std::string makeExportFileNameForExtension(
        const std::string& extension, const std::string& currentFileName);

    /// @brief 按统一导出文件选择器当前格式规范化保存路径。
    /// @param path 文件选择器返回的路径。
    /// @return 应实际导出的目标路径。
    static std::string applySaveAsSelectedFormatToPath(const std::string& path);

    /// @brief 获取另存为对话框默认打开路径。
    /// @return UTF-8 编码的默认目录路径。
    static std::string getSaveAsPickerDefaultPath();

    /// @brief 收集当前谱面导出到指定格式时需要提醒用户的兼容性问题。
    /// @param path 目标导出路径。
    /// @return 需要展示的警告消息列表。
    static std::vector<std::string> collectExportCompatibilityWarnings(
        const std::string& path);

    /// @brief 判断当前 MC 导出目标是否需要显示上架 mode_ext 选项。
    /// @param path 目标导出路径。
    /// @return 导出 MC 且当前谱面含 Flick/折线时返回 true。
    static bool shouldOfferMalodyStoreModeExtForCurrentExport(
        const std::string& path);

    /// @brief 将项目谱面路径规范化为候选比较键。
    /// @param projectRoot 当前项目根目录。
    /// @param path 谱面路径，可为项目相对路径或绝对路径。
    /// @return 规范化后的 UTF-8 路径键。
    static std::string makeProjectBeatmapPathKey(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& path);

    /// @brief 将下一项控件放到当前内容区域的水平中心。
    /// @param itemWidth 控件宽度。
    /// @warning UI 绘制路径：只调整当前 ImGui 游标位置。
    static void centerNextItem(float itemWidth);

    /// @brief 绘制水平居中的按钮。
    /// @param label 按钮文本和 ImGui ID。
    /// @param size 按钮尺寸。
    /// @return 按钮被点击时返回 true。
    /// @warning UI 绘制路径：只调整游标并调用统一反馈按钮。
    static bool drawCenteredButton(const char* label, ImVec2 size);

    /// @brief 在当前内容区域内绘制自动换行文本。
    /// @param text 待绘制的 UTF-8 文本。
    /// @warning UI 绘制路径：只设置 ImGui 文本换行位置并绘制文本。
    static void drawWrappedText(std::string_view text);

    /// @brief 绘制可自动换行的项目符号文本。
    /// @param text 项目符号后的 UTF-8 文本。
    /// @warning UI 绘制路径：只绘制 ImGui 项目符号和换行文本。
    static void drawWrappedBulletText(std::string_view text);

    /// @brief 绘制标签和值，并让值在当前内容区域内自动换行。
    /// @param label 标签文本。
    /// @param value 值文本。
    /// @warning UI 绘制路径：只绘制 ImGui 文本，不执行阻塞操作。
    static void drawWrappedLabelValue(std::string_view label,
                                      std::string_view value);
};

}  // namespace MMM::UI
