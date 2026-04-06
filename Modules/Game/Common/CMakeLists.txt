# Modules/Game/Common/CMakeLists.txt

add_library(Common INTERFACE)

target_include_directories(Common INTERFACE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/3rdpty/sources/entt/src
    ${PROJECT_SOURCE_DIR}/Modules/Config/include
)
