# Modules/Config/CMakeLists.txt

add_library(Config STATIC src/skin/SkinLoader.cpp
                          src/skin/translation/Translation.cpp)

target_include_directories(Config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(
  Config
  PUBLIC luajit::luajit 3rd_nlohmann_json 3rd_sol2 Log)
