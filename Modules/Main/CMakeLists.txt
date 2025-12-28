# Modules/Main/CMakeLists.txt

add_executable(MusicMapMaker-Next src/main.cpp)

target_link_libraries(MusicMapMaker-Next PRIVATE Audio Canvas)
