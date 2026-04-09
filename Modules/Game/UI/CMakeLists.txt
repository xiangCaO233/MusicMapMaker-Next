add_library(UI STATIC
    src/ui/UIManager.cpp
    src/ui/brush/Brush.cpp
    src/ui/imgui/MainDockSpaceUI.cpp
    src/ui/imgui/FloatingManagerUI.cpp
    src/ui/imgui/SideBarUI.cpp
    src/ui/imgui/menu/MainMenuView.cpp
    src/ui/imgui/manager/FileManagerView.cpp
    src/ui/imgui/manager/SearchView.cpp
    src/ui/imgui/manager/AudioManagerView.cpp
    src/ui/imgui/manager/BeatMapManagerView.cpp
    src/ui/imgui/manager/SettingsView.cpp
    src/ui/imgui/manager/ToolbarView.cpp
    src/ui/layout/CLayWrapperCore.cpp
    src/ui/layout/box/CLayBox.cpp
)

target_include_directories(UI
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Game/Logic/include
)

target_link_libraries(UI
    PUBLIC
    Log
    Common
    Event
    Config
    MMM
    Graphic
    3rd_glm
    3rd_imgui
    3rd_clay
    3rd_stb
    3rd_nfd
    3rd_ImGuiFileDialog
    3rd_lunasvg
    3rd_concurrentqueue
    3rd_entt
)
