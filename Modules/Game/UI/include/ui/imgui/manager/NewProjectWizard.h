#pragma once

#include "ui/IUIView.h"
#include "ui/imgui/SideBarUI.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace MMM::UI
{

/// @brief 新建项目向导，收集项目初始设置并提交创建请求。
class NewProjectWizard : public IUIView
{
public:
    /// @brief 构造新建项目向导。
    NewProjectWizard();

    /// @brief 销毁新建项目向导。
    ~NewProjectWizard() override = default;

    /// @brief 更新并绘制新建项目向导弹窗。
    /// @param sourceManager 当前 UI 管理器。
    void update(UIManager* sourceManager) override;

    /// @brief 打开向导并重置输入状态。
    void open();

    /// @brief 关闭向导弹窗。
    void close();

private:
    /// @brief 向导步骤。
    enum class Step { ProjectInfo, Preferences, Location };

    /// @brief 重置向导字段到默认值。
    void reset();

    /// @brief 绘制步骤标题。
    void renderStepHeader() const;

    /// @brief 绘制项目基本信息步骤。
    void renderProjectInfoStep();

    /// @brief 绘制项目初始偏好步骤。
    void renderPreferencesStep();

    /// @brief 绘制项目保存位置步骤。
    void renderLocationStep();

    /// @brief 绘制底部操作按钮。
    void renderFooter();

    /// @brief 绘制带独立标签的输入框，避免长标签被输入框宽度裁切。
    /// @param label 显示给用户的字段名。
    /// @param id ImGui 内部控件 ID。
    /// @param buffer 输入缓冲区。
    /// @param bufferSize 输入缓冲区长度。
    /// @return 输入内容发生变化时返回 true。
    bool renderLabeledInputText(const char* label, const char* id, char* buffer,
                                std::size_t bufferSize);

    /// @brief 打开项目保存父目录选择器。
    void openParentFolderPicker();

    /// @brief 绘制统一文件选择器中的父目录选择窗口。
    /// @param dpiScale 当前窗口内容缩放。
    void renderParentFolderPicker(float dpiScale);

    /// @brief 提交项目创建请求。
    void submitCreateRequest();

    /// @brief 当前步骤是否允许继续。
    /// @return 当前步骤输入有效时返回 true。
    bool canAdvance() const;

    /// @brief 当前保存位置是否有效。
    /// @return 保存位置有效时返回 true。
    bool hasValidTargetPath() const;

    /// @brief 获取当前项目目标目录。
    /// @return 项目目标目录路径。
    std::filesystem::path targetProjectPath() const;

    /// @brief 判断目标目录中是否已有项目描述文件。
    /// @return 已存在项目描述文件时返回 true。
    bool targetHasProjectFile() const;

    /// @brief 根据标题刷新默认文件夹名。
    void refreshFolderNameFromTitle();

    /// @brief 安全写入输入缓冲区。
    /// @param buffer 目标缓冲区。
    /// @param bufferSize 缓冲区长度。
    /// @param value 待写入文本。
    void copyToBuffer(char* buffer, std::size_t bufferSize,
                      std::string_view value);

    /// @brief 向导弹窗是否打开。
    bool m_isOpen{ false };

    /// @brief 下一帧是否需要打开弹窗。
    bool m_shouldOpen{ false };

    /// @brief 当前向导步骤。
    Step m_currentStep{ Step::ProjectInfo };

    /// @brief 用户是否手动编辑过项目文件夹名。
    bool m_folderNameEdited{ false };

    /// @brief 项目标题输入缓冲区。
    char m_titleBuf[256]{ 0 };

    /// @brief 项目艺术家输入缓冲区。
    char m_artistBuf[256]{ 0 };

    /// @brief 项目谱师输入缓冲区。
    char m_mapperBuf[256]{ 0 };

    /// @brief 项目文件夹名输入缓冲区。
    char m_folderNameBuf[256]{ 0 };

    /// @brief 项目默认物件调色方案；空字符串表示继承软件默认。
    std::string m_noteColorPaletteSchemeName;

    /// @brief 新项目首次打开时的侧边栏页签。
    SideBarTab m_initialSideBarTab{ SideBarTab::FileExplorer };

    /// @brief 用户选择的项目保存父目录。
    std::filesystem::path m_parentDirectory;

    /// @brief 最近一次保存位置选择失败时展示给用户的错误文本。
    std::string m_locationErrorText;
};

}  // namespace MMM::UI
