# Modules/Game/Canvas/CMakeLists.txt

# cmake-format: off
# 维护约定：
# - Canvas 是业务 UI 模块，不向 Graphic 反向暴露 Brush、DrawCmd 等高层结构；新增渲染协作继续通过既有抽象回调完成。
# - Basic2DCanvas 与 TimelineCanvas 的交互、渲染和弹窗实现保持分文件登记，避免单个翻译单元继续扩大。
# - PreviewCanvas 复用逻辑快照但拥有独立视口，依赖调整时不能把预览生命周期重新绑定到主画布窗口显隐。
# - AnnotationTableWindow 拥有独立数据缓存，不应因为表格测试方便而重新依赖 TimelineCanvas 的窗口或快照消费状态。
# - BackgroundVideoPlayer 的耗时解码位于共享线程池，CMake 只链接 Runtime 接口，不为播放器单独创建平台线程目标。
# - 所有纯计算测试使用 mmm_add_test_executable 登记，统一继承测试配置隔离和 build/tests/Canvas 输出目录。
# - 测试只链接其直接依赖，禁止为了省略 target_link_libraries 而统一链接完整 Canvas，防止传递依赖掩盖头文件问题。
# - 新增测试必须同时调用 add_test，确保目标可直接运行也可在 CTest 注册完整的构建树中被发现。
# - 本模块没有自动 glob；新增、重命名或拆分源文件时必须显式同步 add_library 清单，便于评审确认职责边界。
# - include 目录作为 PUBLIC 接口导出，src 下的私有实现不得被其它模块通过相对路径直接包含。
# - 测试目标的 include 根保持与业务调用方一致，禁止使用当前文件相对路径绕过 canvas/... 的公开包含约定。
# - 第三方 target 只使用项目统一别名，不能在此硬编码库文件路径、平台后缀或预编译目录布局。
# cmake-format: on

# Canvas 静态库同时承载主画布、预览、时间线和两个独立表格窗口。 源文件按数据/辅助窗口、主画布、预览和 Timeline 职责排列，新增实现应放入
# 对应分组附近，避免只依赖 IDE 自动收集而漏入其它生成器。
add_library(
  Canvas STATIC
  src/AnnotationTableData.cpp
  src/AnnotationTableWindow.cpp
  src/AuxiliaryWindowUi.cpp
  src/BackgroundVideoPlayer.cpp
  src/Basic2DCanvas.cpp
  src/Basic2DCanvas_Interaction.cpp
  src/Basic2DCanvas_Rendering.cpp
  src/MarqueeAutoScroll.cpp
  src/PreviewCanvas.cpp
  src/TimelineCanvas.cpp
  src/TimelineCanvas_Interaction.cpp
  src/TimelineCanvas_Popups.cpp)

# PUBLIC 依赖构成 Canvas 公开头的传递接口；业务实现需要的新依赖应优先检查 能否保持 PRIVATE，避免继续扩大下游翻译单元的包含和链接范围。
target_link_libraries(
  Canvas
  PUBLIC Log
         Common
         Config
         UI
         Graphic
         Logic
         Runtime
         NetworkCollaboration
         3rd_glm
         3rd_entt)
