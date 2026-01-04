# Modules/Config/CMakeLists.txt

add_library(Config SHARED src/SkinLoader.cpp)

target_include_directories(Config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Config
    PUBLIC
    luajit::luajit
    PRIVATE
    sol2::sol2 Log
)
