# 3rdpty/sources/cmake/BuildGLFW.cmake

# 定义源码路径
set(GLFW_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/glfw")

# 覆盖 GLFW 的内部选项
set(GLFW_BUILD_DOCS
    OFF
    CACHE BOOL "Disable GLFW documentation" FORCE)
# 避免上游窗口探针介入项目 CTest；原生平台行为由项目自己的窗口适配器测试覆盖。
set(GLFW_BUILD_TESTS
    OFF
    CACHE BOOL "Disable GLFW tests" FORCE)
# 项目只分发 GLFW 运行所需产物，样例的独立窗口和渲染依赖不应进入构建图。
set(GLFW_BUILD_EXAMPLES
    OFF
    CACHE BOOL "Disable GLFW examples" FORCE)
# 安装布局由父项目负责，禁用上游规则以免写入非选定前缀。
set(GLFW_INSTALL
    OFF
    CACHE BOOL "Disable GLFW install target" FORCE)

# 将 GLFW 作为子项目包含进来 GLFW 会自动创建一个名为 'glfw' 的 target
add_subdirectory(${GLFW_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/glfw_build
                 EXCLUDE_FROM_ALL SYSTEM)

# 创建接口库
add_library(3rd_glfw INTERFACE)
# 包装接口统一源码与预编译消费者的公共链接名称。
target_link_libraries(3rd_glfw INTERFACE glfw)
