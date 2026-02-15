#pragma once

#include <expected>
#include <functional>  // reference_wrapper 需要这个头文件
#include <string>
#include <string_view>

namespace MMM
{
// 前向声明，减少头文件依赖
namespace Graphic
{
class VKContext;
}

/**
 * @brief 游戏主循环类 (单例)
 *
 * 负责管理应用程序的生命周期、窗口初始化以及主渲染循环。
 */
class GameLoop
{
public:
    /**
     * @brief 全局 Vulkan 上下文引用
     *
     * 存储 VKContext 的初始化结果。如果初始化成功，包含上下文引用；
     * 否则包含错误信息字符串。
     * @note 在使用前检查是否包含有效值。
     */
    std::expected<std::reference_wrapper<Graphic::VKContext>, std::string>
        vkContext;

    /**
     * @brief 获取 GameLoop 单例实例
     * @return GameLoop& 唯一实例引用
     */
    static GameLoop& instance();

    /**
     * @brief 启动游戏循环
     *
     * 初始化窗口、图形上下文，并进入主消息/渲染循环。
     * 该函数会阻塞直到窗口关闭。
     *
     * @param window_title 窗口标题
     * @return int 退出代码 (0 表示正常退出)
     */
    int start(std::string_view window_title);

private:
    GameLoop();
    ~GameLoop();

    // 禁用拷贝和移动
    GameLoop(GameLoop&&)                 = delete;
    GameLoop(const GameLoop&)            = delete;
    GameLoop& operator=(GameLoop&&)      = delete;
    GameLoop& operator=(const GameLoop&) = delete;
};
}  // namespace MMM
