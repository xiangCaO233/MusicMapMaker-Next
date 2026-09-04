add_library(
  MMM STATIC
  src/beatmap/BeatMap.cpp src/beatmap/BeatmapSpeedTransform.cpp
  src/note/Note.cpp src/note/Hold.cpp src/project/AudioResource.cpp
  src/timing/Timing.cpp)

target_include_directories(MMM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(MMM PUBLIC Log Event 3rd_nlohmann_json)

# Tests
set(TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test_output")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

# BPM 规范化测试覆盖有限越界值、无穷值和 NaN 回退语义。
mmm_add_test_executable(MMM BpmNormalizationTest tests/BpmNormalizationTest.cpp)
target_link_libraries(BpmNormalizationTest PRIVATE MMM Log)
add_test(NAME Test_BPM_Normalization COMMAND BpmNormalizationTest)

# 1. IMD Test
mmm_add_test_executable(MMM IMDConsistencyTest tests/IMDConsistencyTest.cpp)
target_link_libraries(IMDConsistencyTest PRIVATE MMM Log)
target_include_directories(IMDConsistencyTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(
  NAME Test_IMD_Consistency
  COMMAND
    IMDConsistencyTest "${TEST_DATA_DIR}/[GeneralPolyline]Redemptione.imd"
    "${TEST_OUTPUT_DIR}/Redemptione_export.imd")

# 1. OSU Test
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

# 1. Malody Test
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

# 1. Malody Edge Case Test (程序化构造边缘情况)
mmm_add_test_executable(MMM MalodyEdgeCaseTest tests/MalodyEdgeCaseTest.cpp)
target_link_libraries(MalodyEdgeCaseTest PRIVATE MMM Log)
target_include_directories(MalodyEdgeCaseTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_Malody_Edge_Case COMMAND MalodyEdgeCaseTest)

# 跨格式物件绑定音效测试覆盖 osu! sampleFile、Malody sound 与原生 MMM 往返。
mmm_add_test_executable(MMM BoundNoteSoundTest tests/BoundNoteSoundTest.cpp)
target_link_libraries(BoundNoteSoundTest PRIVATE MMM Log)
add_test(NAME Test_Bound_Note_Sound
         COMMAND BoundNoteSoundTest "${TEST_OUTPUT_DIR}/bound_note_sound")

# 1. MMM Native Format Test
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

# 背景元数据兼容测试使用程序化谱面，覆盖 osu! 视频事件与 MMM 新旧格式往返。
mmm_add_test_executable(MMM MetadataCompatibilityTest
                        tests/MetadataCompatibilityTest.cpp)
target_link_libraries(MetadataCompatibilityTest PRIVATE MMM Config Log)
add_test(NAME Test_Metadata_Compatibility
         COMMAND MetadataCompatibilityTest
                 "${TEST_OUTPUT_DIR}/metadata_compatibility")

mmm_add_test_executable(MMM BeatmapSpeedTransformTest
                        tests/BeatmapSpeedTransformTest.cpp)
target_link_libraries(BeatmapSpeedTransformTest PRIVATE MMM Log)
add_test(NAME Test_Beatmap_Speed_Transform
         COMMAND BeatmapSpeedTransformTest "${MMM_TEST_RESOURCE_DIR}/ma"
                 "${TEST_OUTPUT_DIR}/speed_transform_coverage")

# 打包文件类型测试覆盖各目标格式的资源候选与写入扩展名规则。
mmm_add_test_executable(MMM PackageFileTypesTest tests/PackageFileTypesTest.cpp)
target_link_libraries(PackageFileTypesTest PRIVATE MMM Log)
add_test(NAME Test_Package_File_Types COMMAND PackageFileTypesTest)
