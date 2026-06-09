add_library(Network STATIC
    src/AssetSyncService.cpp
    src/UpdateChecker.cpp
)

target_include_directories(Network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Network
    PUBLIC
        3rd_curl
        3rd_miniz
        3rd_nlohmann_json
        Config
        Log
        Event
)

if(WIN32)
    target_link_libraries(Network PUBLIC shell32)
endif()

# Unit test
add_executable(UpdateCheckerTest tests/UpdateCheckerTest.cpp)
target_link_libraries(UpdateCheckerTest PRIVATE Network Log)
target_include_directories(UpdateCheckerTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(AssetSyncServiceTest tests/AssetSyncServiceTest.cpp)
target_link_libraries(AssetSyncServiceTest PRIVATE Network Log 3rd_miniz)
target_include_directories(AssetSyncServiceTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()
add_test(NAME UpdateCheckerTest COMMAND UpdateCheckerTest)
add_test(NAME AssetSyncServiceTest COMMAND AssetSyncServiceTest)
