add_library(Network STATIC
    src/UpdateChecker.cpp
)

target_include_directories(Network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(Network
    PUBLIC
        3rd_curl
        3rd_nlohmann_json
        Log
        Event
)

# Unit test
add_executable(UpdateCheckerTest tests/UpdateCheckerTest.cpp)
target_link_libraries(UpdateCheckerTest PRIVATE Network Log)
target_include_directories(UpdateCheckerTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()
add_test(NAME UpdateCheckerTest COMMAND UpdateCheckerTest)
