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
add_strip_command_for_release(MusicMapMaker-Next)

target_link_libraries(MusicMapMaker-Next
	PRIVATE
		Log
		Config
		Audio
		Game
)
