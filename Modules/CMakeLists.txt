# Modules/CMakeLists.txt 此入口按底层基础设施到上层应用的顺序注册全部自维护模块。 子目录只能依赖已在此前注册或由顶层提前提供的
# target，禁止形成反向环。 Main 最后加入，确保可执行程序只消费模块公开接口。

# -fno-rtti: 默认仅对项目自身 Module 启用；Log 模块会因 spdlog 多态对象单独恢复 RTTI。 第三方库
# concurrentqueue / fmt 等仍保持自身构建配置。
if(NOT MSVC)
  add_compile_options("-fno-rtti")
endif()

add_subdirectory("Log")
# Event 和 Config 构成业务模块共享的消息与持久化基础。
add_subdirectory("Event")
add_subdirectory("Config")
add_subdirectory("MMM")
add_subdirectory("Runtime")
add_subdirectory("Network")
# Game 组合编辑器运行逻辑与 UI，Audio 在其后注册独立媒体服务。
add_subdirectory("Game")
add_subdirectory("Audio")
add_subdirectory("Updater")
# Updater 保持独立可执行边界，Main 负责最终应用入口。
add_subdirectory("Main")
