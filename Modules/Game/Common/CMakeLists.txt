# Modules/Game/Common/CMakeLists.txt

add_library(Common STATIC
    src/common/AudioInfoUtils.cpp
)

target_include_directories(Common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/3rdpty/sources/entt/src
    ${PROJECT_SOURCE_DIR}/3rdpty/sources/nlohmann_json/include
    ${PROJECT_SOURCE_DIR}/Modules/Config/include
    ${PROJECT_SOURCE_DIR}/Modules/MMM/include
    ${PROJECT_SOURCE_DIR}/Modules/Log/include
)

target_link_libraries(Common PUBLIC Log)
