# Modules/Main/CMakeLists.txt

if(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL Debug)
        add_executable(MusicMapMaker-Next src/main.cpp)
    else()
        add_executable(MusicMapMaker-Next WIN32 src/main.cpp)
    endif()
else() 
    add_executable(MusicMapMaker-Next src/main.cpp)
endif()

target_link_libraries(MusicMapMaker-Next PRIVATE Log Config Audio Game)
