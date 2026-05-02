# Modules/Audio/CMakeLists.txt

add_library(Audio STATIC
	src/AudioManager.cpp
	src/SoundEffectPool.cpp
)

target_link_libraries(Audio
	PUBLIC
		ICE::IonCachyEngine
		Log
		Event
		3rd_rubberband
		MMM
		Config
)

if(MSVC)
	set(FFTW3_DIR "${VCPKG_ROOT}/installed/x64-windows-static/share/fftw3")
	find_package(FFTW3 CONFIG REQUIRED)
	target_link_libraries(Audio PUBLIC fftw3)
	target_include_directories(Audio PUBLIC ${FFTW3_INCLUDE_DIRS})
else()
	target_link_libraries(Audio PUBLIC fftw3)
endif()

if(WIN32)
	target_link_libraries(Audio
		PUBLIC
			samplerate
			SDL3-static
	)
endif()

target_include_directories(Audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
