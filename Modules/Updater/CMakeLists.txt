# Updater - 最小独立的更新器辅助可执行文件
# 完全静态链接，不依赖任何项目库

add_executable(MusicMapMaker-Updater
    main.cpp
)

# 输出到与主程序相同的 bin 目录
set_target_properties(MusicMapMaker-Updater PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Release / MinSizeRel / RelWithDebInfo 模式下剥离符号
# 对于 GCC 编译，需先通过 add_gcc_debug_extract 提取调试信息
add_gcc_debug_extract(MusicMapMaker-Updater)
add_custom_command(
    TARGET MusicMapMaker-Updater
    POST_BUILD
    COMMAND $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:${CMAKE_STRIP}> $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:-s>
        $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:$<TARGET_FILE:MusicMapMaker-Updater>>
    COMMENT "Stripping MusicMapMaker-Updater in Release/MinSizeRel/RelWithDebInfo"
    VERBATIM
)

if(WIN32)
    target_link_libraries(MusicMapMaker-Updater PRIVATE ws2_32)
endif()
