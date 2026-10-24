#pragma once


#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace MMM::Config
{
struct EditorConfig;
struct EditorSettings;
struct VisualConfig;
struct AudioPresets;
struct LayoutPresets;

/// @brief 全局应用配置管理器
class AppConfig
{
public:
    static AppConfig& instance();

    /// @brief 获取编辑器配置
    EditorConfig&       getEditorConfig();
    const EditorConfig& getEditorConfig() const;

    /// @brief 获取仅属于客户端的音频预设库。
    AudioPresets& getAudioPresets();
    /// @brief 获取预设库的只读视图。
    const AudioPresets& getAudioPresets() const;

    /// @brief 获取客户端布局预设库，不参与项目工作区快照。
    LayoutPresets& getLayoutPresets();
    /// @brief 只读访问布局预设集合。
    const LayoutPresets& getLayoutPresets() const;

    /// @brief 获取视觉配置
    VisualConfig&       getVisualConfig();
    const VisualConfig& getVisualConfig() const;

    /// @brief 获取编辑器设置
    EditorSettings&       getEditorSettings();
    const EditorSettings& getEditorSettings() const;

    /// @brief 获取当前应用配置持久化的协作者稳定标识。
    /// @return 不随房间 PeerId 或重连变化的 128 位十六进制标识。
    [[nodiscard]] const std::string& getCollaborationParticipantId() const
    {
        return m_collaborationParticipantId;
    }

    /// @brief 从文件加载配置
    /// @param path 配置文件路径，若为空则使用默认路径
    bool load(const std::filesystem::path& path = "");

    /// @brief 保存配置到文件
    /// @param path 配置文件路径，若为空则使用默认路径
    bool save(const std::filesystem::path& path = "") const;

    /// @brief 在布局实际修改时立即选中用户方案并排队保存不可变快照。
    /// @warning 只由修改事件调用，后台写盘不阻塞连续拖动。
    void saveLayoutChanges(const std::filesystem::path& path = "");

    /// @brief 重置为默认配置
    void reset();

    /// @brief 添加最近打开的项目
    /// @param path 项目文件夹路径
    void addRecentProject(const std::string& path);

    /// @brief 获取设备屏幕刷新率
    int getDeviceRefreshRate() const { return m_deviceRefreshRate; }

    /// @brief 设置设备屏幕刷新率（由图形模块在启动时写入）
    void setDeviceRefreshRate(int rate) { m_deviceRefreshRate = rate; }

    /// @brief 获取窗口缩放比例 (GLFW 原生报告的比例，用于字体加载等)
    float getNativeContentScale() const { return m_nativeContentScale; }

    /// @brief 设置窗口缩放比例
    void setNativeContentScale(float scale) { m_nativeContentScale = scale; }

    /// @brief 获取 UI 布局缩放比例
    /// (已减去系统自动缩放部分，并包含用户自定义倍率)
    float getUIScale() const;

    /// @brief 设置 UI 布局缩放比例 (仅设置系统部分)
    void setUIScale(float scale) { m_uiScale = scale; }

    /// @brief 获取窗口缩放比例 (包含用户自定义倍率)
    float getWindowContentScale() const;

    /// @brief 设置窗口缩放比例 (仅设置系统部分)
    void setWindowContentScale(float scale) { m_uiScale = scale; }

private:
    /// @brief 创建持有默认 EditorConfig 的进程级配置单例。
    AppConfig();
    /// @brief 在完整预设类型可见处销毁私有状态。
    ~AppConfig();

    AppConfig(const AppConfig&)            = delete;
    AppConfig& operator=(const AppConfig&) = delete;

    /// @brief 获取默认用户配置文件路径。
    /// @return 用户目录/.config/mmm/user_config.json。
    std::filesystem::path getDefaultConfigPath() const;

    /// @brief 冻结完整配置文档；后台线程不读取实时可变字段。
    /// @warning 低频保存或明确布局修改路径调用，禁止空闲帧轮询。
    nlohmann::json snapshotForSave() const;

    /// @brief 完整配置与预设共享的长期私有存储，不向标量消费者传播定义。
    struct State;
    /// @brief 复用原有一次私有状态分配；加载和重置仅替换内容，引用地址稳定。
    std::unique_ptr<State> m_state;
    /// @brief 当前应用配置持久化的协作者稳定标识。
    std::string        m_collaborationParticipantId;
    mutable std::mutex m_mutex;
    int                m_deviceRefreshRate{ 60 };  // 默认60Hz
    float              m_nativeContentScale{ 1.0f };
    float              m_uiScale{ 1.0f };
};

}  // namespace MMM::Config
