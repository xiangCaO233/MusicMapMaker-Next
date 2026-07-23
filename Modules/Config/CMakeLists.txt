# Modules/Config/CMakeLists.txt

add_library(
  Config STATIC
  src/skin/SkinLoader.cpp src/skin/translation/Translation.cpp src/AppPaths.cpp
  src/AppConfig.cpp src/FontPreferenceValidator.cpp
  src/NoteColorPaletteFile.cpp)

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

# 皮肤主题绑定测试覆盖旧字符串格式、亮暗双分支以及旧应用配置的 Auto 语义。
mmm_add_test_executable(Config SkinThemeBindingTest
                        tests/SkinThemeBindingTest.cpp)
target_link_libraries(SkinThemeBindingTest PRIVATE Config)
add_test(NAME SkinThemeBindingTest
         COMMAND SkinThemeBindingTest
                 "${CMAKE_BINARY_DIR}/test_output/skin_theme_binding")

# 物件配色方案文件测试覆盖独立文件格式、写入和同路径覆盖。
mmm_add_test_executable(Config NoteColorPaletteFileTest
                        tests/NoteColorPaletteFileTest.cpp)
target_link_libraries(NoteColorPaletteFileTest PRIVATE Config)
add_test(NAME NoteColorPaletteFileTest
         COMMAND NoteColorPaletteFileTest
                 "${CMAKE_BINARY_DIR}/test_output/note_color_palette_file")
