# Modules/Game/Logic/CMakeLists.txt

add_library(Logic STATIC
	src/logic/session/BeatmapSession.cpp
	src/logic/session/BeatmapSession_Loading.cpp
	src/logic/session/BeatmapSession_Commands.cpp
	src/logic/EditorEngine.cpp
	src/logic/BeatmapSyncBuffer.cpp
	src/logic/ecs/system/render/NoteRenderSystem.cpp
	src/logic/ecs/system/render/NoteRenderSystem_Layout.cpp
	src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp
	src/logic/ecs/system/NoteTransformSystem.cpp
	src/logic/ecs/system/ScrollCache.cpp
)

target_link_libraries(Logic
	PUBLIC
		Log
		UI
		Graphic
		3rd_concurrentqueue
		3rd_entt
		3rd_glm
		MMM
)
target_include_directories(Logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
