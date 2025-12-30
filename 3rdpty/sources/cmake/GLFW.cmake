# 3rdpty/sources/cmake/BuildGLFW.cmake

# 定义源码路径
set(GLFW_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/glfw")

# 覆盖 GLFW 的内部选项
set(GLFW_BUILD_DOCS
    OFF
    CACHE BOOL "Disable GLFW documentation" FORCE)
set(GLFW_BUILD_TESTS
    OFF
    CACHE BOOL "Disable GLFW tests" FORCE)
set(GLFW_BUILD_EXAMPLES
    OFF
    CACHE BOOL "Disable GLFW examples" FORCE)
set(GLFW_INSTALL
    OFF
    CACHE BOOL "Disable GLFW install target" FORCE)

# 将 GLFW 作为子项目包含进来 GLFW 会自动创建一个名为 'glfw' 的 target
add_subdirectory(${GLFW_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/glfw_build
                 EXCLUDE_FROM_ALL)

# 创建接口库
add_library(3rd_glfw INTERFACE)
target_link_libraries(3rd_glfw INTERFACE glfw)
