find_package(CURL REQUIRED)
add_library(3rd_curl INTERFACE)
target_link_libraries(3rd_curl INTERFACE CURL::libcurl)
