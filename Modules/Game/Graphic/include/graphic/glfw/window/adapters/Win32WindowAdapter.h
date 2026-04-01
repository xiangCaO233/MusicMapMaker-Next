#pragma once

#ifdef _WIN32
#    include "event/ui/UpdateDragAreaEvent.h"
#    include <vector>
#    include <windows.h>

struct GLFWwindow;

namespace MMM::Graphic
{

/**
 * @class Win32WindowAdapter
 * @brief Windows 原生窗口适配器
 *
 * 负责接收跨平台的窗口事件，并将其转换为 Win32 特有的无边框窗口行为
 * 包括处理拖拽响应、无边框阴影、圆角等特性
 */
class Win32WindowAdapter
{
public:
    /**
     * @brief 构造函数
     * @param window GLFW 窗口句柄指针
     */
    explicit Win32WindowAdapter(GLFWwindow* window);

    /**
     * @brief 析构函数
     */
    ~Win32WindowAdapter();

    // 禁用拷贝和移动
    Win32WindowAdapter(Win32WindowAdapter&&)                 = delete;
    Win32WindowAdapter(const Win32WindowAdapter&)            = delete;
    Win32WindowAdapter& operator=(Win32WindowAdapter&&)      = delete;
    Win32WindowAdapter& operator=(const Win32WindowAdapter&) = delete;

private:
    /**
     * @brief Win32 窗口过程消息拦截器
     * @param hWnd 窗口句柄
     * @param uMsg 消息 ID
     * @param wParam 消息 wParam 参数
     * @param lParam 消息 lParam 参数
     * @param uIdSubclass 子类化 ID
     * @param dwRefData 附加引用数据（这里传入 this 指针）
     * @return LRESULT 消息处理结果
     */
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR uIdSubclass,
                                       DWORD_PTR dwRefData);

    /**
     * @brief 处理拖拽区域更新事件
     * @param e 拖拽区域更新事件
     */
    void onUpdateDragArea(const Event::UpdateDragAreaEvent& e);

private:
    GLFWwindow* m_window{ nullptr };           ///< 关联的 GLFW 窗口指针
    HWND        m_hwnd{ nullptr };             ///< 关联的 Win32 原生窗口句柄
    std::vector<Event::DragArea> m_dragAreas;  ///< 缓存的允许拖拽的矩形区域列表
};

}  // namespace MMM::Graphic
#endif