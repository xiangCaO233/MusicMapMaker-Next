# Modules/Config/CMakeLists.txt Config 集中维护无需图形上下文即可加载的持久化配置、皮肤和翻译服务。
# 源文件显式列出，新增配置实现时必须同时确认其序列化与测试覆盖归属。 本文件同时集中注册模块测试，便于审查每类配置的兼容性覆盖。

# 模块保持静态库边界，供主程序和轻量测试复用同一套配置实现。
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
# 所有实现保持在同一目标内，防止测试与主程序链接出不同配置行为。

# 公开 include 根只暴露稳定配置接口，src 下的加载细节不得成为传递依赖。
target_include_directories(Config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# LuaJIT、sol2 和 JSON 属于公开头契约，消费方需继承其包含与链接要求。
target_link_libraries(Config PUBLIC luajit::luajit 3rd_nlohmann_json 3rd_sol2
                                    fmt::fmt Log)
# miniz 只在皮肤包实现文件中使用，不向其他模块传播归档库依赖。
target_link_libraries(Config PRIVATE 3rd_miniz)

# 入口开关默认服务本机开发，自动化构建必须通过命令行显式关闭。
if(MMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN)
  # 与 AppPaths::baseConfigPath 保持同一优先级，确保构建期和运行期落到相同用户目录。
  # 空值表示尚未找到可安全写入的用户配置基础目录。
  set(_MMM_LOCAL_CONFIG_BASE "")
  if(DEFINED ENV{MMM_CONFIG_ROOT} AND NOT "$ENV{MMM_CONFIG_ROOT}" STREQUAL "")
    # 多客户端或测试环境可显式隔离配置根。 覆盖值本身已是基础根，后续仍统一追加应用目录名。
    set(_MMM_LOCAL_CONFIG_BASE "$ENV{MMM_CONFIG_ROOT}")
  elseif(
    NOT WIN32
    AND DEFINED ENV{XDG_CONFIG_HOME}
    AND NOT "$ENV{XDG_CONFIG_HOME}" STREQUAL "")
    # POSIX 平台优先遵循 XDG 配置根。 XDG_CONFIG_HOME 已包含 .config 语义，禁止再次追加该层。
    set(_MMM_LOCAL_CONFIG_BASE "$ENV{XDG_CONFIG_HOME}")
  elseif(
    WIN32
    AND DEFINED ENV{USERPROFILE}
    AND NOT "$ENV{USERPROFILE}" STREQUAL "")
    # Windows 运行时固定使用用户目录下的 .config。 该布局与跨平台现有用户数据保持一致，不切换到 APPDATA。
    set(_MMM_LOCAL_CONFIG_BASE "$ENV{USERPROFILE}/.config")
  elseif(
    WIN32
    AND DEFINED ENV{HOMEDRIVE}
    AND DEFINED ENV{HOMEPATH}
    AND NOT "$ENV{HOMEDRIVE}$ENV{HOMEPATH}" STREQUAL "")
    # USERPROFILE 缺失时与 AppPaths 一样回退到 HOMEDRIVE/HOMEPATH。
    set(_MMM_LOCAL_CONFIG_BASE "$ENV{HOMEDRIVE}$ENV{HOMEPATH}/.config")
  elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    # 其他平台使用 HOME/.config 作为最终常规路径。 HOME 仅在专用覆盖和 XDG 均缺失后参与选择。
    set(_MMM_LOCAL_CONFIG_BASE "$ENV{HOME}/.config")
  endif()

  # 仅在完整解析出基础根后注册会写文件的构建目标。
  if(_MMM_LOCAL_CONFIG_BASE)
    # 应用目录名与 AppPaths::configRootPath 的 mmm 后缀保持一致。
    set(_MMM_LOCAL_CONFIG_ROOT "${_MMM_LOCAL_CONFIG_BASE}/mmm")
    # ALL 保证普通本地构建会同步；Main 目标另行依赖以覆盖显式单目标构建。
    add_custom_target(
      SyncDefaultConfigAssets ALL
      COMMAND
        "${CMAKE_COMMAND}"
        # 源根固定指向版本库资源，禁止从已安装目录反向覆盖。
        "-DMMM_SYNC_SOURCE_ASSETS_ROOT=${CMAKE_SOURCE_DIR}/assets"
        # 目标根在配置期冻结，避免构建子进程环境变化导致写错用户。
        "-DMMM_SYNC_DESTINATION_CONFIG_ROOT=${_MMM_LOCAL_CONFIG_ROOT}" -P
        "${CMAKE_SOURCE_DIR}/cmake/SyncDefaultConfigAssets.cmake"
      COMMENT "同步默认翻译与内置皮肤到本机配置目录"
      VERBATIM)
    # IDE 中归入 Config 分组，避免辅助目标混入主程序列表。
    set_property(TARGET SyncDefaultConfigAssets PROPERTY FOLDER "Config")
  else()
    # 无法可靠确定用户目录时跳过写入，禁止退化为污染源码树或构建目录。
    message(WARNING "无法确定本机配置目录，跳过默认翻译与皮肤同步。")
  endif()
endif()

mmm_add_test_executable(Config FontPreferenceValidatorTest
                        tests/FontPreferenceValidatorTest.cpp)
# mmm_add_test_executable 统一测试输出目录和基础编译选项，禁止手写可执行文件布局。 字体偏好验证只依赖公开 Config
# 接口，避免引入 UI 或渲染运行时。
target_link_libraries(FontPreferenceValidatorTest PRIVATE Config)
# 测试不读写个人配置，可直接注册目标程序作为 CTest 命令。
add_test(NAME FontPreferenceValidatorTest COMMAND FontPreferenceValidatorTest)

# Creator 身份测试覆盖持久化、规范化和联机前置校验共同边界。
mmm_add_test_executable(Config CreatorIdentityTest
                        tests/CreatorIdentityTest.cpp)
# 身份逻辑独立成测试目标，避免通过 AppConfig 集成测试掩盖规范化边界。
target_link_libraries(CreatorIdentityTest PRIVATE Config)
# 身份测试使用进程内临时值，不需要额外环境包装器。
add_test(NAME CreatorIdentityTest COMMAND CreatorIdentityTest)

# 本机多客户端测试必须把每个进程的配置和 ImGui 状态隔离到独立目录。
mmm_add_test_executable(Config AppPathsProfileOverrideTest
                        tests/AppPathsProfileOverrideTest.cpp)
# 同一目标注册三种启动方式，用来覆盖显式覆盖、helper 隔离和直接执行。
target_link_libraries(AppPathsProfileOverrideTest PRIVATE Config)
# 显式覆盖场景同时传入期望根目录，验证环境解析和返回路径一致。
add_test(
  NAME AppPathsProfileOverrideTest
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "MMM_CONFIG_ROOT=${CMAKE_BINARY_DIR}/test_output/collaboration_profile"
    "$<TARGET_FILE:AppPathsProfileOverrideTest>"
    "${CMAKE_BINARY_DIR}/test_output/collaboration_profile")

# 同一只读探针分别验证 CTest 启动器与直接执行二进制时的默认配置隔离。
add_test(
  NAME TestConfigIsolationTest
  # 目标启动器由公共测试 helper 注入隔离根，此处只给出预期目录。
  COMMAND
    AppPathsProfileOverrideTest
    "${CMAKE_BINARY_DIR}/test_output/config/Config/AppPathsProfileOverrideTest")
# 绕过目标启动器并清空显式覆盖，确保调试器或手动执行测试也不会使用个人配置。
add_test(
  NAME TestConfigIsolationDirectTest
  # 直接二进制路径绕过 helper，验证程序自身仍采用测试构建隔离约定。
  COMMAND
    "${CMAKE_COMMAND}" -E env --unset=MMM_CONFIG_ROOT
    "$<TARGET_FILE:AppPathsProfileOverrideTest>"
    "${CMAKE_BINARY_DIR}/test_output/config/Config/AppPathsProfileOverrideTest")

# 帧率限制换算测试确保渲染与逻辑循环使用一致的刷新率目标。
mmm_add_test_executable(Config FrameLimitUtilsTest
                        tests/FrameLimitUtilsTest.cpp)
# 帧率工具是头文件接口，独立链接 Config 可同时验证公开 include 完整性。
target_link_libraries(FrameLimitUtilsTest PRIVATE Config)
# 纯换算测试不需要资源或输出目录。
add_test(NAME FrameLimitUtilsTest COMMAND FrameLimitUtilsTest)

# 皮肤主题绑定测试覆盖旧格式、Auto 语义及 IVM 内置皮肤资源约束。
mmm_add_test_executable(Config SkinThemeBindingTest
                        tests/SkinThemeBindingTest.cpp)
# 该目标读取版本库资源但只把临时产物写入构建树。
target_link_libraries(SkinThemeBindingTest PRIVATE Config)
# 依次传入输出根、IVM 皮肤、默认翻译和默认皮肤，避免测试自行猜测源码位置。
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
# 归档安全测试使用真实文件系统边界，因此必须获得独占输出子目录。
target_link_libraries(SkinPackageServiceTest PRIVATE Config 3rd_miniz)
# 测试需要 miniz 构造恶意归档夹具，生产服务仍通过 Config 调用。
add_test(NAME SkinPackageServiceTest
         COMMAND SkinPackageServiceTest
                 "${CMAKE_BINARY_DIR}/test_output/skin_package_service")

# 调色方案文件测试覆盖独立文件格式、写入和同路径覆盖。
mmm_add_test_executable(Config ColorPaletteFileTest
                        tests/ColorPaletteFileTest.cpp)
# 示例文件验证对外格式兼容，不能复制到源码目录后原地修改。
target_link_libraries(ColorPaletteFileTest PRIVATE Config)
# 示例调色板作为只读输入，所有导出产物写入 test_output。
add_test(
  NAME ColorPaletteFileTest
  COMMAND
    ColorPaletteFileTest "${CMAKE_BINARY_DIR}/test_output/color_palette_file"
    "${CMAKE_SOURCE_DIR}/docs/plugin/examples/ivm.mmpalette")

# 视觉配置测试覆盖分拍线三态模式、旧布尔开关迁移和自动范围约束。
mmm_add_test_executable(Config VisualConfigTest tests/VisualConfigTest.cpp)
# Log 由序列化失败分支使用，保持为测试目标的显式依赖。
target_link_libraries(VisualConfigTest PRIVATE Config Log)
# 视觉配置测试使用内存 JSON，不触碰用户配置文件。
add_test(NAME VisualConfigTest COMMAND VisualConfigTest)

# 音效配置测试覆盖立体打击音效字段往返和旧版 Flick 开关迁移。
mmm_add_test_executable(Config SfxConfigTest tests/SfxConfigTest.cpp)
target_link_libraries(SfxConfigTest PRIVATE Config Log)
# 音效字段测试同样只验证序列化值语义。
add_test(NAME SfxConfigTest COMMAND SfxConfigTest)

# 协作服务器设置测试覆盖端点持久化、旧配置默认值和非法字段回退。
mmm_add_test_executable(Config CollaborationServerSettingsTest
                        tests/CollaborationServerSettingsTest.cpp)
target_link_libraries(CollaborationServerSettingsTest PRIVATE Config Log)
# 协作服务器测试不启动网络，只校验配置兼容和边界回退。
add_test(NAME CollaborationServerSettingsTest
         COMMAND CollaborationServerSettingsTest)

# 自动保存配置测试覆盖定时单位、范围约束、事件开关和旧配置默认值。
mmm_add_test_executable(Config AutoSaveConfigTest tests/AutoSaveConfigTest.cpp)
target_link_libraries(AutoSaveConfigTest PRIVATE Config Log)
# 自动保存测试不创建真实项目，避免引入项目存储副作用。
add_test(NAME AutoSaveConfigTest COMMAND AutoSaveConfigTest)

# 工具栏可见性测试覆盖分组字段往返、旧配置默认值和部分配置兼容性。
mmm_add_test_executable(Config ToolbarVisibilityConfigTest
                        tests/ToolbarVisibilityConfigTest.cpp)
target_link_libraries(ToolbarVisibilityConfigTest PRIVATE Config Log)
# 工具栏测试仅覆盖嵌套 JSON，不依赖 ImGui 上下文。
add_test(NAME ToolbarVisibilityConfigTest COMMAND ToolbarVisibilityConfigTest)

# 翻译并发测试覆盖 UI 与逻辑线程同时查找、填充和清空缓存。
mmm_add_test_executable(Config TranslationConcurrencyTest
                        tests/TranslationConcurrencyTest.cpp)
target_link_libraries(TranslationConcurrencyTest PRIVATE Config Log)
# 并发用例自行管理线程和临时语言文件，CTest 只负责进程级隔离。
add_test(NAME TranslationConcurrencyTest COMMAND TranslationConcurrencyTest)

# 皮肤翻译覆写测试覆盖默认字典合并、未知字段及皮肤包路径边界。
mmm_add_test_executable(Config SkinTranslationOverrideTest
                        tests/SkinTranslationOverrideTest.cpp)
target_link_libraries(SkinTranslationOverrideTest PRIVATE Config Log)
# 覆写测试在专用输出根创建最小皮肤，禁止写入同步后的用户 assets。
add_test(NAME SkinTranslationOverrideTest
         COMMAND SkinTranslationOverrideTest
                 "${CMAKE_BINARY_DIR}/test_output/skin_translation_override")

# 翻译资源迁移测试覆盖旧版固定路径文件的无条件清理与一次性标记。
mmm_add_test_executable(Config TranslationResourceMigrationTest
                        tests/TranslationResourceMigrationTest.cpp)
target_link_libraries(TranslationResourceMigrationTest PRIVATE Config Log)
# 迁移用例显式获得 test_output 根，只删除其内部构造的旧版文件。
add_test(
  NAME TranslationResourceMigrationTest
  COMMAND TranslationResourceMigrationTest
          "${CMAKE_BINARY_DIR}/test_output/translation_resource_migration")

# 构建期资源同步测试覆盖两套内置皮肤、默认文件增量覆盖、用户扩展以及配置和 ImGui 布局保留。 测试直接运行生产 cmake -P
# 脚本，不依赖默认开启的真实用户目录目标。
add_test(
  NAME DefaultConfigAssetSyncTest
  # 该脚本测试不依赖 Config 二进制，可直接复用生产同步脚本完成黑盒验证。
  COMMAND
    "${CMAKE_COMMAND}"
    "-DMMM_TEST_SYNC_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/SyncDefaultConfigAssets.cmake"
    # 所有测试写入统一 test_output，禁止触碰源码 assets。
    "-DMMM_TEST_OUTPUT_ROOT=${CMAKE_BINARY_DIR}/test_output" -P
    "${CMAKE_SOURCE_DIR}/cmake/tests/DefaultConfigAssetSyncTest.cmake")

# 默认中英文翻译键必须一一对应，避免界面直接回显未翻译的键名。
mmm_add_test_executable(Config DefaultTranslationParityTest
                        tests/DefaultTranslationParityTest.cpp)
# 键集合一致性在资源更新时提供快速失败，避免运行期才发现缺失译文。
target_link_libraries(DefaultTranslationParityTest PRIVATE Config Log)
# 两个默认语言文件均作为只读参数传入，不依赖构建期用户资源同步。
add_test(
  NAME DefaultTranslationParityTest
  COMMAND
    DefaultTranslationParityTest
    "${CMAKE_SOURCE_DIR}/assets/translations/zh_cn.lua"
    "${CMAKE_SOURCE_DIR}/assets/translations/en_us.lua")
