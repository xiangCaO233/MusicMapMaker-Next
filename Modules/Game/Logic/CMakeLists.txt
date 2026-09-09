# Modules/Game/Logic/CMakeLists.txt Logic 固定为业务静态库，依赖库的静态或动态选择由上层依赖配置负责。
# 源文件显式列举，新增实现时需登记到对应职责附近，不能仅创建文件而遗漏编译。 本模块不决定依赖从源码还是预编译包取得；该选择已在顶层完成。 Logic
# 显式声明 STATIC，不能用 BUILD_SHARED_LIBS 推断业务模块本身的产物类型。

add_library(
  Logic STATIC
  # 公开头由编译依赖追踪；这里登记可编译实现，不把测试源加入正式业务库。
  # 会话生命周期、命令分派、播放和交互共享同一逻辑模块，但按实现职责拆分翻译单元。
  src/logic/session/BeatmapSession.cpp
  # 命令入口与帧更新分开编译，避免继续扩张会话主实现。
  src/logic/session/BeatmapSession_Commands.cpp
  src/logic/session/InteractionController.cpp
  src/logic/session/PlaybackController.cpp
  src/logic/session/ActionController_Editing.cpp
  # 加载和运行期同步都使用 SessionUtils，但分别维护各自低频与热路径约束。
  src/logic/session/SessionUtils_Loading.cpp
  src/logic/session/BeatmapSession_Rendering.cpp
  src/logic/session/SessionUtils.cpp
  src/logic/session/ActionController.cpp
  # 音符稳定身份独立于 entt 实体编号，撤销及协作流程共享该实现。
  src/logic/session/NoteIdentity.cpp
  src/logic/session/EditorAction.cpp
  # 自动采样的撤销记录不并入普通音符动作，以保留对象种类边界。
  src/logic/session/SampleAction.cpp
  # 编辑工具由会话调度，不作为独立可执行文件或额外插件目标。
  src/logic/session/tool/GrabTool.cpp
  src/logic/session/tool/MarqueeTool.cpp
  src/logic/session/tool/DrawTool.cpp
  # 剪贴板协议与持久化服务同时供编辑器流程和无界面回归测试使用。
  src/logic/EditorClipboard.cpp
  # 协议编码与剪贴板状态存储拆分，序列化回归可直接调用协议层。
  src/logic/EditorClipboardProtocol.cpp
  src/logic/BeatmapLoadDiagnosticPublisher.cpp
  # 自动备份属于项目数据持久化，不在 UI 目标内直接实现文件写入。
  src/logic/BeatmapBackupService.cpp
  src/logic/ImdPackageExportService.cpp
  # 音频原点对齐是导出模型转换，不要求渲染器介入时间调整。
  src/logic/MczAudioOriginAlignment.cpp
  # 注册表管理会话与渲染同步身份，项目服务管理目录、资源和保存命令。
  src/logic/SessionRegistry.cpp
  src/logic/RenderSyncRegistry.cpp
  src/logic/ProjectDirectoryScanner.cpp
  # 目录扫描和目录变更过滤分别登记，不能只链接其中一个替代完整项目刷新。
  src/logic/ProjectDirectoryWatcher.cpp
  src/logic/ProjectStorage.cpp
  # 草稿轨持久化和媒体资源管理各自维护数据入口，共用项目服务层。
  src/logic/ProjectDraftLaneService.cpp
  src/logic/ProjectResourceService.cpp
  src/logic/MalodyPackageCompatibility.cpp
  src/logic/ProjectCommandService.cpp
  src/logic/ProjectController.cpp
  # EditorEngine 聚合会话调度，其实现留在 Logic，启动程序仅负责装配。
  src/logic/EditorEngine.cpp
  # 音频时间描述和视觉时钟仍属于编辑逻辑，底层播放能力通过 Audio 依赖获得。
  src/logic/audio/AudioTimelineDescriptor.cpp
  src/logic/audio/PlaybackVisualClock.cpp
  src/logic/PreviewDensity.cpp
  # 分拆的渲染实现共同生成快照几何，不在此引入 Game/UI 的高层界面依赖。
  src/logic/ecs/system/render/NoteRenderSystem.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Layout.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp
  # 普通物件和折线的几何规则分别维护，但最终仍链接为一个渲染系统。
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Types.cpp
  src/logic/ecs/system/render/NoteRenderSystem_Notes_Polyline.cpp
  src/logic/ecs/system/render/AudioObjectLabelRenderer.cpp
  # 独立采样与绑定音效共用标签绘制入口，避免两套文字布局规则分叉。
  src/logic/ecs/system/render/SampleRenderSystem.cpp
  src/logic/ecs/system/render/CanvasComponentRenderSystem.cpp
  src/logic/ecs/system/render/BackgroundRenderSystem.cpp
  # 背景频谱与背景图层分别编译，频谱几何不归入画布文字组件。
  src/logic/ecs/system/render/BackgroundSpectrumRenderSystem.cpp
  # ECS 变换、滚动缓存与命中特效作为同一静态库的一部分参与链接。
  src/logic/ecs/system/NoteTransformSystem.cpp
  src/logic/ecs/system/ScrollCache.cpp
  src/logic/ecs/system/HitFXSystem.cpp
  # 双缓冲同步的非内联实现也必须进入库，公开头不替代源文件登记。
  src/logic/BeatmapSyncBuffer.cpp)

