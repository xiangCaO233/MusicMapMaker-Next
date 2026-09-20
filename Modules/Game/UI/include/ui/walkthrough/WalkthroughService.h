#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::UI::Walkthrough
{
struct Topic;
struct Chapter;
struct Step;
class Progress;
/// @brief 独立于演练窗口的主题目录、事件适配、操作注册及进度存储服务。
class Service
{
public:
    /// @brief 加载内置与用户主题、恢复学习记录，并订阅业务结果事件。
    Service(const std::filesystem::path& progressPath,
            const std::filesystem::path& customDirectory);
    /// @brief 解除事件订阅，状态生命周期由 UIManager 管理。
    ~Service();
    /// @brief 消费业务结果；窗口关闭时仍调用。
    /// @warning UI
    /// 每帧只尝试读取事件队列；队列跨逻辑线程传递结果不可避免。仅进度改变时低频保存小文件。
    void update();
    /// @brief 取得已验证的主题目录。
    const std::vector<Topic>& topics() const;
    /// @brief 取得按顺序排列的章节，包括空章节。
    const std::vector<Chapter>& chapters() const;
    /// @brief 取得学习进度的非拥有引用。
    const Progress& progress() const;
    /// @brief 返回当前进程最近一次收到本步骤业务信号的单调序号。
    /// @return 尚未收到步骤所需信号时返回 0；历史持久化完成度不参与。
    /// @warning UI 热路径：只遍历当前步骤的小型信号数组并查询内存映射。
    std::uint64_t latestSignalRevision(const Step& step) const;
    /// @brief 判断步骤要求的业务信号是否在指定序号后重新到达。
    /// @param step 待判断步骤，保留 allSignals 的全满足语义。
    /// @param revision 当前路线步骤启动时记录的序号。
    /// @warning UI 热路径：仅在路线引导活动时遍历当前步骤信号。
    bool receivedSignalAfter(const Step& step, std::uint64_t revision) const;
    /// @brief 返回加载或保存失败说明。
    const std::string& error() const;
    /// @brief 手动确认一个步骤并持久化。
    void acknowledge(const Topic& topic, const Step& step);
    /// @brief 重置当前主题，保留其他主题和自定义内容。
    void reset(const Topic& topic);
    /// @brief 注册可信 C++ 操作，数据文件不能执行未注册命令。
    void registerAction(std::string id, std::function<void()> action);
    /// @brief 查询操作是否已经注册。
    bool hasAction(std::string_view id) const;
    /// @brief 用户点击时调用已注册操作。
    void execute(std::string_view id);

private:
    struct Impl;                   ///< 目录、队列和持久化实现。
    std::unique_ptr<Impl> m_impl;  ///< 稳定服务状态。
};
}  // namespace MMM::UI::Walkthrough
