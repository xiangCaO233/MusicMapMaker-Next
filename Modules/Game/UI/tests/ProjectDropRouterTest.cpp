#include "ui/project/ProjectDropRouter.h"

#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <string>

namespace
{
/// @brief 在没有画布的主视口中投放 BMS 谱面并检查打开请求。
/// @return 大小写扩展名、中文路径与事件消费规则均正确时返回 true。
/// @note 测试不解析谱面内容，导入合法性由独立 BmsFormatTest 覆盖。
/// @note 这里只订阅打开请求，不启动项目服务，因此不会扫描用户目录。
/// @pre 演练路径限制未启用，测试进程未注册项目打开执行者。
/// @post 所有局部回调在返回前解除订阅。
/// @details 空工作区没有画布监听者，必须由主窗口路由独立发出事件。
/// @details 大写扩展名测试模拟 Windows/macOS 用户拖入的常见命名。
/// @details 中文目录保留原始编码，只对扩展名进行 ASCII 归一化。
/// @details 连续 update 用于验证队列消费，不用等待或重放掩盖重复请求。
/// @details 无效事件之后继续处理合法事件，确保拒绝分支不残留待处理路径。
/// @warning 单线程测试：事件总线回调和 update 在同一线程执行。
bool testBmsDrops()
{
    MMM::UI::ProjectDropRouter router;
    auto&                      bus    = MMM::Event::EventBus::instance();
    std::size_t                opened = 0;
    std::string                lastPath;
    bool                       correctOrigin = false;
    // 捕获真实事件，不能仅测试一份独立的扩展名比较函数。
    // 完整路径必须保留，父目录推导留给项目服务。
    const auto subscription =
        bus.subscribe<MMM::Event::OpenProjectEvent>([&](const auto& event) {
            ++opened;
            lastPath = MMM::Config::pathToUtf8(event.m_projectPath);
            correctOrigin =
                event.m_origin == MMM::Event::ProjectOpenOrigin::BeatmapDrop;
        });
    // UTF-8 路径不必真实存在：路由只判断非目录的文件扩展名。
    // 不在资源目录创建文件，也不依赖操作系统目录大小写规则。
    const std::array paths{ "拖放测试/谱面.bms", "chart.BMS", "chart.bme",
                            "chart.BME",         "chart.bml", "chart.BML",
                            "chart.pms",         "chart.PMS" };
    bool             passed = true;
    for ( const char* path : paths ) {
        MMM::Event::GLFWDropEvent drop;
        drop.paths          = { path };
        drop.pos            = { 20.0F, 20.0F };
        const auto previous = opened;
        bus.publish(drop);
        router.update(true);
        // 单次更新应发出一次请求，并原样保留路径和 BeatmapDrop 来源。
        passed = passed && opened == previous + 1 && lastPath == path &&
                 correctOrigin;
        router.update(true);
        // 无新事件的后续帧必须无操作，不能重复切换项目。
        passed = passed && opened == previous + 1;
    }
    const auto                previous = opened;
    MMM::Event::GLFWDropEvent ignored;
    ignored.paths = { "disabled.bms" };
    ignored.pos   = { 20.0F, 20.0F };
    bus.publish(ignored);
    router.update(false);
    router.update(true);
    // 禁用期间丢弃请求，恢复后不能补执行旧投放。
    passed        = passed && opened == previous;
    ignored.paths = { "one.bms", "two.bms" };
    bus.publish(ignored);
    router.update(true);
    // 多路径事件沿用既有策略，不能悄悄只打开第一个。
    passed        = passed && opened == previous;
    ignored.paths = { "outside.bms" };
    ignored.pos   = { -1.0F, 20.0F };
    bus.publish(ignored);
    router.update(true);
    // 主窗口坐标范围外的投放不能影响编辑器。
    passed        = passed && opened == previous;
    ignored.paths = { "chart.bms.backup" };
    ignored.pos   = { 20.0F, 20.0F };
    bus.publish(ignored);
    router.update(true);
    // 文件名包含 .bms 不等于扩展名是 .bms，避免把备份文件当谱面。
    passed = passed && opened == previous;
    // 拒绝边界之后继续投放合法文件，确认队列仍能恢复正常处理。
    ignored.paths = { "valid-after-rejection.bms" };
    bus.publish(ignored);
    router.update(true);
    passed = passed && opened == previous + 1;
    bus.unsubscribe<MMM::Event::OpenProjectEvent>(subscription);
    // 在局部捕获变量析构之前解除订阅，防止后续测试访问悬空引用。
    return passed;
}
}  // namespace

/// @brief 使用最小 ImGui 上下文验证系统文件投放路径。
/// @return 正式路由完整处理 BMS 家族与拒绝边界时返回零。
/// @note 无需字体、渲染后端、音频设备或桌面窗口。
/// @pre 本测试是独立入口，创建自己的 ImGui 上下文。
/// @post 所有路由对象先于 ImGui 上下文销毁。
/// @details 逻辑像素固定，不依赖本机 DPI 或显示器分辨率。
/// @details 视口内部点使用非零坐标，避开窗口边界的包含性差异。
/// @details 路由对象析构时解除 GLFW 订阅，测试不保留跨场景全局监听者。
/// @warning 结果只证明投放路由，不代表目标格式解析或资源加载已完成。
int main()
{
    // 创建上下文后主视口可直接设置逻辑尺寸，无需启动 UI 帧循环。
    // 路由读取的是视口边界与弹窗栈，不依赖当前鼠标悬浮窗口。
    ImGui::CreateContext();
    ImGui::GetMainViewport()->Size = ImVec2(1280.0F, 720.0F);
    const bool passed              = testBmsDrops();
    ImGui::DestroyContext();
    return passed ? 0 : 1;
}
