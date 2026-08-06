add_library(
  UI STATIC
  src/ui/IUIView.cpp
  src/ui/ITextureLoader.cpp
  src/ui/UIManager.cpp
  src/ui/brush/Brush.cpp
  src/ui/utils/DesktopPathUtils.cpp
  src/ui/utils/UIWidgetUtils.cpp
  src/ui/imgui/MainDockSpaceUI.cpp
  src/ui/imgui/MainDockSpaceUI_MenuBar.cpp
  src/ui/imgui/MainDockSpaceUI_Docking.cpp
  src/ui/imgui/MainDockSpaceUI_StatusBar.cpp
  src/ui/imgui/FloatingManagerUI.cpp
  src/ui/imgui/SideBarUI.cpp
  src/ui/imgui/feedback/BeatmapLoadDiagnosticFeedback.cpp
  src/ui/imgui/feedback/SaveResultFeedback.cpp
  src/ui/imgui/menu/MainMenuView.cpp
  src/ui/imgui/menu/MainMenuNavigationController.cpp
  src/ui/imgui/menu/MainMenuRegistry.cpp
  src/ui/imgui/menu/actions/edit/BmsEditingToggleAction.cpp
  src/ui/imgui/menu/actions/edit/CopyAction.cpp
  src/ui/imgui/menu/actions/edit/CutAction.cpp
  src/ui/imgui/menu/actions/edit/MirrorAction.cpp
  src/ui/imgui/menu/actions/edit/MirrorPasteAction.cpp
  src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp
  src/ui/imgui/menu/actions/edit/OpenBeatmapSettingsAction.cpp
  src/ui/imgui/menu/actions/edit/OpenNoteMetadataAction.cpp
  src/ui/imgui/menu/actions/edit/PasteAction.cpp
  src/ui/imgui/menu/actions/edit/PolylineEditingToggleAction.cpp
  src/ui/imgui/menu/actions/edit/RedoAction.cpp
  src/ui/imgui/menu/actions/edit/SelectAllAction.cpp
  src/ui/imgui/menu/actions/edit/TogglePlaybackAction.cpp
  src/ui/imgui/menu/actions/edit/UndoAction.cpp
  src/ui/imgui/menu/actions/file/CloseProjectAction.cpp
  src/ui/imgui/menu/actions/file/OpenAudioImportAction.cpp
  src/ui/imgui/menu/actions/file/OpenNewBeatmapWizardAction.cpp
  src/ui/imgui/menu/actions/file/OpenNewProjectWizardAction.cpp
  src/ui/imgui/menu/actions/file/OpenProjectAction.cpp
  src/ui/imgui/menu/actions/file/OpenProjectDirectoryAction.cpp
  src/ui/imgui/menu/actions/file/OpenRecentProjectAction.cpp
  src/ui/imgui/menu/actions/file/PackBeatmapAction.cpp
  src/ui/imgui/menu/actions/file/SaveBeatmapAction.cpp
  src/ui/imgui/menu/actions/file/SaveBeatmapAsAction.cpp
  src/ui/imgui/menu/actions/help/CheckUpdateAction.cpp
  src/ui/imgui/menu/actions/help/ShowAboutAction.cpp
  src/ui/imgui/menu/actions/tools/AlignSelectedToCommonBeatsAction.cpp
  src/ui/imgui/menu/actions/tools/BpmAutoDetector.cpp
  src/ui/imgui/menu/actions/tools/BpmMeasurementToolView.cpp
  src/ui/imgui/menu/actions/tools/OpenBeatmapSpeedExportAction.cpp
  src/ui/imgui/menu/actions/tools/OpenBpmMeasurementAction.cpp
  src/ui/imgui/menu/actions/tools/OpenDataSourceReplaceWindowAction.cpp
  src/ui/imgui/menu/actions/tools/OpenPluginListAction.cpp
  src/ui/imgui/menu/actions/tools/ReloadPluginsAction.cpp
  src/ui/imgui/menu/actions/tools/ToggleMetadataEditorWindowAction.cpp
  src/ui/imgui/menu/actions/tools/ToggleOverlapCheckWindowAction.cpp
  src/ui/imgui/menu/actions/view/FixedToolWindowToggleAction.cpp
  src/ui/imgui/menu/actions/view/ManagerLabelsToggleAction.cpp
  src/ui/imgui/menu/actions/view/PreviewWindowToggleAction.cpp
  src/ui/imgui/menu/actions/view/TimelineWindowToggleAction.cpp
  src/ui/imgui/menu/actions/view/ToolLabelsToggleAction.cpp
  src/ui/imgui/menu/interfaces/IMainMenuItem.cpp
  src/ui/imgui/menu/interfaces/IMainMenuItemActionHandler.cpp
  src/ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.cpp
  src/ui/imgui/menu/items/MainMenuActionItem.cpp
  src/ui/imgui/menu/items/MainMenuEdit.cpp
  src/ui/imgui/menu/items/MainMenuFile.cpp
  src/ui/imgui/menu/items/MainMenuHelp.cpp
  src/ui/imgui/menu/items/MainMenuItemUtils.cpp
  src/ui/imgui/menu/items/MainMenuRecentProjectsItem.cpp
  src/ui/imgui/menu/items/MainMenuSeparatorItem.cpp
  src/ui/imgui/menu/items/MainMenuTools.cpp
  src/ui/imgui/menu/items/MainMenuToggleItem.cpp
  src/ui/imgui/menu/items/MainMenuViewMenu.cpp
  src/ui/imgui/menu/utils/MenuUtil.cpp
  src/ui/imgui/status/StatusMessageService.cpp
  src/ui/imgui/windows/PgoUploadConsentWindow.cpp
  src/ui/imgui/manager/FileManagerView.cpp
  src/ui/imgui/manager/FileManagerView_Hierarchy.cpp
  src/ui/imgui/manager/FileManagerView_Interactions.cpp
  src/ui/imgui/manager/SearchView.cpp
  src/ui/imgui/manager/AudioManagerView.cpp
  src/ui/imgui/manager/ProjectAudioToolView.cpp
  src/ui/imgui/manager/BeatMapManagerView.cpp
  src/ui/imgui/manager/CollaborationLogWindow.cpp
  src/ui/imgui/manager/CollaborationView.cpp
  src/ui/imgui/manager/NewBeatmapWizard.cpp
  src/ui/imgui/manager/NewProjectWizard.cpp
  src/ui/imgui/manager/SettingsView.cpp
  src/ui/imgui/manager/SettingsView_BeatmapTab.cpp
  src/ui/imgui/manager/SettingsView_DebugTab.cpp
  src/ui/imgui/manager/SettingsView_EditorTab.cpp
  src/ui/imgui/manager/SettingsView_ProjectTab.cpp
  src/ui/imgui/manager/SettingsView_ShortcutTab.cpp
  src/ui/imgui/manager/SettingsView_SkinPackage.cpp
  src/ui/imgui/manager/SettingsView_SoftwareTab.cpp
  src/ui/imgui/manager/SettingsView_Tabs.cpp
  src/ui/imgui/manager/SettingsView_VisualTab.cpp
  src/ui/imgui/manager/ToolbarView.cpp
  src/ui/imgui/audio/AudioTrackControllerUI.cpp
  src/ui/imgui/audio/AudioTrackControllerUI_Widgets.cpp
  src/ui/imgui/audio/AudioTrackControllerUI_EQ.cpp
  src/ui/imgui/audio/ProjectAudioPreviewControls.cpp
  src/ui/imgui/audio/AudioWaveformView.cpp
  src/ui/imgui/audio/AudioSpectrumView.cpp
  src/ui/imgui/DebugWindowUI.cpp
  src/ui/imgui/CanvasTabManager.cpp
  src/ui/layout/CLayWrapperCore.cpp
  src/ui/layout/box/CLayBox.cpp)

