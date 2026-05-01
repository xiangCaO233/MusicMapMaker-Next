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
    find_package(FFTW3 CONFIG REQUIRED)
    target_link_libraries(Audio PUBLIC FFTW3::fftw3)
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
