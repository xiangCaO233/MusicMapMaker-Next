# Updater - 最小独立的更新器辅助可执行文件
# 完全静态链接，不依赖任何项目库

if(WIN32)
    set(UPDATER_RESOURCE MusicMapMaker-Updater.rc)
endif()

add_executable(MusicMapMaker-Updater
    main.cpp
    ${UPDATER_RESOURCE}
)

# 输出到与主程序相同的 bin 目录
set_target_properties(MusicMapMaker-Updater PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Release / MinSizeRel / RelWithDebInfo 模式下剥离符号
# 对于 GCC 编译，需先通过 add_gcc_debug_extract 提取调试信息
add_gcc_debug_extract(MusicMapMaker-Updater)
add_strip_command_for_release(MusicMapMaker-Updater)

if(WIN32)
    target_link_libraries(MusicMapMaker-Updater PRIVATE shell32 ws2_32)
endif()
