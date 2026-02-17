# Modules/Main/CMakeLists.txt

if(WIN32)
    # Windows 可执行文件编译配置
    # 编译MusicMapMaker-Next.exe
    if(CMAKE_BUILD_TYPE STREQUAL Debug)
        add_executable(MusicMapMaker-Next src/main.cpp)
    else()
        # Windows 非Debug模式下添加WIN32标识不显示控制台
        add_executable(MusicMapMaker-Next WIN32 src/main.cpp)
        add_strip_command_for_release(MusicMapMaker-Next)
    endif()
else()
    # Linux/Macos 可执行文件编译配置
    # 编译MusicMapMaker-Next可执行文件
    add_executable(MusicMapMaker-Next src/main.cpp)
endif()

# 编译imgui-demo(.exe)可执行文件
add_executable(imgui-demo src/imgui_main_demo.cpp)
add_strip_command_for_release(imgui-demo)
target_link_libraries(imgui-demo PUBLIC Vulkan::Vulkan 3rd_glfw 3rd_imgui Log Config Audio Game)

target_link_libraries(
        MusicMapMaker-Next
        PRIVATE
        Log Config Audio Game
)
