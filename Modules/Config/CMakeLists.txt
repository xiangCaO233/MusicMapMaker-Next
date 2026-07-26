# Modules/Config/CMakeLists.txt

add_library(
  Config STATIC
  src/skin/SkinLoader.cpp
  src/skin/translation/Translation.cpp
  src/AppPaths.cpp
  src/AppConfig.cpp
  src/FontPreferenceValidator.cpp
  src/ColorPaletteFile.cpp
  src/VisualConfig.cpp)

target_include_directories(Config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Config PUBLIC luajit::luajit 3rd_nlohmann_json 3rd_sol2
                                    Log)

mmm_add_test_executable(Config FontPreferenceValidatorTest
                        tests/FontPreferenceValidatorTest.cpp)
target_link_libraries(FontPreferenceValidatorTest PRIVATE Config)
add_test(NAME FontPreferenceValidatorTest COMMAND FontPreferenceValidatorTest)

# 帧率限制换算测试确保渲染与逻辑循环使用一致的刷新率目标。
mmm_add_test_executable(Config FrameLimitUtilsTest
                        tests/FrameLimitUtilsTest.cpp)
target_link_libraries(FrameLimitUtilsTest PRIVATE Config)
add_test(NAME FrameLimitUtilsTest COMMAND FrameLimitUtilsTest)

# 皮肤主题绑定测试覆盖旧格式、Auto 语义及 IVM 内置皮肤资源约束。
mmm_add_test_executable(Config SkinThemeBindingTest
                        tests/SkinThemeBindingTest.cpp)
target_link_libraries(SkinThemeBindingTest PRIVATE Config)
add_test(
  NAME SkinThemeBindingTest
  COMMAND
    SkinThemeBindingTest "${CMAKE_BINARY_DIR}/test_output/skin_theme_binding"
    "${CMAKE_SOURCE_DIR}/assets/skins/ivm/skin.lua")

# 调色方案文件测试覆盖独立文件格式、写入和同路径覆盖。
mmm_add_test_executable(Config ColorPaletteFileTest
                        tests/ColorPaletteFileTest.cpp)
target_link_libraries(ColorPaletteFileTest PRIVATE Config)
add_test(
  NAME ColorPaletteFileTest
  COMMAND
    ColorPaletteFileTest "${CMAKE_BINARY_DIR}/test_output/color_palette_file"
    "${CMAKE_SOURCE_DIR}/docs/plugin/examples/ivm.mmpalette")

# 视觉配置测试覆盖分拍线三态模式、旧布尔开关迁移和自动范围约束。
mmm_add_test_executable(Config VisualConfigTest tests/VisualConfigTest.cpp)
target_link_libraries(VisualConfigTest PRIVATE Config Log)
add_test(NAME VisualConfigTest COMMAND VisualConfigTest)

# 音效配置测试覆盖立体打击音效字段往返和旧版 Flick 开关迁移。
mmm_add_test_executable(Config SfxConfigTest tests/SfxConfigTest.cpp)
target_link_libraries(SfxConfigTest PRIVATE Config Log)
add_test(NAME SfxConfigTest COMMAND SfxConfigTest)