# PUBLIC 保留这些依赖对 Logic 使用方的传播，测试不需要重建另一套依赖集合。 3rd_* 使用项目统一目标，预编译与源码来源的差异不应进入本模块。
# 保留 target 依赖才能继承必要包含路径和编译要求，不应替换为手写归档文件路径。 调整 PUBLIC 为 PRIVATE
# 前需检查公开头中使用的类型，不能只以当前实现文件编译通过为依据。
target_link_libraries(
  Logic
  PUBLIC Log
         MMM
         Common
         # 队列、实体和数学类型出现在逻辑接口中，依赖的使用要求向消费者传播。
         3rd_concurrentqueue
         3rd_entt
         3rd_glm
         # 压缩和图片辅助能力通过统一依赖目标接入，不写平台相关库文件路径。
         3rd_miniz
         3rd_stb
         Audio
         Config
         Runtime)
# 对外公开模块 include 根，使调用方统一使用 logic/... 路径。 src 目录不对消费者公开；跨模块调用应使用 include
# 下的接口，而非引用实现目录。
target_include_directories(Logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 所有测试通过顶层 helper 创建，统一输出目录与测试目标配置。 BUILD_TESTING 关闭时 helper
# 仍保留显式可构建目标，但把它们排除在默认构建之外。 是否能由 CTest 发现还依赖顶层测试启用状态，目标构建成功不等于 CTest 已执行。
# 以下测试链接同一份 Logic，不通过复制业务源文件建立行为可能分叉的测试版本。 测试名称直接引用可执行目标，由 CMake 解析具体构建配置下的程序位置。
# 不硬编码 bin 路径或平台扩展名，保持单配置与多配置生成器的目标寻址一致。

# 顶层 helper 注入公共配置隔离启动代码，防止测试调用正式保存入口时污染个人配置。 每个测试有独立配置根；直接运行使用启动代码，CTest
# 则通过目标启动器覆盖继承的 MMM_CONFIG_ROOT。 配置隔离目录与显式传入的产物目录不同，不能把两者当同一清理范围。

# TestConfigIsolation.cpp 已由顶层注入，各测试不要重复登记该公共启动源文件。

# helper 统一 Debug 与发布配置的测试输出位置，不依赖生成器默认子目录。 shared 预编译运行库按目标部署到独立测试程序旁；支持的 macOS
# 工具链链接限制也由顶层设置。 不应绕过 helper 另建普通 add_executable，否则会丢失这些运行与隔离约定。

# 多配置目标共用 tests/Logic 输出位置，运行前应先构建需要验证的具体配置。

# BUILD_TESTING=OFF 时仍配置目标与依赖，但顶层不开启测试发现；依赖仍须能被 CMake 正常解析。 模块不自行修改
# BUILD_TESTING，也不根据当前机器能否运行测试来删除目标。 配置阶段只登记命令；手动构建后需另行运行程序，cmake --build 不会自动执行
# add_test。

# helper 的第一个 Logic 参数用于测试目录归属，不会自动链接 Logic 库。 每个目标仍须显式声明
# target_link_libraries，不能把目录归属当作链接依赖。 可执行目标名必须在整个构建树唯一，不能在另一个模块复用同名目标。

# add_test 的 NAME 是筛选测试用的名称，COMMAND 则指向实际可执行目标。 COMMAND
# 使用目标名才能保留目标启动器与配置环境，不改成猜测的二进制绝对路径。

# 共享资源只读根通过 MMM_TEST_RESOURCE_DIR 传入，允许调用方选用其他谱面资源集。 默认皮肤与翻译不是共享谱面测试数据，不跟随
# MMM_TEST_RESOURCE_DIR 切换。 未传资源参数的目标由测试自身准备夹具，不为整个模块追加统一的资源命令行。

# 自建夹具不等于不用隔离配置，未传资源路径的目标也必须通过相同 helper 创建。
mmm_add_test_executable(Logic EditorClipboardProtocolTest
                        tests/EditorClipboardProtocolTest.cpp)
target_link_libraries(EditorClipboardProtocolTest PRIVATE Logic Log)
add_test(NAME EditorClipboardProtocolTest COMMAND EditorClipboardProtocolTest)

# 预览密度与时间线批量元数据分成目标，便于只运行受影响的数据处理回归。
mmm_add_test_executable(Logic PreviewDensityTest tests/PreviewDensityTest.cpp)
target_link_libraries(PreviewDensityTest PRIVATE Logic Log)
add_test(NAME PreviewDensityTest COMMAND PreviewDensityTest)

mmm_add_test_executable(Logic TimelineBatchMetadataTest
                        tests/TimelineBatchMetadataTest.cpp)
target_link_libraries(TimelineBatchMetadataTest PRIVATE Logic Log)
add_test(NAME TimelineBatchMetadataTest COMMAND TimelineBatchMetadataTest)

mmm_add_test_executable(Logic ProjectCompatibilityTest
                        tests/ProjectCompatibilityTest.cpp)
target_link_libraries(ProjectCompatibilityTest PRIVATE Logic Log)
add_test(NAME ProjectCompatibilityTest COMMAND ProjectCompatibilityTest)

# 谱面自动备份测试覆盖原生快照写入、单谱面数量轮转和路径隔离。 可写输出置于构建树，不允许将备份产物写入 tests/data 源资源目录。
mmm_add_test_executable(Logic BeatmapBackupServiceTest
                        tests/BeatmapBackupServiceTest.cpp)
target_link_libraries(BeatmapBackupServiceTest PRIVATE Logic Log)
add_test(
  NAME BeatmapBackupServiceTest
  COMMAND
    BeatmapBackupServiceTest
    # 传给程序的是可清理工作区，不是备份读取用的源码资源根。
    "${CMAKE_BINARY_DIR}/test_output/beatmap_backup_service")

# 放置吸附与拍号累计分别验证局部定位和跨 BPM 编号，保留独立失败入口。
mmm_add_test_executable(Logic ObjectPlacementSnapTest
                        tests/ObjectPlacementSnapTest.cpp)
target_link_libraries(ObjectPlacementSnapTest PRIVATE Logic Log)
add_test(NAME ObjectPlacementSnapTest COMMAND ObjectPlacementSnapTest)

# 拍号累计测试覆盖短 BPM 段与整拍边界。
mmm_add_test_executable(Logic BeatIndexTest tests/BeatIndexTest.cpp)
target_link_libraries(BeatIndexTest PRIVATE Logic Log)
add_test(NAME BeatIndexTest COMMAND BeatIndexTest)

# 项目分片存储测试覆盖职责拆分、完整往返和旧单文件迁移。 测试自行准备临时项目结构，这里无需依赖用户的现有项目目录。
mmm_add_test_executable(Logic ProjectStorageTest tests/ProjectStorageTest.cpp)
target_link_libraries(ProjectStorageTest PRIVATE Logic Log)
add_test(NAME ProjectStorageTest COMMAND ProjectStorageTest)

# 临时谱面包测试覆盖旧编码文件名通过 Info-ZIP Unicode Path 恢复为 UTF-8。
# 这里只传入共享资源根供读取，临时解包目录由项目打开流程管理。
mmm_add_test_executable(Logic TemporaryPackageUnicodePathTest
                        tests/TemporaryPackageUnicodePathTest.cpp)
target_link_libraries(TemporaryPackageUnicodePathTest PRIVATE Logic Log)
add_test(NAME TemporaryPackageUnicodePathTest # 资源根是运行参数，不用编译期宏固化到测试源码。
         COMMAND TemporaryPackageUnicodePathTest "${MMM_TEST_RESOURCE_DIR}")

# 音频引用测试与波形解码测试分离，当前目标归属于项目资源和引用管理职责。
mmm_add_test_executable(Logic ProjectAudioReferenceTest
                        tests/ProjectAudioReferenceTest.cpp)
target_link_libraries(ProjectAudioReferenceTest PRIVATE Logic Log)
add_test(NAME ProjectAudioReferenceTest COMMAND ProjectAudioReferenceTest)

# Malody 导出模式测试覆盖临时模式覆盖、Key 自动转换与会话元数据恢复。 该目标验证 MC 文件模式选择，不替代下面 IMD 资源包的打包回归。
mmm_add_test_executable(Logic MalodyExportModeTest
                        tests/MalodyExportModeTest.cpp)
target_link_libraries(MalodyExportModeTest PRIVATE Logic Log)
add_test(NAME MalodyExportModeTest COMMAND MalodyExportModeTest)

# MCZ 音频原点对齐测试覆盖正负相位、整数拍前导和安全失败。 相位模型测试不需要在 CTest 命令行传入音频文件。
# 此处验证时间转换，不将模型测试通过解释为音频编解码链路通过。
mmm_add_test_executable(Logic MczAudioOriginAlignmentTest
                        tests/MczAudioOriginAlignmentTest.cpp)
target_link_libraries(MczAudioOriginAlignmentTest PRIVATE Logic Log)
add_test(NAME MczAudioOriginAlignmentTest COMMAND MczAudioOriginAlignmentTest)

mmm_add_test_executable(Logic AudioTimelineDescriptorTest
                        tests/AudioTimelineDescriptorTest.cpp)
target_link_libraries(AudioTimelineDescriptorTest PRIVATE Logic Log)
add_test(NAME AudioTimelineDescriptorTest COMMAND AudioTimelineDescriptorTest)
# 描述层回归与设备播放启动分离，描述测试通过不能代替播放重启测试。

# IMD 资源包测试覆盖同名前缀、时间线拼装 MP3、zip 内容和 IMD 回读。 导出输出使用专用子目录，避免与自动备份测试共享可清理的工作目录。
# 两个输出目录都按 CMAKE_BINARY_DIR 定位，多份构建树不会共用源目录里的可写产物。
mmm_add_test_executable(Logic ImdPackageExportServiceTest
                        tests/ImdPackageExportServiceTest.cpp)
target_link_libraries(ImdPackageExportServiceTest PRIVATE Logic Log)
add_test(
  NAME ImdPackageExportServiceTest
  COMMAND
    ImdPackageExportServiceTest
    # 程序在运行前清理此目录，必须保持为该测试独占的输出路径。
    "${CMAKE_BINARY_DIR}/test_output/imd_package_export_service")

mmm_add_test_executable(Logic ProjectDirectoryWatcherTest
                        tests/ProjectDirectoryWatcherTest.cpp)
target_link_libraries(ProjectDirectoryWatcherTest PRIVATE Logic Log)
add_test(NAME ProjectDirectoryWatcherTest COMMAND ProjectDirectoryWatcherTest)

# 播放重启与后续视觉时钟测试独立登记，运输状态与时间推演不互相替代验证。
mmm_add_test_executable(Logic PlaybackRestartTest tests/PlaybackRestartTest.cpp)
target_link_libraries(PlaybackRestartTest PRIVATE Logic Log)
add_test(NAME PlaybackRestartTest COMMAND PlaybackRestartTest)

# Unlimited 空闲门控测试确保暂停时不争抢 Session 锁，实时工作仍保持无限更新。
mmm_add_test_executable(Logic UnlimitedIdleUpdateGateTest
                        tests/UnlimitedIdleUpdateGateTest.cpp)
target_link_libraries(UnlimitedIdleUpdateGateTest PRIVATE Logic Log)
add_test(NAME UnlimitedIdleUpdateGateTest COMMAND UnlimitedIdleUpdateGateTest)

# 连续播放时钟测试覆盖 block 内推演、校准、暂停、Seek、循环与 UI 补间。 视觉时钟单独链接成测试程序，便于与真实音频设备启动故障区分。
mmm_add_test_executable(Logic PlaybackVisualClockTest
                        tests/PlaybackVisualClockTest.cpp)
target_link_libraries(PlaybackVisualClockTest PRIVATE Logic Log)
add_test(NAME PlaybackVisualClockTest COMMAND PlaybackVisualClockTest)

# 主画布相机测试覆盖中键二维平移、逻辑像素投影和 Resize 比例保持。 相机坐标回归单独登记，不要求先启动编辑器窗口才能检查坐标变换。
mmm_add_test_executable(Logic CanvasCameraTest tests/CanvasCameraTest.cpp)
target_link_libraries(CanvasCameraTest PRIVATE Logic Log)
add_test(NAME CanvasCameraTest COMMAND CanvasCameraTest)

# 背景覆盖层和文字组件各有独立目标，避免只能通过整套界面检查底层几何。 背景测试检查快照生成的 CPU 几何，不代表 GPU 合成或皮肤视觉验收。
mmm_add_test_executable(Logic BackgroundRenderSystemTest
                        tests/BackgroundRenderSystemTest.cpp)
target_link_libraries(BackgroundRenderSystemTest PRIVATE Logic Log)
add_test(NAME BackgroundRenderSystemTest COMMAND BackgroundRenderSystemTest)

mmm_add_test_executable(Logic CanvasComponentRenderSystemTest
                        tests/CanvasComponentRenderSystemTest.cpp)
target_link_libraries(CanvasComponentRenderSystemTest PRIVATE Logic Log)
add_test(NAME CanvasComponentRenderSystemTest
         COMMAND CanvasComponentRenderSystemTest)

# 自动采样渲染测试确保 BGM 物件复用当前皮肤的 Note 纹理。 玩家绑定采样标签另由下一个目标覆盖，两类对象不共用同一条测试判定。
mmm_add_test_executable(Logic SampleRenderSystemTest
                        tests/SampleRenderSystemTest.cpp)
target_link_libraries(SampleRenderSystemTest PRIVATE Logic Log)
add_test(NAME SampleRenderSystemTest COMMAND SampleRenderSystemTest)

# 玩家物件标签测试确保绑定音效显示只在主画布启用并复用采样标签滚动。 标签目标不传默认皮肤文件参数，不能机械复制下方重叠遮罩测试的启动命令。
mmm_add_test_executable(Logic NoteBoundSampleLabelRenderTest
                        tests/NoteBoundSampleLabelRenderTest.cpp)
target_link_libraries(NoteBoundSampleLabelRenderTest PRIVATE Logic Log)
add_test(NAME NoteBoundSampleLabelRenderTest
         COMMAND NoteBoundSampleLabelRenderTest)

# 拖动虚影测试确保跨入 BGM 区的单键使用统一轨道投影并避开批注沟槽。
mmm_add_test_executable(Logic NoteDragGhostRenderTest
                        tests/NoteDragGhostRenderTest.cpp)
target_link_libraries(NoteDragGhostRenderTest PRIVATE Logic Log)
add_test(NAME NoteDragGhostRenderTest COMMAND NoteDragGhostRenderTest)

# 极大 SV 渲染测试确保超长 Hold 在进入 Batcher 前裁剪到画布范围。 独立目标直接检查 CPU 顶点，避免回归结果依赖具体 Vulkan
# 驱动。
mmm_add_test_executable(Logic ExtremeSvHoldRenderTest
                        tests/ExtremeSvHoldRenderTest.cpp)
target_link_libraries(ExtremeSvHoldRenderTest PRIVATE Logic Log)
add_test(NAME ExtremeSvHoldRenderTest COMMAND ExtremeSvHoldRenderTest)

# 多选拖动测试确保宽折线不会阻塞以单键为焦点的草稿区虚影更新。
mmm_add_test_executable(Logic GroupedDraftDragPreviewTest
                        tests/GroupedDraftDragPreviewTest.cpp)
target_link_libraries(GroupedDraftDragPreviewTest PRIVATE Logic Log)
add_test(NAME GroupedDraftDragPreviewTest COMMAND GroupedDraftDragPreviewTest)

# 音符渲染测试覆盖草稿物件独立配色与播放中的顶层重叠提示。 显式传入仓库内默认皮肤和翻译路径，避免使用机器上安装资源的不同版本。
# 参数保留引号，源码目录包含空格时仍应作为单个路径传给测试。 这两个位置是只读输入；新增输出参数时应另指向构建树下的 test_output。
# 默认皮肤入口取自源码树，不使用构建同步到个人配置目录后的资源副本。 翻译根与皮肤脚本是两个不同参数，手动执行该测试时也必须完整传入。
mmm_add_test_executable(Logic NoteOverlapMaskRenderTest
                        tests/NoteOverlapMaskRenderTest.cpp)
target_link_libraries(NoteOverlapMaskRenderTest PRIVATE Logic Log)
add_test(
  NAME NoteOverlapMaskRenderTest
  COMMAND
    NoteOverlapMaskRenderTest
    # 皮肤入口与翻译根目录保持各自独立参数，不拼接成一个资源路径。
    "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/skin.lua"
    "${CMAKE_SOURCE_DIR}/assets/translations")

# 交互拾取包围盒测试覆盖独立横纵缩放、中心保持和非法值回退。
mmm_add_test_executable(Logic InteractionHitboxScaleTest
                        tests/InteractionHitboxScaleTest.cpp)
target_link_libraries(InteractionHitboxScaleTest PRIVATE Logic Log)
add_test(NAME InteractionHitboxScaleTest COMMAND InteractionHitboxScaleTest)

# Logo 占位画布测试确保空谱面图标不继承轨道布局裁剪。
mmm_add_test_executable(Logic LogoPlaceholderScissorTest
                        tests/LogoPlaceholderScissorTest.cpp)
target_link_libraries(LogoPlaceholderScissorTest PRIVATE Logic Log)
add_test(NAME LogoPlaceholderScissorTest COMMAND LogoPlaceholderScissorTest)

# 编辑器级画笔预设继承和会话级鼠标路由在此回归，不要求真实画布窗口。 预设继承通过关闭、重建会话验证，不替代应用重启后的配置持久化测试。
mmm_add_test_executable(Logic BrushPaletteSessionStateTest
                        tests/BrushPaletteSessionStateTest.cpp)
target_link_libraries(BrushPaletteSessionStateTest PRIVATE Logic Log)
add_test(NAME BrushPaletteSessionStateTest COMMAND BrushPaletteSessionStateTest)

# 注册表快照生命周期测试确保关闭会话和替换图集后旧快照可回收。 此目标检查所有权边界，不需要真实图形设备完成一帧渲染。
mmm_add_test_executable(Logic RegistrySnapshotLifetimeTest
                        tests/RegistrySnapshotLifetimeTest.cpp)
target_link_libraries(RegistrySnapshotLifetimeTest PRIVATE Logic Log)
add_test(NAME RegistrySnapshotLifetimeTest COMMAND RegistrySnapshotLifetimeTest)

# 打击音效立体声测试覆盖普通物件固定声像和 Flick 全程线性移动。 此目标显式列出 Audio 链接依赖，以保留测试直接使用音频能力的职责声明。
# Audio 依赖保持 PRIVATE，测试程序不向其他目标导出音频使用要求。 此处只注册测试目标，不在 CMake 配置阶段打开音频设备或执行测试程序。
mmm_add_test_executable(Logic HitEffectStereoTest tests/HitEffectStereoTest.cpp)
target_link_libraries(HitEffectStereoTest PRIVATE Logic Audio Log)
add_test(NAME HitEffectStereoTest COMMAND HitEffectStereoTest)

# 旧 MMM 加载诊断测试覆盖一次载入内的事件生成、去重与空输入行为。
mmm_add_test_executable(Logic BeatmapLoadDiagnosticPublisherTest
                        tests/BeatmapLoadDiagnosticPublisherTest.cpp)
target_link_libraries(BeatmapLoadDiagnosticPublisherTest PRIVATE Logic Log)
add_test(NAME BeatmapLoadDiagnosticPublisherTest
         COMMAND BeatmapLoadDiagnosticPublisherTest)

# 协作观察者绑定测试确保访客不会把收到的房主快照回传为本地编辑。 两个目标分别覆盖变更来源与打开入口，不将项目切换来源混同为协作编辑来源。
mmm_add_test_executable(Logic BeatmapMutationObserverBindingTest
                        tests/BeatmapMutationObserverBindingTest.cpp)
# 验证用户入口随项目切换请求传递，不受取消或覆盖请求污染。 打开来源是独立测试目标，不能只构建协作观察者测试就认为它也已执行。 两个目标分别链接
# Logic，不因声明位置相邻就形成相互依赖或运行先后关系。
mmm_add_test_executable(Logic ProjectOpenOriginTest
                        tests/ProjectOpenOriginTest.cpp)
target_link_libraries(ProjectOpenOriginTest PRIVATE Logic Log)
add_test(NAME ProjectOpenOriginTest COMMAND ProjectOpenOriginTest)
target_link_libraries(BeatmapMutationObserverBindingTest PRIVATE Logic Log)
add_test(NAME BeatmapMutationObserverBindingTest
         COMMAND BeatmapMutationObserverBindingTest)
