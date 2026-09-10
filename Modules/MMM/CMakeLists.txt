# 模块构建边界： - MMM 只包含项目自维护的谱面领域模型。 - 格式读写头由 BeatMap 的实现入口统一分派。 - 公共头目录不发布 src
# 下的格式细节。 - JSON target 作为模型序列化的公共依赖。 - Log 与 Event 保持现有传递依赖关系。
#
# 测试资源边界： - tests/data 只保存只读基准夹具。 - 所有生成文件统一写入 test_output。 - 程序化场景不额外创建静态资源夹具。
# - 格式内部测试可单独包含模块 src 目录。 - 业务模块仍只能使用公开 BeatMap 分派入口。
#
# 测试职责分组： - BpmNormalizationTest 固化 BPM 边界语义。 - IMDConsistencyTest 验证二进制区块与模型。 -
# OSUConsistencyTest 覆盖三个代表性 osu! 场景。 - MalodyConsistencyTest 覆盖普通与折线谱面。 -
# MalodyEdgeCaseTest 覆盖程序化边界组合。 - BoundNoteSoundTest 区分两类音频采样职责。 -
# MapConsistencyTest 验证原生 MMM 承接能力。 - MetadataCompatibilityTest 覆盖新旧元数据。 -
# BeatmapSpeedTransformTest 遍历共享格式资源。 - PackageFileTypesTest 校验打包扩展名规则。
#
# 每个 add_test 使用独立稳定名称，便于按场景筛选。 有输出参数的测试必须显式传入构建树路径。 新增测试继续使用
# mmm_add_test_executable 注册。

# 格式区块比较负责检查磁盘表示。

# 统一模型比较负责检查公共业务语义。

# 程序化探针用于覆盖罕见字段组合。

# 真实资源夹具用于覆盖生产文件解析。

# 每层失败都必须产生非零退出状态。

# 测试日志应明确标识失败所属层级。

# 纯规则测试不声明无用的资源路径。

# 内部格式头只向对应测试目标开放。

# 测试目标复用生产读写器而非测试替身。

# 新增输出文件时必须避免测试间名称冲突。

# MMM 静态库只编译具有独立实现单元的核心模型；各格式读写器以内部头文件形式 由 BeatMap 分派入口包含，避免把格式细节暴露为业务模块的公共链接目标。
add_library(
  MMM STATIC
  src/beatmap/BeatMap.cpp src/beatmap/BeatmapSpeedTransform.cpp
  src/note/Note.cpp src/note/Hold.cpp src/project/AudioResource.cpp
  src/timing/Timing.cpp)

