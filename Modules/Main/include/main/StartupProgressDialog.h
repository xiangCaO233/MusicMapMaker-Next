#pragma once

#include "network/AssetSyncService.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace MMM::Main
{

/// @brief 启动期资源同步进度弹窗。
class StartupProgressDialog
{
public:
    /// @brief 创建系统进度弹窗。
    StartupProgressDialog();

    /// @brief 关闭系统进度弹窗。
    ~StartupProgressDialog();

    StartupProgressDialog(const StartupProgressDialog&)            = delete;
    StartupProgressDialog& operator=(const StartupProgressDialog&) = delete;

    /// @brief 判断进度阶段是否值得打开弹窗。
    /// @param progress 资源同步进度。
    /// @return 需要向用户展示等待状态时返回 true。
    static bool shouldOpenFor(const Network::AssetSyncProgress& progress);

    /// @brief 更新系统进度弹窗。
    /// @param progress 资源同步进度。
    void update(const Network::AssetSyncProgress& progress);

    /// @brief 主动关闭系统进度弹窗。
    void close();

private:
    /// @brief 计算进度百分比。
    /// @param progress 资源同步进度。
    /// @return 0 到 100 的进度值。
    int progressPercent(const Network::AssetSyncProgress& progress) const;

    /// @brief 生成弹窗文本。
    /// @param progress 资源同步进度。
    /// @return 面向用户的进度说明。
    std::string progressText(const Network::AssetSyncProgress& progress) const;

    /// @brief 写入 zenity 进度。
    /// @param percent 进度百分比。
    /// @param text 进度文本。
    void writeProgress(int percent, const std::string& text);

    FILE*       m_pipe{ nullptr };    ///< POSIX zenity 进度管道。
    int         m_lastPercent{ -1 };  ///< 上次写入的进度百分比。
    std::string m_lastText;           ///< 上次写入的进度文本。
    std::chrono::steady_clock::time_point
        m_lastUpdateTime{};  ///< 上次写入时间。
};

}  // namespace MMM::Main
