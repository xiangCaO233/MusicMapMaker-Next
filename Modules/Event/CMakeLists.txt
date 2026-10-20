# Modules/Event/CMakeLists.txt

# Event 仅编译需要稳定动态库实例或依赖 GLFW 定义的实现文件。
add_library(Event src/core/EventBus.cpp)
if(NOT ANDROID)
  # Android 的输入由 ImGui SDL3 后端转换，不引入 GLFW 头或链接依赖。
  target_sources(Event PRIVATE src/input/translators/GLFWTranslator.cpp)
endif()

# Common、GLM 与 ImGui 出现在公共事件头中，GLFW 只服务原生翻译实现。
target_link_libraries(Event PUBLIC Common 3rd_glm 3rd_imgui)
if(NOT ANDROID)
  target_link_libraries(Event PRIVATE 3rd_glfw)
endif()

# 对外暴露统一事件类型和翻译接口，业务模块不依赖内部源码布局。
target_include_directories(Event PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
