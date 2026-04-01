set(IMPLOT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/implot")

add_library(3rd_implot STATIC
    "${IMPLOT_SOURCE_DIR}/implot.cpp"
    "${IMPLOT_SOURCE_DIR}/implot_items.cpp"
)

target_link_libraries(3rd_implot PUBLIC 3rd_imgui)

target_include_directories(3rd_implot PUBLIC ${IMPLOT_SOURCE_DIR})