target_include_directories(
  UI
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Game/Logic/include
  PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Game/Canvas/include
  PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Audio/include)

target_link_libraries(
  UI
  PUBLIC Audio
         Log
         Common
         Event
         Config
         MMM
         Runtime
         Graphic
         3rd_glm
         3rd_imgui
         3rd_implot
         3rd_clay
         3rd_stb
         3rd_nfd
         3rd_ImGuiFileDialog
         3rd_lunasvg
         3rd_concurrentqueue
         3rd_entt
         Network)
target_link_libraries(UI PRIVATE NetworkCollaboration)

# Windows 系统文件管理器调用 ShellExecuteW，需要显式链接 Shell32。
if(WIN32)
  target_link_libraries(UI PRIVATE shell32)
endif()

mmm_add_test_executable(UI BpmPlaybackRoutingTest
                        tests/BpmPlaybackRoutingTest.cpp)
target_include_directories(BpmPlaybackRoutingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME BpmPlaybackRoutingTest COMMAND BpmPlaybackRoutingTest)

# 空格播放路由测试覆盖主画布与时间线的活动拖拽交互。
mmm_add_test_executable(UI PlaybackShortcutRoutingTest
                        tests/PlaybackShortcutRoutingTest.cpp)
target_include_directories(PlaybackShortcutRoutingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME PlaybackShortcutRoutingTest COMMAND PlaybackShortcutRoutingTest)

# 分拍线快捷键测试覆盖最近两个显示模式间的双向切换。
mmm_add_test_executable(UI BeatLineDisplayModeHistoryTest
                        tests/BeatLineDisplayModeHistoryTest.cpp)
target_include_directories(BeatLineDisplayModeHistoryTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(BeatLineDisplayModeHistoryTest PRIVATE Config)
add_test(NAME BeatLineDisplayModeHistoryTest
         COMMAND BeatLineDisplayModeHistoryTest)

mmm_add_test_executable(UI PackageDefaultSelectionTest
                        tests/PackageDefaultSelectionTest.cpp)
target_include_directories(PackageDefaultSelectionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME PackageDefaultSelectionTest COMMAND PackageDefaultSelectionTest)

mmm_add_test_executable(UI ProjectUiLifecycleStateTest
                        tests/ProjectUiLifecycleStateTest.cpp)
target_include_directories(ProjectUiLifecycleStateTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectUiLifecycleStateTest COMMAND ProjectUiLifecycleStateTest)

# 项目音频工具布局测试覆盖边缘/中心吸附、滞回和叠层可见比例。
mmm_add_test_executable(UI ProjectAudioToolLayoutTest
                        tests/ProjectAudioToolLayoutTest.cpp)
target_include_directories(ProjectAudioToolLayoutTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectAudioToolLayoutTest COMMAND ProjectAudioToolLayoutTest)

# 项目音频工具搜索测试覆盖精确、子串、顺序模糊和 UTF-8 文件名匹配。
mmm_add_test_executable(UI ProjectAudioToolSearchTest
                        tests/ProjectAudioToolSearchTest.cpp)
target_include_directories(ProjectAudioToolSearchTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectAudioToolSearchTest COMMAND ProjectAudioToolSearchTest)
