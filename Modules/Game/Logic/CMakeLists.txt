# Modules/Game/Logic/CMakeLists.txt

add_library(
  Logic STATIC
  src/logic/session/BeatmapSession.cpp
  src/logic/session/BeatmapSession_Commands.cpp
  src/logic/session/InteractionController.cpp
  src/logic/session/PlaybackController.cpp
  src/logic/session/ActionController_Editing.cpp
  src/logic/session/SessionUtils_Loading.cpp
  src/logic/session/BeatmapSession_Rendering.cpp
  src/logic/session/SessionUtils.cpp
  src/logic/session/ActionController.cpp
  src/logic/session/EditorAction.cpp
  src/logic/session/tool/GrabTool.cpp
  src/logic/session/tool/MarqueeTool.cpp
  src/logic/session/tool/DrawTool.cpp
  src/logic/EditorClipboard.cpp
  src/logic/SessionRegistry.cpp
  src/logic/RenderSyncRegistry.cpp
  src/logic/ProjectDirectoryScanner.cpp
  src/logic/ProjectDirectoryWatcher.cpp
  src/logic/ProjectResourceService.cpp
  src/logic/ProjectCommandService.cpp
  src/logic/ProjectController.cpp
  src/logic/EditorEngine.cpp
  src/logic/BeatmapSyncBuffer.cpp
  src/logic/ecs/system/render/NoteRenderSystem.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Layout.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Types.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Polyline.cpp
  src/logic/ecs/system/render/BackgroundRenderSystem.cpp
  src/logic/ecs/system/NoteTransformSystem.cpp
  src/logic/ecs/system/ScrollCache.cpp
  src/logic/ecs/system/HitFXSystem.cpp)

target_link_libraries(
  Logic
  PUBLIC Log
         MMM
         Common
         UI
         Graphic
         3rd_concurrentqueue
         3rd_entt
         3rd_glm
         3rd_miniz
         Audio
         Config
         Runtime)
target_include_directories(Logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
