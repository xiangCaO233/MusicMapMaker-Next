# Modules/Main/CMakeLists.txt

if(WIN32)
	# Windows 可执行文件编译配置
	# 编译MusicMapMaker-Next.exe
	set(ICON_RESOURCE src/icon.rc)
	if(CMAKE_BUILD_TYPE STREQUAL Debug)
		add_executable(MusicMapMaker-Next src/main.cpp ${ICON_RESOURCE})
	else()
		# Windows 非Debug模式下添加WIN32标识不显示控制台
		add_executable(MusicMapMaker-Next WIN32 src/main.cpp ${ICON_RESOURCE})
	endif()
	if(MSVC)
		set_target_properties(MusicMapMaker-Next
			PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup"
		)
	endif()
else()
	# Linux/Macos 可执行文件编译配置
	# 编译MusicMapMaker-Next可执行文件
	add_executable(MusicMapMaker-Next src/main.cpp)
endif()
add_gcc_debug_extract(MusicMapMaker-Next)
add_strip_command_for_release(MusicMapMaker-Next)

# 构建完成后分析 Clang -ftime-trace 并打印编译耗时 Top 10
find_package(Python3 QUIET)
if(Python3_EXECUTABLE)
    add_custom_command(
        TARGET MusicMapMaker-Next
        POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/analyze_build_times.py" "${CMAKE_BINARY_DIR}"
        COMMENT "Analyzing compile times (Top 10 slowest TUs)..."
    )
endif()

target_sources(MusicMapMaker-Next PRIVATE src/PGOProfiler.cpp)
target_include_directories(MusicMapMaker-Next PRIVATE include)

if(MMM_PGO_INSTRUMENT)
    # PGO 插桩版本需要 curl 上传 profile
    target_link_libraries(MusicMapMaker-Next PRIVATE Network)
endif()

target_link_libraries(MusicMapMaker-Next
	PRIVATE
		Log
		Config
		Audio
		Game
)
