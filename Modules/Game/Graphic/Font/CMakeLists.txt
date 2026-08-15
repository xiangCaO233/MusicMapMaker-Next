# Modules/Game/Graphic/Font/CMakeLists.txt

add_library(Font STATIC src/FontPool.cpp src/SystemFontResolver.cpp
                        src/AsciiFontRasterizer.cpp)

target_include_directories(Font PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
  Font
  PUBLIC Common
  PRIVATE Log freetype::freetype)

# 独立验证软件 ASCII 字体能被栅格化为画布图集数据。
mmm_add_test_executable(Graphic AsciiFontRasterizerTest
                        tests/AsciiFontRasterizerTest.cpp)
target_link_libraries(AsciiFontRasterizerTest PRIVATE Font Config)
add_test(
  NAME AsciiFontRasterizerTest
  COMMAND
    AsciiFontRasterizerTest
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/font/0xProtoNerdFontPropo-Regular.ttf"
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/font/NotoSansMonoCJKsc-Regular.otf"
    "${CMAKE_BINARY_DIR}/test_output")

# 启动期系统字体解析仅链接当前平台的原生字体查询依赖。
if(WIN32)
  target_link_libraries(Font PRIVATE dwrite user32)
elseif(APPLE)
  find_library(MMM_CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(MMM_CORE_TEXT_FRAMEWORK CoreText REQUIRED)
  target_link_libraries(Font PRIVATE "${MMM_CORE_FOUNDATION_FRAMEWORK}"
                                     "${MMM_CORE_TEXT_FRAMEWORK}")
elseif(UNIX)
  find_package(Fontconfig REQUIRED)
  target_link_libraries(Font PRIVATE Fontconfig::Fontconfig)
endif()
