#pragma once

#include <string_view>

struct GLFWwindow;

namespace MMM
{
namespace Graphic {
class VKContext;
}
namespace Canvas
{
class TestCanvas
{
public:
    TestCanvas(int w, int h, std::string_view windwo_title);
    ~TestCanvas();

    // 窗口是否点击了关闭
    bool shouldClose();

    // 更新并交换缓冲区
    void update(Graphic::VKContext& vkContext);

    // 获取窗口句柄
    GLFWwindow* getWindowHandle();

private:
    GLFWwindow* m_windowHandle{ nullptr };
};
}  // namespace Canvas
}  // namespace MMM
