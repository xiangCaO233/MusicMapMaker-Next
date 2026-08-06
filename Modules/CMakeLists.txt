# Modules/CMakeLists.txt

# -fno-rtti: 默认仅对项目自身 Module 启用；Log 模块会因 spdlog 多态对象单独恢复 RTTI。 第三方库
# concurrentqueue / fmt 等仍保持自身构建配置。
if(NOT MSVC)
  add_compile_options("-fno-rtti")
endif()

add_subdirectory("Log")
add_subdirectory("Event")
add_subdirectory("Config")
add_subdirectory("MMM")
add_subdirectory("Runtime")
add_subdirectory("Collaboration")
add_subdirectory("Game")
add_subdirectory("Audio")
add_subdirectory("Network")
add_subdirectory("Updater")
add_subdirectory("Main")