# 公共 include 目录只发布稳定模型接口，日志、事件和 JSON 则作为传递依赖供 这些接口中的元数据与序列化声明使用。
target_include_directories(MMM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(MMM PUBLIC Log Event 3rd_nlohmann_json)

# 所有测试夹具都从源码目录只读加载，生成文件统一进入构建目录，禁止覆盖基准。
set(TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test_output")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

# BPM 规范化测试覆盖有限越界值、无穷值和 NaN 回退语义。
mmm_add_test_executable(MMM BpmNormalizationTest tests/BpmNormalizationTest.cpp)
target_link_libraries(BpmNormalizationTest PRIVATE MMM Log)
add_test(NAME Test_BPM_Normalization COMMAND BpmNormalizationTest)

# IMD 测试同时覆盖二进制区块、统一模型和同名前缀资源发现。
mmm_add_test_executable(MMM IMDConsistencyTest tests/IMDConsistencyTest.cpp)
target_link_libraries(IMDConsistencyTest PRIVATE MMM Log)
target_include_directories(IMDConsistencyTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(
  NAME Test_IMD_Consistency
  COMMAND
    IMDConsistencyTest "${TEST_DATA_DIR}/[GeneralPolyline]Redemptione.imd"
    "${TEST_OUTPUT_DIR}/Redemptione_export.imd")

# osu! 使用三个代表性夹具覆盖普通谱面、效果 SV 与高密度 BPM/SV 组合。
mmm_add_test_executable(MMM OSUConsistencyTest tests/OSUConsistencyTest.cpp)
target_link_libraries(OSUConsistencyTest PRIVATE MMM Log)
target_include_directories(OSUConsistencyTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_OSU_Consistency_Scandal
         COMMAND OSUConsistencyTest "${TEST_DATA_DIR}/[General]Scandal.osu"
                 "${TEST_OUTPUT_DIR}/Scandal_export.osu")
add_test(
  NAME Test_OSU_Consistency_ICanFly
  COMMAND
    OSUConsistencyTest
    "${TEST_DATA_DIR}/[Effect SV]I Can Fly In The Universe.osu"
    "${TEST_OUTPUT_DIR}/I_Can_Fly_export.osu")
add_test(
  NAME Test_OSU_Consistency_Mopemope
  COMMAND OSUConsistencyTest "${TEST_DATA_DIR}/[BPM SV(Giant)]Mopemope.osu"
          "${TEST_OUTPUT_DIR}/Mopemope_export.osu")

# Malody 使用普通谱面与折线谱面分别覆盖 JSON 分区和节点连续性。
mmm_add_test_executable(MMM MalodyConsistencyTest
                        tests/MalodyConsistencyTest.cpp)
target_link_libraries(MalodyConsistencyTest PRIVATE MMM Log)
target_include_directories(MalodyConsistencyTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(
  NAME Test_Malody_Consistency_Unsan
  COMMAND MalodyConsistencyTest "${TEST_DATA_DIR}/[General]Unsan-musho.mc"
          "${TEST_OUTPUT_DIR}/Unsan-musho_export.mc")
add_test(
  NAME Test_Malody_Consistency_Collision
  COMMAND MalodyConsistencyTest "${TEST_DATA_DIR}/[Polyline]Collision.mc"
          "${TEST_OUTPUT_DIR}/Collision_export.mc")

# Malody 边界测试通过程序化夹具覆盖损坏字段和罕见组合，不引入额外资源文件。
mmm_add_test_executable(MMM MalodyEdgeCaseTest tests/MalodyEdgeCaseTest.cpp)
target_link_libraries(MalodyEdgeCaseTest PRIVATE MMM Log)
target_include_directories(MalodyEdgeCaseTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_Malody_Edge_Case COMMAND MalodyEdgeCaseTest)

# 跨格式绑定音效测试区分玩家命中采样和时间线自动采样，并写入独立输出目录。
mmm_add_test_executable(MMM BoundNoteSoundTest tests/BoundNoteSoundTest.cpp)
target_link_libraries(BoundNoteSoundTest PRIVATE MMM Log)
add_test(NAME Test_Bound_Note_Sound
         COMMAND BoundNoteSoundTest "${TEST_OUTPUT_DIR}/bound_note_sound")

# 原生 MMM 往返测试从两种外部格式导入，验证公共模型能被规范 JSON 完整承接。
mmm_add_test_executable(MMM MapConsistencyTest tests/MapConsistencyTest.cpp)
target_link_libraries(MapConsistencyTest PRIVATE MMM Log)
target_include_directories(MapConsistencyTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_MMM_Consistency_Scandal
         COMMAND MapConsistencyTest "${TEST_DATA_DIR}/[General]Scandal.osu"
                 "${TEST_OUTPUT_DIR}/Scandal_export.mmm")
add_test(NAME Test_MMM_Consistency_Collision
         COMMAND MapConsistencyTest "${TEST_DATA_DIR}/[Polyline]Collision.mc"
                 "${TEST_OUTPUT_DIR}/Collision_export.mmm")

# 元数据兼容测试使用程序化谱面，覆盖 osu! 视频事件与 MMM 新旧格式往返， 因此不需要从 tests/data 增加仅服务字段组合的静态 JSON
# 夹具。
mmm_add_test_executable(MMM MetadataCompatibilityTest
                        tests/MetadataCompatibilityTest.cpp)
target_link_libraries(MetadataCompatibilityTest PRIVATE MMM Config Log)
add_test(NAME Test_Metadata_Compatibility
         COMMAND MetadataCompatibilityTest
                 "${TEST_OUTPUT_DIR}/metadata_compatibility")

mmm_add_test_executable(MMM BeatmapSpeedTransformTest
                        tests/BeatmapSpeedTransformTest.cpp)
# 变速覆盖测试遍历共享资源目录，所有派生谱面与音频路径都写入 test_output。
target_link_libraries(BeatmapSpeedTransformTest PRIVATE MMM Log)
add_test(NAME Test_Beatmap_Speed_Transform
         COMMAND BeatmapSpeedTransformTest "${MMM_TEST_RESOURCE_DIR}/ma"
                 "${TEST_OUTPUT_DIR}/speed_transform_coverage")

# 打包文件类型测试是纯规则测试，不读取或生成资源文件。
mmm_add_test_executable(MMM PackageFileTypesTest tests/PackageFileTypesTest.cpp)
target_link_libraries(PackageFileTypesTest PRIVATE MMM Log)
add_test(NAME Test_Package_File_Types COMMAND PackageFileTypesTest)
