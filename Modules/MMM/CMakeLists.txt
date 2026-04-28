add_library(MMM STATIC
    src/beatmap/BeatMap.cpp
    src/note/Note.cpp
    src/note/Hold.cpp
    src/timing/Timing.cpp
)

target_include_directories(MMM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(MMM PUBLIC Log Event 3rd_nlohmann_json)

# Tests
set(TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test_output")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

# 1. IMD Test
add_executable(IMDConsistencyTest tests/IMDConsistencyTest.cpp)
target_link_libraries(IMDConsistencyTest PRIVATE MMM Log)
target_include_directories(IMDConsistencyTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_IMD_Consistency
    COMMAND IMDConsistencyTest
    "${TEST_DATA_DIR}/[GeneralPolyline]Redemptione.imd"
    "${TEST_OUTPUT_DIR}/Redemptione_export.imd"
)

# 2. OSU Test
add_executable(OSUConsistencyTest tests/OSUConsistencyTest.cpp)
target_link_libraries(OSUConsistencyTest PRIVATE MMM Log)
target_include_directories(OSUConsistencyTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_OSU_Consistency_Scandal
    COMMAND OSUConsistencyTest
    "${TEST_DATA_DIR}/[General]Scandal.osu"
    "${TEST_OUTPUT_DIR}/Scandal_export.osu"
)
add_test(NAME Test_OSU_Consistency_ICanFly
    COMMAND OSUConsistencyTest
    "${TEST_DATA_DIR}/[Effect SV]I Can Fly In The Universe.osu"
    "${TEST_OUTPUT_DIR}/I_Can_Fly_export.osu"
)
add_test(NAME Test_OSU_Consistency_Mopemope
    COMMAND OSUConsistencyTest
    "${TEST_DATA_DIR}/[BPM SV(Giant)]Mopemope.osu"
    "${TEST_OUTPUT_DIR}/Mopemope_export.osu"
)

# 3. Malody Test
add_executable(MalodyConsistencyTest tests/MalodyConsistencyTest.cpp)
target_link_libraries(MalodyConsistencyTest PRIVATE MMM Log)
target_include_directories(MalodyConsistencyTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_Malody_Consistency_Unsan
    COMMAND MalodyConsistencyTest
    "${TEST_DATA_DIR}/[General]Unsan-musho.mc"
    "${TEST_OUTPUT_DIR}/Unsan-musho_export.mc"
)
add_test(NAME Test_Malody_Consistency_Collision
    COMMAND MalodyConsistencyTest
    "${TEST_DATA_DIR}/[Polyline]Collision.mc"
    "${TEST_OUTPUT_DIR}/Collision_export.mc"
)

# 4. MMM Native Format Test
add_executable(MapConsistencyTest tests/MapConsistencyTest.cpp)
target_link_libraries(MapConsistencyTest PRIVATE MMM Log)
target_include_directories(MapConsistencyTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME Test_MMM_Consistency_Scandal
    COMMAND MapConsistencyTest
    "${TEST_DATA_DIR}/[General]Scandal.osu"
    "${TEST_OUTPUT_DIR}/Scandal_export.mmm"
)
add_test(NAME Test_MMM_Consistency_Collision
    COMMAND MapConsistencyTest
    "${TEST_DATA_DIR}/[Polyline]Collision.mc"
    "${TEST_OUTPUT_DIR}/Collision_export.mmm"
)
