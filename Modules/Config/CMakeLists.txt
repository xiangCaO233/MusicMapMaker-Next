# Modules/Config/CMakeLists.txt

add_library(
  Config STATIC
  src/skin/SkinLoader.cpp
  src/skin/SkinPackageService.cpp
  src/skin/TranslationResourceMigration.cpp
  src/skin/translation/Translation.cpp
  src/AppPaths.cpp
  src/AppConfig.cpp
  src/CreatorIdentity.cpp
  src/EditorSettingTypes.cpp
  src/FontPreferenceValidator.cpp
  src/ColorPaletteFile.cpp
  src/VisualConfig.cpp)

target_include_directories(Config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Config PUBLIC luajit::luajit 3rd_nlohmann_json 3rd_sol2
                                    fmt::fmt Log)
target_link_libraries(Config PRIVATE 3rd_miniz)

mmm_add_test_executable(Config FontPreferenceValidatorTest
                        tests/FontPreferenceValidatorTest.cpp)
target_link_libraries(FontPreferenceValidatorTest PRIVATE Config)
add_test(NAME FontPreferenceValidatorTest COMMAND FontPreferenceValidatorTest)

# Creator 身份测试覆盖持久化、规范化和联机前置校验共同边界。
mmm_add_test_executable(Config CreatorIdentityTest
                        tests/CreatorIdentityTest.cpp)
target_link_libraries(CreatorIdentityTest PRIVATE Config)
add_test(NAME CreatorIdentityTest COMMAND CreatorIdentityTest)

# 本机多客户端测试必须把每个进程的配置和 ImGui 状态隔离到独立目录。
mmm_add_test_executable(Config AppPathsProfileOverrideTest
                        tests/AppPathsProfileOverrideTest.cpp)
target_link_libraries(AppPathsProfileOverrideTest PRIVATE Config)
add_test(
  NAME AppPathsProfileOverrideTest
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "MMM_CONFIG_ROOT=${CMAKE_BINARY_DIR}/test_output/collaboration_profile"
    "$<TARGET_FILE:AppPathsProfileOverrideTest>"
    "${CMAKE_BINARY_DIR}/test_output/collaboration_profile")

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
    "${CMAKE_SOURCE_DIR}/assets/skins/ivm/skin.lua"
    "${CMAKE_SOURCE_DIR}/assets/translations"
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/skin.lua")

# MSK 皮肤包测试覆盖安全导入、目录定位、覆盖保护和导出往返。
mmm_add_test_executable(Config SkinPackageServiceTest
                        tests/SkinPackageServiceTest.cpp)
target_link_libraries(SkinPackageServiceTest PRIVATE Config 3rd_miniz)
add_test(NAME SkinPackageServiceTest
         COMMAND SkinPackageServiceTest
                 "${CMAKE_BINARY_DIR}/test_output/skin_package_service")

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

# 翻译并发测试覆盖 UI 与逻辑线程同时查找、填充和清空缓存。
mmm_add_test_executable(Config TranslationConcurrencyTest
                        tests/TranslationConcurrencyTest.cpp)
target_link_libraries(TranslationConcurrencyTest PRIVATE Config Log)
add_test(NAME TranslationConcurrencyTest COMMAND TranslationConcurrencyTest)

# 皮肤翻译覆写测试覆盖默认字典合并、未知字段及皮肤包路径边界。
mmm_add_test_executable(Config SkinTranslationOverrideTest
                        tests/SkinTranslationOverrideTest.cpp)
target_link_libraries(SkinTranslationOverrideTest PRIVATE Config Log)
add_test(NAME SkinTranslationOverrideTest
         COMMAND SkinTranslationOverrideTest
                 "${CMAKE_BINARY_DIR}/test_output/skin_translation_override")

# 翻译资源迁移测试覆盖旧版固定路径文件的无条件清理与一次性标记。
mmm_add_test_executable(Config TranslationResourceMigrationTest
                        tests/TranslationResourceMigrationTest.cpp)
target_link_libraries(TranslationResourceMigrationTest PRIVATE Config Log)
add_test(
  NAME TranslationResourceMigrationTest
  COMMAND TranslationResourceMigrationTest
          "${CMAKE_BINARY_DIR}/test_output/translation_resource_migration")
