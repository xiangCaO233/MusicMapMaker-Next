# 运行期基础设施模块。

add_library(Runtime STATIC src/AppThreadPool.cpp)

target_include_directories(Runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
  Runtime
  PUBLIC ICE::IonCachyEngine
  PRIVATE Log)
