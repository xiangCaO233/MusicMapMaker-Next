# 内置演练声明嵌入程序，保证安装包缺少外置资源时仍可阅读；自定义主题另行加载。
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS
           "${PROJECT_SOURCE_DIR}/assets/walkthroughs/open-project.json")
file(READ "${PROJECT_SOURCE_DIR}/assets/walkthroughs/open-project.json"
     MMM_BUILTIN_WALKTHROUGH)
# 章节目录与占位主题同样嵌入程序；修改声明时自动重新生成。
foreach(_mmm_walkthrough IN ITEMS chapters create-project create-beatmap
                                  compose-beatmap)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY
      CMAKE_CONFIGURE_DEPENDS
      "${PROJECT_SOURCE_DIR}/assets/walkthroughs/${_mmm_walkthrough}.json")
  file(READ
       "${PROJECT_SOURCE_DIR}/assets/walkthroughs/${_mmm_walkthrough}.json"
       MMM_WALKTHROUGH_${_mmm_walkthrough})
endforeach()
configure_file(
  src/ui/walkthrough/BuiltinWalkthrough.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltinWalkthrough.h" @ONLY)

# cmake-format: off
# UI 静态库源文件维护规则：
# - 只登记本模块自维护实现，不使用递归 glob 自动收集；
# - 新文件必须显式加入对应职责分组，确保清单可审查；
# - 视图基础与工具实现排在高层管理视图之前；
# - 菜单动作按 edit、file、help、tools、view 分类；
# - 音频、协作和设置的拆分实现保持相邻；
# - 自动生成的演练头仅通过私有生成目录提供。
# cmake-format: on
add_library(
  UI STATIC
  # 视图基础接口与演练流程。
  src/ui/IUIView.cpp
  src/ui/walkthrough/WalkthroughModel.cpp
  src/ui/walkthrough/WalkthroughService.cpp
  src/ui/walkthrough/WalkthroughSpotlight.cpp
  src/ui/walkthrough/WalkthroughPage.cpp
  src/ui/walkthrough/WelcomeView.cpp
  # 工程拖放入口和欢迎页打开动作。
  src/ui/project/ProjectDropRouter.cpp
  src/ui/imgui/menu/actions/help/OpenWelcomeAction.cpp
  # UI 生命周期、纹理加载和顶层管理器。
  src/ui/ITextureLoader.cpp
  src/ui/UIManager.cpp
  # 通用绘制画刷及桌面、文件对话框、控件工具。
  src/ui/brush/Brush.cpp
  src/ui/utils/DesktopPathUtils.cpp
  src/ui/utils/NativeFileDialog.cpp
  src/ui/utils/UIWidgetUtils.cpp
  # 主窗口固定区域、停靠布局和浮动管理器。
  src/ui/imgui/MainDockSpaceUI.cpp
  src/ui/imgui/MainDockSpaceUI_MenuBar.cpp
  src/ui/imgui/MainDockSpaceUI_Docking.cpp
  src/ui/imgui/MainDockSpaceUI_StatusBar.cpp
  src/ui/imgui/FloatingManagerUI.cpp
  src/ui/imgui/SideBarUI.cpp
  # 跨操作复用的诊断和保存结果反馈。
  src/ui/imgui/feedback/BeatmapLoadDiagnosticFeedback.cpp
  src/ui/imgui/feedback/SaveResultFeedback.cpp
  # 主菜单注册、导航、Markdown 内容与图片缓存。
  src/ui/imgui/menu/MainMenuView.cpp
  src/ui/imgui/menu/MainMenuNavigationController.cpp
  src/ui/imgui/menu/MainMenuRegistry.cpp
  src/ui/imgui/markdown/MarkdownRenderer.cpp
  src/ui/imgui/markdown/MarkdownImageCache.cpp
  # 编辑菜单动作：模式切换、剪贴板、元数据和撤销重做。
  src/ui/imgui/menu/actions/edit/BmsEditingToggleAction.cpp
  src/ui/imgui/menu/actions/edit/AddSelectedObjectAnnotationAction.cpp
  src/ui/imgui/menu/actions/edit/CopyAction.cpp
  src/ui/imgui/menu/actions/edit/CutAction.cpp
  src/ui/imgui/menu/actions/edit/EditSelectedObjectVolumeAction.cpp
  src/ui/imgui/menu/actions/edit/MirrorAction.cpp
  src/ui/imgui/menu/actions/edit/MirrorPasteAction.cpp
  src/ui/imgui/menu/actions/edit/MetadataEditorWindowRenderers.cpp
  src/ui/imgui/menu/actions/edit/OpenBeatmapSettingsAction.cpp
  src/ui/imgui/menu/actions/edit/OpenNoteMetadataAction.cpp
  src/ui/imgui/menu/actions/edit/PasteAction.cpp
  src/ui/imgui/menu/actions/edit/PolylineEditingToggleAction.cpp
  src/ui/imgui/menu/actions/edit/ProfessionalModeToggleAction.cpp
  src/ui/imgui/menu/actions/edit/RedoAction.cpp
  src/ui/imgui/menu/actions/edit/SelectAllAction.cpp
  src/ui/imgui/menu/actions/edit/TimelineCanvasMenuActions.cpp
  src/ui/imgui/menu/actions/edit/TogglePlaybackAction.cpp
  src/ui/imgui/menu/actions/edit/UndoAction.cpp
  # 文件菜单动作：工程、谱面、音频与打包保存入口。
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
  # 帮助菜单动作：更新检查、帮助路径和关于窗口。
  src/ui/imgui/menu/actions/help/CheckUpdateAction.cpp
  src/ui/imgui/menu/actions/help/OpenHelpPathAction.cpp
  src/ui/imgui/menu/actions/help/ShowAboutAction.cpp
  # 工具菜单动作：节拍分析、导出、插件和诊断窗口。
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
  # 视图菜单动作：画布附属窗口和工具显示选项。
  src/ui/imgui/menu/actions/view/AnnotationDetailsToggleAction.cpp
  src/ui/imgui/menu/actions/view/FixedToolWindowToggleAction.cpp
  src/ui/imgui/menu/actions/view/ManagerLabelsToggleAction.cpp
  src/ui/imgui/menu/actions/view/PreviewWindowToggleAction.cpp
  src/ui/imgui/menu/actions/view/TimelineWindowToggleAction.cpp
  src/ui/imgui/menu/actions/view/ToolLabelsToggleAction.cpp
  # 菜单项目抽象接口和具体菜单节点。
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
  src/ui/imgui/menu/items/MainMenuToolbarVisibilityItem.cpp
  src/ui/imgui/menu/items/MainMenuTools.cpp
  src/ui/imgui/menu/items/MainMenuToggleItem.cpp
  src/ui/imgui/menu/items/MainMenuViewMenu.cpp
  # 菜单通用工具、状态消息与独立应用弹窗。
  src/ui/imgui/menu/utils/MenuUtil.cpp
  src/ui/imgui/status/StatusMessageService.cpp
  src/ui/imgui/windows/PgoUploadConsentWindow.cpp
  # 资源、谱面、音频、协作、新建流程和设置管理视图。
  src/ui/imgui/manager/FileManagerView.cpp
  src/ui/imgui/manager/FileManagerView_Hierarchy.cpp
  src/ui/imgui/manager/FileManagerView_Interactions.cpp
  src/ui/imgui/manager/SearchView.cpp
  src/ui/imgui/manager/AudioManagerView.cpp
  src/ui/imgui/manager/ProjectAudioToolView.cpp
  src/ui/imgui/manager/BeatMapManagerView.cpp
  src/ui/imgui/manager/CollaborationLogWindow.cpp
  src/ui/imgui/manager/CollaborationRoomCoverImage.cpp
  src/ui/imgui/manager/CollaborationView.cpp
  src/ui/imgui/manager/NewBeatmapWizard.cpp
  src/ui/imgui/manager/NewProjectWizard.cpp
  src/ui/imgui/manager/SettingsView.cpp
  src/ui/imgui/manager/SettingsView_BeatmapTab.cpp
  src/ui/imgui/manager/SettingsView_CollaborationTab.cpp
  src/ui/imgui/manager/SettingsView_DebugTab.cpp
  src/ui/imgui/manager/SettingsView_EditorTab.cpp
  src/ui/imgui/manager/SettingsView_ProjectTab.cpp
  src/ui/imgui/manager/SettingsView_ShortcutTab.cpp
  src/ui/imgui/manager/SettingsView_SkinPackage.cpp
  src/ui/imgui/manager/SettingsView_SoftwareTab.cpp
  src/ui/imgui/manager/SettingsView_Tabs.cpp
  src/ui/imgui/manager/SettingsView_VisualTab.cpp
  src/ui/imgui/manager/ToolbarView.cpp
  # 音轨控制、试听控件、波形和频谱视图。
  src/ui/imgui/audio/AudioTrackControllerUI.cpp
  src/ui/imgui/audio/AudioTrackControllerUI_Widgets.cpp
  src/ui/imgui/audio/AudioTrackControllerUI_EQ.cpp
  src/ui/imgui/audio/ProjectAudioPreviewControls.cpp
  src/ui/imgui/audio/AudioWaveformView.cpp
  src/ui/imgui/audio/AudioSpectrumView.cpp
  # 调试、画布标签和 Clay 布局桥。
  src/ui/imgui/DebugWindowUI.cpp
  src/ui/imgui/CanvasTabManager.cpp
  src/ui/layout/CLayWrapperCore.cpp
  src/ui/layout/box/CLayBox.cpp)

# 公共 include 目录导出 UI API；生成头和上层实现细节保持私有。 Logic 与 Audio 私有 include 只服务实现文件，不能泄漏到
# UI 消费者接口。
target_include_directories(
  UI
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
  PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Game/Logic/include
  PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Audio/include)

# cmake-format: off
# 链接依赖边界：
# - PUBLIC 列表覆盖 UI 公开 API 暴露或消费者必须继承的目标；
# - Graphic 提供底层绘制能力，但 UI 不反向泄漏到 Graphic；
# - ImGui、ImPlot 与 Clay 分别承担控件、图表和布局；
# - stb、NFD 与 ImGuiFileDialog 支持资源和文件选择；
# - concurrentqueue 用于跨线程结果进入 UI；
# - NetworkCollaboration 只服务内部协作管理实现，保持 PRIVATE；
# - 第三方 target 必须与预编译查找脚本导出名称一致。
# cmake-format: on
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

# cmake-format: off
# UI 测试注册约定：
# - 测试均通过 mmm_add_test_executable 创建；
# - 二进制统一输出到构建树 tests/UI；
# - 每个目标显式列出 include 和链接依赖；
# - 测试名称与可执行文件名称保持一致；
# - 纯策略测试不链接完整 Game 或 GPU；
# - 需要 ImGui 状态的测试只创建最小上下文；
# - 测试不得写入源码 tests/data。
# cmake-format: on
# BPM 播放路由测试验证工具入口把播放请求送到正确会话。 测试只需要 UI 公开头，由 mmm_add_test_executable 统一设置输出目录。
mmm_add_test_executable(UI BpmPlaybackRoutingTest
                        tests/BpmPlaybackRoutingTest.cpp)
target_include_directories(BpmPlaybackRoutingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME BpmPlaybackRoutingTest COMMAND BpmPlaybackRoutingTest)

# 空格播放路由测试覆盖活动拖拽交互和控件导航激活隔离。 显式链接 ImGui，以构造路由判定所需的最小输入上下文。
mmm_add_test_executable(UI PlaybackShortcutRoutingTest
                        tests/PlaybackShortcutRoutingTest.cpp)
target_include_directories(PlaybackShortcutRoutingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(PlaybackShortcutRoutingTest PRIVATE 3rd_imgui)
add_test(NAME PlaybackShortcutRoutingTest COMMAND PlaybackShortcutRoutingTest)

# cmake-format: off
# Markdown 测试分层：
# - Parser 只验证块级和行内语法拆分；
# - Renderer 直接编译绘制实现并创建最小 ImGui 帧；
# - ImageCache 默认离线，不依赖实时网络；
# - 相对地址和非法协议必须在缓存边界验证；
# - GIF 覆盖多帧解码与帧选择；
# - 图片测试不创建真实 Vulkan 纹理。
# cmake-format: on
# Markdown 解析器为批注与更新日志共享，覆盖块级和行内语法的稳定拆分。 解析器测试不创建窗口或 GPU，只验证纯文本语法树。
mmm_add_test_executable(UI MarkdownParserTest tests/MarkdownParserTest.cpp)
target_include_directories(MarkdownParserTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME MarkdownParserTest COMMAND MarkdownParserTest)

# Markdown 绘制测试创建最小 ImGui 帧，覆盖测量、流式绘制与矩形裁剪入口。 实现文件直接加入测试目标，隔离完整 UI 静态库的高层依赖。
mmm_add_test_executable(UI MarkdownRendererTest tests/MarkdownRendererTest.cpp
                        src/ui/imgui/markdown/MarkdownRenderer.cpp)
target_include_directories(MarkdownRendererTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(MarkdownRendererTest PRIVATE 3rd_imgui)
add_test(NAME MarkdownRendererTest COMMAND MarkdownRendererTest)

# 更新图片测试默认离线，覆盖相对地址、非法协议和多帧 GIF 解码。 链接 UI 以复用正式 Markdown 图片缓存实现及其资源边界。
mmm_add_test_executable(UI MarkdownImageCacheTest
                        tests/MarkdownImageCacheTest.cpp)
target_link_libraries(MarkdownImageCacheTest PRIVATE UI)
add_test(NAME MarkdownImageCacheTest COMMAND MarkdownImageCacheTest)

# 分拍线快捷键测试覆盖最近两个显示模式间的双向切换。 该状态依赖 Config 枚举，测试不需要完整 Game 目标。
mmm_add_test_executable(UI BeatLineDisplayModeHistoryTest
                        tests/BeatLineDisplayModeHistoryTest.cpp)
target_include_directories(BeatLineDisplayModeHistoryTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(BeatLineDisplayModeHistoryTest PRIVATE Config)
add_test(NAME BeatLineDisplayModeHistoryTest
         COMMAND BeatLineDisplayModeHistoryTest)

# 打包默认选项测试覆盖不同工程状态下的初始选择策略。 策略位于轻量公开头，测试无需额外图形依赖。
mmm_add_test_executable(UI PackageDefaultSelectionTest
                        tests/PackageDefaultSelectionTest.cpp)
target_include_directories(PackageDefaultSelectionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME PackageDefaultSelectionTest COMMAND PackageDefaultSelectionTest)

# cmake-format: off
# 工程 UI 状态测试约定：
# - 默认打包选择保持与工程状态一致；
# - 打开、切换和关闭通过轻量状态对象验证；
# - 加载进度覆盖乱序事件与完成清理；
# - 进度数值必须在 UI 边界钳制；
# - Event 目标提供生产事件定义；
# - 测试不打开真实文件选择器；
# - 运行时使用隔离配置根目录。
# cmake-format: on
# 工程 UI 生命周期测试覆盖打开、切换、关闭过程中的状态迁移。 测试使用 UI 公开状态对象，不启动真实文件选择器。
mmm_add_test_executable(UI ProjectUiLifecycleStateTest
                        tests/ProjectUiLifecycleStateTest.cpp)
target_include_directories(ProjectUiLifecycleStateTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectUiLifecycleStateTest COMMAND ProjectUiLifecycleStateTest)

# 项目加载进度测试覆盖跨线程事件乱序、数值约束和完成清理。 Event 显式链接确保测试使用与生产代码相同的进度事件定义。
mmm_add_test_executable(UI ProjectOpenProgressStateTest
                        tests/ProjectOpenProgressStateTest.cpp)
target_include_directories(ProjectOpenProgressStateTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ProjectOpenProgressStateTest PRIVATE Event)
add_test(NAME ProjectOpenProgressStateTest COMMAND ProjectOpenProgressStateTest)

# cmake-format: off
# 协作 UI 测试约定：
# - 入口按钮验证选择切换和样式栈平衡；
# - EntryPolicy 验证房主与访客前置条件；
# - 房卡封面使用真实受管图片资源；
# - 封面覆盖居中裁剪和 Base64 往返；
# - 非法、过大和尺寸不符输入必须拒绝；
# - 测试直接复用正式 UI 实现；
# - 资源根目录由测试框架传入。
# cmake-format: on
# 协作入口切换测试覆盖按钮点击改变选中状态时的 ImGui 颜色栈配对。 链接正式 UI 实现以验证反馈按钮包装，而非复制控件逻辑。
mmm_add_test_executable(UI CollaborationSelectableButtonTest
                        tests/CollaborationSelectableButtonTest.cpp)
target_include_directories(CollaborationSelectableButtonTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CollaborationSelectableButtonTest PRIVATE UI)
add_test(NAME CollaborationSelectableButtonTest
         COMMAND CollaborationSelectableButtonTest)

# Dock 标签关闭按钮测试覆盖标签悬浮色与固定红色危险高亮。 测试需要 ImGui 内部标签状态，因此同时链接 UI 与 3rd_imgui。
mmm_add_test_executable(UI DockTabCloseHoverTest
                        tests/DockTabCloseHoverTest.cpp)
target_include_directories(DockTabCloseHoverTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(DockTabCloseHoverTest PRIVATE UI 3rd_imgui)
add_test(NAME DockTabCloseHoverTest COMMAND DockTabCloseHoverTest)

# 保存反馈测试覆盖原生文件选择器或耗时导出后的长帧首次绘制。 长帧场景只推进反馈状态机，不真正执行磁盘写入。
mmm_add_test_executable(UI SaveResultFeedbackTest
                        tests/SaveResultFeedbackTest.cpp)
target_include_directories(SaveResultFeedbackTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(SaveResultFeedbackTest PRIVATE UI)
add_test(NAME SaveResultFeedbackTest COMMAND SaveResultFeedbackTest)

# 协作入口策略测试确保只有房主开启房间要求预先打开项目。 策略测试保持轻量，仅链接日志设施以满足生产接口依赖。
mmm_add_test_executable(UI CollaborationEntryPolicyTest
                        tests/CollaborationEntryPolicyTest.cpp)
target_include_directories(CollaborationEntryPolicyTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CollaborationEntryPolicyTest PRIVATE Log)
add_test(NAME CollaborationEntryPolicyTest COMMAND CollaborationEntryPolicyTest)

# 协作房卡封面测试覆盖真实图片居中缩略、Base64 往返和无效输入拒绝。 测试资源通过 MMM_TEST_RESOURCE_DIR
# 传入，禁止向源码资源目录写输出。
mmm_add_test_executable(UI CollaborationRoomCoverImageTest
                        tests/CollaborationRoomCoverImageTest.cpp)
target_include_directories(CollaborationRoomCoverImageTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CollaborationRoomCoverImageTest PRIVATE UI)
add_test(NAME CollaborationRoomCoverImageTest
         COMMAND CollaborationRoomCoverImageTest
                 "${MMM_TEST_RESOURCE_DIR}/ma/Hai Yorokonde/cover.jpg")

# cmake-format: off
# 项目音频工具测试约定：
# - Layout 验证边缘、中心吸附和滞回；
# - 叠层布局维持最小可见比例；
# - 草稿区扩充后轨道控件行同步增长；
# - Search 覆盖精确、子串和顺序模糊；
# - UTF-8 文件名作为搜索输入验证；
# - 几何测试不创建实际窗口；
# - helper 测试不加载音频文件。
# cmake-format: on
# 项目音频工具布局测试覆盖边缘/中心吸附、滞回和叠层可见比例。 布局算法测试不创建实际窗口，直接验证确定性几何结果。
mmm_add_test_executable(UI ProjectAudioToolLayoutTest
                        tests/ProjectAudioToolLayoutTest.cpp)
target_include_directories(ProjectAudioToolLayoutTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectAudioToolLayoutTest COMMAND ProjectAudioToolLayoutTest)

# 音效工具轨道布局测试覆盖草稿区自动扩充后控件行同步增加。 草稿轨道数量与布局映射通过纯状态接口验证。
mmm_add_test_executable(UI SoundEffectToolTrackLayoutTest
                        tests/SoundEffectToolTrackLayoutTest.cpp)
target_include_directories(SoundEffectToolTrackLayoutTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME SoundEffectToolTrackLayoutTest
         COMMAND SoundEffectToolTrackLayoutTest)

# 项目音频工具搜索测试覆盖精确、子串、顺序模糊和 UTF-8 文件名匹配。 搜索测试只包含公开 helper，不依赖项目资源文件。
mmm_add_test_executable(UI ProjectAudioToolSearchTest
                        tests/ProjectAudioToolSearchTest.cpp)
target_include_directories(ProjectAudioToolSearchTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectAudioToolSearchTest COMMAND ProjectAudioToolSearchTest)

# cmake-format: off
# 欢迎页与演练测试约定：
# - WelcomeView 覆盖启动偏好和响应式排版；
# - 内嵌演练支持进入、返回和进度恢复；
# - Walkthrough 核心测试不链接图形视图；
# - Model 与 Service 直接加入轻量目标；
# - generated 目录提供内嵌声明；
# - 事件队列依赖显式链接；
# - 输出只写构建树 test_output。
# cmake-format: on
# 欢迎页测试覆盖启动偏好、宽窄窗口排版和内嵌演练往返，不创建 GPU。 链接 Game 提供欢迎页完整集成所需的 UI 和业务目标。
mmm_add_test_executable(UI WelcomeViewTest tests/WelcomeViewTest.cpp)
target_link_libraries(WelcomeViewTest PRIVATE Game)
add_test(NAME WelcomeViewTest COMMAND WelcomeViewTest)

# 演练核心测试不链接图形视图，验证窗口关闭时的事件计数与独立进度恢复。 单独编译 Model 与 Service，保持测试不依赖 Vulkan 和 ImGui
# 视图。 生成目录提供内嵌演练头，输出目录参数指向构建树 test_output。
mmm_add_test_executable(
  UI WalkthroughTest tests/WalkthroughTest.cpp
  src/ui/walkthrough/WalkthroughModel.cpp
  src/ui/walkthrough/WalkthroughService.cpp)
target_include_directories(
  WalkthroughTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                          ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(WalkthroughTest PRIVATE Event 3rd_nlohmann_json
                                              3rd_concurrentqueue)
add_test(NAME WalkthroughTest COMMAND WalkthroughTest
                                      "${CMAKE_BINARY_DIR}/test_output")

# 突出层测试链接正式 UI 实现，覆盖任意控件矩形、阶段确认和不抢占导航焦点。
mmm_add_test_executable(UI WalkthroughSpotlightTest
                        tests/WalkthroughSpotlightTest.cpp)
target_include_directories(WalkthroughSpotlightTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(WalkthroughSpotlightTest PRIVATE UI 3rd_imgui)
add_test(NAME WalkthroughSpotlightTest COMMAND WalkthroughSpotlightTest)

# cmake-format: off
# 工程资源导入测试约定：
# - 覆盖 UTF-8 路径解析和目标命名；
# - 覆盖已有资源冲突与安全失败；
# - 输入资源保持只读；
# - 生成内容只进入 test_output；
# - 禁止写回 tests/data；
# - 不修改用户最近工程和布局；
# - 新资源继续遵守 Git LFS 规则。
# cmake-format: on
# 工程资源导入测试覆盖路径解析、冲突处理和输出目录约束。 所有生成文件写入构建树 test_output，不污染 tests/data 源资源。
mmm_add_test_executable(UI ProjectResourceImportTest
                        tests/ProjectResourceImportTest.cpp)
target_include_directories(ProjectResourceImportTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME ProjectResourceImportTest
         COMMAND ProjectResourceImportTest "${CMAKE_BINARY_DIR}/test_output")
