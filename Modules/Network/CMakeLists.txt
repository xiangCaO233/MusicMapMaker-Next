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
