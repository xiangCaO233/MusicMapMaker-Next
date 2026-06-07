# Modules/CMakeLists.txt

# -fno-rtti: 仅对项目自身 Module 启用 (第三方库 concurrentqueue / fmt 仍依赖 RTTI)
if(NOT MSVC)
    add_compile_options("-fno-rtti")
endif()

add_subdirectory("Log")
add_subdirectory("Event")
add_subdirectory("Config")
add_subdirectory("MMM")
add_subdirectory("Runtime")
add_subdirectory("Game")
add_subdirectory("Audio")
add_subdirectory("Network")
add_subdirectory("Updater")
add_subdirectory("Main")
