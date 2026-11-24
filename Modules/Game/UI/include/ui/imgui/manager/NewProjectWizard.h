#pragma once

#include "event/project/ProjectOpenOrigin.h"
#include "ui/IUIView.h"
#include "ui/imgui/SideBarUI.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
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
    /// @param origin 唤出向导的用户入口，用于创建成功后的演练归因。
    void open(
        Event::ProjectOpenOrigin origin = Event::ProjectOpenOrigin::Unknown);

    /// @brief 关闭向导弹窗。
    void close();

private:
    /// @brief 回归测试通过实际探测入口注入隔离位置与 file pool 门闩。
    friend struct NewProjectLocationTestAccess;

    /// @brief 向导步骤。
    enum class Step { ProjectInfo, Preferences, Location };

    /// @brief 重置向导字段到默认值。
    void reset();

    /// @brief 绘制步骤标题。
    void renderStepHeader() const;

    /// @brief 绘制项目基本信息步骤。
    void renderProjectInfoStep();

    /// @brief 绘制项目初始偏好步骤，并上报展开列表的可交互范围。
    /// @param sourceManager 提供当前引导遮罩，可为空。
    void renderPreferencesStep(UIManager* sourceManager);

    /// @brief 绘制项目保存位置步骤。
    void renderLocationStep();

    /// @brief 绘制底部操作按钮并上报当前步骤的主操作目标。
    /// @param sourceManager 提供配置驱动突出层，可为空。
    void renderFooter(UIManager* sourceManager);

    /// @brief 绘制带独立标签的输入框，避免长标签被输入框宽度裁切。
    /// @param label 显示给用户的字段名。
    /// @param id ImGui 内部控件 ID。
    /// @param buffer 输入缓冲区。
    /// @param bufferSize 输入缓冲区长度。
    /// @return 输入内容发生变化时返回 true。
    bool renderLabeledInputText(const char* label, const char* id, char* buffer,
                                std::size_t bufferSize);

    /// @brief 请求在弹窗绘制结束后打开项目保存父目录选择器。
    void requestParentFolderPicker();

    /// @brief 如果已有挂起请求，则打开项目保存父目录选择器。
    void processPendingParentFolderPicker();

    /// @brief 打开统一文件选择器中的项目保存父目录选择窗口。
    /// @param initialPath 初始目录路径。
    void openUnifiedParentFolderPicker(const std::string& initialPath);

    /// @brief 绘制统一文件选择器中的父目录选择窗口。
    /// @param dpiScale 当前窗口内容缩放。
    void renderParentFolderPicker(float dpiScale);

    /// @brief 提交项目创建请求。
    void submitCreateRequest();

    /// @brief 判断本帧是否应禁止底部按钮响应。
    /// @return 原生目录选择器刚返回后的保护帧内返回 true。
    bool shouldSuppressFooterActionsThisFrame() const;

    /// @brief 当前步骤是否允许继续。
    /// @return 当前步骤输入有效时返回 true。
    bool canAdvance() const;

    /// @brief 保存位置的只读探测状态，区分未查、已占用和无法读取。
    enum class LocationStatus { Invalid, Available, ExistingProject };

    /// @brief 文件线程返回规范路径及探测结论；只适用于发出时的请求编号。
    struct LocationResult {
        std::uint64_t         revision;  ///< 打开实例内的探测序号。
        std::filesystem::path target;    ///< 确认后才提交的目标路径。
        LocationStatus        status;    ///< 路径是否可用于创建工程。
    };

    /// @brief 检查本轮录入位置，不访问向导实例或用户界面。
    static LocationResult probeLocation(std::filesystem::path parent,
                                        std::string           folder,
                                        std::uint64_t         revision);

    /// @brief 仅在输入改变或用户确认时探测，绘制帧只消费完成的结果。
    void updateLocationValidation();

    /// @brief 已复查的创建请求交给工程所有者。
    void publishCreateRequest();

    /// @brief 根据标题刷新默认文件夹名。
    void refreshFolderNameFromTitle();

    /// @brief 安全写入输入缓冲区。
    /// @param buffer 目标缓冲区。
    /// @param bufferSize 缓冲区长度。
    /// @param value 待写入文本。
    void copyToBuffer(char* buffer, std::size_t bufferSize,
                      std::string_view value);

    /// @brief 向导窗口是否打开。
    bool m_isOpen{ false };

    /// @brief 下一帧是否需要聚焦向导窗口。
    bool m_shouldOpen{ false };

    /// @brief 本轮新建流程的用户入口，提交请求后继续传到项目加载结果。
    Event::ProjectOpenOrigin m_openOrigin{ Event::ProjectOpenOrigin::Unknown };

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

    /// @brief 项目默认调色方案；空字符串表示继承软件默认。
    std::string m_colorPaletteSchemeName;

    /// @brief 新项目首次打开时的侧边栏页签。
    SideBarTab m_initialSideBarTab{ SideBarTab::FileExplorer };

    /// @brief 用户选择的项目保存父目录。
    std::filesystem::path m_parentDirectory;

    /// @brief 最近请求的父目录，用于比较用户输入而无需访问文件系统。
    std::filesystem::path m_probedParent;
    /// @brief 最近请求的文件夹名。
    std::string m_probedFolder;
    /// @brief 已进入文件池的探测。packaged_task future 的析构不会阻塞 UI。
    std::future<LocationResult> m_locationJob;
    /// @brief 只在当前父目录、名称和编号一致时提供可用的创建路径。
    std::optional<LocationResult> m_locationResult;
    /// @brief 输入变化、再次打开、用户确认时递增，旧结果不会替换新输入。
    std::uint64_t m_locationRevision{};
    /// @brief 创建按钮点击后重新读取路径；修改输入或退回时取消这次提交。
    bool m_confirmLocation{ false };

    /// @brief 最近一次保存位置选择失败时展示给用户的错误文本。
    std::string m_locationErrorText;

    /// @brief 原生文件夹选择器返回后需要丢弃底部按钮点击的剩余帧数。
    int m_suppressFooterActionFrames{ 0 };

    /// @brief 是否有等待当前 ImGui 弹窗绘制结束后打开的目录选择器请求。
    bool m_pendingParentFolderPicker{ false };
};

}  // namespace MMM::UI
