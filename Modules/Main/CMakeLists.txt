# Modules/Main/CMakeLists.txt

if(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL Debug)
        add_executable(MusicMapMaker-Next src/main.cpp)
    else()
        add_executable(MusicMapMaker-Next WIN32 src/main.cpp)
    endif()
else()
    add_executable(MusicMapMaker-Next src/main.cpp)
    add_executable(imgui-demo src/imgui_main_demo.cpp)
    target_link_libraries(imgui-demo PUBLIC Vulkan::Vulkan 3rd_glfw 3rd_imgui Log Config Audio Game)
endif()

target_link_libraries(
    MusicMapMaker-Next
    PRIVATE
    Log Config Audio Game
)
