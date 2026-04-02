# Modules/Game/Logic/CMakeLists.txt

add_library(Logic STATIC
 src/logic/BeatmapSession.cpp
 src/logic/EditorEngine.cpp
 src/logic/BeatmapSyncBuffer.cpp
 src/logic/ecs/system/NoteSystem.cpp
)

target_link_libraries(Logic PUBLIC Log UI Graphic 3rd_concurrentqueue 3rd_entt 3rd_glm MMM)
target_include_directories(Logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
