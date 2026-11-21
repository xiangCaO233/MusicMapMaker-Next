#pragma once

#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"

#include <optional>

namespace MMM::Main
{

/// @brief 管理真实启动页和测试预览共用的窗口与最小渲染上下文。
/// @details 先初始化平台与 Vulkan 实例，再创建原生窗口和交换链。
/// 上层必须先销毁使用该设备的启动页资源，随后才能销毁窗口对象。
class StartupWindow final
{
public:
    /// @brief 按当前界面倍率创建启动窗口，并初始化 Bootstrap 渲染资源。
    /// @param title 操作系统窗口标题，预览程序可使用独立名称。
    explicit StartupWindow(const char* title);

    /// @brief 在原生窗口还存在时释放 Vulkan 与 ImGui 资源。
    /// @warning 退出低频路径：Vulkan 可能等待已提交的帧完成。
    ~StartupWindow();

    /// @brief 判断原生窗口和 Bootstrap 渲染器是否均已初始化。
    [[nodiscard]] bool isReady() const { return m_ready; }

    /// @brief 借用已准备好的上下文。
    /// @pre isReady() 为 true。
    Graphic::VKContext& context() { return m_context; }

    /// @brief 借用已创建的启动窗口。
    /// @pre isReady() 为 true。
    Graphic::NativeWindow& window() { return *m_window; }

private:
    /// @brief 上下文先构造以初始化窗口系统，析构体先释放设备与 surface。
    Graphic::VKContext m_context;
    /// @brief 仅在平台初始化成功后创建，部分初始化失败时也安全回收。
    std::optional<Graphic::NativeWindow> m_window;
    /// @brief 只在原生窗口、交换链和 ImGui 均准备完成后置为 true。
    bool m_ready{ false };
};

}  // namespace MMM::Main
