#pragma once

#include <memory>

namespace MMM
{
namespace Canvas
{
class TestCanvas
{
public:
    TestCanvas(int w, int h);
    ~TestCanvas();

    // 窗口是否点击了关闭
    bool shouldClose();

    // 更新并交换缓冲区
    void update();

private:
    // GLFW上下文实现指针
    struct GLFWContextImpl;
    std::unique_ptr<GLFWContextImpl> m_glfwImplPtr;
};
}  // namespace Canvas
}  // namespace MMM