# 仅公开本模块 include 根目录，调用方统一使用 canvas/... 路径包含头文件。
target_include_directories(Canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 协作视野投影是无图形依赖的纯计算，覆盖缺少 ScrollSegment 时的锚点映射。 目标刻意不链接 Canvas，保证头内算法没有意外依赖渲染器或 UI
# 单例。
mmm_add_test_executable(Canvas CollaborationViewportProjectionTest
                        tests/CollaborationViewportProjectionTest.cpp)
target_include_directories(CollaborationViewportProjectionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME CollaborationViewportProjectionTest
         COMMAND CollaborationViewportProjectionTest)

# 密度栏坐标映射为无图形依赖的纯函数，测试目标无需链接完整 Canvas。
# 测试还覆盖颜色渐变与拖动发布状态，保持为轻量头文件级目标；若实现迁出头文件，应只链接新增的轻量纯逻辑目标。
mmm_add_test_executable(Canvas PreviewDensityInteractionTest
                        tests/PreviewDensityInteractionTest.cpp)
target_include_directories(PreviewDensityInteractionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME PreviewDensityInteractionTest
         COMMAND PreviewDensityInteractionTest)

# 背景视频时钟是无图形依赖的纯计算，单独覆盖暂停、倍速与开始偏移。 解码线程与 FFmpeg 不参与此目标，时钟换算回归可快速独立执行，也不会要求 CI
# 提供可解码视频资源。
mmm_add_test_executable(Canvas BackgroundVideoTimingTest
                        tests/BackgroundVideoTimingTest.cpp)
target_include_directories(BackgroundVideoTimingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME BackgroundVideoTimingTest COMMAND BackgroundVideoTimingTest)

# 布局编辑使用无图形依赖的纯计算，覆盖轨道、判定线和可选组件。 Common 提供组件几何 helper，Config 提供布局 DTO 与 JSON
# 往返实现。 不链接 Canvas 可验证编辑算法仍能在无图形设备环境中运行。
mmm_add_test_executable(Canvas LayoutEditingTest tests/LayoutEditingTest.cpp)
target_include_directories(LayoutEditingTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(LayoutEditingTest PRIVATE Common Config)
add_test(NAME LayoutEditingTest COMMAND LayoutEditingTest)

# 画布标签测试覆盖长标题省略时仍固定显示前置脏标志。 标题和协作标记均为头内纯逻辑，因此无需引入 UI 或会话目标；窗口固定 ID 与停靠行为由实际
# Canvas 集成负责。
mmm_add_test_executable(Canvas CanvasTabTitleTest tests/CanvasTabTitleTest.cpp)
target_include_directories(CanvasTabTitleTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME CanvasTabTitleTest COMMAND CanvasTabTitleTest)

# 欢迎占位页可见性测试覆盖无谱面时隐藏状态与悬浮检视内容。 可见性 helper 归属 UI，显式链接 UI
# 避免依赖其它测试的传递链接顺序，并固定占位页与真实谱面的信息边界。
mmm_add_test_executable(Canvas CanvasContentVisibilityTest
                        tests/CanvasContentVisibilityTest.cpp)
target_include_directories(CanvasContentVisibilityTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CanvasContentVisibilityTest PRIVATE UI)
add_test(NAME CanvasContentVisibilityTest COMMAND CanvasContentVisibilityTest)

# 批注区阻挡手势测试覆盖拖入浮层后继续按住及在浮层内释放的收尾行为。 Common 提供 EditTool 枚举，测试本身不创建画布或 ImGui
# 输入状态，全部手势组合在编译期求值。
mmm_add_test_executable(Canvas CanvasBlockedGestureTest
                        tests/CanvasBlockedGestureTest.cpp)
target_include_directories(CanvasBlockedGestureTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(CanvasBlockedGestureTest PRIVATE Common)
add_test(NAME CanvasBlockedGestureTest COMMAND CanvasBlockedGestureTest)

# Timeline Timing 悬浮提示使用纯映射，独立覆盖界面术语与数值单位。 MMM 提供 TimingEffect 定义，目标不需要链接完整
# TimelineCanvas。
mmm_add_test_executable(Canvas TimelineTimingTooltipTest
                        tests/TimelineTimingTooltipTest.cpp)
target_include_directories(TimelineTimingTooltipTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(TimelineTimingTooltipTest PRIVATE MMM)
add_test(NAME TimelineTimingTooltipTest COMMAND TimelineTimingTooltipTest)

# 时间点表格分拍拟合使用纯数值计算，覆盖最高 1920 分母的保存边界。 独立链接 MMM 的分拍模型，使精度回归不受编辑器会话初始化影响，也不读取活动谱面
# Timing 缓存。
mmm_add_test_executable(Canvas TimingTableFractionTest
                        tests/TimingTableFractionTest.cpp)
target_include_directories(TimingTableFractionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(TimingTableFractionTest PRIVATE MMM)
add_test(NAME TimingTableFractionTest COMMAND TimingTableFractionTest)

# 物件拖拽边缘自动平移使用纯数值计算，覆盖速度和两侧轨道区域边界。 Logic 提供轨道投影，3rd_glm
# 提供二维位移值类型；不需要真实输入设备或窗口尺寸查询。
mmm_add_test_executable(Canvas ObjectDragAutoPanTest
                        tests/ObjectDragAutoPanTest.cpp)
target_include_directories(ObjectDragAutoPanTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ObjectDragAutoPanTest PRIVATE Logic 3rd_glm)
add_test(NAME ObjectDragAutoPanTest COMMAND ObjectDragAutoPanTest)

# 主画布悬浮层选择使用纯候选去重，覆盖同一物件的多个相交命中框。 Common 与 entt 只提供对象种类和实体值，测试不构造 ECS 注册表，也不依赖当前
# Session。
mmm_add_test_executable(Canvas HoverLayerSelectionTest
                        tests/HoverLayerSelectionTest.cpp)
target_include_directories(HoverLayerSelectionTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(HoverLayerSelectionTest PRIVATE Common 3rd_entt)
add_test(NAME HoverLayerSelectionTest COMMAND HoverLayerSelectionTest)

# 批注目标提示只在详情卡片悬浮时解析，覆盖时间戳、折线子物件与自动采样。 Logic 提供轨道投影，Common 提供渲染命中框，entt 提供实体标识。
# 保持独立目标可防止提示解析意外访问活动 Session 或完整画布状态。
mmm_add_test_executable(Canvas AnnotationTargetHintTest
                        tests/AnnotationTargetHintTest.cpp)
target_include_directories(AnnotationTargetHintTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(AnnotationTargetHintTest PRIVATE Logic Common 3rd_entt)
add_test(NAME AnnotationTargetHintTest COMMAND AnnotationTargetHintTest)

# 批注详情卡片布局是无图形依赖的纯计算，覆盖拥挤标记的避让与边界约束。 测试只包含 inline 布局 helper，不链接 ImGui 或 Canvas
# 静态库。
mmm_add_test_executable(Canvas AnnotationDetailLayoutTest
                        tests/AnnotationDetailLayoutTest.cpp)
target_include_directories(AnnotationDetailLayoutTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME AnnotationDetailLayoutTest COMMAND AnnotationDetailLayoutTest)

# Timeline 独立表格快照状态是无图形依赖的纯状态机，覆盖菜单打开后的异步等待。 实例令牌使用整数模拟，确保测试不需要创建真实 BeatMap
# 或渲染快照。
mmm_add_test_executable(Canvas TimelineTableSnapshotStateTest
                        tests/TimelineTableSnapshotStateTest.cpp)
target_include_directories(TimelineTableSnapshotStateTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME TimelineTableSnapshotStateTest
         COMMAND TimelineTableSnapshotStateTest)

# Timeline 独立表格窗口状态覆盖重复菜单激活与跨显示器位置恢复。 通用辅助窗口几何全部在头内，测试不依赖平台窗口后端或
# imgui.ini，因此可验证离屏恢复而无需创建真实显示器。
mmm_add_test_executable(Canvas TimelineTableWindowStateTest
                        tests/TimelineTableWindowStateTest.cpp)
target_include_directories(TimelineTableWindowStateTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME TimelineTableWindowStateTest COMMAND TimelineTableWindowStateTest)

# Timeline 快照唤醒状态仅覆盖主 Timeline 与自身时间点表格。 批注表拥有独立数据源，不加入该状态，测试固定这一模块解耦边界，防止再次出现隐藏
# Timeline 后批注表无法打开的问题。
mmm_add_test_executable(Canvas TimelineAuxiliaryWindowStateTest
                        tests/TimelineAuxiliaryWindowStateTest.cpp)
target_include_directories(TimelineAuxiliaryWindowStateTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME TimelineAuxiliaryWindowStateTest
         COMMAND TimelineAuxiliaryWindowStateTest)
