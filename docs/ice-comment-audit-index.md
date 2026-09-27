# ICE 注释审计验收索引

## 用户要求提交并暂停（2026-09-09）

按用户指示保存当前审计阶段成果并暂停。完整 ICE 审计尚未完成，不能据注释率或现有测试通过宣称全面验收。最近代码版本的主项目与独立 ICE 构建、独立 CTest 1/1 均已通过，后续仅增加验证记录。新布局及接口尚未更新主项目预编译包，使用新头须配套重建库。

暂停时正在只读检查 FFmpegFileReceiver：构造中的缓冲 resize 返回值被忽略，start 循环会再次检查 resize；是否应在打开输出文件前完成缓冲准备尚未修改或验证。后续从此处继续，并保留已有异常、实时性、跨平台和预编译集成缺口清单。临时故障探针及其结果位于忽略的 build/test_output/comment-audit，本次提交不包含这些临时产物。

## TimeStretcher 首次无活动状态时静音及恢复验证（2026-09-09，当前）

本轮未修改生产代码。检查 process 在接收 pending 后先判断 m_currentState，缺少活动状态即清零输出并返回，不进入后端或上游。临时 stretcher-initial-state-failure-probe.cpp 在首次构造时使全部对齐申请返回 ENOMEM，确认基类缓冲及初始旁路状态输入缓冲两次失败，活动代际仍为零。绑定来源后处理不调用上游，八帧双声道输出全部静音；关闭注入显式 prepare 成功后，下一块安装非零代际、调用来源一次并产生有效样本。结果见 build/test_output/comment-audit/stretcher-initial-state-failure-results.json。

探针未注册永久 CTest，只验证当前 Linux 对齐申请失败及单位倍率恢复，不代表所有构造异常或非旁路状态。生产代码与上一轮构建、CTest 通过版本一致，本轮不重复构建和逐文件复扫。无新增或修改纳入统计的代码文件。

沿用上一轮全项目自维护业务、头文件、测试、构建及工具脚本逐文件统计，包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源：1018 个有效文件，ICE 95/95 注释率达标，其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## TimeStretcher 拒绝发布准备失败的候选状态（2026-09-09，当前）

ProcessingState 输入缓冲未达到请求容量时立即结束构造准备，不再创建后端；publish_prepared_state 在让出所有权前检查输入容量和非旁路 RStretcher::isValid。失败返回 false，候选在控制侧回收，不覆盖当前状态或已成功准备的 pending。期望倍率、控制配置及代际分配保持既有语义，不新增回滚或相同 setter 自动重试；调用方可通过 prepare 显式重试。首次状态准备失败的其他上层使用方式仍需继续审计。

临时 stretcher-publish-failure-probe.cpp 定点注入后端补零样本申请失败：设置非旁路音高和显式 prepare 均失败，处理后活动代际保持旧值且旧旁路输出仍为有效样本；随后成功准备 pending，再次失败的 prepare 不将其覆盖，下一块仍能安装此前成功候选。结果见 build/test_output/comment-audit/stretcher-publish-failure-results.json。探针未注册永久 CTest，不覆盖所有输入缓冲失败或参数重试组合。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过；主项目预编译包未替换。

本轮 src/ice/core/effect/TimeStretcher.cpp：322 注释行、740 代码行、30.32%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## RStretcher 工作缓冲失败形成不可处理状态（2026-09-09，当前）

构造检查补零与延迟丢弃缓冲的 resize 返回值，预热检查局部输入、输出的实际准备容量；失败不进入后续补偿或排空循环。新增 m_isPrepared 与 isValid 查询，未准备实例拒绝 process_into、drain_into、reset 和倍率更新；便捷 process/drain 沿原契约清零未产出部分。is_finished 对无效实例返回 true，避免零产出排尾循环永不结束，调用方须用 isValid 区分准备失败与正常结束。后端及已取得工作区由现有独占所有权在析构释放。第三方构造及指针向量仍可能抛异常，上层发布是否检查有效状态仍待审计；未宣称完整失败传播已完成。

临时 stretcher-workspace-failure-probe.cpp 包装现有 setMaxProcessSize 调用，在其完成后依次注入四个包装器对齐样本申请失败，避免将后端初始分配混为工作区失败。四模式均确认调用次数止于失败点、isValid 为 false、is_finished 为 true；reset/倍率更新不恢复，偏移接口返回零且不改已有输出，便捷接口返回零并清零输出。结果见 build/test_output/comment-audit/stretcher-workspace-failure-results.json。探针使用当前 Linux C++ 符号包装，未注册永久 CTest，不代表其他 ABI、真实系统耗尽或第三方内部故障。正常路径由独立 CTest 1/1 回归通过；主项目与独立 ICE 满线程构建、格式检查通过。新增状态改变类布局，新头须配套重建库，主项目预编译包未替换。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | 142 | 328 | 30.21% |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | 104 | 70 | 59.77% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## PitchAlter 校验上游输入块契约（2026-09-09，当前）

PitchAlter 在输入准备后及上游返回后使用 hasMatchingInput 检查格式、活动帧数、准备容量及全部声道指针。异常块使输出静音，不进入旁路混音或后端；准备前遗留的空声道表也不能再次交给上游。空块允许没有地址表，检查不分配、不增加共享引用计数。非空悬空指针无法被该检查识别，后端的惰性创建及逐块容量调整仍未实时化。

临时 pitch-input-contract-probe 分别使上游修改采样率、缩短活动帧和置空第二声道；修改前前两模式残留输出退出 1，第三模式 SIGSEGV，已禁用 core；修改后三模式均静音返回，空指针模式再次处理不再调用上游。既有缓冲分配失败与重试探针回归通过。四个执行结果见 build/test_output/comment-audit/pitch-input-contract-results.json。探针未注册永久 CTest，主要覆盖单位倍率路径，不代表完整变调行为。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮 src/ice/core/effect/PitchAlter.cpp：27 注释行、59 代码行、31.40%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## PitchAlter 拉取上游改用观察访问（2026-09-09，当前）

PitchAlter::process 使用现有 get_inputnode_observer 取得一次观察指针，替代条件判断与调用中两次按值获取 shared_ptr。上游仍由基类成员维持所有权，处理期间禁止控制侧替换或释放它；观察访问不增加新的并发能力。空上游的现有行为、单位倍率旁路和分配失败静音逻辑未改变。头文件及实现热路径说明同步移除已消除的共享复制缺口，但逐块容量调整与惰性后端创建仍待处理。

主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，既有 pitch-buffer-failure 探针重新编译链接后通过，覆盖失败不拉取及成功重试的旁路行为；格式检查通过。本轮没有为低风险观察访问替换新增镜像测试，也不宣称取得实时性能基准或完整并发证明。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/core/effect/PitchAlter.cpp` | 23 | 36 | 38.98% |
| `include/ice/core/effect/PitchAlter.hpp` | 38 | 31 | 55.07% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## PitchAlter 输入缓冲失败不再拉取上游（2026-09-09，当前）

PitchAlter::process 检查输入 AudioBuffer::resize 返回值，失败后清零输出并立即返回，不沿用旧块长推进上游。成功路径维持原有旁路累加及变调行为。实现文件自身头置于首位，移除未直接使用的 RubberBand 原始头，显式包含实际使用的项目类型及标准头。逐块 resize、共享所有权复制、首次后端创建等历史实时性缺口本轮未解决。

临时 pitch-buffer-failure-probe.cpp 在单位倍率下定点注入对齐申请失败，输出块大于默认输入容量。修改前失败后仍调用来源，退出 1；修改后确认失败一次、来源未调用、整个输出块为零，关闭注入后重试成功且来源调用一次、输出有效样本。结果见 build/test_output/comment-audit/pitch-buffer-failure-results.json。探针未注册永久 CTest，不覆盖非单位倍率后端或所有实时行为。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮 src/ice/core/effect/PitchAlter.cpp：23 注释行、36 代码行、38.98%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## 效果基类与混音总线准备状态包含分配结果（2026-09-09，当前）

IEffectNode::prepare 与 MixBus::prepare 将 AudioBuffer::resize 的结果写入 m_isPrepared，仅成功后清零准备缓冲。原实现只按参数有效性置位，重新准备分配失败后，短块仍可能沿用旧容量继续拉取上游。现在失败进入已有的静音拒绝路径，不在 process 中重试分配；后续控制侧 prepare 成功后恢复处理。原有 void prepare 接口不变。

临时 graph-prepare-failure-probe.cpp 为效果派生节点和 MixBus 绑定计数来源，先准备小块，再包装 posix_memalign 使大块准备失败，随后处理小输出块。修改前探针退出 1；修改后每种节点均确认分配失败一次、上游调用为零、输出全部静音，关闭注入重新准备后调用上游一次并输出有效样本。结果见 build/test_output/comment-audit/graph-prepare-failure-results.json。探针未注册永久 CTest，不代表所有节点或系统耗尽覆盖。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。PitchAlter、RStretcher 等其余分配调用点仍需继续审计。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/core/MixBus.cpp` | 116 | 262 | 30.69% |
| `src/ice/core/effect/IEffectNode.cpp` | 19 | 42 | 31.15% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## Linux AudioBuffer 首次申请失败与重试验证（2026-09-09，当前）

本轮未修改生产代码。新增临时 buffer-initial-failure-probe.cpp，针对带格式与非零帧数的首次构造，分别包装 malloc 使声道表失败、包装 posix_memalign 使样本失败。声道表失败时未申请样本；样本失败时已准备的临时表释放一次。两模式均保持请求格式、零活动帧、零准备容量，可写及只读 raw_ptrs 均为空，拒绝激活一帧；clear、clear_from(0)、自身空混音及激活零帧安全。关闭注入后相同对象 resize 成功，两声道八帧全部为零。两模式退出码均为零，见 build/test_output/comment-audit/buffer-initial-failure-results.json。

这补充了已有缓冲扩容失败之外的首次构造状态证据，不将请求格式仍存在解释为分配成功；构造接口没有 bool 返回，调用方仍须检查准备容量。探针未注册永久 CTest，未覆盖真实系统耗尽或非 Linux 分配路径。生产代码与上一轮构建、CTest 通过版本相同，不重复构建和逐文件复扫。

本轮无新增或修改纳入统计的代码文件。沿用上一轮全项目自维护业务、头文件、测试、构建及工具脚本逐文件统计，包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源：1018 个有效文件，ICE 95/95 注释率达标，其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## Linux 声道表申请失败通过返回值传播（2026-09-09，当前）

Linux AudioBuffer 声道表由 vector 改为 malloc/free 配对的独占存储，独立记录已准备的指针容量。扩容时先准备候选表，申请失败立即返回 false；对齐样本申请成功后才移交候选并更新元数据，sync_pointers 仅填充地址、不再分配。移动构造与赋值同时转移表容量并将源归零。空状态保留容量，但 raw_ptrs 依据准备帧容量决定是否暴露表：set_active_frames(0) 保留平面访问，resize(...,0) 返回空表。这一差异在首次空混音探针及 CTest 失败后修正，最终回归通过。非 Linux 容器实现不在本轮替换范围。

buffer-table-return-probe 包装 malloc，对三声道候选表定点返回空，确认 resize 返回 false、申请失败一次，旧格式、长度、容量、声道表、样本地址与内容保持不变，重试成功。样本申请失败探针、Linux/替代分支移动与空混音探针及存储复用探针全部重新编译后通过，共七个执行结果见 build/test_output/comment-audit/buffer-table-return-results.json。替代分支不是原生 Windows 验证；临时探针未注册永久 CTest。修正后独立 ICE 满线程构建及 CTest 1/1 通过，主项目构建、格式检查通过。

Linux 私有布局继续变化，消费者须配套重建，不能把新头与旧 ICE 库混用；主项目预编译包未更新。该修改补齐 Linux resize 的表分配返回路径，不代表其他模块或非 Linux 已满足无异常要求。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 155 | 339 | 31.38% |
| `src/ice/manage/AudioBuffer.cpp` | 53 | 121 | 30.46% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## Linux PCM 对齐存储以返回值传播分配失败（2026-09-09，当前）

追踪确认 AlignedAllocator 仅用于 AudioBuffer 的连续 float vector，不能作为未使用代码直接删除。现在用 AudioBuffer 私有 AlignedSampleStorage 独占对齐块，仍使用匹配的 _mm_malloc/_mm_free，但申请失败返回 false，外层 resize 不提交元数据。移除通用 AlignedAllocator 及其两处显式 throw，公开头不再包含 new。新存储保留连续前缀、增长部分清零、缩短及 clear 保留容量、移动后源归零和自移动保护；扩容先准备新块，失败保留旧地址及样本。它按请求容量增长，不承诺原 vector 的增长策略。声道指针表及非 Linux 标准容器仍可能分配抛异常，未宣称整个 AudioBuffer 无异常。

Linux AudioBuffer 私有布局改变，旧外部分配器模板被移除；仓内 Modules 和 ICE include/src/tests 未发现其他模板用户，但不保证未知外部用户兼容。使用新头的所有消费者必须与重建的 ICE 二进制配套，不能混用旧预编译包。主项目预编译包本轮未替换，其构建成功不能代替新布局集成验证；独立 ICE 满线程构建已重编译实际受影响源并成功。

临时 buffer-allocation-return-probe 定点包装 posix_memalign 返回 ENOMEM，确认 resize 完成且返回 false、故障一次，旧格式、活动帧数、容量、声道表、首样本地址及内容均保持，关闭注入后扩容成功。buffer-branch-contract 和 buffer-empty-mix 在 Linux 与 -U__linux__ 替代分支均通过，覆盖尺寸拒绝、部分清零、移动及空状态；替代分支不等同于真实 Windows 验证。buffer-storage-reuse 确认 32 字节对齐、缩短后增长清零、空状态后复用清零、移动后源可清零并重新分配。证据见 build/test_output/comment-audit/buffer-owned-storage-results.json 与 buffer-storage-reuse-results.json。临时探针未注册永久 CTest，不是系统耗尽或性能基准。主项目构建成功，独立 CTest 1/1 通过，格式检查通过。

本轮 include/ice/manage/AudioBuffer.hpp：151 注释行、332 代码行、31.26%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 线程回收空句柄与重复清理验证（2026-09-09，当前）

本轮未修改生产代码。核对 SDL_WaitThread 官方文档：https://wiki.libsdl.org/SDL3/SDL_WaitThread ，空 thread 为合法无操作；有效线程回收后句柄失效，且同一线程只能由一个等待者回收。因此当前 join 在回收后清空成员，stop/close 重复传入空句柄不是资源错误，无需增加防御分支。控制侧串行调用和禁止供数线程自等待的既有契约仍然适用，不据此宣称并发 stop 安全。

临时 sdl-join-ownership-probe.cpp 基于停止边界探针，包装 SDL_WaitThread 并调用真实函数；队列查询和节点处理两种停止交错下，stop 后连续 join、stop、close、close，非空线程等待均恰好一次，线程成员保持已回收，取消后的提交仍为零。两模式退出码为零，见 build/test_output/comment-audit/sdl-join-ownership-results.json。探针未注册永久 CTest，不覆盖自等待或多个控制线程同时清理。本轮生产源码与上一轮构建及 CTest 通过版本相同，不重复构建、测试或注释复扫。

本轮无新增或修改纳入统计的代码文件。沿用上一轮全项目自维护业务、头文件、测试、构建及工具脚本逐文件统计，包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源：1018 个有效文件，ICE 95/95 注释率达标，其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 供数阶段之间重新检查停止请求（2026-09-09，当前）

供数循环在队列查询后、节点处理后和交错提交前重新读取 relaxed 停止请求。查询期间观察到停止后不再推进图，处理期间停止后不继续转换及提交该块，转换结束也会再次检查。检查沿用已有请求标志和线程创建/join 的资源同步契约，不增加锁或等待。最后一次检查与 SDL 调用之间仍有窗口，不能承诺 stop 发起瞬间绝无新调用；已进入的节点或 SDL 调用也不可被该标志中断，stop 返回后的无访问保证仍来自 join。

临时 sdl-stop-boundary-probe.cpp 分别在包装队列查询及节点处理中等待控制侧停止请求。修改前两模式均在取消后继续提交并退出 2；修改后查询模式处理次数为零、节点模式处理一次，两者提交均为零且 stop 后线程已回收。测试等待仅在忽略目录的探针中，用于确定交错，不加入生产调用链。样本边界探针回归继续通过。结果见 build/test_output/comment-audit/sdl-stop-boundary-results.json。探针使用包装设备，未注册永久 CTest，也不代表完整并发状态空间覆盖。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：108 注释行、227 代码行、32.24%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 非有限样本提交前置为静音（2026-09-09，当前）

SDL 平面转交错时使用 std::isfinite，将 NaN、正负无穷转换为零，与现有 OpenAL 浮点路径一致；有限样本保持原值，包括超出单位幅度的值和负零。不改变图内平面存储，不增加逐块分配、日志或额外遍历，显式包含 cmath。这里只约束提交给 SDL 的输入，不保证后续设备格式转换保持所有有限极值。

临时 sdl-sample-boundary-probe.cpp 从节点输出八类样本：NaN、正负无穷、正负最大有限值、正负零和 0.25；左右声道采用相反顺序，在包装 SDL_PutAudioStreamData 中检查前八帧交错值及零符号。修改前退出 2，修改后退出零，一次提交中非有限值为正零、所有有限值及声道顺序保持，提交包装返回失败后线程退出。结果见 build/test_output/comment-audit/sdl-sample-boundary-results.json。探针未注册永久 CTest，使用当前工具链，不代表真实设备、所有浮点模式或性能压测。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：105 注释行、224 代码行、31.91%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 供数与重启拒绝空声道指针（2026-09-09，当前）

节点能够通过 AudioBuffer::raw_ptrs 改写声道观察指针，原有格式、活动长度及容量检查无法识别空平面。交错转换前新增整块声道指针验证，任一为空立即结束供数，不读取样本也不提交数据。start 同时检查指针，防止上轮留下的损坏状态进入下一轮清零或读取。检查只遍历声道表，不分配、不复制共享所有权；它不识别任意悬空的非空指针，节点仍须遵守缓冲生命周期契约。

临时 sdl-null-plane-probe.cpp 在节点处理中分别将首声道与第二声道置空。修改前首声道场景以 SIGSEGV（退出 -11）复现，已禁用 core 输出；修改后两模式正常退出，队列只查询一次、提交为零，stop 后 start 拒绝且设备恢复次数仍为一。结果见 build/test_output/comment-audit/sdl-null-plane-results.json。探针使用包装设备，未注册永久 CTest，不代表所有指针破坏或实际设备场景覆盖。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。NaN/无穷样本的 SDL 提交策略尚待继续审计。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：102 注释行、221 代码行、31.58%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 重开设备前不读取可变音频块（2026-09-09，当前）

open(deviceid) 的构造有效性判断由 m_buffer.num_frames() 改为不可变的 m_blockFrames。旧实现先读取活动帧数再 close/join，供数节点可同时修改该值；现在关闭旧线程前不访问其可变缓冲。活动帧数被节点置零后也能重新打开设备，不再将其误判为构造失败。设备重开不自动修复缓冲；start 仍严格拒绝被节点破坏的块契约，生命周期与源绑定仍要求控制侧串行操作。

临时 sdl-reopen-contract-probe.cpp 让节点分别将活动帧置零、修改采样率或扩容，供数退出并 stop 后重开设备。修改前零帧模式在重开断言退出 4；修改后三模式均可重开，旧流回收一次、最终关闭新流后合计回收两次；start 继续拒绝损坏块，没有第二次设备恢复，也没有提交损坏 PCM。结果见 build/test_output/comment-audit/sdl-reopen-contract-results.json。并发风险判断来自代码读写路径，不宣称探针或 ThreadSanitizer 动态复现；探针使用包装设备，未注册永久 CTest。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：98 注释行、211 代码行、31.72%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 设备枚举数组局部所有权守卫（2026-09-09，当前）

list_devices 在取得 SDL 枚举数组后立即用 unique_ptr 与 SDL_free 接管，替代函数末尾手动释放；显式包含 memory。正常返回和成功取得空列表时均释放一次，名称复制或容器增长引起栈展开时也不会遗漏数组。返回列表仍保存名称及 ID 副本，未改变设备排序或空名称回退。官方所有权契约已核对：https://wiki.libsdl.org/SDL3/SDL_GetAudioPlaybackDevices ，非空结果须由 SDL_free 回收。本轮只修复释放完整性，不宣称标准容器分配已不抛异常。

临时 sdl-enumeration-owner-probe.cpp 包装枚举、名称查询与释放：正常两设备检查名称和 Unknown Device 回退；非空零设备数组释放一次；空指针无释放。在名称查询包装函数中以标准容器越界制造栈展开，由 packaged_task 捕获，验证数组释放；修改前该场景退出 3，修改后四场景整体退出零。它是栈展开故障探针，不是实际 SDL 行为或真实内存耗尽测试；未加入生产异常处理，未注册永久 CTest。结果见 build/test_output/comment-audit/sdl-enumeration-owner-results.json。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：97 注释行、211 代码行、31.49%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## SDL 播放后端保留线程诊断并移除终端输出（2026-09-09，当前）

SDLPlayer.cpp 移除初始化失败、重复初始化及设备打开失败三处 fmt::print，同时移除无用 fmt/base.h 与 SDL_error.h 包含。失败返回及状态语义不变，不在库内格式化或输出诊断。公开头说明 SDL 调用失败后可在同线程立即查询 SDL_GetError；重复初始化和本地格式拒绝不设置新 SDL 错误，不能误用历史错误作为本次失败原因。已核对官方 SDL3 文档：https://wiki.libsdl.org/SDL3/SDL_GetError ，其错误存储按线程区分，成功调用不自动清除错误，跨后续 SDL 调用保存时须复制文本。

重新编译并运行现有临时 sdl-open-failure 和 sdl-backend-scope 探针：设备打开注入失败后原始诊断仍可查询，没有使用空流查询设备，设备 ID 为零且不能启动线程；初始化重复调用只取得一次音频引用，重复退出仅释放一次且没有全局 SDL_Quit。两个探针退出码均为零、标准输出均为空，见 build/test_output/comment-audit/sdl-silent-diagnostic-results.json。未注入 SDL_Init 自身失败，也不代表真实设备覆盖；临时探针未注册永久 CTest。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。主项目预编译 ICE 包未替换。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 95 | 209 | 31.25% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 58 | 62 | 48.33% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。诊断实现、演示程序的直接输出及显式异常等缺口仍在，完整 ICE 审计尚未完成。

## FFmpeg 工厂移除直接终端输出及异常缺口复核（2026-09-09，当前）

FFmpegDecoderFactory::probe 删除无音频流和未知时长两个 fmt::print，移除不再使用的 fmt/format.h。无音频流仍返回 false 并保留调用方元信息，未知时长仍发布零帧数；库不再为这两种状态输出媒体路径。没有新增错误查询接口，也没有改变候选元信息发布、资源守卫和时长估算逻辑。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过，格式检查通过。本轮为日志删除，不新增镜像实现的测试；现有实时测试并不专门覆盖工厂探测分支。

重新扫描 ICE include/src 的自维护 C++，仍有三个文件包含显式异常机制：AudioBuffer.hpp 的标准分配器 allocate 中两处 throw，src/diagnostics/AllocationTracker.cpp 的全局分配替换中一处 throw，CachyDecoder.cpp 的 future.get 异常捕获。不能简单将分配器失败改成返回空指针，也不能删掉 catch 后宣称失败契约不变；这些路径仍未通过异常规范审计。直接输出仍存在于 SDLPlayer.cpp、诊断实现及手动演示 main.cpp，未将本次局部清理扩大为全库日志完成。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp：69 注释行、155 代码行、30.80%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## FFmpeg seek 候选上下文失败诊断与回退（2026-09-09，当前）

seek(0) 准备候选解码上下文时，参数复制和 avcodec_open2 的负返回值接入 checkSetupResult，保留到 getLastSetupErrorCode。候选失败继续由局部 unique_ptr 回收，不破坏旧实例的有效状态、解码位置及已缓存 PCM；成功重试按既有契约保留历史错误码。本轮没有扩大参数或分配失败的诊断覆盖，也没有替换主项目预编译 ICE 包。

临时 ffmpeg-seek-candidate-probe.cpp 使用 build/test_output 下生成的递增 PCM WAV，在首批读取后分别定点注入候选参数复制和打开失败。修改前两模式均在诊断断言退出 5，其资源回收与续读断言已通过；修改后两模式通过：故障调用一次、候选上下文释放一次、seek 返回 false、实例仍有效，后续 32 帧逐样本等于未定位的对照实例，诊断为 -ENOMEM；关闭注入后 seek(0) 成功且保留历史诊断。既有重采样返回值三模式回归通过，五模式均无标准输出。证据见 build/test_output/comment-audit/ffmpeg-seek-candidate-results.json。临时探针未注册永久 CTest，未模拟真实内存耗尽，不代表所有编码格式覆盖。主项目与独立 ICE 满线程构建成功，CTest 1/1 通过，格式检查通过。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 227 | 499 | 31.27% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 24 | 24 | 50.00% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## FFmpeg 原有终端诊断改为整数查询（2026-09-09，当前）

移除 FFmpegDecoderInstance.cpp 的全部 fmt::print 与错误字符串构造，不在库内输出媒体路径、未知时长或准备失败。原有返回值宏通过成员 checkSetupResult 保存负返回值，无音频流保存 AVERROR_STREAM_NOT_FOUND；未知时长仍以零帧数表达。新增公开 getLastSetupErrorCode()，由调用方选择何时格式化及输出，未让独立 ICE 反向依赖主项目 Log 模块。该查询只覆盖原先打印的准备与定位诊断点，零不证明实例有效，也不覆盖所有参数、分配和 read 失败；成功操作不清除历史错误，头文件明确其范围与串行访问契约。公共包装对象布局未变，新增成员函数需由更新后的 ICE 二进制提供；主项目预编译 ICE 包本轮未替换。

扩展临时 ffmpeg-resampler-return-probe：初始化失败和 seek 重启失败均保留 -ENOMEM，失效、无写入和一次释放契约继续通过；真实初始化成功后合成正返回值的模式查询零，仍能解码。三种模式退出码均为零，标准输出均为空；上轮两种故障会输出 averr 文本。结果见 build/test_output/comment-audit/ffmpeg-setup-diagnostic-results.json。探针未注册永久 CTest，不代表所有准备错误、媒体格式或真实内存耗尽覆盖。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；格式检查通过。

本轮修改文件（相对 ICE）：

| 文件 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 226 | 498 | 31.22% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 24 | 24 | 50.00% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## FFmpeg 返回值宏与重采样失败状态验证（2026-09-09，当前）

本轮未修改生产代码。检查 AVCALL_CHECKRETB 的现有调用点：参数仅出现一次，辅助函数仅将负值判为失败；临时声道布局在传播 swr 配置错误前已释放。宏目前没有用于带 else 的无大括号分支，因此未为假设性调用方式扩大修改范围。错误辅助函数仍使用 fmt::print 向标准输出写入，且构造 std::string；日志统一及潜在分配异常仍待处理，不能据此次返回值验证认定完全合规。

临时 ffmpeg-resampler-return-probe.cpp 包装 swr_init/swr_free，使用既有 build/test_output 下 WAV：初始化首次返回 -ENOMEM 时实例失效；正常初始化后 seek(0) 的重采样重启返回 -ENOMEM 时 seek 失败且实例失效。两种失败后总帧数均为零，再次 seek 被拒绝，read 返回零且所有哨兵样本不变；每次故障调用仅执行一次，析构释放重采样上下文一次。第三种模式在真实 swr_init 成功后合成正返回值 1，初始化和 seek 均成功，读出两个左右一致的正样本。三种模式退出码均为零，结果见 build/test_output/comment-audit/ffmpeg-resampler-return-probe-results.json。正返回值是宏分类测试，不表示 FFmpeg 正常会返回 1；故障注入不等同于真实内存耗尽，临时探针未注册永久 CTest。

本轮只更新审计文档，无新增或修改纳入统计的代码文件；生产代码与上一轮构建及 CTest 通过版本相同，不重复运行构建和测试。沿用上一轮全项目自维护代码逐文件复扫范围及结果：包含业务、头文件、测试、构建、工具脚本及 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源；1018 个有效文件，ICE 95/95 注释率达标，其他 685 个不足 30% 文件见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## FFmpeg packet 分配失败立即终止准备（2026-09-09，当前）

初始化在 av_packet_alloc 失败后立即返回，不再继续申请无法使用的 frame。frame 分配失败时已取得的 packet 仍由实例析构统一回收，后续读取和定位沿用无效实例的拒绝契约。未改变成功路径的初始化顺序或解码行为。

临时 ffmpeg-container-allocation-probe.cpp 使用 build/test_output 下生成的单声道 WAV，并在媒体探测完成且本实例解码器打开后定点注入容器分配失败，避免误计探测内部的分配。修改前 packet 失败仍观察到 frame 申请，退出码 3；修改后 frame 申请为零。单独 frame 失败时 packet 回收一次，失败实例 read 不写样本、seek 返回 false；关闭注入后正常读出两个目标双声道帧，左右样本一致。五类前置非法输入探针回归通过。临时资源未写入 tests/data，探针未注册永久 CTest，不代表真实系统耗尽或所有分配点覆盖。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp：222 注释行、501 代码行、30.71%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## FFmpeg 失败实例所有权与无副作用读取验证（2026-09-09，当前）

本轮未修改生产代码。FFmpegDecoderInstance 已声明析构且持有 unique_ptr，当前不能隐式复制或移动；没有通过正常公开移动接口产生空 ffimpl 的状态，因此没有为不存在的移动路径增加分支。临时 ffmpeg-invalid-instance-probe.cpp 用类型特征静态断言确认四种复制/移动能力均禁用，并确认 IDecoderInstance 有虚析构。

探针对空路径、内嵌空字符路径、零声道、零采样率和 UINT_MAX 采样率直接构造实例，均报告无效；转交 unique_ptr<IDecoderInstance> 后查询总帧数为零，seek(0)/seek(SIZE_MAX) 返回 false，普通平面及空表读取均返回零，已有样本保持不变。包装 avformat_open_input 确认五类前置拒绝没有媒体打开调用。探针编译并运行通过，未注册永久 CTest；只验证初始化前置拒绝，不覆盖所有中途资源故障或完整媒体解码。生产代码与上一轮已构建、CTest 通过版本相同，本轮不重复运行这些检查。

本轮无新增或修改纳入统计的代码文件。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## FFmpeg 解码实例移除无用工厂依赖（2026-09-09，当前）

FFmpegDecoderInstance.cpp 不使用工厂类型，移除 FFmpegDecoderFactory.hpp 包含；自身头置于首位，项目头、第三方头与标准头分组，并显式补齐 cstddef、string_view。资源管理与解码函数实现未改变。具体后端仍在实现文件中完整定义，独占指针析构不依赖工厂头。

独立 ICE 满线程构建成功。Ninja 实际依赖图确认 FFmpegDecoderFactory.hpp 的翻译单元扇出从 3 降至 2，解码实例未通过其他路径重新引入该头；证据见 build/test_output/comment-audit/ffmpeg-factory-fanout-before/after.json。主项目构建成功，现有 CTest 1/1 通过。本轮没有新增只复述包含关系的测试，现有 CTest 不据此扩称为完整媒体解码验证。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp：220 注释行、500 代码行、30.56%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## 解码抽象接口前向声明音频格式（2026-09-09，当前）

IDecoderFactory 与 IDecoderInstance 均已有虚析构，检查未发现通过接口销毁的遗漏。二者仅以引用声明 AudioDataFormat，现以 struct 前向声明替代定义头；FFmpegDecoderInstance 也不存储按值格式，移除重复格式包含，并显式包含 cstddef 和 string_view。未改变方法签名、字段布局或解码行为，拥有后端的析构仍在完整类型可见的实现文件中定义。

四个相关公开头（两个抽象接口和两个 FFmpeg 派生接口）独立重复包含的语法编译通过，编译器 -H 确认均不再引入 AudioFormat.hpp。Ninja 依赖图显示整个独立构建依赖该格式头的翻译单元仍为 23 个，其他真实使用路径继续需要其定义，因此不宣称全局重编译扇出减少。证据见 build/test_output/comment-audit/decoder-header-dependencies.json 与 decoder-format-fanout-before/after.json。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.34 秒）；临时头编译探针未注册永久 CTest。

本轮 include/ice/manage/dec/IDecoderFactory.hpp：16 注释行、20 代码行、44.44%；include/ice/manage/dec/IDecoderInstance.hpp：20 注释行、16 代码行、55.56%；include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp：20 注释行、23 代码行、46.51%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## ICE 非空公开头统一使用 pragma once（2026-09-09，当前）

ALSource.hpp 是空占位文件，没有类或实现可供行为审计。随后检查公开头保护方式，将剩余 19 个使用宏保护的头统一为 #pragma once，移除对应首尾保护指令。修改前对 Modules、cmake 以及 ICE include/src/tests 检索保护宏，仅发现各自头文件内部引用；未修改类接口或函数实现。当前 include 下全部非空 .hpp 均具备 pragma once，空占位文件不据此宣称有实现或通过行为审计。

全部 37 个公开头独立重复包含的语法编译通过，正序和逆序组合包含通过。验证命令及结果见 build/test_output/comment-audit/header-guard-migration.json 与 header-guard-validation.json；仅覆盖当前 Linux 工具链，不等于全平台或所有包含排列验证。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；临时编译探针未注册永久 CTest。

本轮修改文件逐项统计：

| 文件（相对 ICE） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/config/config.hpp` | 18 | 25 | 41.86% |
| `include/ice/core/IAudioNode.hpp` | 15 | 12 | 55.56% |
| `include/ice/core/MixBus.hpp` | 102 | 103 | 49.76% |
| `include/ice/core/PlayCallBack.hpp` | 18 | 15 | 54.55% |
| `include/ice/core/SourceNode.hpp` | 168 | 174 | 49.12% |
| `include/ice/core/effect/Clipper.hpp` | 7 | 13 | 35.00% |
| `include/ice/core/effect/Compresser.hpp` | 39 | 51 | 43.33% |
| `include/ice/core/effect/GraphicEqualizer.hpp` | 75 | 49 | 60.48% |
| `include/ice/core/effect/IEffectNode.hpp` | 44 | 38 | 53.66% |
| `include/ice/core/effect/PitchAlter.hpp` | 37 | 31 | 54.41% |
| `include/ice/core/effect/filter/BiquadFilter.hpp` | 22 | 24 | 47.83% |
| `include/ice/execptions/buffer_error.hpp` | 5 | 9 | 35.71% |
| `include/ice/execptions/instance_build_error.hpp` | 5 | 9 | 35.71% |
| `include/ice/execptions/load_error.hpp` | 5 | 9 | 35.71% |
| `include/ice/manage/AudioFormat.hpp` | 9 | 10 | 47.37% |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 15 | 20 | 42.86% |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 19 | 16 | 54.29% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 8 | 16 | 33.33% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 20 | 22 | 47.62% |

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。头保护规则本轮已统一，但完整 ICE 行为、异常及其他规则审计仍未完成。

## OpenAL 移除只有读端使用的图锁（2026-09-09，当前）

全文件核对 m_sourceMutex 仅在 queueAudioBuffer 供数入口使用，基类 set_source 直接替换绑定且明确要求停止拉取后调用，没有对应写端锁。移除该私有锁及逐块加锁作用域，继续依靠唯一供数线程与 stop 等待完成后的换图契约；这不增加运行中并发换图能力。后端 alMutex 及图内部同步保持原有职责，不能由本次改动推断整条音频路径无锁。公开头删除无用 mutex 包含，实现显式包含 IAudioNode 定义。

四种损坏后换图重启、四种输出组合采样边界、双轮容量复用和线程创建失败探针均回归通过，公开头独立重复包含的语法检查通过。没有新增镜像实现的测试，也没有进行性能基准，故不报告定量加速。相关临时探针未注册永久 CTest。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.39 秒）；本轮 ICE 源码由独立构建验证，主项目仍使用预编译 ICE。

本轮 include/ice/out/play/openal/ALPlayer.hpp：97 注释行、63 代码行、60.62%；src/ice/out/play/openal/ALPlayer.cpp：458 注释行、1031 代码行、30.76%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 打开失败通过诊断接口返回而不直接打印（2026-09-09，当前）

删除设备打开失败、打开后设备错误、上下文创建失败、上下文绑定失败四处 fmt::print，并移除不再使用的 fmt/base.h。四个分支仍保留错误文本及已有库日志快照，调用方通过 getLastError 决定记录方式；默认设备回退的中间失败不再由此文件重复打印。本轮不改 OpenAL 库自身的 stderr 或回调诊断，也不声称全引擎日志整改完成。

临时 al-open-diagnostic-probe.cpp 分别包装四个失败阶段。修改前各分支返回失败且有诊断，同时 stdout 非空；修改后四种情形仍返回失败、诊断非空，stdout 均为零字节。名称自引用和关闭上下文失败探针回归通过，亦无 stdout。探针未注册永久 CTest，不使用实体设备。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过。

本轮 src/ice/out/play/openal/ALPlayer.cpp：459 注释行、1033 代码行、30.76%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 关闭上下文绑定失败提供诊断（2026-09-09，当前）

close 的上下文绑定失败分支原跳过源和缓冲操作并继续释放后端，但没有说明失败原因。现记录 bind closing context 阶段，通过既有控制侧收集器在关闭返回前生成文本；不查询缺少绑定时的 AL 错误，也不让诊断改变后续上下文和设备回收顺序。先前错误仍按既有优先级保留。

临时 al-close-context-failure-probe.cpp 使用 null 驱动，将扩展绑定入口设为关闭阶段失败。修改前诊断检查失败、退出码 1；修改后绑定失败一次、上下文销毁和设备关闭各调用一次，并提供阶段诊断，重复 close 不再增加回收调用且文本不变。调用计数不等价于第三方内部无泄漏证明。供数线程绑定失败、退队错误及重开错误探针回归通过；探针未注册永久 CTest，不使用实体设备。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.34 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：456 注释行、1038 代码行、30.52%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 供数线程上下文绑定失败保留诊断（2026-09-09，当前）

audio_thread_loop 的上下文绑定失败原只清运行标记，控制侧 stop 后仍没有错误。现在已有后端锁约束下保存静态操作名，通过延迟诊断收集器在控制侧格式化。该失败来自绑定入口返回值，不查询依赖当前上下文的 AL 错误，也不伪造驱动错误码；以 AL_NO_ERROR 表示没有提供 AL 码，收集器只输出操作失败说明。

临时 al-thread-context-failure-probe.cpp 使用 null 驱动，包装扩展查询，将线程绑定入口替换为只对非控制线程失败的测试实现。修改前停止后缺少绑定诊断，退出码 1；修改后单次失败自动停播、stop 后有阶段说明且不包含 AL_NO_ERROR。关闭注入后可重新启动和停止，旧诊断清空。上传错误、关闭错误及线程创建失败探针回归通过。探针未注册永久 CTest，不使用实体设备。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.35 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：454 注释行、1036 代码行、30.47%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 空间参数拒绝非有限输入（2026-09-09，当前）

set_spatial_parameters 对七个输入字段先整组检查有限性，任一 NaN 或正负无穷即写诊断并返回，不修改旧缓存或调用空间设置后端。原有有限负距离等边界归约保持不变，非法控制输入不会停止正在运行的播放器。公开接口补充拒绝及保留状态的说明。

临时 al-spatial-input-probe.cpp 使用 null 驱动，在正常空间配置和播放状态下依次注入七字段乘三种非有限值的 21 种组合。包装 alSource3f 确认无新的坐标写入，并检查运行保持、错误非空；关闭再开启空间模式后原坐标仍为 (2,0,0)，验证旧缓存保留。修改前第一项失败、退出码 1；修改后全部通过。四种输出组合采样边界及线程创建失败探针回归通过。探针未注册永久 CTest，不使用实体设备。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.39 秒）。

本轮 include/ice/out/play/openal/ALPlayer.hpp：99 注释行、65 代码行、60.37%；src/ice/out/play/openal/ALPlayer.cpp：450 注释行、1026 代码行、30.49%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 非有限样本静音及宽精度下混（2026-09-09，当前）

新增无分配的采样净化入口，将 NaN 和正负无穷视为静音；有限 float 保持动态范围，整型转换在净化后裁剪再 lrint。两个下混分支使用 double 累加后求平均，避免有限大值先在 float 求和阶段溢出；非有限声道仍计入原声道数，不放大剩余声道的权重。浮点立体声同样逐样本净化，整型立体声通过量化入口净化。

临时 al-sample-boundary-probe.cpp 使用 null 驱动，包装扩展查询切换 float/int16，并截取上传数据检查。修改前浮点单声道情形失败、退出码 1；修改后 float/int16 与 mono/stereo 四种组合均验证 NaN、正负无穷静音，同号最大有限值不会使浮点平均溢出，异号最大值下混相消，混合非有限与 0.5 样本保留原下混权重和预期量化结果。探针只检查提交样本，不证明实体音质或全平台浮点行为；未注册永久 CTest。交错容量复用与延迟错误探针回归通过。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.33 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：448 注释行、1019 代码行、30.54%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 重开成功不保留旧关闭错误（2026-09-09，当前）

open 原先在 close 前清理 m_lastError，旧后端退队错误会在 close 中再次写入，随后新设备成功打开也残留该文本。现将新打开操作的错误清理放在旧后端完全回收之后；独立 close 的诊断仍保留，非法设备名称提前拒绝的语义也不改变。输入副本仍在成员清理之前创建，保持名称自引用安全。

临时 al-reopen-error-probe.cpp 使用 null 驱动，注入旧后端退队错误再重新打开。修改前 open 成功但错误非空，退出码 2；修改后成功重开时错误为空，随后单独 close 注入失败仍可读关闭错误。关闭错误、名称自引用与内嵌空字符探针回归通过。探针未注册永久 CTest，不使用实体设备。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.34 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：442 注释行、1010 代码行、30.44%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 设备名称自引用先复制后清理（2026-09-09，当前）

open 的 string_view 可能直接借用 getOpenedDeviceName 或 getLastError 返回的成员字符串。原实现先清空这些成员、关闭设备，再复制输入，导致传给 C 接口的名称已被改写。现通过空字符校验后立即复制为独立 const string，再执行成员清理及设备切换；副本覆盖打开与诊断期间的名称生命周期。

临时 al-name-alias-probe.cpp 使用 null 驱动，并包装设备打开记录实际名称。修改前 open(p.getOpenedDeviceName()) 无法保持完整名称，退出码 1；修改后重新打开成功，后端收到原名称。另将诊断文本自身作为名称传入，验证成员清理不改变实际传给后端的输入；该诊断文本不是有效设备名，此项仅检查字符串生命周期。含空字符拒绝及线程创建失败探针回归通过。探针未注册永久 CTest，不使用实体设备。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.34 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：440 注释行、1010 代码行、30.34%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 拒绝内嵌空字符设备名称（2026-09-09，当前）

open(string_view) 在清理名称、重置日志和关闭旧设备前拒绝含内嵌空字符的输入，防止 C 接口截断后打开不同名称或默认设备。无效请求只写错误文本，原设备名称、运行状态与设备句柄保留。空 string_view 仍按既有语义请求默认设备，合法设备切换流程未改变；实现补齐 string_view 的直接包含。

临时 al-device-name-probe.cpp 使用 null 驱动先打开并启动播放器，再传入当前设备名加空字符后缀以及首字符为空的名称。包装设备打开/关闭调用确认两次非法请求均没有设备操作，原名称及运行状态保持，且返回失败并提供诊断。修改前第一项检查失败、退出码 1；修改后通过。线程创建失败及关闭诊断探针回归通过。探针未注册永久 CTest，不使用实体设备。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过。

本轮 include/ice/out/play/openal/ALPlayer.hpp：97 注释行、65 代码行、59.88%；src/ice/out/play/openal/ALPlayer.cpp：438 注释行、1010 代码行、30.25%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 构造前校验格式与上传整数边界（2026-09-09，当前）

ALPlayer 构造阶段在 PCM 分配之前拒绝零声道、零采样率、超出 ALsizei/ALCint 的采样率，以及超过 ALsizei 最大字节长度除以双声道 float 宽度的块长。该上限覆盖浮点与整型上传的最坏情况，避免后续有符号转换截断。准备未成功的对象 open 直接返回失败，不访问设备；start 仍保持原有后端完整性检查。

临时 al-format-boundary-probe.cpp 包装设备打开与 posix_memalign。修改前仅运行零声道对照，检查失败、退出码 1。修改后零声道、零采样率、UINT_MAX 采样率、UINT_MAX 块长、刚超过浮点上传帧数上限五类配置均拒绝，设备打开及观察窗口内 PCM 对齐分配次数均为零。未对修改前超大块申请运行探针，也未尝试分配接近合法上限的巨额缓冲。正常双轮容量复用和线程创建失败探针回归通过；探针未注册永久 CTest。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.39 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：436 注释行、1005 代码行、30.26%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 运行请求使用显式 relaxed 内存序（2026-09-09，当前）

逐项审阅 ALPlayer 的 34 处 m_running 访问，标记只控制供数、停止和控制操作的继续条件，不发布缓冲、设备或诊断文本。初始化状态由线程创建交接，停止后的回收依赖 SDL_WaitThread，设备访问及延迟错误由后端锁保护；外部生命周期调用仍须串行。现全部运行标记 load/store 显式使用 relaxed，成员和热路径 Doxygen 补充写入者、读取者及不可替代同步边界。暂停、空间模式和队列重建标记保留原有内存序，本轮不对其作合并推论。

线程创建失败、四种缓冲损坏后重启、供数延迟诊断、关闭错误和双轮容量复用探针均回归通过；静态检查确认 34/34 运行标记访问包含显式 relaxed。探针未注册永久 CTest，未执行弱内存序平台或 ThreadSanitizer 验证；这些运行结果仅支持路径回归，同步正确性的依据是上述生命周期与锁契约。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过。

本轮 include/ice/out/play/openal/ALPlayer.hpp：96 注释行、65 代码行、59.63%；src/ice/out/play/openal/ALPlayer.cpp：432 注释行、993 代码行、30.32%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 关闭返回前收集退队错误（2026-09-09，当前）

close 先调用 stop，再清队列与释放设备；清队列产生的延迟错误原发生在 stop 已收集记录之后，导致第一次 close 返回时没有诊断。现提取 collectPendingError，供 stop 与 close 共用；close 释放后端锁后收集其自身新增的错误，避免递归锁与锁内字符串分配。记录仅含静态操作名和错误码，设备销毁后不再调用 AL 查询。已有错误优先，重复 stop/close 保留诊断。

临时 al-close-error-probe.cpp 使用 null 驱动，打开设备后注入首次错误查询失败。修改前首次 close 后缺少 clear source queue 诊断，退出码 1；修改后立即可读，随后重复 close/stop 仍保持相同内容。上传与退队连续失败、线程创建失败探针同时回归通过。探针未注册永久 CTest，不使用实体设备。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.34 秒）。

本轮 include/ice/out/play/openal/ALPlayer.hpp：94 注释行、65 代码行、59.12%；src/ice/out/play/openal/ALPlayer.cpp：427 注释行、988 代码行、30.18%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 供数错误延迟到控制侧格式化（2026-09-09，当前）

checkALError 不再直接打印。控制侧调用仍写入诊断字符串；供数调用仅在已有后端锁内记录首个 AL 错误码和静态操作名，不分配文本、不增加原子变量或互斥量。stop 等待线程退出后取出记录再格式化，已有控制侧错误优先，次生退队错误不覆盖首个供数错误；新 start 清空待处理记录。公开错误查询说明已更新，调用方需 stop 后读取完整供数诊断。其他 OpenAL 初始化输出及日志钩子未在本轮改造。

临时 al-deferred-error-probe.cpp 使用 null 驱动，包装上传及 AL 错误查询注入失败，包装普通分配符号观测从错误返回到停源之前的窗口。修改前探针失败并捕获直接打印的上传错误。修改后该窗口普通分配计数为零、stdout 为空，stop 后可读上传错误；追加清队列失败仍保留首个上传错误。此观测不覆盖第三方 C 堆或所有错误入口，不能证明整条音频路径无分配。正常交错容量复用、线程创建失败探针回归通过；临时探针未注册永久 CTest。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.39 秒）；最后补充锁和诊断优先级注释后再次构建。

本轮 include/ice/out/play/openal/ALPlayer.hpp：92 注释行、64 代码行、58.97%；src/ice/out/play/openal/ALPlayer.cpp：423 注释行、982 代码行、30.11%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 故障重启恢复固定块契约（2026-09-09，当前）

在私有后端保存初次成功准备的固定帧数，start 回收旧供数线程后检查缓冲格式、长度、容量和各平面非空。发现损坏时在控制侧创建完整替代缓冲再移动提交，避免同尺寸 resize 快速返回保留被改坏的地址表。正常启停不重建，继续复用原存储；初次准备失败则拒绝启动。此检查不能判断任意非空悬空指针是否合法，且分配异常仍属待整改的历史通道。

临时 al-block-restart-probe.cpp 使用 null 驱动，先由异常节点破坏缓冲，自动停播后换为正常节点再启动。修改前采样率改写情形重启无法上传，退出码 3。修改后采样率、短块、声道数、空平面四种损坏均可重启，正常节点观察到初次准备的完整块长并恢复上传。正常两轮启动的交错容量复用探针和线程创建失败回归均通过。探针未注册永久 CTest，不使用实体设备。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；最后补齐直接包含后独立构建再次通过。

本轮 src/ice/out/play/openal/ALPlayer.cpp：410 注释行、941 代码行、30.35%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 交错缓冲在控制侧准备并跨重启复用（2026-09-09，当前）

将供数循环入口的两个临时 vector 移至私有 ALBackend。start 等待旧线程结束后，在发布运行状态前校验容量乘法上限并 reserve；供数线程仅借用，两种输出存储保留至后端销毁。最坏输出始终为双声道，多声道输入下混为单声道，因此按 frames * 2 准备，避免随输入声道数无谓扩大交错存储。分配失败仍可能通过历史异常通道离开控制侧，但不再发生于供数线程入口；本轮未完成全引擎异常整改。

临时 al-scratch-preparation-probe.cpp 使用 null 驱动，包装普通分配符号并按两种交错存储尺寸统计调用线程。修改前观察到非控制线程的匹配分配，退出码 2；修改后两轮启动/停止均正常上传，匹配分配在控制侧共两次、非控制侧为零，第二次启动不增加匹配分配。该统计按尺寸匹配，不覆盖其他尺寸、第三方 C 堆或整个音频图的分配，不构成全路径零分配证明。线程创建失败及五种块契约探针回归通过，临时探针未注册永久 CTest。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.34 秒）。

本轮 src/ice/out/play/openal/ALPlayer.cpp：404 注释行、918 代码行、30.56%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## OpenAL 上传前验证图输出块契约（2026-09-09，当前）

queueAudioBuffer 在图处理前保存块长与容量，处理后先检查固定播放格式、非零且未变化的活动长度、容量一致性与帧数上限，再验证声道表和全部平面非空。无效块直接返回 false，由既有供数循环停止并清队列；检查先于交错数组 resize、声道索引和上传，避免格式改写导致错速上传或空平面访问。此校验不证明任意非空指针的有效性，也不回滚上游图的副作用或自动恢复被破坏的缓冲。

临时 al-block-contract-probe.cpp 使用 OpenAL Soft null 驱动及合成图节点，包装 alBufferData 记录上传次数。修改前仅运行采样率改写情形，旧播放器仍上传且未自动停止，退出码 1。修改后采样率改写、活动长度缩至 1、声道数改为 3、首平面置空四种情形均自动停止且上传次数为零；正常未改写块仍持续运行并产生上传，五种情形均通过。不使用实体设备，未验证实际音质；探针未注册永久 CTest。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过。

本轮 src/ice/out/play/openal/ALPlayer.cpp：399 注释行、911 代码行、30.46%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。线程启动时的 scratch 分配等既有问题仍待审，完整 ICE 审计未完成。

## OpenAL 线程创建失败回滚运行状态（2026-09-09，当前）

ALPlayer::start 原在设置运行标记后构造 std::thread，创建失败会通过异常离开，留下虚假的运行状态。现使用 SDL_CreateThread，空句柄时清运行标记、保存诊断并返回 false；启动重试和 stop 使用 SDL_WaitThread 回收旧线程后立即清空句柄。SDL 入口桥接在实现文件中声明调用约定，公开头只前向声明线程句柄，不暴露 SDL 系统头。音频循环仍保留原有等待和缓冲准备逻辑，本轮不表示其热路径问题全部解决。

修改前临时 al-start-failure-probe.cpp 使用 OpenAL Soft null 驱动打开后端，再包装 pthread_create 注入 EAGAIN；packaged_task 保存旧异常，正常返回标志和运行状态检查失败，探针退出码 1。修改后 start 正常返回 false、运行标记为 false、错误非空，关闭注入可连续启动和停止两轮；包装 SDL_WaitThread 验证失败创建不等待，成功两轮各等待一次，重复 stop/close 不额外等待。探针通过，不使用实体音频设备，不验证音质或所有线程故障；未注册永久 CTest。当前环境 OpenAL 输出调度权限与 D-Bus 警告，null 驱动生命周期检查仍通过。

调用前核对 [SDL_CreateThread](https://wiki.libsdl.org/SDL3/SDL_CreateThread) 空指针失败契约及宏调用方式、[SDL_WaitThread](https://wiki.libsdl.org/SDL3/SDL_WaitThread) 回收后句柄失效契约。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.33 秒）。本轮 include/ice/out/play/openal/ALPlayer.hpp：91 注释行、64 代码行、58.71%；src/ice/out/play/openal/ALPlayer.cpp：395 注释行、901 代码行、30.48%。

全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成，后台分配、日志、历史异常及其他规则缺口继续待审。

## 分配统计在输出前读取计数（2026-09-09，当前）

AllocationTracker 的打印入口原在输出链中依次读取全局计数，输出自身发生的分配和释放可能进入同次报告。现先以 relaxed 读取两个局部值，再执行输出；重置也使用显式 relaxed store，因为计数不承担对象发布或工作线程同步。调用方仍必须先停止被测任务，两次读取不构成并发事务快照。历史 cout 与异常失败路径尚待整改。

临时 tracker-output-snapshot-probe.cpp 使用固定容量 streambuf，在每次写入时递增两个计数，模拟诊断输出引入的计数事件。修复前预置 17/11 后输出内容不再为 17/11，退出码 1；修复后报告保持 17/11，而全局计数继续增加，随后 reset 归零。此探针验证读取先于输出，不声称标准流在所有平台都会实际分配。跨翻译单元共享计数探针同时回归通过；两者均未注册永久 CTest。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过。

本轮 include/ice/tool/AllocationTracker.hpp：13 注释行、10 代码行、56.52%；src/diagnostics/AllocationTracker.cpp：38 注释行、43 代码行、46.91%。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1018 个有效文件，ICE 95/95 注释率达标，95 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## AllocationTracker 声明与诊断实现分离（2026-09-09，当前）

将 AllocationTracker.hpp 的计数定义、全局分配替换及诊断函数实现移至 src/diagnostics/AllocationTracker.cpp，公开头仅保留共享计数和函数声明。实现位于 ice/ 库源码收集范围外，只加入 testIonCachyEngin；引擎宿主和实时回归测试不会因包含该头自动替换分配入口。此后调用诊断接口的外部目标需要显式链接一份实现文件。原有异常与 cout 输出仅迁移，尚未消除，不能据此宣称全文件规则审计通过。

临时 tracker-multi 两个翻译单元在修改前链接出现三个全局替换函数重复定义。修改后链接并运行成功，在一个翻译单元设置计数、另一个调用 reset，验证观察同一组共享计数。nm 检查显示两个计数和两个诊断函数只存在于演示程序，在引擎静态库及 TimeStretcherRealtimeTest 中均不存在；证据见 build/test_output/comment-audit/tracker-multi-command.json 与 tracker-symbol-boundary.json。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过（4.33 秒）。未运行依赖实体音频设备的演示，临时探针未注册永久 CTest。

本轮逐文件统计：include/ice/tool/AllocationTracker.hpp 为 11 注释行、10 代码行、52.38%；src/diagnostics/AllocationTracker.cpp 为 36 注释行、39 代码行、48.00%；src/CMakeLists.txt 为 23 注释行、50 代码行、31.51%。已将新增实现加入统计输入，全项目 1018 个有效文件，ICE 95/95 注释率达标；统计涵盖自维护业务、头文件、测试、构建与工具脚本，排除第三方源码、构建/生成/代理文件、文档和纯资源。其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计仍未完成。

## 公开头独立及组合包含验证（2026-09-09，当前）

本轮未修改生产代码，对 include 下全部 37 个 .hpp/.h 公开头各生成独立翻译单元，每个头连续包含两次，以 Clang 22、C++26、AVX2 和独立 ICE 构建使用的预编译依赖头路径执行语法编译，37/37 通过。完整命令与诊断保存在 build/test_output/comment-audit/header-alone-results.json。此验证证明当前 Linux 工具链下无需调用方先包含其他业务头即可解析，不证明每个类型都显式包含其定义头，也不覆盖模板的全部实例化。

随后将同一组头按正序与逆序组合，末尾包含 immintrin.h 并调用全局 SIMD 函数，两组语法编译均通过；证据见 build/test_output/comment-audit/headers-combined-results.json。两种顺序不等价于穷举所有排列，也不验证跨翻译单元链接或 Windows。逐头词法检查未发现首个 namespace 声明之后仍有 include 指令，补充上一轮 AudioBuffer 系统头污染修复的同类扫描证据。临时探针未注册永久 CTest。

AllocationTracker.hpp 仍包含全局分配/释放替换、显式异常和 cout 诊断，并要求单翻译单元使用；本轮编译通过不能解除这些规则缺口。没有修改纳入统计的代码文件，因此未重复执行上一轮已通过的构建与 CTest。全项目自维护业务、头文件、测试、构建及工具脚本逐文件复扫包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，94 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 审计尚未完成。

## AudioBuffer 系统头移出业务命名空间（2026-09-09，当前）

AudioBuffer.hpp 原在 ice 命名空间内包含 mm_malloc.h 和 immintrin.h，先包含业务头时会将 SIMD 声明引入 ice，并使系统头的包含保护阻止后续恢复全局声明。现将 Linux 系统头统一移到全局作用域，删除自维护 include/src 中没有使用点的 ICE_RESTRICT 宏；AudioBuffer.cpp 同时将对应头置于首位，与标准库头分组。未改变混音算法、分配器和公开缓冲接口。

临时 buffer-include-order 两个探针在修复前分别验证业务头优先和 SIMD 头优先：前者访问全局 _mm256_setzero_ps/_mm256_movemask_ps 编译失败，后者通过。修复后两种顺序均编译并运行通过。buffer-empty-mix 和 buffer-branch-contract 探针在 Linux 与本机 -U__linux__ 备用分支均回归通过；备用分支不代表 Windows 原生验证，临时探针未注册永久 CTest。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；主项目使用预编译 ICE，独立构建编译本次源码。

本轮逐文件统计：include/ice/manage/AudioBuffer.hpp 为 143 注释行、316 代码行、31.15%；src/ice/manage/AudioBuffer.cpp 为 52 注释行、119 代码行、30.41%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，94 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。完整 ICE 行为及规则审计仍未完成，历史异常通道等问题继续保留为待审事项。

## 空缓冲混音避免访问未分配的声道表（2026-09-09，当前）

AudioBuffer 两个平台分支的 operator+= 原先在零帧时仍索引声道表，默认构造的双声道空缓冲因此访问空指针。现先检查格式和活动长度兼容性，再对零帧直接返回成功，无需访问存储；格式不匹配仍返回 false，非空混音保持原实现。

临时 buffer-empty-mix-probe.cpp 在修改前于 Linux 和本机 -U__linux__ 备用分支均触发段错误（退出码 -11，已禁用 core 输出）。修改后两个分支均通过默认空缓冲互混及自身混音、显式零容量、空缓冲格式拒绝、17 帧正常混音、仅活动长度归零后保留全部样本、resize(0) 后混音检查。备用分支验证不代表 Windows 原生测试，探针未注册永久 CTest。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建实际编译本轮 ICE 源码，主项目使用预编译 ICE。

本轮 include/ice/manage/AudioBuffer.hpp：143 注释行、315 代码行，31.22%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## Linux resize 快速返回同时检查准备容量（2026-09-09，当前）

AudioBuffer 的 Linux resize 原先只比较格式和活动长度，会在 set_active_frames 缩短后跳过同帧数的显式容量调整。现同时要求准备容量等于请求帧数才快速返回，保持 set_active_frames 只调整活动范围、resize 明确重设准备容量的区别；同格式同容量同长度仍保留原地址和样本。

临时 buffer-resize-active-capacity-probe.cpp 在修改前以 16 帧容量缩短活动长度到 5，再 resize(5) 时容量仍为 16，退出码 2。修改后容量为 5，激活 6 帧被拒绝；活动长度归零后 resize(0) 清空容量；重新准备 8 帧和重复相同请求保持正常。Linux 及本机 -U__linux__ 单独构建的备用分支均通过，不代表 Windows 原生验证。探针未注册永久 CTest。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本次源码，主项目使用预编译 ICE。

本轮 include/ice/manage/AudioBuffer.hpp：139 注释行、313 代码行，30.75%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## Linux 指针表分配失败及容量复用验证（2026-09-09，当前）

本轮未修改生产代码，补齐此前只静态审阅的 preparedPointers 分配失败分支。新增临时 buffer-pointer-table-failure-probe.cpp，包装 operator new 链接符号，在指针表分配时由标准分配器的超大元素数请求触发分配类异常，并用 packaged_task 保存。验证失败发生一次且 posix_memalign 样本分配次数为零，原格式、活动长度、容量、表及样本地址和所有原样本均保留；关闭注入后可成功调整。

在成功扩容后的容量内缩短再扩大，继续开启分配观测，普通分配与对齐分配次数均为零、样本地址保持，验证准备指针表的改动没有给此复用路径增加分配。探针通过。该注入模拟分配点失败，不是真实系统内存耗尽，不覆盖所有编译器或平台；未注册永久 CTest。生产代码与上一轮 CTest 通过版本相同，本轮没有重复运行 CTest。

主项目构建成功，独立 ICE 构建无待编译内容。全项目逐文件复扫包含自维护业务、头文件、测试、构建及工具脚本和 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，94 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。本轮无新增或修改纳入统计的代码文件。完整 ICE 行为及规则审计仍未完成，异常机制尚未全部移除。

## 非 Linux 缓冲扩容先准备后提交（2026-09-09，当前）

AudioBuffer 非 Linux resize 在需要分配时构造完整替代声道与指针表，复制各旧声道前缀，全部成功后交换拥有存储并更新元数据。失败不会改变原格式、长度、容量、样本和借用地址；新样本保持 vector 的零初始化语义。现有声道容量足够时直接调整长度，不复制整块音频或重新分配。真实扩容时峰值内存增加到新旧存储并存，只允许控制阶段使用；分配失败仍通过历史异常通道传播。

临时 buffer-alternate-allocation-probe.cpp 使用 Linux Clang 的 -U__linux__ 单独构建备用分支，在五个分配调用位置包装 operator new 的链接符号，利用标准分配器超大元素数请求触发分配类异常，由 packaged_task 保存。五个位置均保持原状态；成功路径保留双声道前缀、新声道及尾部为零；随后容量内缩短再扩大时分配调用为零且地址保持。该测试是合成分配点故障，不是真实内存耗尽，也不验证 Windows ABI 或运行库。未执行修改前运行对照。两个分支的既有契约探针回归通过；探针未注册永久 CTest。

主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立主构建使用 Linux 分支，非 Linux 修改由单独探针编译运行，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 138 | 311 | 30.73% |
| `src/ice/manage/AudioBuffer.cpp` | 52 | 119 | 30.41% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成，异常机制尚未全部移除。

## Linux 缓冲分配成功后提交元数据（2026-09-09，当前）

AudioBuffer 的 Linux resize 原先先更新格式、活动长度、跨度和容量，再调整连续样本存储，分配失败会留下元数据与旧存储不一致。现先在必要时用临时容器准备更大的指针表，再调整连续存储；成功后转移表并提交元数据，最后在已准备容量内刷新指针。指针表容量足够时不额外分配。保留既有连续前缀语义，未扩大为声道重排改造。零格式和零帧清理仍不分配。

临时 buffer-allocation-state-probe.cpp 包装 posix_memalign 注入 ENOMEM，以 packaged_task 接收既有异常而不在探针写 try/catch：修改前元数据已变化，退出码 2；修改后格式、帧数、容量、表地址、样本地址及原样本均保留，取消注入后调整成功。指针表自身分配失败未单独注入，该分支按临时容器生命周期静态复核。两个存储分支的尺寸、清零、移动和零声道探针回归通过；本轮只为 Linux 分支建立该失败状态保证，非 Linux 分配失败回滚尚未处理。历史分配异常仍存在，不能宣称满足全库无异常要求。

主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建重新编译受影响源码，主项目使用预编译 ICE。临时探针未注册永久 CTest。本轮 include/ice/manage/AudioBuffer.hpp：137 注释行、311 代码行，30.58%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 音频缓冲移除未使用的历史错误头依赖（2026-09-09，当前）

AudioBuffer.hpp 已不引用 buffer_error，移除其包含及 Linux 分支中重复的 AudioFormat 包含，头文件保护改为 pragma once。保留历史错误类型文件本身，不扩大本轮兼容性变更；AlignedAllocator 中两处 bad_alloc 抛出仍存在，不能把移除包含视为异常机制治理完成。

两个缓冲实现分支的尺寸拒绝、局部清零、移动及零声道契约探针重新编译运行通过。独立 ICE 满线程构建后的 Ninja deps 显示 buffer_error.hpp 影响编译单元 19→0，数据保存在 build/test_output/comment-audit/buffer-error-fanout-before/after.json，仅代表当前构建配置。主项目构建与现有 CTest 1/1 通过；独立构建编译本轮源码，主项目使用预编译 ICE。临时分支探针未注册永久 CTest，也不替代 Windows 原生验证。

本轮 include/ice/manage/AudioBuffer.hpp：138 注释行、304 代码行，31.22%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 音频缓冲双分支契约对照与直接包含（2026-09-09，当前）

AudioBuffer.hpp 无条件包含 algorithm，消除仅 Clang 显式包含而其他编译器依赖传递包含的差异；补齐 cstdint、new、utility 对应实际使用定义，项目头置于标准头前。new 头用于既有 bad_alloc 类型，不新增分配表达式；分配器中的历史 throw 尚未消除。本轮不改变缓冲行为。

先用 Linux Clang 单独编译 AudioBuffer.cpp 与零声道探针，并用 -U__linux__ 选择非 Linux 实现，回归通过。新增 buffer-branch-contract-probe.cpp 分别编译两个分支，验证超大尺寸拒绝保留地址和长度、活动五帧内的局部清零不影响十七帧容量尾部、移动构造/赋值及自移动保持地址、源对象清空和零声道归零，均通过。备用分支可执行文件不链接 Linux ICE 库，避免混合两种类布局；这只是本机分支逻辑验证，不代表 Windows ABI、运行库或系统行为验证。临时探针未注册永久 CTest。

主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建重新编译受影响源码，主项目使用预编译 ICE。本轮 include/ice/manage/AudioBuffer.hpp：138 注释行、308 代码行，30.94%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 零声道缓冲统一活动长度与容量（2026-09-09，当前）

AudioBuffer::resize 在 Linux 与非 Linux 分支均把零声道的请求帧数归零。修正 Linux 原先活动帧数非零但容量为零、非 Linux 原先无样本平面却保留非零容量的不一致状态。零声道仍是可建立的空状态，不改为报错；重新准备有效格式继续正常工作。公开说明同步约定零声道不能激活样本。

临时 buffer-zero-channel-probe.cpp 在当前 Linux 上修改前失败（退出码 2）；修改后验证普通非零及 SIZE_MAX 请求归为空活动长度/容量、激活一帧被拒绝、激活零帧成功、恢复立体声八帧及零声道构造，全部通过。非 Linux 修改仅静态复核，未声称完成该平台运行验证；探针未注册永久 CTest。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过。独立构建重新编译受 AudioBuffer 影响的业务源文件，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 138 | 307 | 31.01% |
| `src/ice/manage/AudioBuffer.cpp` | 43 | 99 | 30.28% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成，分配器异常等既有问题仍待处理。

## 线程池非模板实现移出公开头（2026-09-09，当前）

ThreadPool 的构造、析构、工作循环及停止回收移至现有 ThreadPool.cpp，提交模板保留在头中。私有 WorkerEntry 在实现中封装 SDL 调用约定，公开头只前向声明 SDL_Thread，不再包含 SDL_thread.h 或为硬件线程数查询直接包含 thread。保持对象成员布局与任务行为；使用线程池现在需要链接 ICE 的非模板实现，不再是可独立仅头文件使用的类型。构建已有该翻译单元，无需增加 CMake 目标。容器分配及其他异常风险仍未全部解决。

公开头重复包含及 sizeof 独立编译通过，编译器依赖输出不含 SDL_thread.h。Ninja deps 在当前独立 ICE 构建中记录该 SDL 头影响编译单元 5→4，前后数据见 pool-header-fanout-before/after.json。首个和部分线程创建失败、拒绝任务保留资源、健康池 64 个任务排空及混合参数探针全部通过；临时探针未注册永久 CTest。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；此后仅补中文说明并再次构建，没有重复运行测试。独立构建编译本轮源码，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/thread/ThreadPool.hpp` | 59 | 101 | 36.88% |
| `src/ice/thread/ThreadPool.cpp` | 32 | 74 | 30.19% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 流式公开接口直接依赖与容量契约（2026-09-09，当前）

StreamingDecoder.hpp 显式包含 cstddef、cstdint、span、vector，不再依赖 IDecoder 的传递包含提供这些声明；实现文件补齐对应直接依赖。补充 create 的格式、线程池及工厂参数契约，num_frames 的快照含义，decode 的每声道容量、末尾未覆盖区间和静音推进语义，以及 origin 保持容器不变且始终返回零的约定。不改变公开签名、对象布局或解码行为。

临时 streaming-header-probe.cpp 重复单独包含公开头并检查 sizeof，独立编译通过。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本轮源码，主项目使用预编译 ICE。本轮未重复执行媒体/故障注入探针，不将头文件编译结果视为运行行为新增覆盖；公开头探针未注册永久 CTest。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 118 | 240 | 32.96% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 38 | 31 | 55.07% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 流式极大帧位置与输出边界验证（2026-09-09，当前）

本轮未修改生产代码，审阅并验证现有范围计算。decode 先检查 start<total，再按 total-start 裁剪 wanted，故循环中的 start+copied 不越过长度上界；页偏移和单次拷贝受 PAGE_FRAMES-offset 限制。预读下一页先检查可加范围，工作线程跳过阶段按 target-m_cursor 裁剪，最终读按 EMPTY-m_cursor 裁剪，均避免直接对未经约束的请求做加法。后端返回量仍由已有 wanted 上界检查约束。

新增临时 streaming-frame-boundary-probe.cpp 使用总帧数 SIZE_MAX 的合成后端，实际仅分配固定页缓存：从 SIZE_MAX-3 请求八帧只返回三帧，输出尾部哨兵保持；start=SIZE_MAX、零帧及第二声道为空的请求返回零且输出不变；析构等待线程并释放后端一次。探针通过，不依赖真实巨大媒体，也未遍历到最大帧位置，不能用它证明所有长距离 IO 或线程交错均正确。探针未注册永久 CTest。

主项目构建成功，独立 ICE 构建无待编译内容。生产代码与上一轮通过 CTest 的版本相同，本轮没有重复运行 CTest。全项目逐文件复扫及摘要复核通过：统计包含自维护业务、头文件、测试、构建及工具脚本和 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，94 个摘要匹配；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。本轮没有新增或修改纳入统计的代码文件。完整 ICE 行为及规则审计仍未完成。

## 流式临时页在线程启动前准备（2026-09-09，当前）

StreamingDecoder::State 持有后台临时页，create 在所有缓存页准备后、启动线程前分配，run 仅借用并继续与淘汰页交换存储。短文件仍在首块分支直接返回，不分配临时页。首块、其余缓存页及临时页的 resize 返回值均检查，拒绝尺寸时不向后端传空表或发布未准备好的状态。底层分配器实际内存不足的异常行为仍未统一转换为返回值。

临时 streaming-scratch-preparation-probe.cpp 在已有启动失败/正常回收/短文件探针上包装 posix_memalign，按线程身份统计：修改前后台存在对齐分配，退出码 7；修改后后台计数为零，启动失败回收、正常 join 及短文件样本验证通过。这里只覆盖当前 Linux 合成解码器下的对齐分配，不代表第三方后端没有任何分配。新增 resize 拒绝分支仅静态检查，未注入尺寸拒绝或真实内存耗尽。探针未注册永久 CTest。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本轮源码，主项目使用预编译 ICE。

本轮 src/ice/manage/dec/StreamingDecoder.cpp：118 注释行、237 代码行，33.24%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 流式预读线程创建失败返回空对象（2026-09-09，当前）

StreamingDecoder 私有 State 使用 SDL_Thread 句柄替代 std::thread，静态 SDLCALL 入口转发现有 run。首块准备和初始请求完成后创建线程，空句柄立即返回失败，由局部解码对象释放后端及页缓存；正常析构继续先发布停止并唤醒，再 SDL_WaitThread 后释放状态。短文件不启动线程的分支保持不变。SDL 依赖仅加入实现文件，未暴露到公开头。缓冲分配和后端内部异常仍未因此全部消除。

按 [SDL_CreateThread](https://wiki.libsdl.org/SDL3/SDL_CreateThread) 与 [SDL_WaitThread](https://wiki.libsdl.org/SDL3/SDL_WaitThread) 官方接口核验创建及回收。临时 streaming-start-failure-probe.cpp 使用合成解码实例，在当前 Linux 包装 pthread_create 注入 EAGAIN：线程启动失败返回空对象、后端销毁一次、没有等待无效线程；正常启动对象析构等待一次后释放后端。四帧短文件在注入仍开启时创建成功且无新增线程尝试，双声道样本均为预期值。未执行修改前运行对照，未验证 Windows 或真实媒体的全部预读行为。探针未注册永久 CTest。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本轮源码，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 114 | 232 | 32.95% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 25 | 27 | 48.08% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 流式解码入口统一拒绝无效路径（2026-09-09，当前）

StreamingDecoder::create 在分配状态及调用工厂前拒绝空路径和嵌入空字符的路径，避免具体后端按 C 字符串截断输入。实现显式包含 string_view。公开头补充路径契约，并修正输出表无效时可能已清零的过期描述：当前实现已在任何清零前检查全部声道指针。

临时 streaming-path-validation-probe.cpp 使用返回空实例的计数工厂，修改前两种无效路径仍被转交工厂，探针退出码 2；修改后两种路径均不触发工厂，后续合法路径使工厂累计调用一次，探针通过。未访问真实媒体，不把入口验证描述为完整流式预读覆盖。探针未注册永久 CTest。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本次源码，主项目使用预编译 ICE。流式线程创建及缓冲准备的失败路径仍待继续审阅。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 108 | 225 | 32.43% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 25 | 27 | 48.08% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 缓存解码器同步拒绝零目标格式（2026-09-09，当前）

CachyDecoder::create 将零声道或零采样率检查从后台任务移至任务绑定前，直接返回空指针，避免已知无效格式仍发布异步策略。格式按值捕获后不可被调用方配置变化影响，后台无需重复相同检查；空工厂及实际媒体失败继续保留异步空结果语义。未增加第三方调用或文件系统检查。

临时 cachy-input-format-probe.cpp 在修改前以零声道格式创建仍得到策略对象，退出码 1；修改后零声道、零采样率及两者均零三种输入都返回空指针。计数工厂仅被后续合法格式调用一次，合法策略仍可消费空实例结果。路径校验及线程池拒绝探针回归通过。测试没有读取真实媒体，未注册永久 CTest。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本次源码，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 83 | 151 | 35.47% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 50 | 54 | 48.08% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 缓存解码入口拒绝空路径与嵌入空字符（2026-09-09，当前）

CachyDecoder::create 在复制路径及构造后台任务前检查 path.empty 和嵌入空字符，非法路径直接返回空指针，防止后端 C 字符串接口将路径截断为前缀。头文件同步说明输入与失败契约。这里不检查文件存在性，也不把合法路径等同于可解码媒体。

临时 cachy-path-validation-probe.cpp 使用计数工厂：修改前空路径仍返回策略对象，探针退出码 1；修改后空路径及嵌入空字符路径均返回空指针，合法路径仍返回异步策略并调用工厂，累计实例创建调用一次。工厂返回空实例，不访问真实文件；因此验证的是入口拦截和调用边界，不是所有后端的文件行为。线程池拒绝及异步空结果探针回归通过。探针未注册永久 CTest；现有 CTest 1/1 通过。主项目与独立 ICE 满线程构建成功，独立构建编译本轮源码，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 81 | 151 | 34.91% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 50 | 54 | 48.08% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 缓存解码器公开头收窄实现依赖（2026-09-09，当前）

CachyDecoder.hpp 将只按引用或智能指针出现的 ThreadPool、IDecoderFactory 改为前向声明，显式包含公开声明及按值成员需要的标准头，使用 pragma once。实现文件显式包含工厂、线程池、格式及实际使用的标准类型定义，不借公开头传递重依赖。没有修改按值存储或引入堆封装。

临时 cachy-header-probe.cpp 重复单独包含公开头并检查 sizeof，编译通过；编译器依赖输出确认不含 ThreadPool.hpp、IDecoderFactory.hpp 或 SDL_thread.h。独立 ICE 满线程构建后通过 Ninja deps 比较：ThreadPool.hpp 影响编译单元 4→3，SDL_thread.h 5→4，IDecoderFactory.hpp 6→6；工厂头仍有实际实现依赖，不宣称全部相关扇出下降。前后统计保存在 build/test_output/comment-audit/cachy-header-fanout-before.json 与 after.json，仅代表当前独立 ICE 构建。主项目构建及现有 CTest 1/1 通过；主项目使用预编译 ICE。头文件探针未注册永久 CTest。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 79 | 149 | 34.65% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 50 | 54 | 48.08% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 缓存解码器传播线程池提交拒绝（2026-09-09，当前）

CachyDecoder::create 在提交后检查 future.valid，任务没有被接收时直接返回空指针，不再创建持有无效结果的策略对象。AudioTrack::create 已有 m_decoder 为空时返回空音轨的分支，本轮仅静态核对该传播路径。已接收任务的媒体成功与失败仍在消费时确定，未把所有异步空结果改为同步失败。get_data 中原有无效 future 防御检查保留。

临时 cachy-rejected-task-probe.cpp 通过 pthread_create 失败构造停止池，修改前 create 仍返回对象，探针退出码 1；修改后返回空指针。健康池提交空工厂任务仍返回非空策略，首次读取帧数为零，验证既有异步空结果语义保持。未注入实际媒体解码或对 AudioTrack 做运行级验证；探针未注册永久 CTest。主项目与独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译当前源码，主项目使用预编译 ICE。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 79 | 137 | 36.57% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 48 | 51 | 48.48% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成，CachyDecoder 既有异常机制仍需后续处理。

## 停止线程池在绑定前拒绝结果任务（2026-09-09，当前）

ThreadPool::enqueue 在构造闭包和 packaged_task 前持锁检查停止标志，启动失败的池直接返回无效 future，不移动函数对象或参数中的独占资源。绑定仍放在锁外，避免用户定义的复制/移动操作重入队列时死锁；实际入队保留持锁复查。正常提交增加一次短临界区，仍属于低频后台 API。该保证针对进入 enqueue 时已停止的池，不提供与析构并发调用的生命周期保护；按值接收函数的 enqueue_void 也不因此具有相同的参数保留保证。

临时 pool-rejected-ownership-probe.cpp 在失败池提交 unique_ptr，修改前提交被拒绝但原指针已空，退出码 7；修改后独占参数与独占函数对象均保持原值。首个和第三个线程创建失败两种模式通过，包含既有精确回收、健康池恢复与 64 个任务析构排空检查。混合参数探针回归通过。探针未注册永久 CTest，现有 CTest 1/1 通过。主项目与独立 ICE 满线程构建成功，独立构建编译本轮源码，主项目使用预编译 ICE。

本轮 include/ice/thread/ThreadPool.hpp：75 注释行、152 代码行，33.04%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 线程池部分启动失败回收（2026-09-09，当前）

ThreadPool 工作线程改用项目既有 SDL 线程接口，以空句柄处理创建失败，避免 std::thread 创建失败的异常通道。构造先 reserve 句柄容器，再启动线程；中途失败复用 stopWorkers，在锁内发布停止、唤醒全部等待者、逐一等待并清空句柄。失败池保留停止状态，enqueue 返回无效 future、enqueue_void 返回 false。正常析构沿用同一流程，仍完成已接收任务。工作循环移到私有成员，SDL 入口只转发池地址。容器分配、标准锁及任务执行的既有异常风险没有因此全部消除。

接口核验：[SDL_CreateThread](https://wiki.libsdl.org/SDL3/SDL_CreateThread) 失败返回空句柄，[SDL_WaitThread](https://wiki.libsdl.org/SDL3/SDL_WaitThread) 等待并释放线程资源。本轮公开头引入 SDL_thread.h，使用该头的独立消费者需要 SDL 头与链接依赖；ICE 已有该依赖。未修改第三方源码。

临时 pool-start-failure-probe.cpp 在当前 Linux 包装 pthread_create 注入 EAGAIN，覆盖首个线程失败和第三个线程失败，包装 SDL_WaitThread 验证分别等待零个和两个已创建线程。失败池拒绝两种提交，析构无重复等待；随后健康池完成结果任务及 64 个普通任务，析构等待四个线程，全部通过。混合参数探针回归通过。未执行修改前启动失败运行对照；Windows 路径未经运行验证。探针未注册永久 CTest。检索 ICE 内部提交调用点仅 CachyDecoder，其 get_data 已检查无效 future。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本轮源码，主项目使用预编译 ICE。

本轮 include/ice/thread/ThreadPool.hpp：72 注释行、148 代码行，32.73%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 线程池混合引用与独占参数正确转发（2026-09-09，当前）

ThreadPool::enqueue 原有绑定在参数不能全部作为左值调用时移动整个元组，导致 int& 与按值 unique_ptr 同时出现的合法任务无法实例化。现保留优先左值调用的兼容分支，在需要消费参数时通过索引序列按提交参数的值类别分别转发：原左值访问衰减后的存储副本，原右值允许移动；不把隐式副本改成外部引用。显式 std::ref 继续承担借用语义。头文件补齐 std::size_t 所需 cstddef，并按项目约定改用 pragma once。

临时 pool-mixed-arguments-probe.cpp 的混合参数用例在修改前编译失败（invoke_r 无匹配，日志保存在同目录）；修改后编译和运行通过，结果为 8 且提交侧原值仍为 3。扩展验证独占参数、仅右值可调用对象、显式引用与 void 返回，全部通过。探针未注册永久 CTest；现有 CTest 1/1 通过，不将既有音频测试视为模板场景覆盖。主项目及独立 ICE 满线程构建成功，独立构建重新编译线程池及其依赖源文件；主项目使用预编译 ICE。线程池构造中的分配及线程启动失败、其他既有规则缺口仍未完成处理。

本轮 include/ice/thread/ThreadPool.hpp：62 注释行、137 代码行，31.16%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 初始化缺句柄诊断与接口负错误码分离（2026-09-09，当前）

open_encoder 与 open_resampler 将负返回码和成功码但缺少必要句柄的检查拆分。前者仍通过 FFmpeg 原始错误码报告原因；后者使用明确的缺上下文、格式或转换器描述，避免在失败消息中拼入成功码对应文本。资源清理及初始化成功条件保持不变。

临时 receiver-missing-handle-probe.cpp 通过包装器分别注入容器与重采样分配返回零但句柄为空、返回 EIO 两类场景，共四项通过；前两类检查具体诊断，负值场景比较 av_strerror 的原始错误文本。重采样包装签名核对 [FFmpeg 官方文档](https://ffmpeg.org/doxygen/trunk/group__lswr.html)。缺输出格式的半初始化清理探针及正常五帧 WAV 导出回归通过。缺句柄是注入的接口异常状态，不代表真实库通常如此返回；本轮没有运行修改前探针对照，也不将静态判断描述为前后运行对照。探针未注册永久 CTest。

主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译本轮 ICE 源码，主项目使用预编译库。本轮 src/ice/out/io/FFmpegFileReceiver.cpp：370 注释行、824 代码行，30.99%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件接收端独立原子标志使用 relaxed（2026-09-09，当前）

审阅 m_running 与 m_stopRequested 全部读写：前者仅用于跨线程状态展示及同步配置重入拒绝，后者仅传递取消请求，均不发布普通成员或资源所有权。13 处 load/store 显式使用 memory_order_relaxed，成员及访问函数说明写入者、读取者和同步边界。start/close 的普通资源访问仍要求串行；观察 running=false 或发送 stop 均不代替等待 start 返回，open 仍会清除旧停止请求。

重新编译并运行配置重入及下一轮配置、音源处理中停止、正常五帧 WAV 导出探针，全部通过；静态检查确认 13 处访问均显式 relaxed。内存序依据来自独立标志不承载数据发布的接口契约，探针不是并发正确性的完整证明，本轮未运行 ThreadSanitizer 或性能基准，不宣称可量化提速。临时探针未注册永久 CTest。主项目及独立 ICE 满线程构建成功，现有 CTest 1/1 通过；主项目使用预编译 ICE，独立构建编译本次源码。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 368 | 816 | 31.08% |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 116 | 69 | 62.70% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 缺输出格式的半初始化上下文安全清理（2026-09-09，当前）

open_encoder 已检查缺失 oformat，但 close 原先在对应失败路径直接读取 oformat->flags。现先检查 IO 句柄及格式存在性，缺格式时仍回收残留显式 IO，已知 NOFILE 容器保留原处理边界。另修正 FIFO 容量注释：setter 已验证 int 上界，不能继续声称未检查。

临时 receiver-partial-context-probe.cpp 注入已分配但缺 oformat 的上下文：修改前触发 SIGSEGV（退出码 -11，禁用 core 输出），修改后 open 返回失败且多次 close 加析构只释放一次上下文。扩展模拟 IO 句柄场景，关闭包装器确认只关闭一次，随后上下文只释放一次。该异常状态通过包装器构造，不代表真实 FFmpeg 分配接口通常返回该状态；测试覆盖的是既有显式检查后的清理一致性。探针的上下文分配/释放签名按 [FFmpeg 官方核心接口文档](https://ffmpeg.org/doxygen/trunk/group__lavf__core.html) 核验。正常五帧真实 WAV 导出通过，探针未注册永久 CTest。

主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译了本次 ICE 源码，主项目使用预编译 ICE。本轮 src/ice/out/io/FFmpegFileReceiver.cpp：363 注释行、815 代码行，30.81%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 运行期间拒绝配置接口同步重入（2026-09-09，当前）

set_target_frames、set_block_frames 与 set_progress_callback 在运行期间保留原配置。避免进度回调缩减预算后把部分输出报告为完成，也避免音源重入调整缓冲或回调替换自身导致正在执行的对象失效。运行标志检查不构成配置互斥锁，仍禁止与 start 并发配置；关闭后可以正常更新下一轮配置。公开头同时修正 FIFO 缺资源的过期返回值说明，使其与现有实现一致。

临时 receiver-config-reentry-probe.cpp 在进度回调中修改预算和块大小，修改前原定五帧提前完成，探针退出码 1。修复后扩展验证回调替换请求被忽略，三次块处理保持 2、2、1 帧，原回调收到三次进度；第一轮结束后重新配置为一帧并替换回调，第二轮成功。正常五帧 WAV 及处理期间停止探针回归通过。探针未注册永久 CTest，不据此声称配置接口支持并发访问。主项目和独立 ICE 满线程构建成功，现有 CTest 1/1 通过；独立构建编译当前 ICE 源码，主项目继续使用预编译库。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 360 | 814 | 30.66% |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 114 | 69 | 62.30% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码取包按阶段验证终止状态（2026-09-09，当前）

send_frame 将是否发送结束帧传入 drain_packets：普通输入阶段只接受 EAGAIN 作为本轮取包结束，发送空帧成功后的最终排空必须收到 EOF。状态不匹配保留原始 FFmpeg 错误，不能继续写正常 trailer，也不通过等待或盲目重试掩盖状态错误。依据 [FFmpeg 官方 send/receive 状态机说明](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)，最终排空应持续取包至 EOF，EAGAIN 表示仍需要新输入。

临时 receiver-drain-state-probe.cpp 包装发送、接收及写尾接口，最终排空注入 EAGAIN：修改前错误地成功，探针退出码 1；修改后返回失败且 trailer 调用为零。补充普通阶段提前 EOF 场景，同样返回失败且不写尾。正常五帧真实 WAV 导出及音源处理期间停止的探针回归通过。这些属于异常状态注入，不能据此宣称真实编码器普遍存在该故障；探针未注册永久 CTest。主项目与独立 ICE 满线程构建、现有 CTest 1/1 通过；主项目使用预编译 ICE，独立构建编译了本轮修改。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 354 | 811 | 30.39% |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 111 | 69 | 61.67% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 音源处理返回后响应导出取消（2026-09-09，当前）

FFmpegFileReceiver::start 在音源 process 返回后重新读取停止标志，已观察到取消时不再将当前块交给 write_buffer，也不增加进度；后续统一记录停止错误并清理。音源已经推进的内部状态不回滚，检查不提供对处理函数或编码调用的强制中断，也不保证检查后才发生的取消能阻止本块提交。

临时 receiver-process-stop-probe.cpp 通过公开接口绑定音源，在 process 内调用 stop，验证 start 返回失败、音源调用一次、frames_written 为零且错误文本非空。修改前退出码 1，修改后通过；正常五帧真实 WAV 导出探针也通过。探针保存在忽略的 build/test_output/comment-audit，未注册永久 CTest。主项目与独立 ICE 满线程构建通过；现有 CTest 1/1 通过。主项目使用预编译 ICE，独立构建才编译本轮源代码。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 351 | 811 | 30.21% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件逐项清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 无效导出格式在音频分配前拒绝（2026-09-09，当前）

新增匿名命名空间输入格式检查，构造、块配置和 open_encoder 共用非零声道、正采样率及 int 上界约束。无效格式不分配 AudioBuffer，保留空状态并由 open 提供错误文本，防止明显无效配置仍先分配固定块存储。合法格式的实际内存不足处理仍由底层分配器决定，不宣称构造完全无异常。

临时 receiver-invalid-format-allocation-probe.cpp 包装 posix_memalign，对零采样率与 UINT32_MAX 采样率依次构造和设置块大小。修改前有对齐分配，退出码 2；修改后零次对齐分配且 open 返回错误。该探针针对当前 Linux 分配路径，不涵盖字符串等所有分配。正常五帧真实 WAV 导出回归通过。探针未注册永久 CTest；主项目和独立 ICE 满线程构建、格式与差异检查通过，现有 CTest 1/1 通过（4.38 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：349 注释行、808 代码行，30.16%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件接收端直接依赖与公开头自包含（2026-09-09，当前）

FFmpegFileReceiver.hpp 显式包含按值格式 AudioFormat，整理项目头与标准头顺序；实现文件补齐 IAudioNode、AudioBuffer、AudioFormat、IReceiver 及 atomic/cerrno/cstddef/filesystem/functional 的直接依赖，第三方定义置于标准头之前。构造说明明确能力验证与分配失败边界。未改变行为或第三方调用签名。

临时 receiver-header-self-contained.cpp 仅重复包含该公开头并检查类型大小，独立编译通过。主项目及独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.33 秒）。未重复运行编码故障探针，不将本轮头文件检查扩展为全库依赖治理已完成。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 344 | 802 | 30.02% |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 111 | 69 | 61.67% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码能力查询保留原始错误（2026-09-09，当前）

select_sample_format 与 select_sample_rate 改为 std::expected 返回值，将成功格式/采样率与失败错误码分离。查询失败传递原码，无候选或无效能力项传递 EINVAL；open_encoder 通过 set_ffmpeg_error 保存完整诊断，只在已检查成功的分支解引用，不使用 value 的异常访问。默认块大小复查为固定 65536，并非未经限制的全局动态值，本轮未修改默认块配置。

重新编译格式与采样率能力表探针，保留既有选择和拒绝场景，并在查询失败模式比较 av_strerror 生成的原始错误文本，全部通过。探针未注册永久 CTest。主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.38 秒）。本轮未重复运行真实导出探针。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：340 注释行、793 代码行，30.01%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件编码缺失资源不再返回成功（2026-09-09，当前）

write_converted_to_fifo 仅保留零帧无操作成功，负帧或正帧缺资源报告失败；encode_fifo 和 send_frame 缺少上下文时保存错误返回 false。finish_encoding 对已打开且未写尾的链路检查重采样、FIFO、编码器、容器、流和包完整性，避免缺资源仍写正常尾部。未打开及已写尾的幂等早退保留，失败诊断使 close 跳过补尾重试并释放资源。

重新编译运行正常五帧导出、零容量输入消费、FIFO 容量边界及负排空长度四组探针，全部通过。新增缺私有资源分支按调用链与清理顺序静态审阅，未直接破坏内部句柄进行运行注入；不能将现有探针通过描述为新增全部分支覆盖。探针未注册永久 CTest。主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.40 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：338 注释行、786 代码行，30.07%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FIFO 排空状态与编码时间戳上界（2026-09-09，当前）

encode_fifo 每轮只获取一次队列长度，负值保存错误返回 false，零值才作为排空结束。分配帧和移出 FIFO 前检查 m_nextPts 非负且不超过 INT64_MAX-frameSamples，阻止有符号累计溢出；帧数已由前置分支保证为正。原有成功发送与失败后的不可回滚语义不变。

临时 receiver-fifo-read-state-probe.cpp 在真实 FIFO 写入后将长度查询包装为负，验证导出失败、进度为零且有诊断；普通真实五帧 WAV 导出回归通过。时间戳极限分支依据从零累加及相加前范围检查审阅，未运行超长导出或直接注入内部计数。探针未注册永久 CTest。主项目和独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.37 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：334 注释行、777 代码行，30.06%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码 FIFO 累加容量溢出保护（2026-09-09，当前）

write_converted_to_fifo 在相加前验证 queuedFrames 非负且不超过 INT_MAX-frame_count；无效状态保存错误并退出，不依赖溢出后的值交给 FFmpeg 处理。正新增帧数由既有入口检查保证。依据 [FFmpeg FIFO 容量接口](https://www.ffmpeg.org/doxygen/trunk/group__lavu__audiofifo.html)，容量单位为每声道样本数，参数使用 int。

临时 receiver-fifo-range-probe.cpp 包装队列查询和扩容接口，分别注入负队列长度、INT_MAX 残留加一帧、零残留加一帧，确认前两者不扩容、后者请求容量一且扩容失败正确传播，均不增加导出进度。普通真实五帧 WAV 导出探针重新编译并通过。没有在旧实现执行有符号溢出，也没有实际分配巨型 FIFO；探针未注册永久 CTest。主项目及独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.38 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：333 注释行、767 代码行，30.27%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 重采样零输出容量仍消费输入（2026-09-09，当前）

write_buffer 不再在 swr_get_out_samples 返回零时直接报告成功。保留一个样本的有效输出存储并如实传递容量，仍调用 swr_convert 接收输入；实际零输出继续按内部延迟处理。负查询值仍返回失败。依据 [FFmpeg 转换与容量契约](https://www.ffmpeg.org/doxygen/trunk/group__lswr.html)，容量估计不等于输入已经消费。

临时 receiver-zero-capacity-probe.cpp 包装首次正输入的容量查询使其返回零，转换仍使用真实 swr_convert，并累计实际提交的输入帧数。修改前五帧预算中首个四帧块被跳过，探针退出码 1；修改后实际提交五帧且导出成功。普通四帧加一帧真实 WAV 导出回归也通过。未解码输出逐样本比较，未注册永久 CTest。主项目及独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（测试 4.32 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：331 注释行、762 代码行，30.28%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件导出验证源节点输出块契约（2026-09-09，当前）

write_buffer 在读取声道表前核对缓冲格式、活动帧数和容量，源节点改变固定块契约时保存错误并返回 false，调用方不增加该块进度。避免缩短活动区间仍按旧预算读取，以及减少声道后按原声道数索引。遵循 IAudioNode 固定块约束，不将修改活动帧数当成合法短读。不能撤销源节点内部已发生的越界访问或已创建的部分输出文件。

临时 receiver-source-contract-probe.cpp 注入零活动帧、采样率变化、重建为单声道三种情形。修改前安全运行零活动帧场景仍报告导出成功，探针退出码 1；修改后三种场景均返回失败，源仅调用一次且导出进度为零。重新编译运行正常四帧加一帧的真实 WAV 导出探针，通过。未注册永久 CTest。主项目与独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.32 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：331 注释行、762 代码行，30.28%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件导出块配置提交与缓冲失败处理（2026-09-09，当前）

set_block_frames 拒绝零值和超过 int 上界的帧数；先确认 AudioBuffer::resize 成功再更新 m_blockFrames，避免尺寸拒绝后配置与容量不一致。start 每轮检查 resize 返回值，失败保存诊断并在拉取源及增加进度前结束循环。公开参数说明同步更新。实际内存不足引发的底层容器异常仍未由 bool 检查解决。

临时 receiver-block-range-probe.cpp 设置四帧块，随后提交 SIZE_MAX 和零值，再真实导出五帧 WAV；源节点确认两次拉取分别四帧和一帧，最终进度为五帧，探针通过。未单独注入循环 resize 失败，未注册永久 CTest。主项目与独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.37 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 330 | 757 | 30.36% |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 111 | 68 | 62.01% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码候选声道布局结构验证（2026-09-09，当前）

select_channel_layout 对生成的默认布局及遍历到的候选调用 av_channel_layout_check，结构无效时释放临时布局并返回 EINVAL，避免仅凭正声道数把数量与掩码不一致的布局复制到编码器。依据 [FFmpeg 布局校验契约](https://ffmpeg.org/doxygen/trunk/group__lavu__audio__channels.html)。此校验仍以能力表指针及存储有效为前提，不保证编码器支持布局与其他参数的任意组合。

临时 receiver-layout-validity-probe.cpp 在原五种能力查询场景外增加双声道掩码配三个声道的无效候选；修改前退出码 2，修改后六种场景通过，无效候选不进入编码器打开。重新编译运行真实 WAV 与路径保护探针，通过。默认布局失败出口仅代码检查，未单独注入。探针未注册永久 CTest；主项目和独立 ICE 满线程构建、格式与差异检查通过，现有 CTest 1/1 通过（4.33 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：329 注释行、754 代码行，30.38%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码声道布局查询错误传播（2026-09-09，当前）

select_channel_layout 不再将查询失败或非空但无可选项的能力表当作默认布局许可。查询失败释放临时 desiredLayout 并返回原始错误码，无可选项返回 EINVAL；只有查询成功且 configs 为空时使用默认布局。匹配声道数和首项回退行为保留。对新增失败出口逐项检查临时布局释放，依据 [FFmpeg 布局释放契约](https://ffmpeg.org/doxygen/trunk/group__lavu__audio__channels.html)。未完整验证能力项内部布局结构的有效性。

临时 receiver-layout-query-probe.cpp 覆盖查询失败、全部支持、零项表、立体声匹配、单声道回退五种场景，检查一次查询、错误场景不打开编码器、有效场景传入正确声道数；全部通过。重新编译运行真实 WAV 打开关闭与路径保护探针，通过。未注册永久 CTest；没有运行泄漏检测器，资源释放证据为代码出口检查。主项目和独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.38 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：327 注释行、746 代码行，30.48%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码采样率查询失败与无效表处理（2026-09-09，当前）

select_sample_rate 区分查询失败、成功且不限制取值、非空但零项表；失败或无有效候选返回零，遍历遇到负采样率同样拒绝。正常表仍精确匹配优先，否则选首个候选；不会用输入值掩盖空能力表。open_encoder 在取得临时布局前检查采样率，禁止零值进入编码器时间基。沿用已核对的 FFmpeg 能力查询空指针契约；本轮不更改调用签名。错误文本仍未携带原始能力查询错误码。

临时 receiver-rate-query-probe.cpp 覆盖查询失败、全部支持、零项表、精确匹配、首项回退、负项六种情形，检查每次只查询一次、拒绝场景不打开编码器、有效场景实际传入 48000 或 22050。六种场景通过，重新编译运行真实 WAV 打开关闭与路径保护探针也通过。探针未注册永久 CTest。主项目及独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（测试 4.33 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：325 注释行、738 代码行，30.57%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 编码采样格式查询失败不再假定支持（2026-09-09，当前）

合并采样格式能力查询为一次，删除按候选重复查询的 supports_sample_format。查询负返回值、非空但零项的能力表返回 NONE；成功且空指针按 FFmpeg 契约表示全部支持，选择 FLTP。正常表仍按既有浮点优先顺序选择，无常用格式则采用表首项。open_encoder 在取得临时声道布局前拒绝 NONE，避免错误出口新增布局泄漏。依据 [FFmpeg 能力查询契约](https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html)。错误文本目前不包含原始查询错误码，采样率查询的错误策略仍待复查。

临时 receiver-format-query-probe.cpp 包装能力查询和编码器打开，覆盖查询失败、支持全部、零项表、含 S16/FLTP 的候选表。每次仅查询一次，失败与零项不打开编码器，支持全部与候选表传入 FLTP；探针通过。重新编译运行真实 WAV 打开关闭与路径保护探针，通过。未注册永久 CTest。主项目、独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.32 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：323 注释行、733 代码行，30.59%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件导出输入采样率上界（2026-09-09，当前）

FFmpegFileReceiver::open_encoder 在创建上下文前拒绝超过 int 最大值的输入采样率，保留已有零值和零声道拒绝。select_sample_rate 的窄化前提现在由唯一调用入口保证，避免无符号高位值变成负采样率进入编码或重采样初始化。未更改合法值的编码器能力回退策略，也不代表任意正值都被编码器支持。

临时 receiver-rate-range-probe.cpp 包装上下文分配接口，验证零、INT_MAX+1、UINT32_MAX 在分配前拒绝；修改前退出码 2，修改后通过。正常默认采样率仍进入上下文分配。重新编译运行真实 FFmpeg 路径拒绝及正常 WAV 打开关闭探针，通过。探针未注册永久 CTest；主项目与独立 ICE 满线程构建、格式及差异检查通过，现有 CTest 1/1 通过（测试 4.32 秒）。

本轮 src/ice/out/io/FFmpegFileReceiver.cpp：329 注释行、752 代码行，30.43%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 文件导出拒绝嵌入空字符路径（2026-09-09，当前）

FFmpegFileReceiver::open_encoder 在 UTF-8 路径转换后、格式选择和 FFmpeg 文件创建前拒绝空路径及嵌入空字符路径，保存错误并返回 false。避免 C 字符串截断后覆盖前缀路径对应的其他文件。此检查不解决合法路径上的一般覆盖或后续编码失败回滚问题。

临时 receiver-path-nul-probe.cpp 在忽略的审计输出目录创建哨兵文件，传入前缀后附 NUL 和 suffix.wav 的路径。修改前探针退出码 2，前缀文件被替换为 78 字节 RIFF 文件；修改后 open 返回 false 且哨兵完整保留。空路径拒绝与合法 WAV 路径打开/关闭也通过。使用真实 FFmpeg 编码链路，未注册永久 CTest，未触碰用户资源目录。

主项目与独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.34 秒）。本轮 src/ice/out/io/FFmpegFileReceiver.cpp：328 注释行、751 代码行，30.40%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 交错缓冲在启动前分配（2026-09-09，当前）

交错存储从供数线程局部 vector 移到实例 unique_ptr，使用 malloc/free 配对且由智能指针拥有；start 在恢复设备和创建线程前准备固定容量，空分配返回 false，重启复用，析构先 join 再释放。供数循环仅借用指针并逐样本填满，沿用固定块检查，提交字节数使用已验证边界的 int。移除此处后台 vector 分配不代表整个音频图或 SDL 内部无分配；构造平面 AudioBuffer 的分配异常仍待处理。新增成员改变类布局，使用者需与当前库一起重新编译。

临时 sdl-interleaved-allocation-probe.cpp 在播放器打开后包装 malloc 强制失败，验证仅一次分配被拒、start 返回 false、没有运行请求或线程句柄、没有恢复设备，取消故障后可重试启动并关闭。重新编译运行供数失败、析构清理、线程创建失败探针，均通过。探针无实体设备，未注册永久 CTest。主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.33 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 93 | 214 | 30.29% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 54 | 62 | 46.55% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 线程创建失败返回状态（2026-09-09，当前）

SDLPlayer 以 SDL_CreateThread 和 SDL_WaitThread 替代 std::thread 的创建与回收，增加 SDLCALL 静态入口适配；线程句柄由实例持有，join 等待并置空，stop 与析构继续统一回收。创建返回空时清零运行请求并返回 false，不进入 std::thread 构造异常路径。采用官方 [创建契约](https://wiki.libsdl.org/SDL3/SDL_CreateThread) 与 [等待及释放契约](https://wiki.libsdl.org/SDL3/SDL_WaitThread)。类布局改变，使用者需与当前库重新编译。恢复设备仍早于创建线程，失败不会撤销既有队列播放；缓冲及容器分配等潜在异常未由本轮解决。

临时 sdl-thread-create-failure-probe.cpp 包装 pthread_create 返回 EAGAIN，通过真实 SDL 线程创建实现验证连续两次 start 均返回 false、运行状态为 false、无待回收句柄，close 只销毁一次流。另重新编译运行暂停恢复、供数故障重启、析构、异常缓冲重启拒绝四组探针，全部通过。注入只验证 Linux 当前工具链，Windows CRT 宏适配依赖官方 SDL_CreateThread 宏，本轮未构建 Windows。探针无实体设备，未注册永久 CTest。

主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.32 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 90 | 209 | 30.10% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 52 | 57 | 47.71% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 宿主共存实测与私有成员命名（2026-09-09，当前）

SDLPlayer 剩余五个私有成员改为 m_playFormat、m_buffer、m_currentDevice、m_audioStream、m_audioThread，保留类型、成员顺序和公开方法，不改变行为。公共 sdl_inited 和既有公开方法命名仍未迁移，不能据此宣称全部接口规则已达标。

临时 sdl-host-coexist-probe.cpp 链接真实 SDL，以 SDL_AUDIODRIVER=dummy 执行，无包装接口：宿主先取得音频和事件引用，ICE 初始化再退出后两者仍存在；释放宿主音频引用后音频退出、事件保留；ICE 单独取得再释放音频引用时同样保留宿主事件。使用 [SDL_WasInit 官方状态查询](https://wiki.libsdl.org/SDL3/SDL_WasInit)，主线程串行验证，通过。此测试补足上一轮仅包装调用计数的证据，不涉及实体设备发声或热插拔；未注册永久 CTest。

主项目与独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.38 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 86 | 200 | 30.07% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 50 | 56 | 47.17% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 后端只释放音频子系统引用（2026-09-09，当前）

SDLPlayer::quit_backend 从全局 SDL_Quit 改为 SDL_QuitSubSystem(SDL_INIT_AUDIO)，与成功初始化取得的一次音频引用配对，避免强制关闭宿主其他 SDL 子系统或其音频引用。公开契约明确主线程初始化、串行子系统退出、宿主最终执行 SDL_Quit；手动演示 main 在 test 返回且局部播放器销毁后执行全局收尾。依据官方 [初始化引用计数](https://wiki.libsdl.org/SDL3/SDL_Init) 和 [子系统退出契约](https://wiki.libsdl.org/SDL3/SDL_QuitSubSystem)。

临时 sdl-backend-scope-probe.cpp 包装初始化、子系统退出和全局退出接口，修改前退出码 4；修改后验证未初始化退出不操作、重复初始化不重复取得引用、两次退出仅释放一次音频引用且不调用全局退出，探针通过。该探针验证调用边界，未运行真实宿主其他子系统共存测试，未注册永久 CTest。主项目与独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.37 秒）。未运行依赖硬编码媒体和实体设备的手动演示。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 86 | 198 | 30.28% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 50 | 56 | 47.17% |
| `src/main.cpp` | 75 | 168 | 30.86% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 头文件自包含与实现直接依赖（2026-09-09，当前）

SDLPlayer.hpp 改用 pragma once，以 SDL_audio.h 代替 SDL.h 聚合头，显式包含按值 AudioBuffer、AudioDataFormat 和 string 定义。实现文件以自身头开头，补齐 IAudioNode、AudioFormat、SDL_stdinc 以及 atomic/chrono/cstddef/cstdint/thread/vector 等实际依赖，避免借助无关 SDL 或业务头传递定义。没有变更第三方调用签名或行为。搜索 ICE 自维护 include/src 未找到统一日志封装，现有 fmt 诊断迁移尚未完成，未擅自引入主项目高层日志依赖。

临时 sdl-header-self-contained.cpp 仅重复包含该公开头并实例化 sizeof，独立编译通过。独立 ICE 满线程构建通过；Ninja 依赖复查该头由 SDLPlayer.cpp 和手动演示 main.cpp 两个翻译单元使用。主项目构建、格式与差异检查通过；现有 CTest 1/1 通过（4.33 秒）。未运行实体设备演示，不以该头编译通过证明完整头文件治理完成。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 85 | 198 | 30.04% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 48 | 56 | 46.15% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 热路径同步职责收窄（2026-09-09，当前）

核对 IReceiver::set_source 与 SDLPlayer 全部访问点：源绑定必须在 stop/join 后进行，原 source_mutex 仅供数侧持有，不能提供并发替换保护；移除该锁和成员，按引用借用源指针，不复制共享所有权。暂停状态仅由串行控制侧访问，改为 bool m_paused。跨线程运行状态保留 atomic m_running，全部读写采用 relaxed：标志仅传递请求与状态，线程创建发布资源，join 完成资源回收同步。补充热路径和成员的中文同步约束。本轮不允许运行中 set_source 或并发控制生命周期，不声称整个供数链无锁或硬实时；SDL 与源内部行为仍需分别审计。移除成员改变类布局，使用者必须与当前 ICE 库重新编译。

重新编译暂停恢复重试、供数失败退出重启、析构清理、异常块重启拒绝四组临时探针，全部通过；检查运行标志访问均显式使用 relaxed，SDLPlayer 不再包含 source_mutex/scoped_lock。探针未注册永久 CTest，无实体设备，未运行 ThreadSanitizer，测试通过不能替代上述并发契约推导。主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.33 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 85 | 189 | 31.02% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 48 | 55 | 46.60% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 重启复查原始块契约（2026-09-09，当前）

SDLPlayer 新增 m_blockFrames，仅在构造缓冲成功后记录固定帧数。start 在恢复设备和创建线程前检查非零固定帧数、格式、活动帧数及容量；线程使用保存的固定帧数，不能把源节点改变后的长度当成新契约。异常缓冲不自动重建，调用方需重建播放器；普通设备错误未破坏缓冲时仍可 stop 后重启。新增私有成员改变类布局，使用者必须与当前 ICE 库一起重编译，主项目预编译库构建不证明该包 ABI 已更新。

临时 sdl-restart-contract-probe.cpp 先触发源节点破坏块契约并 stop，再检查重启拒绝且设备恢复次数仍为一次。修改前零帧场景重启成功，退出码 3；修改后零帧、格式改变、扩容一帧均通过。重新编译并运行供数失败重启及析构探针，均通过，确认未破坏正常恢复路径。探针不使用实体设备且未注册永久 CTest。主项目、独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.38 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 85 | 192 | 30.69% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 46 | 54 | 46.00% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成，线程创建失败、原子访问及第三方资源封装等仍需检查。

## SDL 源节点输出块契约检查（2026-09-09，当前）

SDLPlayer 在线程开始保存固定块帧数，源节点 process 返回后、交错转换前验证格式与活动帧数未变且容量覆盖该块。变化时退出供数，避免扩大帧数写出交错数组边界，或缩短活动区间却仍按整个数组提交旧尾部。此策略遵循 IAudioNode 固定格式和帧数契约，不把节点改变活动长度当作合法短读。检查不能撤销源节点内部已发生的非法内存访问，也未恢复节点改变后的缓冲；后续重启入口对该状态的复查仍待处理。

临时 sdl-block-contract-probe.cpp 在 process 中注入零活动帧、采样率变化、扩容一帧三种场景。修改前仅安全运行零帧场景，仍调用提交并退出码 2；修改后三种场景均只查询一次且零提交，close 回收线程。扩容仅用于错误注入，不是允许生产热路径分配。探针无实体设备且未注册永久 CTest。主项目、独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.37 秒）。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：83 注释行、187 代码行，30.74%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## SDL 格式与缓冲长度边界（2026-09-09，当前）

SDLPlayer 构造阶段先拒绝零声道、零采样率、采样率超过 int 最大值，以及零帧或超过 INT_MAX / sizeof(float) / 3 / channels 的缓冲帧数，再分配图缓冲。除法式上界确保三个块的队列阈值和单块提交字节数可表示，避免先乘法溢出。非法输入保持空缓冲，open 直接返回 false，不调用设备接口；合法采样率改为显式 static_cast<int>。核对 [SDL_AudioSpec 官方字段定义](https://wiki.libsdl.org/SDL3/SDL_AudioSpec)。此检查不保证设备支持任意合法数值格式，设备仍可拒绝打开，也不解决真实内存分配失败。

临时 sdl-format-range-probe.cpp 包装打开接口计数，修改前三种非法格式仍调用 SDL，退出码 2；修改后非法格式、零缓冲、UINT32_MAX 帧和超过立体声阈值一帧均在设备调用前拒绝，恢复默认配置后正常打开。超大缓冲仅在修复后运行，避免旧实现巨量分配。探针无实体设备，未注册永久 CTest。主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.37 秒）。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：81 注释行、182 代码行，30.80%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成；源节点改变块长度、线程创建失败、原子访问与其他后端仍待检查。

## SDL 供数错误退出与线程回收（2026-09-09，当前）

SDLPlayer 供数循环在队列查询为负或提交失败时立即退出，并以 relaxed 写入 running=false；标志只表达运行状态，资源同步仍依赖 join。stop 不再因 running 已清零而提前返回，start 拒绝覆盖尚可 join 的线程对象；故障退出后先 stop 回收，再允许重启。移除 stop 清零 running 后调用 resume 的无效分支。核对官方 [队列查询](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued) 与 [提交数据](https://wiki.libsdl.org/SDL3/SDL_PutAudioStreamData) 错误契约。提交失败前已推进的当前块不回滚，当前接口未提供跨线程错误详情；后台退出也不自动排空或关闭设备。

临时 sdl-supply-error-probe.cpp 分别注入查询与提交失败，修改前两种模式均在测试期限内保持运行并退出码 2；修改后确认仅查询一次，查询失败不提交，提交失败仅提交一次，运行标志清零后线程仍须回收，未回收时拒绝启动，stop 后可重启并 close。重编译并复验已有析构、启动失败、暂停恢复重试探针，均通过。探针不使用实体设备且未注册永久 CTest。主项目与独立 ICE 满线程构建、格式及差异检查通过；现有 CTest 1/1 通过（4.34 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 77 | 171 | 31.05% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 44 | 52 | 45.83% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成；SDL 缓冲字节数窄化、格式校验、线程创建失败及其他后端规则仍待检查。

## SDL 析构回收线程和音频流（2026-09-09，当前）

SDLPlayer 增加覆盖析构函数，在成员和基类音频源销毁前调用 close，先停止并 join 供数线程，再释放音频流。显式 close 与析构可重复收尾，不退出全局 SDL。公开生命周期约束更新为控制侧销毁，且未关闭流时必须早于 SDL 后端退出；不能从供数线程自身销毁播放器。新增析构符号需要使用当前 ICE 源码构建的库，主项目预编译库构建不证明该符号已更新到预编译包。

临时 sdl-destruction-probe.cpp 包装 SDL 资源接口：修改前已打开但未启动的对象销毁后释放计数仍为零，退出码 4；修改后验证未打开、已打开、运行中、显式 close 后析构以及基类 unique_ptr 持有场景，流仅释放一次，运行线程在析构返回前回收。探针通过，无实体设备，未注册永久 CTest。主项目及独立 ICE 满线程构建、格式与差异检查通过；现有 CTest 1/1 通过（4.44 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 78 | 172 | 31.20% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 42 | 52 | 44.68% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成；SDL 供数错误处理、线程创建失败和其他后端规则仍待审计。

## SDL 暂停与恢复失败允许重试（2026-09-09，当前）

SDLPlayer::pause/resume 仅在对应 SDL 操作成功后更新 paused，失败保留已有标记，避免下一次同向请求被状态条件跳过。修正注释中的物理设备说法，这两个接口操作逻辑设备。已核对官方 [暂停](https://wiki.libsdl.org/SDL3/SDL_PauseAudioDevice) 与 [恢复](https://wiki.libsdl.org/SDL3/SDL_ResumeAudioDevice) 返回值契约。现有 void 接口保持不变，未新增失败返回通道；标记也不代表热插拔后的设备即时状态。

临时 sdl-pause-retry-probe.cpp 使用 SDL 包装接口分别注入暂停失败、恢复失败，检查后续重试实际调用设备操作，成功后重复调用不再提交。修改前两种场景分别退出 3、5，修改后均通过；供数队列包装为已满并在 close 回收线程，无需实体设备，未注册永久 CTest。主项目及独立 ICE 满线程构建通过，现有 CTest 1/1 通过（4.46 秒），格式及差异检查通过。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：74 注释行、168 代码行，30.58%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成；SDL 生命周期自动清理、供数错误和显式异常等问题仍需继续审计处理。

## SDL 启动传播设备恢复失败（2026-09-09，当前）

SDLPlayer::start 检查 SDL_ResumeAudioDevice 的返回值，失败时不发布 running、不改 paused、不创建供数线程，保留流供调用方重试或关闭；成功后才发布运行状态并启动线程。已核对 [SDL 官方返回值契约](https://wiki.libsdl.org/SDL3/SDL_ResumeAudioDevice)。此调整不覆盖 std::thread 构造失败或后台提交错误，不能视为完整播放成功保证。

临时 sdl-start-failure-probe.cpp 包装 SDL 打开、查询、恢复、队列及销毁接口，无需实体设备。修改前强制恢复失败仍启动，探针退出码 2；修改后连续两次失败均无运行请求和待 join 线程，随后恢复成功可启动，重复启动拒绝且不重复恢复，close 回收线程并仅销毁一次流。探针通过，未注册永久 CTest。主项目和独立 ICE 满线程构建通过；现有 CTest 1/1 通过（4.45 秒），格式及差异检查通过。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：74 注释行、166 代码行，30.83%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成；SDL pause/resume 仍先改标记并忽略设备操作结果，需继续处理。

## SDL 打开失败保留原始诊断（2026-09-09，当前）

SDLPlayer::open 在 SDL_OpenAudioDeviceStream 返回空时立即读取错误并返回 false，同时将缓存设备 ID 清零；仅成功创建流后调用 SDL_GetAudioStreamDevice，避免空流查询覆盖最初的打开错误。调用契约已核对 SDL 官方文档：[打开流](https://wiki.libsdl.org/SDL3/SDL_OpenAudioDeviceStream)、[查询流设备](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamDevice)。

临时 sdl-open-failure-probe.cpp 包装打开和查询 API，强制打开失败并设置独有诊断。修改前探针退出码 2，打印被查询覆盖的错误；修改后通过，确认零次设备查询、原始错误保留、设备 ID 为零、start 拒绝且未创建线程。探针未注册永久 CTest，也未使用实体音频设备。主项目与独立 ICE 满线程构建通过，现有 CTest 1/1 通过（4.37 秒）。

本轮 src/ice/out/play/sdl/SDLPlayer.cpp：73 注释行、164 代码行，30.80%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成，SDL start 忽略设备恢复结果等问题仍待处理。

## 手动演示传播加载与设备失败（2026-09-09，当前）

演示 test 返回 bool，音轨加载、SDL 初始化、设备打开或启动接口返回失败时不进入播放时长等待。打开/启动失败先 close 再 quit_backend；main 传播失败为退出码 1，正常完成为 0。启动返回值的底层覆盖范围不因本轮扩大，不能据此宣称设备恢复、实际发声或排空已经验证。

主项目及独立 ICE 满线程构建、格式与差异检查通过，现有 CTest 1/1 通过（4.37 秒）。未运行依赖硬编码媒体和实体设备的演示，未对设备失败分支进行运行注入；构建证据只证明返回类型、调用及清理路径可编译链接。

本轮 src/main.cpp：74 注释行、166 代码行，30.83%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 手动演示诊断线程生命周期（2026-09-09，当前）

src/main.cpp 移除按引用捕获局部节点的 detach 诊断线程，改用 jthread 与停止令牌。500ms 诊断间隔使用可取消条件变量等待，主播放等待结束后先 request_stop/join，再停止供数和关闭设备；诊断已开始时 join 等待其读数完成。补充退出同步的 Doxygen 约束。此修改不解决示例主线程按总时长 sleep 的交互取消问题，也不将其升级为生产播放控制流程。

主项目和独立 ICE 满线程构建、格式及差异检查通过，现有 CTest 1/1 通过（测试 4.38 秒）。后续仅补充同步约束注释，独立构建再次通过。没有运行依赖本机硬编码媒体和实体设备的手动演示，不能将 CTest 通过描述为该演示运行验证。

本轮 src/main.cpp：71 注释行、162 代码行，30.47%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 显式异常复查与旧统计器零尺寸分配（2026-09-09，当前）

本轮对 ICE include/src/tests 下 cpp/hpp/h 的 throw/try/catch/new/delete 命中逐项分类：AudioBuffer.hpp 对齐分配器仍有两个 throw；AllocationTracker.hpp 仍有一个 throw；CachyDecoder.cpp 仍有一组 try/catch。= delete 禁用声明、注释、头文件名以及全局分配钩子定义不是原始分配表达式，不能仅以关键词计数判定违规数量。此扫描不涵盖隐式标准容器异常、库内部行为或全部构建脚本，不能替代完整规则审计。

旧 AllocationTracker 的普通分配钩子将零字节请求规范化为一个字节，避免 malloc(0) 合法返回空时被误判为分配失败。临时 allocation-zero-probe.cpp 包装 malloc，对零参数强制返回空，验证 operator new(0) 实际未传零且返回可释放地址，探针通过。没有把真实分配失败改为终止进程或伪造成功；原有异常及 cout 日志仍待处理。

主项目与独立 ICE 满线程构建、格式和差异检查通过，CTest 1/1 通过（4.37 秒）。独立构建中此头作用于手动演示程序，未运行依赖设备的演示；零尺寸行为由临时探针验证，未注册永久 CTest。

本轮 include/ice/tool/AllocationTracker.hpp：36 注释行、40 代码行，47.37%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## 缓存与 FFmpeg 输出空指针检查（2026-09-09，当前）

CachyDecoder::decode 在 get_data 前验证指针表及各目标声道，错误请求不进入缓存等待或复制。FFmpeg 后端 decode 在读包和余量消费前验证输出，空指针表、空声道或零帧立即返回零。检查不代替调用方对地址有效性、表长度及缓冲容量的保证。

临时 cache-null-output-probe.cpp 与 ffmpeg-null-output-probe.cpp 验证首声道/后续声道为空时有效声道哨兵不变，修正指针后正常读取；FFmpeg 同时检查空指针表及既有路径拒绝场景。两项探针通过，未注册永久 CTest，未直接测量缓存等待或精确比较解码游标。主项目及独立 ICE 满线程构建、格式与差异检查通过，CTest 1/1 通过（4.33 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 77 | 136 | 36.15% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 220 | 499 | 30.60% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## StreamingDecoder 输出指针先验证后写入（2026-09-09，当前）

decode 先验证所有目标声道指针，再执行清零与页复制。后续声道为空时不再留下前面声道已被清零的部分结果；合法输入的缺页静音和复制语义不变。检查只判断空指针，调用方仍须保证表长度、目标容量和地址有效。

临时 stream-null-output-probe.cpp 创建单帧流式音轨，检查首声道或第二声道为空时返回零且有效声道哨兵不变，再修正指针检查双声道正常读取，探针通过。未注册永久 CTest，未覆盖悬空指针或并发缓存竞争。主项目及独立 ICE 满线程构建、格式和差异检查通过，CTest 1/1 通过（4.32 秒）。

本轮 src/ice/manage/dec/StreamingDecoder.cpp：106 注释行、223 代码行，32.22%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## StreamingDecoder 后端返回长度验证（2026-09-09，当前）

首块读取超过请求容量时拒绝创建；后台跳过和页读取超过请求时停止消费，将可用长度收敛到已知游标，不累加或发布违约长度。页读取请求同时限制在 size_t 剩余范围内，避免整数末端游标回绕。检查不能撤销恶意后端已实施的越界写入，仅约束返回值的后续使用。

临时 stream-overread-probe.cpp 的假后端只报告过大长度、不实际越界写入。首块场景验证创建失败；后台第二块场景验证线程结束、长度保留首块、末尾读取返回零且输出哨兵不变，两种通过。测试等待仅用于观察后台状态，不进入生产路径。未注册永久 CTest，未覆盖远距离跳过错误或实际 SIZE_MAX 末端运行。主项目和独立 ICE 满线程构建、格式与差异检查通过，CTest 1/1 通过（4.37 秒）。

本轮 src/ice/manage/dec/StreamingDecoder.cpp：106 注释行、222 代码行，32.32%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioPool 线程池与工厂前向声明（2026-09-09，当前）

AudioPool.hpp 移除 ThreadPool.hpp 和 IDecoderFactory.hpp，直接声明仅以引用或共享句柄使用的类型。模板只将线程池引用交给音轨工厂，不调用其成员；工厂完整定义由实际创建 FFmpeg 工厂的实现包含。已有临时音频池探针补充自身使用 ThreadPool 的直接包含。

主项目与独立 ICE 满线程构建、音频池单头语法检查、格式及差异检查通过，CTest 1/1 通过（4.32 秒）。独立 ICE Ninja 中 ThreadPool.hpp 扇出 6→4，IDecoderFactory.hpp 扇出 8→6；前后清单保存于 build/test_output/comment-audit/pool-deps-before.json 与 pool-deps-after.json，仅覆盖当前独立构建对象。

本轮 include/ice/manage/AudioPool.hpp：74 注释行、160 代码行，31.62%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioPool 私有状态与直接包含规范（2026-09-09，当前）

私有成员统一为 m_poolMutex、m_decoderFactory、m_pool，同步构造、缓存操作及注释引用。公开头采用 pragma once，直接包含实际使用的 concepts、functional、initializer_list；实现直接包含 memory。未改缓存行为、对象布局或公开函数名，线程池头的进一步依赖收窄仍可后续检查。

主项目与独立 ICE 满线程构建、格式及差异检查通过，既有 CTest 1/1 通过（4.39 秒）。本轮不为命名和包含调整增加行为测试。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioPool.hpp` | 74 | 160 | 31.62% |
| `src/ice/manage/AudioPool.cpp` | 8 | 18 | 30.77% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 直接入口路径完整性检查（2026-09-09，当前）

probe 与解码后端 initialize 在构造 C 路径字符串前拒绝空路径和内嵌零字节，直接构造 FFmpegDecoderInstance 也覆盖此检查，不依赖音频池先行验证。探测失败保留原输出，实例失败保持无效状态。

临时 decoder-path-probe.cpp 包装 avformat_open_input，对空视图及有效文件名前缀加零字节后缀分别执行 probe、工厂创建和直接构造，验证全部拒绝且打开调用数为零；普通 WAV 仍可探测和创建实例。探针通过，未注册永久 CTest。主项目与独立 ICE 满线程构建、格式和差异检查通过，CTest 1/1 通过（4.37 秒）。随后移除同一初始化函数中的重复检查，独立构建及重新链接的路径探针再次通过。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 68 | 158 | 30.09% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 219 | 497 | 30.59% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioPool 空路径与内嵌零字节拒绝（2026-09-09，当前）

get_or_load 在文件签名查询前拒绝空 string_view 与内嵌零字节，避免默认空视图地址参与路径范围构造，以及 C 文件接口按零字节截断后让缓存键对应错误文件。合法路径命中语义不变，直接解码工厂等其他入口仍需分别审计。

临时 pool-path-probe.cpp 先建立有效音轨，再提交默认空视图及有效文件名前缀加零字节后缀，验证请求为空且后续仍命中同一所有权；同时保留非法策略回归，探针通过。未注册永久 CTest，未检验所有路径编码或直接后端接口。主项目与独立 ICE 满线程构建、格式及差异检查通过，CTest 1/1 通过（4.32 秒）。

本轮 include/ice/manage/AudioPool.hpp：74 注释行、159 代码行，31.76%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioPool 非法策略不逐出有效缓存（2026-09-09，当前）

get_or_load 在选表及文件系统查询前验证策略，仅允许 CACHY/STREAMING。原实现将其他枚举值落入普通缓存，命中策略不匹配后删除已有音轨，再由工厂拒绝创建。现在非法请求返回空且不触碰缓存，避免有效资源因无效参数失效。

临时 pool-strategy-probe.cpp 先加载有效 WAV，再以 -1 策略请求同路径，检查非法请求为空、原弱引用仍有效且后续普通请求共享同一所有权。旧实现因原弱引用失效返回失败，修复后通过。未注册永久 CTest，不代表并发加载已全面验证。主项目及独立 ICE 满线程构建、格式与差异检查通过，CTest 1/1 通过（4.37 秒）。

本轮 include/ice/manage/AudioPool.hpp：72 注释行、157 代码行，31.44%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioPool 失败加载不保留空缓存键（2026-09-09，当前）

get_or_load 在 AudioTrack::create 返回空时立即返回，不再插入空音轨条目。连续请求不同无效路径不会因此累计无用缓存键；没有负缓存，后续请求仍可重试。非空但后台尚未解码完成的音轨仍按既有异步契约缓存，本轮不将其误判为失败。

临时 pool-failed-cache-probe.cpp 使用尚未实现的 COREAUDIO 后端产生空工厂，分别向两种策略请求 32 个无效路径，检查全部弱引用为空且 release_unused 无空条目可回收，探针通过。此测试未注册永久 CTest，不覆盖后台解码失败、并发加载或文件变化。主项目及独立 ICE 满线程构建、格式和差异检查通过，CTest 1/1 通过（4.32 秒）。

本轮 include/ice/manage/AudioPool.hpp：70 注释行、154 代码行，31.25%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioTrack 私有成员与头保护规范（2026-09-09，当前）

三个私有成员统一为 m_mediaInfo、m_filePath、m_decoder，同步构造初始化、访问函数及工厂发布检查；公开函数名称与对象布局保持原样。AudioTrack.hpp 使用 pragma once。此轮为命名与头保护规范修正，不增加行为测试。

主项目与独立 ICE 满线程构建、格式及差异检查通过，既有 CTest 1/1 通过（4.38 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 48 | 55 | 46.60% |
| `src/ice/manage/AudioTrack.cpp` | 50 | 79 | 38.76% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。其他历史命名、异常和行为问题仍待审计，完整 ICE 目标未完成。

## AudioTrack 构造凭证防止聚合绕过（2026-09-09，当前）

CreationKey 原为私有空聚合类型，外部无需命名类型即可传入 {}，绕过 create 的探测与策略检查。改为非聚合类，默认构造私有且仅友元 AudioTrack 可调用，公开复制构造供 make_shared 转发既有凭证。公开音轨构造函数继续服务标准库分配，正常工厂路径保持可编译。

临时 track-construction-rejection.cpp 直接以 {} 和空工厂构造音轨，修改前语法检查成功，修改后明确因 CreationKey 私有构造被拒绝。这是预期失败编译探针，未执行绕过对象或注册永久 CTest。主项目与独立 ICE 满线程构建通过，验证工厂 make_shared 路径仍成立；CTest 1/1 通过（4.38 秒），格式及差异检查通过。

本轮 include/ice/manage/AudioTrack.hpp：48 注释行、57 代码行，45.71%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioTrack 解码策略依赖封装（2026-09-09，当前）

音轨析构及 num_frames 查询移到完整解码类型可见的实现文件，公开头移除 IDecoder/IDecoderFactory 定义并使用前向声明，补齐 span/vector 直接包含。维持 unique_ptr 独占策略，不引入额外堆对象或共享引用。析构公开契约注明流式策略可能等待工作线程；无隐式复制或移动能力变更。新外置符号需要使用方与更新后的 ICE 一起重新构建。

主项目及独立 ICE 满线程构建、AudioTrack 单头语法检查、格式和差异检查通过，CTest 1/1 通过（4.38 秒）。独立 ICE Ninja 中 IDecoder.hpp 扇出由 7 降至 3，AudioPool.cpp、SourceNode.cpp、config.cpp 及测试主入口不再依赖该头。前后清单见 build/test_output/comment-audit/track-decoder-before.txt 与 track-decoder-after.txt，仅代表当前构建对象范围。主项目为既有预编译配置，ICE 源码编译与链接由独立构建验证。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 46 | 51 | 47.42% |
| `src/ice/manage/AudioTrack.cpp` | 50 | 79 | 38.76% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioTrack 读取目标容量验证（2026-09-09，当前）

read 在调用只接收裸指针的解码策略前检查 frame_capacity，超容量请求返回零且不修改样本，不进入底层缓存等待。容量内读取保持原行为，允许填充尚未激活的预留空间，不自动改变活动长度或分配存储。公开头同步拒绝契约；仍要求缓冲格式与自有地址表一致。

临时 track-read-range-probe.cpp 验证 SIZE_MAX 请求保留双声道哨兵、零容量正长度请求拒绝、活动长度零但容量足够时正常填充且活动长度不变，并运行已有 origin 浮点范围检查，全部通过。探针未注册永久 CTest，未据此证明所有解码器或首次调用均无阻塞。主项目及独立 ICE 满线程构建、格式与差异检查通过，CTest 1/1 通过（4.37 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 44 | 51 | 46.32% |
| `src/ice/manage/AudioTrack.cpp` | 45 | 72 | 38.46% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 工厂移除上层音轨依赖（2026-09-09，当前）

FFmpegDecoderFactory.cpp 移除 AudioTrack.hpp，直接包含实际使用的 AudioFormat、MediaInfo、AlbumArt 与标准长度/字符串定义，并将对应头置于首位。工厂仅构造媒体值和解码实例，不需要上层音轨策略类型；未改变探测行为。

主项目及独立 ICE 满线程构建通过，CTest 1/1 通过（4.37 秒），格式及差异检查通过。独立 ICE Ninja 记录中 AudioTrack.hpp 扇出由 6 降至 5，移除的对象为 FFmpegDecoderFactory.cpp；前后证据见 build/test_output/comment-audit/factory-track-before.txt 与 factory-track-after.txt。此统计限于当前独立构建，不推断全部平台或主项目源码构建的扇出。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp：67 注释行、156 代码行，30.04%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 探测采样率回退范围（2026-09-09，当前）

源采样率缺失时，内部配置必须处于正 int 范围才能作为时长换算的回退值。零配置和超界配置使探测失败并保留旧输出，不再窄化后强制变为 1 Hz。源文件已有正采样率时仍优先使用源值，不受未使用的回退配置影响。

临时 probe-sample-fallback.cpp 在流信息阶段模拟缺失采样率，覆盖 44100 有效回退、零配置、UINT32_MAX 配置，以及有效源采样率加超界配置四种场景；结果与关闭一次的检查均通过。探针未注册永久 CTest，不代表全局配置并发读写安全已经验证。主项目、独立 ICE 满线程构建和格式/差异检查通过，CTest 1/1 通过（4.37 秒）。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp：66 注释行、153 代码行，30.14%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 探测码率与声道整数边界（2026-09-09，当前）

元信息声道数在转 uint16_t 前验证上限，超界直接拒绝候选，避免 65536 等输入变成零或其他格式。码率非正值转为未知值零，正值先按 size_t 上限饱和再转换，避免负值变为巨大无符号数及窄平台截断。内部采样率配置回退的窄化规则仍待进一步审计。

临时 probe-numeric-range.cpp 在流信息探测后注入负码率与 65536 声道，检查前者得到零码率、后者返回失败且保持旧输出；连同正常探测、流信息失败、无音频三种关闭回归，共五种场景通过且输入恰好关闭一次。未在 32 位平台运行正码率饱和分支，探针未注册永久 CTest。主项目、独立 ICE 满线程构建及格式/差异检查通过，CTest 1/1 通过（4.38 秒）。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp：64 注释行、149 代码行，30.05%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 探测输入作用域所有权（2026-09-09，当前）

打开成功后立即用 unique_ptr 与自定义删除器接管 AVFormatContext，删除器调用 avformat_close_input。移除分支手动 cleanup，正常及提前返回共用释放路径，标准容器分配异常展开时也由所有权守卫回收输入。没有新增显式异常处理，分配失败的对外错误通道仍待审计。释放契约参考 [FFmpeg 官方解复用文档](https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html)。

临时 probe-close-guard.cpp 包装流信息探测和关闭入口，检查正常完成、流信息失败、无音频流三种情况均恰好关闭一次，失败保持原输出。三种场景通过，未注入实际分配失败，探针未注册永久 CTest。主项目与独立 ICE 满线程构建通过，CTest 1/1 通过（4.33 秒），格式和差异检查通过。

本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp：62 注释行、143 代码行，30.24%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 探测结果完整替换（2026-09-09，当前）

probe 使用独立 MediaInfo 候选收集字段，成功后移动替换输出；缺失标签和封面保持候选空值，不再混入旧文件数据。失败分支不发布候选，保留调用方原结果，取消无音频分支对旧输出声道数的单独修改。公开头同步此契约。分配异常与输入上下文的异常释放路径仍待后续审计。

临时 probe-reuse.cpp 为输出预填旧标签、码率、帧数和封面，验证打开失败保持旧结果，再探测无标签/封面 WAV，验证旧标签与封面清空且取得新长度。探针通过，未注册永久 CTest，也未覆盖每种中途失败注入。主项目与独立 ICE 满线程构建、格式和差异检查通过，CTest 1/1 通过（4.38 秒）。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 61 | 142 | 30.05% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 8 | 18 | 30.77% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AlbumArt 容器所有权与封面替换（2026-09-09，当前）

AlbumArt 由公开原始数组与独立长度改为私有 vector<uint8_t>，通过只读 data()/size() 访问，assign(span) 先建立候选副本再交换，允许输入借用自身字节。保持深拷贝、自赋值安全及移动后源为空的契约，移除本类显式 new/delete。FFmpeg 封面提取同步使用 assign，不再直接覆盖旧数组地址。尺寸拒绝保持原内容，实际分配失败仍可能由标准容器抛出，完整无异常目标尚未达到。

公开字段改为方法且对象布局改变，使用方必须和新 ICE 库一起重新编译，不能把新头用于旧预编译二进制。已搜索自维护 ICE 与主项目 Modules 的字段访问，找到的写入入口位于 FFmpeg 工厂并已迁移，未修改其他第三方源码。

临时 album-ownership-probe.cpp 使用 AddressSanitizer/UBSan，覆盖外部输入独立复制、复制构造/赋值、移动构造/赋值与源清空、自复制/自移动、自身切片替换和清空，运行通过。此探针未注册永久 CTest，未注入分配失败或运行嵌入封面文件的完整解码测试。主项目和独立 ICE 满线程构建通过，CTest 1/1 通过（4.37 秒），格式及差异检查通过。实际 ICE 源码由独立构建验证，主项目继续使用既有预编译配置。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/AlbumArt.hpp` | 21 | 39 | 35.00% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 60 | 140 | 30.00% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## MediaInfo 默认数值与元信息头保护（2026-09-09，当前）

MediaInfo 的 bitrate 与 frame_count 增加零值成员初始化，工厂遗漏字段时复制元信息不再读取未初始化标量。零表示尚无有效数值，不代替 probe 的成功状态。MediaInfo.hpp 和 AlbumArt.hpp 补充 pragma once 及 cstddef 直接包含，避免经多个公开头引入时重复定义，并显式满足长度类型依赖。AlbumArt 的原始数组分配、释放与复制失败问题未在本轮解决。

临时 media-default-probe.cpp 以多种顺序重复包含两个头，使用 Clang -ftrivial-auto-var-init=pattern 编译，检查默认零值、默认空封面、默认对象复制及已填写数值复制，运行通过。该探针未注册永久 CTest，也不是完整内存诊断。主项目和独立 ICE 满线程构建通过，CTest 1/1 通过（4.38 秒），格式及差异检查通过。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/MediaInfo.hpp` | 12 | 17 | 41.38% |
| `include/ice/manage/dec/AlbumArt.hpp` | 36 | 54 | 40.00% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioTrack 视图区间浮点转换验证（2026-09-09，当前）

AudioTrack::origin 移到实现文件并在浮点转 size_t 前验证两个参数：拒绝负值、NaN、无穷及达到 2^size_t位数的输入，保留合法小数向零截断。使用精确二次幂作为排他上界，避免 SIZE_MAX 转 double 向上舍入造成边界漏洞。返回类型从 void 改为 size_t，转发实际取得的每声道帧数；非法请求返回零且不触碰视图容器。调用方仍可忽略结果，但须与当前 ICE 库重新构建，不与旧预编译符号混用。

临时 track-origin-range-probe.cpp 通过音轨工厂创建单帧双声道缓存，覆盖五种非法值分别用于起点和长度、合法小数截断，以及紧邻整数上界下方的可表示长度。检查非法请求不改变已有视图，合法请求正确追加双声道样本并返回一帧；全部通过。探针未注册永久 CTest，未声称已跨平台验证浮点实现。

主项目和独立 ICE 满线程构建通过，CTest 1/1 通过（4.38 秒），格式与差异检查通过。主项目仍使用既有预编译配置，ICE 新实现由独立源码构建验证。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 44 | 51 | 46.32% |
| `src/ice/manage/AudioTrack.cpp` | 43 | 71 | 37.72% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## AudioTrack 缓冲读取实现移出公开头（2026-09-09，当前）

AudioTrack::read 从头文件内联实现移到 AudioTrack.cpp，返回类型明确为原有推导结果 size_t。公开头只保留 AudioBuffer 前向声明，实现文件直接包含其定义。读取转发、返回帧数和缓冲前置条件不变，没有新增所有权复制或动态分配。调用方需与更新后的 ICE 库一起重新构建，不能使用新声明链接不含该外置实现的旧预编译库。

主项目与独立 ICE 均执行 nproc 满线程构建通过；主项目使用现有预编译配置，实际 ICE 源码编译和链接由独立构建验证。CTest 1/1 通过（4.37 秒），AudioTrack 单头语法检查、格式和差异检查通过。独立 ICE Ninja 依赖中 AudioBuffer 扇出由 22 降至 19，config.cpp、AudioPool.cpp、FFmpegDecoderFactory.cpp 不再依赖该头；结果保存于 build/test_output/comment-audit/audiotrack-fanout-after.txt，前值见 idecoder-fanout-after.txt。不据此推断跨平台 ABI 或主项目所有翻译单元均已验证。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 46 | 54 | 46.00% |
| `src/ice/manage/AudioTrack.cpp` | 32 | 56 | 36.36% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## IDecoder 标准类型依赖收窄（2026-09-09，当前）

检查确认部分声道输出属于当前 IDecoder/CachyDecoder 明示契约，本轮不擅自改为全声道成功语义。IDecoder.hpp 移除无实际类型用途的 AudioBuffer.hpp，直接包含 cstdint/vector，并采用 pragma once。源码构建暴露并修正两处传递包含依赖：AudioTrack 的内联 decode 访问缓冲成员，显式包含 AudioBuffer；StreamingDecoder 的参数只引用 AudioDataFormat，补充结构体前向声明。

主项目与独立 ICE 使用 nproc 满线程构建通过，CTest 1/1 通过（测试 4.33 秒）。IDecoder 与 StreamingDecoder 单头独立语法检查通过，格式及差异检查通过。独立 ICE Ninja 依赖记录中 AudioBuffer 扇出由 23 个对象降为 22 个，CachyDecoder.cpp 不再依赖该头；前后清单存于 build/test_output/comment-audit/idecoder-fanout-before.txt 和 idecoder-fanout-after.txt。该统计只覆盖当前独立构建对象，不代表所有平台或主项目源码依赖扇出。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/IDecoder.hpp` | 26 | 20 | 56.52% |
| `include/ice/manage/AudioTrack.hpp` | 47 | 61 | 43.52% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 24 | 27 | 47.06% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## CachyDecoder 单声道扩展去除整轨临时副本（2026-09-09，当前）

单声道缓存扩展先调整外层声道容器，再直接以 pcm[0] 为复制源。移除原有 mono_pcm 整轨临时副本，避免扩展期间额外持有一份完整单声道 PCM；源访问发生在外层 resize 后，不跨越容器搬迁保存引用。目标各声道仍持有独立样本存储，发布方式不变。此修改减少一次完整复制和临时存储，并不为整文件缓存建立总内存上限，也没有解决分配异常。

十项既有 cache-format 临时探针重新链接当前 ICE 后通过，其中单声道扩展检查左右样本一致。未新增永久测试或进行峰值内存测量，不将去除临时对象的代码证据表述为实测内存数值。主项目、独立 ICE 构建通过，CTest 1/1 通过（4.37 秒），格式与差异检查通过。

本轮 src/ice/manage/dec/CachyDecoder.cpp：76 注释行、134 代码行，36.19%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 坏包静音按请求交付（2026-09-09，当前）

坏包补偿不再按 packet 时长扩容并预填整段 PCM。queueRecoveredSilence 只保存 m_recoveredSilenceFrames，decode 按本次调用方剩余请求直接填零并递减计数，余量耗尽后继续正常解码。寻道重置时清除旧静音余量，避免跳转后交付旧位置补偿。保留原有时间轴补偿长度，不设置任意时长截断；单次静音处理的工作量受请求长度约束。

临时 silence-stream-probe.cpp 包装读包、送包和转换入口，首包送入返回 INVALIDDATA，包时长分别设为 32 和 1000000000000。检查连续两次 16 帧读取均为静音、目标后缀哨兵不变、期间不重新送包或转换；普通长度耗尽后恢复转换，巨大长度在 seek(0) 后恢复转换。两种场景通过，另四项容量估计/转换失败探针重新链接当前库后通过。此探针未注册永久 CTest，不证明整个解码调用无分配；实际 PCM 解码仍允许扩容。整文件缓存累计巨长逻辑静音的资源边界仍待处理。

主项目及独立 ICE 构建、格式和差异检查通过，CTest 1/1 通过（4.38 秒）。本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp：218 注释行、494 代码行，30.62%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## FFmpeg 输出容量估计错误与窄化边界（2026-09-09，当前）

ensure_conversion_buffer_capacity 拒绝负输入帧数、空重采样上下文以及 swr_get_out_samples 的负错误码，不再用旧容量掩盖估计失败。正常零上界仍保证至少一个输出槽，排尾由转换结果决定。返回容量先在 size_t 域内限制到 INT_MAX，再转 int，避免大补静音缓冲留下的容量窄化为负数。依据 [FFmpeg 官方 libswresample 文档](https://www.ffmpeg.org/doxygen/trunk/group__lswr.html)，容量估计的负返回值表示错误。

临时 capacity-error-probe.cpp 包装容量估计与转换入口，分别在第一次和第二次估计注入错误，检查当次不调用转换、返回已转换前缀、未交付输出哨兵保持不变。原实现两个场景均失败，修复后均通过；两个既有转换失败探针也通过。探针未注册永久 CTest，未实际分配超过 INT_MAX 帧的缓冲验证窄化分支，不将范围推导声称为大容量运行验证。

主项目和独立 ICE 构建通过，CTest 1/1 通过（4.44 秒），格式及差异检查通过。本轮 src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp：219 注释行、494 代码行，30.72%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前内容。完整审计仍未完成，巨大静音补偿的资源消耗、分配异常及其他调用链继续审计。

## AudioBuffer 混音错误返回值化（2026-09-09，当前）

两套 AudioBuffer::operator+= 从 void 改为 bool noexcept，格式或活动长度不兼容时返回 false，并在访问样本前整块拒绝，目标内容保持不变。成功返回 true，保留原有自身累加和活动前缀处理。移除两个 buffer_error 显式抛出点；现有忽略返回值的调用可继续编译，错误输入现在不贡献混音。PitchAlter 旁路调用后本就直接返回，本轮不扩大到其准备阶段、共享所有权及参数同步问题。分配器的显式异常仍未消除，不能宣称整个缓冲类无异常。

正式 testMixActiveBounds 补充声道数、采样率、活动长度三类独立拒绝场景，源使用非零样本，逐声道检查目标全容量不变，并检查拒绝路径无堆分配/释放；原有成功路径检查返回 true。CTest 1/1 通过（4.47 秒）。临时 mix-rejection-portable.cpp 以 -U__linux__ 独立编译运行，验证标量实现三类拒绝及自身累加；这是 Linux 上的分支检查，不等同于 Windows 平台验收。主项目、独立 ICE 构建及格式/差异检查通过。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 135 | 306 | 30.61% |
| `tests/TimeStretcherRealtimeTest.cpp` | 534 | 1243 | 30.05% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件清单见 build/test_output/comment-audit/under-30.csv。94 个摘要匹配当前文件。完整 ICE 行为与规则审计仍未完成。

## AudioBuffer 尺寸越界拒绝与解码扩容检查（2026-09-09，当前）

AudioBuffer::resize 返回 bool，在修改格式、活动长度及容量前检查 SIMD 对齐加法和容器总元素数上限。尺寸不可表示时返回 false，保留旧格式、容量、地址和样本；非 Linux 标量实现同步检查并返回状态。FFmpeg 初始化、转换扩容及损坏包补静音检查结果，拒绝扩容后结束当前读取，避免用旧缓冲承接超界写入。此返回值只覆盖尺寸检查，实际内存耗尽仍沿用现有分配器异常机制，其他调用方的失败传播仍待审计。

正式 tests/TimeStretcherRealtimeTest.cpp 新增 testBufferResizeRange，内部用例由十五增至十六。覆盖三个超大尺寸请求、拒绝后状态与样本不变、无堆分配/释放，以及后续合法扩容。修改前 SIZE_MAX 临时探针暴露活动长度/容量损坏；修改后正式 CTest 1/1 通过（4.58 秒）。使用 -U__linux__ 在本机独立编译运行标量分支探针通过，这不等同于 Windows 平台验证。两个已有重采样错误注入探针重新链接当前 ICE 后通过；未新增 FFmpeg 容量拒绝的故障注入测试，不宣称覆盖所有解码失败分支。

主项目与独立 ICE 构建、clang-format 检查和差异检查通过。本轮修改逐文件统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 131 | 305 | 30.05% |
| `src/ice/manage/AudioBuffer.cpp` | 42 | 98 | 30.00% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 215 | 496 | 30.24% |
| `tests/TimeStretcherRealtimeTest.cpp` | 523 | 1220 | 30.01% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件完整清单见 build/test_output/comment-audit/under-30.csv。94 个审计摘要与当前文件匹配。注释达标不代表完整行为及规则审计完成，分配失败、显式异常和其余调用方仍需继续处理。

## AudioBuffer 活动范围混音与自身累加（2026-09-09，当前）

Linux operator+= 只累加活动帧：完整向量组保留对齐访问，不足一组的尾部逐样本处理，未激活容量保持原样。移除混音源目标的 restrict 承诺，允许同一缓冲自身累加。非 Linux 标量算法原本只处理活动帧，本轮未改其实现。格式错误的显式异常及尺寸分配边界仍待审计。

正式 `tests/TimeStretcherRealtimeTest.cpp` 新增 testMixActiveBounds 并接入 main，Linux 内部用例由十四增至十五。覆盖 0/1/7/8/9/15/16 活动帧、双声道全容量哨兵、自身累加，以及独立源混音时无堆分配/释放。旧实现 CTest 出现尾部与自身累加断言失败，修改后 CTest 1/1 通过（4.58 秒）。测试亦可编译到非 Linux，但本轮没有跨平台执行，不声称验证所有指令集。

主项目和独立 ICE 构建、格式及差异检查通过。最后仅补测试说明注释后独立 ICE 再次构建通过。逐文件统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 126 | 293 | 30.07% |
| `tests/TimeStretcherRealtimeTest.cpp` | 510 | 1186 | 30.07% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。完整 ICE 行为及规则审计仍未完成。

## CachyDecoder 空请求不等待缓存（2026-09-09，当前）

decode 的零帧/零声道请求以及 origin 的零帧请求在 get_data 前返回零，不触碰输出，不等待后台整文件解码。同步公开契约，零声道不再报告未实际写出的帧数；非空首次读取仍需等待，实时使用前必须预备缓存。

临时 `build/test_output/comment-audit/cache-empty-probe.cpp` 用未释放的 promise 阻止后台工厂完成，在独立调用线程检查三类空请求能否在放行工厂前返回。旧实现三种失败，修改后三种通过；500ms 仅为测试观察截止，不存在于生产交互路径。十项缓存回归通过。探针未注册永久 CTest，不能证明非空首次访问无阻塞。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.59 秒）。本次修改：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 74 | 135 | 35.41% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 48 | 51 | 48.48% |

全项目自维护业务、头文件、测试、构建和工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。其他异常、生命周期及行为/规范审计仍未完成。

## CachyDecoder 输出格式契约检查（2026-09-09，当前）

缓存创建拒绝零值目标格式，并核对后端实际采样率与请求一致。声道数只允许直接匹配或原有单声道复制扩展，其他转换应由后端完成；本层不进行重采样或矩阵混音。避免错误格式缓存按目标时基播放而变速，或静默丢失/缺少声道。

临时 `build/test_output/comment-audit/cache-format-probe.cpp` 的采样率不匹配和三声道返回两个旧版用例失败，修改后被拒绝；单声道扩展验证左右样本相同，修改前后均通过。连同七项既有缓存场景共十项通过。探针未注册永久 CTest，不代表所有后端的格式转换均已验证。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.60 秒）。本次修改：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 72 | 133 | 35.12% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 48 | 51 | 48.48% |

全项目自维护业务、头文件、测试、构建和工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档与纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。错误接口、异常路径及其他 ICE 行为/规范审计仍未完成。

## 线程池与缓存私有成员命名（2026-09-09，当前）

ThreadPool 的五个私有成员统一为 m_workers、m_tasks、m_queueMutex、m_condition、m_stop；CachyDecoder 的三个私有状态成员统一为 m_futureData、m_dataCache、m_dataReadyFlag。同步全部实现访问和成员名注释，保持公开接口、对象布局及执行顺序不变。已有私有状态探针同步名称，未为纯命名修改新增测试。

主项目和独立 ICE 构建、格式及差异检查通过，现有 CTest 1/1 通过（4.60 秒）。本次修改统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/thread/ThreadPool.hpp` | 57 | 130 | 30.48% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 47 | 51 | 47.96% |
| `src/ice/manage/dec/CachyDecoder.cpp` | 71 | 127 | 35.86% |

全项目自维护业务、头文件、测试、构建和工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档及纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。其余命名、异常路径及行为/规范问题继续审计，完整目标未完成。

## ThreadPool 独占参数与限定调用支持（2026-09-09，当前）

任务绑定由 std::bind 改为保存衰减参数的 tuple 和一次性调用包装。优先沿用可行的左值存储访问，必要时移动参数；函数对象按可调用性选择左值或右值形式，通过 C++23 invoke_r 保持结果类型。支持按值接收 unique_ptr，不破坏显式引用或对参数副本的引用访问。

临时 `build/test_output/comment-audit/threadpool-move-probe.cpp` 原实现编译失败，修改后验证七种组合：独占参数、参数副本引用、std::ref、左值限定函数接独占参数、右值限定函数、右值函数接参数引用、独占捕获函数。停止拒绝探针及七项缓存回归通过。探针未注册永久 CTest，不宣称穷举任意模板组合。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.60 秒）。本轮修改 `include/ice/thread/ThreadPool.hpp`：57 注释行、129 代码行，30.65%。全项目自维护业务、头文件、测试、构建和工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档与纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前文件。线程构造失败、任务错误通道及其他 ICE 行为/规范仍待审计，完整目标未完成。

## ThreadPool 停止提交返回值化（2026-09-09，当前）

线程池停止分支不再显式抛异常：enqueue 返回无共享状态的 future，enqueue_void 返回 bool 表示是否接收。CachyDecoder 首次消费检查 future.valid，拒绝提交时发布空缓存，避免 get 引发 future_error。补齐 ThreadPool 直接使用的 memory/mutex/type_traits/utility 标准头。没有放宽析构期间禁止并发提交的前置条件。

临时 `build/test_output/comment-audit/threadpool-rejection-probe.cpp` 独立编译头文件，检查正常结果、普通任务完成及两个停止拒绝返回值。停止分支通过测试翻译单元暴露私有字段后持锁设置 stop 触发，未并发调用析构，不是生产停止接口或生命周期并发证明。七项缓存回归通过，探针未注册永久 CTest。任务自身异常、线程创建失败和分配失败仍未全面处理。

主项目及独立 ICE 构建、格式和差异检查通过，CTest 1/1 通过（4.57 秒）。最后补标准头后独立 ICE 再次构建通过，未重复无行为变化的测试。本次统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/thread/ThreadPool.hpp` | 53 | 101 | 34.42% |
| `src/ice/manage/dec/CachyDecoder.cpp` | 71 | 127 | 35.86% |

全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档及纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。完整 ICE 行为及规范审计仍未完成。

## CachyDecoder 智能指针创建入口（2026-09-09，当前）

工厂改用 std::make_unique，移除原始 new。新增只能由工厂生成的私有 ConstructionKey，构造函数公开仅为标准库转发提供入口；凭据默认构造私有且非聚合，不能用空花括号绕过。没有引入派生包装对象，缓存对象的所有权和生命周期保持原语义。

临时 `build/test_output/comment-audit/cache-construction-rejection.cpp` 的外部直接构造按预期编译失败，诊断明确为私有凭据构造不可访问；正常工厂构建和七项缓存回归通过。探针未注册永久 CTest。主项目及独立 ICE 构建、格式和差异检查通过，CTest 1/1 通过（4.71 秒）。

本次修改统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 70 | 123 | 36.27% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 47 | 51 | 47.96% |

全项目自维护业务、头文件、测试、构建及工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。现有异常捕获、线程池失败机制及其他 ICE 行为/规范仍待审计，完整目标未完成。

## CachyDecoder 不可信时长预分配移除（2026-09-09，当前）

缓存构建不再按 get_source_total_frames 的媒体估计对每个声道 reserve，而是随实际 PCM 追加采用 vector 的容量增长。避免极大或错误时长在读取前触发巨量分配或 length_error，亦不因估计失真丢弃本可解码的音频。真实整文件数据仍需相应内存，上层流式策略及全局内存预算不在本轮完成范围。

临时 `build/test_output/comment-audit/cache-estimate-probe.cpp` 在正常单帧实例上报告 SIZE_MAX 总帧数，检查实际单帧仍被交付且没有 __cxa_throw 调用。旧实现失败，修改后通过，六项既有缓存场景也通过。探针未注册永久 CTest，没有声称解决真实超大媒体或分配器耗尽。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.65 秒）。本轮修改 `src/ice/manage/dec/CachyDecoder.cpp`：69 注释行、122 代码行，36.13%。全项目自维护业务、头文件、测试、构建及工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。其他异常路径、资源管理和行为/规范审计仍未完成。

## CachyDecoder 已知加载失败返回值化（2026-09-09，当前）

缓存解码的实例创建失败和首次 seek 失败改为直接返回空 DecodedData，不再主动抛异常再由 future 消费端捕获。空工厂、零声道/采样率格式同样返回空缓存；read 返回超过临时块容量时拒绝构造越界迭代器并丢弃本次缓存。保留现有空缓存表示失败的外部语义。现存 catch、线程池/其他后端异常和私有构造的原始 new 尚未移除，不据此宣称本模块全面无异常或满足全部资源管理规则。

临时 `build/test_output/comment-audit/cache-failure-probe.cpp` 使用可控工厂，覆盖空实例、寻道失败、正常单帧、超额 read 返回、零声道、空工厂六种场景，通过 __cxa_throw 包装计数验证已知失败不走抛出路径。旧实现前两种失败断言不通过、正常场景通过；修改后六种均通过。未在旧实现运行可能越界/空指针的后三种。探针未注册永久 CTest，不能覆盖分配器耗尽或任意第三方抛出行为。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.59 秒）。本次修改：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/CachyDecoder.cpp` | 69 | 126 | 35.38% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 43 | 41 | 51.19% |

全项目自维护业务、头文件、测试、构建和工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标，其余 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要均匹配当前内容。完整行为及代码规范审计仍未完成。

## FFmpeg 无效音频时间基拒绝（2026-09-09，当前）

时长换算在工厂和解码端均检查时间基的分子、分母严格为正，无效流时长不参与估算，可继续使用合法容器时长。解码器在选择音频流后拒绝无效时间基，防止后续寻道和坏包补偿猜测时间单位；这种元信息可探测但不可解码的区别是显式策略。

临时 `build/test_output/comment-audit/timebase-invalid-probe.cpp` 在流探测后分别注入零分子、零分母、负分子、负分母；包装换算入口记录无效输入而不执行潜在危险计算。旧实现四个用例失败，修改后四个通过，检查元信息探测仍可完成、解码工厂返回空且没有无效换算。十四项既有解码回归也通过。探针未注册永久 CTest，不代表所有畸形媒体均已覆盖。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.59 秒）。本次修改：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 212 | 487 | 30.33% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 60 | 137 | 30.46% |

全项目自维护业务、头文件、测试、构建和工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档与纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要均匹配当前内容。错误接口、上层异常与其他 ICE 行为/规范审计仍未完成。

## FFmpeg 寻道时间换算结果校验（2026-09-09，当前）

帧位置通过范围检查后，av_rescale_q 仍可能因目标时间基放大而溢出。寻道现在拒绝负换算结果，在调用 av_seek_frame 前返回 false，保留原解码位置与缓存。输入非负，因此负结果不能作为本接口的合法目标位置。

临时 `build/test_output/comment-audit/seek-rescale-probe.cpp` 用链接包装改变目标时间基，调用真实 av_rescale_q 对 INT64_MAX 帧计算并确认返回负值，检查不会继续底层寻道且随后 seek(0) 成功。旧实现失败，修改后通过；十三项既有解码回归也通过。探针未注册永久 CTest，不表示所有异常容器时间基均已覆盖。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.59 秒）。本轮修改 `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：210 注释行、483 代码行，30.30%。全项目自维护业务、头文件、测试、构建和工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源；1017 个有效文件，ICE 94/94 注释率达标，其余 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前文件。错误状态接口、上层异常及其他 ICE 行为/规则审计仍未完成。

## FFmpeg 参数有符号范围校验（2026-09-09，当前）

初始化在任何文件打开前拒绝超出 int 上界的采样率；寻道在时间转换前拒绝超出 int64_t 上界的 size_t 帧位置。防止无符号高位经转换变成负采样率或负时间戳；未添加任意业务采样率上限。时间基转换自身的溢出策略仍需另行审计。

临时 `build/test_output/comment-audit/decoder-range-probe.cpp` 检查 SIZE_MAX 寻道被拒绝且不调用 av_seek_frame、UINT32_MAX 采样率不打开输入，并验证原有效实例仍可 seek(0)。旧实现失败，修改后通过；十二项既有解码回归也通过。该探针按当前 64 位平台验证，未注册永久 CTest。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.58 秒）。本轮修改 `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：208 注释行、482 代码行，30.14%。全项目自维护业务、头文件、测试、构建和工具脚本统计包含 ICE，排除第三方源码、构建/生成/代理文件、文档与纯资源。1017 个有效文件，ICE 94/94 注释率达标，其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要均匹配当前内容。其他数值边界、错误接口与上层异常等 ICE 审计工作仍未完成。

## FFmpeg 寻道重采样重启失败状态（2026-09-09，当前）

寻道改变输入位置并关闭重采样器前将实例标为不可用，只有 swr_init 成功才恢复可用状态。重启失败时 read 返回零、seek 返回 false、isValid 返回 false，要求重新创建实例，避免继续向未初始化重采样器提交数据。公开头同步说明这一失效契约。

临时 `build/test_output/comment-audit/seek-restart-probe.cpp` 在第二次 swr_init（零帧寻道重启）注入失败，随后检查实例失效、读取为零、不发生转换、后续寻道被拒绝。旧实现失败，修改后通过；十一项既有初始化、指针表和转换错误回归均通过。探针未注册永久 CTest，不能证明全部寻道与恢复路径均已通过。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.57 秒）。本次修改：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 206 | 476 | 30.21% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 20 | 24 | 45.45% |

全项目自维护业务、头文件、测试、构建和工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前文件。错误与 EOF 区分、上层异常及其他 ICE 行为/规则审计仍未完成。

## FFmpeg 重采样失败中止当前读取（2026-09-09，当前）

swr_convert 的负返回值不再与零输出一起跳过：释放当前帧、清除本包补偿估计，返回此前已交付前缀。零输出仍继续读取以满足滤波积累需求。依据为 [FFmpeg 重采样返回值契约](https://www.ffmpeg.org/doxygen/4.2/group__lswr.html)。现有 read 接口仍以短读同时表示 EOF/错误，本轮没有增加错误状态查询或跨读取的故障锁存。

临时 `build/test_output/comment-audit/resample-error-probe.cpp` 在第一/第二次非空输入转换注入错误，断言不再继续转换、返回值等于成功前缀，且输出尾部保持哨兵值。旧实现两个用例失败，修改后通过；九项既有指针表与初始化/读取回归亦通过。测试输入在 build/test_output 生成，未改源码资源；探针未注册永久 CTest。

主项目与独立 ICE 构建、格式和差异检查通过，CTest 1/1 通过（4.60 秒）。本轮修改 `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：205 注释行、474 代码行，30.19%。全项目自维护业务、头文件、测试、构建与工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源；1017 个有效文件，ICE 94/94 注释率达标，其余 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。错误与 EOF 区分、上层异常及其他 ICE 行为/规范审计仍未完成。

## FFmpeg 解码输入扩展声道表（2026-09-09，当前）

解码帧送入 swr_convert 时改用 AVFrame::extended_data，避免平面声道数超过固定 data 数组容量时从数组外读取指针。普通交错音频也使用这一字段，依据为 [FFmpeg AVFrame 官方契约](https://www.ffmpeg.org/doxygen/trunk/structAVFrame.html)。

临时 `build/test_output/comment-audit/decoder-planes-probe.cpp` 将普通解码帧的扩展表设为独立有效表，检查重采样收到的是否为该表，并在解除帧引用前恢复原字段。旧实现失败，修改后通过；这验证调用点使用扩展表，不是超过八声道真实平面媒体的端到端测试。八项初始化/正常读取回归也通过。探针未注册永久 CTest。

主项目与独立 ICE 构建、格式和差异检查通过，CTest 1/1 通过（4.61 秒）。本轮修改 `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：203 注释行、469 代码行，30.21%。全项目自维护业务、头文件、测试、构建与工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档和纯资源；1017 个有效文件，ICE 94/94 注释率达标，其余 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前文件。解码错误语义、上层异常及其他行为/规则问题仍待审计，全部通过目标未完成。

## FFmpeg 解码器初始化返回值与资源清理（2026-09-09，当前）

解码后端初始化改用 bool 结果，不再通过显式 load_error 抛出跳过部分构造资源清理。工厂检查 isValid，失败销毁候选对象并返回空指针；直接构造的无效实例 read 返回零、seek 返回 false、总帧数返回零，析构统一释放已取得的句柄。补齐 packet/frame 分配空值检查及源布局失败清理，删除工厂内未使用的历史异常包装类与宏。CachyDecoder 调用方仍会把工厂空返回转换为自己的历史异常，此上层问题尚未修复。

临时 `build/test_output/comment-audit/decoder-init-probe.cpp` 验证七类 C 接口失败（流信息、解码上下文分配、参数复制、解码器打开、packet/frame 分配、重采样初始化）、正常 WAV 读取及无效直接实例的访问。八次运行均通过，失败时检查工厂为空且成功打开输入次数等于关闭次数；部分包装函数也在 FFmpeg 内部打开流程调用，故不能据此宣称每个后期故障点均被独立命中，亦未做全量内存泄漏或 C++ 分配耗尽验证。探针未注册永久 CTest。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.57 秒）。本次修改统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 201 | 469 | 30.00% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 59 | 136 | 30.26% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 19 | 24 | 44.19% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 8 | 18 | 30.77% |

复查全项目自维护业务、头文件、测试、构建和工具脚本，包含 ICE，排除第三方源码、构建/生成/代理文件、文档及纯资源。1017 个有效文件，ICE 94/94 注释率达标；其他 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要均匹配当前文件。解码读错传播、多平面输入、上层异常及其他 ICE 规则和行为审计仍未完成。

## OpenAL 方向归一化溢出（2026-09-09，当前）

有限但接近 float 上界的方向分量会使原平方和溢出，导致逆长度为零、单位方向退化为零向量。normalizeDirection 在相乘前提升到 double，平方和及归一化乘法均使用 double，最后将有界分量转换回 float；保留原有近零长度阈值和默认方向，不改变输入必须有限的契约。

临时空后端 `build/test_output/comment-audit/direction-range-probe.cpp` 捕获实际提交的声源位置，验证六组极大值、符号组合、普通、零与近零方向。旧实现失败，修改后通过；三个空间设置故障用例和队列模式用例也通过。临时探针未注册永久 CTest，不代表实体设备或跨平台验收。

主项目与独立 ICE 构建、格式检查通过，CTest 1/1 通过（4.61 秒）。本轮修改 `src/ice/out/play/openal/ALPlayer.cpp`：389 注释行、883 代码行，30.58%。全项目自维护业务、头文件、测试、构建及工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档与纯资源：1017 个有效文件，ICE 94/94 注释率达标，其余 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要均匹配当前内容。清理故障、实时诊断与其他 ICE 行为/规范审计仍未完成。

## OpenAL 停止后的供数边界（2026-09-09，当前）

queueAudioBuffer 在图拉取前、处理返回后及上传前复查运行标记；观察到停止后返回失败，退出当前填充，避免首块控制失败后继续拉取剩余七槽。正在执行的图处理不能中断，其结果丢弃；与停止请求并发且已经越过检查的设备调用不承诺撤销。

临时 `build/test_output/comment-audit/stop-boundary-probe.cpp` 在首块中分别注入暂停/恢复失败，等待线程回收后断言仅一次图拉取且零次上传。旧实现两个用例均失败，修改后均通过，原有十四项 OpenAL 用例亦通过。这些空后端链接包装探针未注册永久 CTest，不代表实体设备或跨平台验证。

主项目及独立 ICE 构建、格式检查通过；CTest 1/1 通过（4.62 秒）。本轮修改 `src/ice/out/play/openal/ALPlayer.cpp`：387 注释行、879 代码行，30.57%。全项目自维护业务/头文件/测试/构建/工具脚本统计包含 ICE，排除第三方源码、生成与代理文件、构建目录、文档及纯资源；1017 个有效文件，ICE 94/94 达标，其余 685 个不足 30% 文件见 `build/test_output/comment-audit/under-30.csv`。94 个摘要匹配当前内容。清理故障、实时诊断与其余 ICE 行为/规范仍待审计，全部通过目标尚未实现。

## OpenAL 空间设置失败传播（2026-09-09，当前）

applySpatialState 返回明确结果，线程绑定或 AL 属性设置失败保存控制侧诊断并停止供数。open 先检查监听者初始化，再检查空间设置结果，任一失败均回滚；关闭设备时仍允许缓存配置，部分驱动属性更新不承诺事务回滚。

临时空后端链接包装探针对空间模式切换和参数更新注入声源属性错误，旧实现均未立即停止/保存诊断，修改后通过。第三个探针确认打开失败仍回滚，原有十一项 OpenAL 用例全部通过。三个新增用例见 `build/test_output/comment-audit/spatial-failure-probe.cpp`，未注册永久 CTest，不代表实体设备及跨平台验证。

主项目和独立 ICE 构建、格式及差异检查通过，CTest 1/1 通过（4.59 秒）。本次修改文件：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/openal/ALPlayer.cpp` | 384 | 877 | 30.45% |
| `include/ice/out/play/openal/ALPlayer.hpp` | 87 | 63 | 58.00% |

全项目自维护业务、头文件、测试、构建与工具脚本复查包含 ICE，排除第三方源码、构建/生成/代理文件、文档及纯资源：1017 个有效文件，ICE 94/94 达标；其他 685 个不足 30% 的文件见 `build/test_output/comment-audit/under-30.csv`，未扩展修改。94 个登记摘要匹配当前文件。剩余实时诊断、清理故障与其他 ICE 行为/规范问题继续审计，全部通过目标未完成。

## OpenAL 暂停与恢复错误传播（2026-09-09，当前）

暂停/恢复在控制调用线程立即消费 AL 错误并保存诊断；设备命令或线程上下文绑定失败均停止本次供数，交由既有退出路径清理。恢复在 AL 锁内复查运行标志，设备播放命令成功后才解除暂停，避免错误后由后台自动恢复播放。

临时链接包装探针对暂停和恢复分别注入无效声源，两种旧实现均未满足“立即停止并保存诊断”断言，修复后通过。此前九个 OpenAL 回归用例也通过。探针位于 `build/test_output/comment-audit/control-failure-probe.cpp`，未注册为永久 CTest，不能替代实体设备/跨平台验证。

主项目与独立 ICE 构建通过，CTest 1/1 通过（4.59 秒），格式与差异检查通过。本轮修改 `src/ice/out/play/openal/ALPlayer.cpp`：379 注释行、872 代码行，30.30%。全项目按既定自维护代码范围复查：1017 个有效文件，ICE 94/94 达标，其他 685 个不足 30% 的文件清单为 `build/test_output/comment-audit/under-30.csv`；排除第三方源码、生成/代理文件、构建目录、文档及纯资源。94 个登记摘要全部匹配。空间参数失败传播、实时诊断及其他行为/规范问题仍待处理，完整审计保持未完成。

## OpenAL 上下文隔离与初始队列清理（2026-09-09，当前）

供数线程使用独立线程上下文，控制操作用作用域守卫恢复原绑定，关闭时先解除绑定再销毁资源。设备打开明确要求 ALC_EXT_thread_local_context 扩展及两个入口可用，不回退到进程级绑定。依据：[线程上下文扩展契约](https://openal-soft.org/openal-extensions/EXT_thread_local_context.txt)。

双播放器同步首批上传探针在旧实现复现上下文串用，修改后通过。探针同时发现初始状态源 stop 后仍不能逐个退回未播放缓冲；清理改用 AL_BUFFER = AL_NONE 整体解除队列，失败停止供数，重建前复查停止状态。依据：[OpenAL 1.1 队列清理契约](https://www.openal.org/documentation/openal-1.1-specification.pdf)。

九个空后端诊断用例通过（上下文隔离一个、查询故障三个、排队故障两个、模式一致性一个、暂停竞态两个），同时断言不再产生原先的 Unqueueing 清理错误。这些是 build/test_output 下的临时链接包装探针，未注册为永久 CTest；不代表实体设备或 Windows 验证。主项目与独立 ICE 构建通过，现有 CTest 1/1 通过（4.60 秒），格式检查通过。

本次文件统计：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/openal/ALPlayer.cpp` | 373 | 862 | 30.20% |
| `include/ice/out/play/openal/ALPlayer.hpp` | 86 | 63 | 57.72% |

复查全项目自维护业务、头文件、测试及构建/工具脚本，包含 ICE，排除第三方源码、构建产物、代理/生成文件、文档和纯资源。1017 个有效文件中 ICE 94/94 注释率达标；主项目另有 685 个文件不足 30%，完整清单见 `build/test_output/comment-audit/under-30.csv`，未扩大修改范围。94 个登记摘要全部匹配当前文件。控制侧/清理故障策略、实时诊断及其余 ICE 行为和规则问题仍待审计，全部通过目标保持未完成。

## OpenAL 正常供数查询错误处理（2026-09-09，当前）

处理数量、声源状态与队列长度各次查询独立检查；失败立即进入清理，不使用默认值继续操作。正常退队、首批播放与自动恢复失败也结束本次供数。三个查询故障时点的空后端探针通过，旧版“错误后仍继续查询”断言失败；此前五个播放探针亦通过。

主项目与独立 ICE 构建、格式检查、CTest 1/1 通过。当前实现 350 注释行、801 代码行、30.41%，摘要更新，其余 93 个匹配。上下文隔离、控制侧/清理阶段错误及实时诊断仍待处理，全部行为审计保持未通过。

## OpenAL 排队失败退出修复（2026-09-09，当前）

上传与排队失败现在结束当前供数并执行统一停源退队出口，避免失败槽丢失后仍报告运行；上传失败不继续排队旧缓冲。stop 无论运行标志是否已清零都回收 joinable 线程，故障后 start 先 join 旧线程再启动。保持已推进的图游标，不自动重放失败音频。

空后端故障探针在旧版超时失败，修复后第 1/9 次入队失败均触发清理退出，并允许显式重启；模式与暂停三个探针也通过。主项目/独立 ICE 构建、格式、CTest 1/1 通过，94 个当前摘要已复查。多实例上下文、其余驱动错误与实时错误诊断仍待审查，全部行为验收未完成。

## OpenAL 队列模式一致性修复（2026-09-09，当前）

供数线程在空队列开始填充时固定空间模式，后续每槽复用同一普通布尔快照；控制侧请求不再直接改变单槽上传格式。模式变化仍通过既有清队列流程接收，未新增原子或等待。旧版探针观察到首批格式混用；修复后首批八槽一致，第二批八槽一致且切换为新格式。

两种暂停竞态探针亦通过，主项目与独立 ICE 构建、格式检查、CTest 1/1 通过。两文件摘要及计数更新，其余 92 个匹配；ICE 94/94 注释率达标。排队失败恢复、全局上下文等其余行为审计仍未完成。

## OpenAL 自动播放暂停竞态修复（2026-09-09，当前）

首次/重建队列播放与欠载自动恢复两个提交点，在 AL 锁内同时复查运行和暂停请求，避免覆盖已完成的暂停，也避免 stop 清暂停标志后重新播放。空后端链接包装探针在旧版发现暂停后的播放调用，修复后首次填队及正常回填两个时序均通过。

主项目和独立 ICE 构建、格式检查、CTest 1/1 通过。实现 336 注释行、770 代码行、30.38%，摘要更新，其余 93 个匹配。模式切换、排队失败恢复、多个实例上下文和实体设备等问题仍未全部解决，全部行为验收继续未通过。

## 输出关闭错误传播修复（2026-09-09，当前）

FFmpegFileReceiver 现在检查 avio_closep 结果，并在无先前错误时保存关闭诊断；start 在释放资源后汇总关闭结果。取消或较早编码错误不被关闭错误覆盖。旧实现忽略故障注入的关闭失败，修复后关闭失败与取消优先级探针均通过，正常导出仍成功。

主项目和独立 ICE 构建、格式检查及 CTest 1/1 通过。两文件计数和摘要已更新，其余 92 个摘要匹配；ICE 94/94 注释率达标。下方历史“关闭错误未传播”已修复，其他尺寸边界和设备竞态仍未解决，全部审计未通过。

## 文件编码声道表修复（2026-09-09，当前）

FFmpegFileReceiver 的 FIFO 读出改用 AVFrame::extended_data，避免把超过八个平面声道索引到固定 data 数组之外。修复依据为 FFmpeg 帧接口契约；当前预编译包不支持尝试的十六声道 AAC/FLAC，不能声称该崩溃已经端到端复现。成功验证二声道 AAC、八声道 FLAC/Ogg、十六声道 PCM/WAV，各导出并重新解码得到 1024 帧。

主项目与独立 ICE 构建、格式检查及 CTest 1/1 通过。修改实现 323 注释行、744 代码行、30.27%，当前摘要已更新，其余 93 个匹配；全项目统计未变。其他文件错误传播、尺寸边界和播放竞态仍需处理，全部行为审计未通过。

## SIMD 解交错修复（2026-09-09，当前）

Linux AudioBuffer 双声道拆分现以四帧输入对应每声道四浮点写入，使用实际帧数及标量短尾，不再按对齐长度读取输入。空指针、非双声道与零帧保持目标不变。新增九组长度回归，旧实现触发段错误，修复后完整 CTest 通过；当前 Linux 测试包含 14 个用例。主项目与独立 ICE 构建成功，格式检查通过。

两文件当前计数及摘要已更新，其余 92 个摘要匹配；全项目统计仍为 ICE 94/94 达标，主仓 685/923 不足。下方历史 SIMD 风险记录已由本次修复取代；其他尺寸溢出、异常错误通道和设备竞态等问题继续阻止全部行为验收通过。

## 近单位倍率旁路修复（2026-09-09，当前）

修复 TimeStretcher 容差旁路造成的输出越界：仅精确 1.0 速度与 0.0 半音直通，其他参数使用预热后端。保留用户实际倍率，未通过丢弃输入或强制单位速度绕过缺陷。新增 0.9991/1.0009、512 帧、32 块、双声道后缀边界回归；最终测试在旧实现上失败，在修复后通过。

本次改变执行逻辑，因此下方“只改注释”门槛是此前阶段记录。当前主项目和独立 ICE 构建成功，实时 CTest 1/1 通过（现含 13 个用例），新增窗口同时检查 C/C++ 分配与释放。94 个摘要已按当前内容核对，三个受影响文件已更新计数和摘要。其他内存边界、错误传播、设备竞态仍未解决，全部行为验收保持未通过。

## 全部注释终核与当前验收门槛（2026-09-09）

当前 **94/94** 个有效文件完成注释终核，2 个空文件不适用；全部 94 个当前内容摘要已核对。该结论只证明此次逐文件注释与契约核对完成，**全部代码行为审计仍未通过**。本节取代下方各历史快照的当前进度与门槛。

| 门槛 | 当前证据 | 状态 |
| --- | --- | --- |
| ICE 自维护范围与逐文件注释率 | cloc 2.08：94 有效均 ≥30%，2 空 | 通过 |
| 注释、关键实现说明和接口契约 | 94 个文件逐项记录及 SHA-256 | 注释终核完成 |
| 本轮修改不改变执行逻辑 | 修改文件去注释 token 比较 | 通过 |
| 格式 | 分批 clang-format/CMake 检查，最后测试文件检查通过 | 通过 |
| 当前源码构建 | 主项目和独立 ICE Debug 静态构建成功 | Linux 当前配置通过 |
| 已有实时回归 | 独立 CTest 1/1；已核对 12 个用例的实际断言范围 | 通过，覆盖有限 |
| 全项目统计与其他不足 | 1017 有效、5 空；主仓 685/923 不足，under-30.csv | 已报告，不扩改主仓 |
| 安全边界及全部行为规范 | 旁路容量、SIMD 拆分、错误传播、设备竞态等已记录缺陷尚存 | 未通过 |
| 所有平台、设备与媒体分支 | 未执行完整专项覆盖 | 未证明 |
| 用户目标全部完成 | 行为缺陷与验证缺口仍待处理 | 未完成 |

最后测试文件只修正说明：final 完成指应用层预算；默认旁路测试不覆盖近单位倍率；超容量拒绝测试不检查合法块内部复制边界。当前 451 注释行、1047 代码行、30.11%，执行 token 不变。后续行为修复需更新受影响文件摘要、相关测试和逐文件统计，不能沿用本快照宣称新实现已验收。

## 文件输出与 OpenAL 终核快照（2026-09-09）

当前累计 **93/94** 个有效文件完成注释终核，剩余实时测试一个文件。本批只读核对 FFmpegFileReceiver 与 ALPlayer 的头和实现；既有注释准确记录主要接口、生命周期及失败路径，四文件格式检查通过，没有源码修改。已有 89 个摘要全部匹配，新增四个当前摘要登记如下。

文件导出仍有关闭错误未传播、整数容量及多声道平面边界；OpenAL 仍有暂停被同轮自动播放覆盖、模式切换期间队列格式混用、排队失败不传播及跨实例上下文隔离限制。上述行为不因注释终核而通过。沿用上批源码未变化的构建与 CTest 证据，不把实时节点测试解释为文件编码或实体播放测试。逐文件比例沿用同一源码快照的 cloc 结果：ICE 94/94 达标、2 空；主仓不足清单仍为 685 项。

## 变速链终核快照（2026-09-09）

当前累计 **89/94** 个有效文件完成注释终核，剩余 **5** 个；本段取代较早进度。新增 TimeStretcher 与 RStretcher 的头和实现，核对预热、启动补偿、参数状态发布/退役、边界切段和 final 帧预算。修正构造补偿量与预算完成语义，补充容量、流协议和原子访问约束；四文件执行 token 均未改变。

**行为审计未全部通过：** TimeStretcher 在近单位倍率旁路仍按非单位倍率累计输入，复制时不检查输出余量。倍率 1.0009、输出 512 帧时，第三块预算为 513 帧，可越过输出活动边界；该风险仅记录，未修复。RStretcher 单次容量和 final 提交协议依赖调用方，兼容 setter 不重新预热或刷新补偿。不能将注释终核和现有测试通过解释为这些行为安全。

主项目及独立 ICE 构建成功，实时 CTest 1/1 通过（4.46 秒）；全项目复扫仍为 1017 有效、5 空，ICE 94/94 数值达标，主仓 685/923 不足。修改文件逐项计数见任务单，完整不足清单为 `build/test_output/comment-audit/under-30.csv`。剩余 FFmpegFileReceiver、ALPlayer 头/实现和实时测试文件尚未登记终核。

## 源码构建、SDL 与示例终核快照（2026-09-09）

当前累计 **85/94** 个有效文件完成注释终核，剩余 **9** 个；本段取代下方较早快照的当前进度。本批新增九个源码依赖构建脚本、SDL 播放器头与实现、main/sample 两个示例文件，共 13 个。源码构建脚本只审查自维护适配层，没有读取或构建外部第三方源码。

修正 main.cpp 中对不同策略音轨缓存复用的过时说明，实际为按策略独立缓存；执行 token 不变。其余 12 文件只读核对。C++/CMake 格式检查通过，主项目及独立 ICE 构建成功；完整实时测试结果见同日任务记录。当前统计仍为 ICE 94/94 数值达标、2 空；全项目 1017 有效、5 空，主仓 685/923 不足。源码模式、多平台外部构建及实体 SDL 设备不作运行通过声明。

## 缓冲、FFmpeg 与预编译工具终核快照（2026-09-09）

当前累计 **72/94** 个有效文件完成注释终核，剩余 **22** 个；本段取代下方较早快照的当前进度。本批新增缓冲两文件、FFmpeg 工厂/实例四文件、十个 Find 模块及 PrebuiltLayout、三个交叉工具脚本、MSVC 环境 helper、架构探测及源码依赖入口，共 23 个。

修正 FFmpeg 实例头仍声称 EOF 小块读丢尾的过时说明；当前实现把排尾纳入余量缓存，并在零帧定位时重建编解码状态。其余本批文件只读终核。C++、CMake 格式检查与 Bash 语法检查通过；主项目和当前 ICE 独立源码构建成功，独立实时测试 1/1 通过（4.47 秒）。未运行真实媒体/多声道/所有寻道失败、Windows/MSVC 归档或多配置 DLL 部署专项验证，不作相关运行通过声明。

ICE 仍为 **94/94** 达到 30%，2 空文件；全项目复扫仍为 1017 有效、5 空，主仓 685/923 不足。最新文件统计及不足清单已刷新。已知 Linux SIMD 拆分风险、错误通道、头目录校验、DLL/导入库独立选择、多配置同名 DLL 覆盖及可选符号检查均保留明确边界，注释终核不代表已修复这些行为。

## 流式、资源生命周期与效果器终核快照（2026-09-09）

当前累计 **49/94** 个有效文件完成注释终核，剩余 **45** 个；本段取代下方较早恢复快照的当前数值。新增 27 个文件：流式/音轨/缓存池/缓存解码/封面/线程池/分配跟踪/链接转发共 14 个，节点基类与限幅占位/变调/压缩/双二阶滤波/均衡器共 13 个。

本次修正流式短读、估算长度、锁竞争请求及工厂失败通道的注释，并为链接转发脚本补充实际入口契约。ICE **94/94** 数值达标，2 空文件不适用；全项目复扫 **1017** 个有效文件、5 空文件，主仓 **685/923** 不足，见 `build/test_output/comment-audit/under-30.csv`。三份 `.h.in` 模板按 C/C++ Header 计数；其余沿用源码、脚本、运行时 Lua/着色器范围，排除第三方、预编译、代理头、生成文件与纯资源/翻译表。

主项目 Linux 构建与当前 ICE 独立源码构建均成功；独立 `IonTimeStretcherRealtimeTest` 1/1 通过（4.47 秒），启用 Linux C 堆包装。主项目当前未注册 CTest；独立实时测试不覆盖流式文件、线程池停机或所有效果器声音结果，不能据此宣称这些专项运行通过。修改文件格式与注释外文本一致性检查通过，新增只读终核文件格式检查通过。原有行为规范差异仍见职责记录，注释终核不是同步策略或历史异常实现已修复的声明。

## 恢复复核快照（2026-09-09，较早记录）

当前 ICE HEAD 为 `3a4a3eb`。上次提交 `658224d` 后新增流式解码与 MSVC 构建变更，共 10 个文件发生变化；历史职责记录仅对原内容有效。已登记的 13 个 SHA-256 全部仍匹配，本次只读终核新增 9 个文件，累计 **22/94** 个有效文件，**72** 个待终核。

使用 cloc 2.08、`--by-file --skip-uniqueness` 重新统计 ICE 自维护代码与脚本，包含新增 `cmake/ICEMsvcExternalEnvironment.cmake`，并补入旧索引漏列的 `cmake/cross/link.exe`：它是 Bash 转发脚本，不是二进制。当前为 **94 个有效文件、2 个空文件，93/94 达到 30%**；唯一不足为该转发脚本（0 注释行、3 代码行，0%）。本批只修改审计文档，未为补比例修改源码。

配置、音频格式、解码工厂/实例/媒体信息、接收端与播放回调这 9 个文件已对照当前实现和调用点确认契约，clang-format 23 只读检查通过。注释终核不豁免历史命名、头文件保护、异常和共享所有权等行为规范差异。

本机为 Linux；旧 Windows 构建与全项目统计产物未随仓库保存。下方历史门槛表保留原日期的证据，不代表当前验证状态；本次 ICE 数值明细已刷新，主仓 685 个不足仍仅为历史数字，未重新全项目扫描。本次验证结果见任务单同日恢复记录。

## 范围与证据边界（2026-09-08 历史）

本索引于 2026-09-08 从全项目 cloc 逐文件明细提取，只列 ICE 自维护范围：92 个有效文件和 2 个空文件。包含本引擎维护的构建与工具脚本，排除内部第三方源码、预编译包、生成目录及纯资源。不合并头文件与实现文件比例。

“复核记录定位”对应 [任务单](comment-audit-tasks.md) 的职责批次或复核记录，便于逐项回查，不表示每个文件均通过所有行为规范或所有平台运行测试。数值表不能单独证明注释质量。最终验收仍须逐项确认记录覆盖当前文件，未完成前不勾选全部通过。

## 验收门槛（2026-09-08 历史，不代表当前状态）

| 门槛 | 当前证据 | 状态 |
| --- | --- | --- |
| 范围完整与逐文件比例 | 全项目 files.csv，ICE 92/92 ≥30%，2 个空文件不适用 | 数值通过 |
| 接口契约、函数体说明、中文及热路径警告 | 下表对应职责记录及内容快照 | 13/92 已登记终核，79 个有效文件待登记确认 |
| 不改变业务、同步与构建命令 | C++ 去注释比较与构建脚本词法比较 | 当前工作区检查通过 |
| 格式化 | 每批 clang-format / cmake-format 记录 | 已执行，最终复验待完成 |
| 当前 ICE 源码构建与相关 CTest | 独立 Clang64、预编译外部依赖、12 并行 | 已通过 |
| 主项目要求的构建验证 | main-build.log：libc++ 原子 shared_ptr 特化不可用 | 未通过，不改写为 ICE 独立构建替代 |
| 未覆盖分支的证据边界 | Windows C 堆钩子未启用；设备/文件/其他平台未完整运行 | 不作运行通过声明 |
| 全项目复扫与其他不足清单 | 1013 有效、5 空；主仓 685 个不足见 under-30.csv | 已提供，非本轮扩改范围 |
| 修改文件逐项交付统计 | 下表和各批任务记录 | 已提供 |
| 全部审计通过 | 上述门槛尚有未完成项 | 未证明 |

统计产物位于 `build/test_output/comment-audit/`；本索引是本轮快照，后续源文件修改后必须重新核对，不能沿用旧比例充当新证据。

## 已完成注释终核的内容快照

2026-09-08：以下 13 个文件已对照当前内容与对应职责记录完成注释终核：混音/来源节点四文件，以及构建入口和历史错误类型九文件。新增九文件本轮只读确认，格式检查通过，未再次修改源码。此状态仅指注释内容，不豁免已记录的运行行为限制或主项目构建门槛。其余文件继续按已有记录核对，不将“有历史记录”自动升级为终核完成。

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `include/ice/core/MixBus.hpp` | `38bed3556a305681945cca9616cff720041b4cd6e0bf3b935c3f2f1441021e63` |
| `include/ice/core/SourceNode.hpp` | `e03cbe1546b4cd25c29f6500d789b1e9079cab2b5dc47645092d91d56fd67efd` |
| `src/ice/core/MixBus.cpp` | `328f7d524b9a987e1f450809ba79b19fc345502973ff6ff2e182b62c4848100e` |
| `src/ice/core/SourceNode.cpp` | `dff492233ce2175e4f57266e35c07cfdc702db25d1281b0a880b68a748de8738` |

| `3rdpty/CMakeLists.txt` | `9d9ee2c5b0ad249edb2b54808bb9d78cb01976ed0ec2765d6458f42bd2bc78a1` |
| `3rdpty/sources/cmake/Buildfmt.cmake` | `24a7db89b5b1975670868ab66129ce5451a940a2af5979b1c23d0a9e122767e6` |
| `cmake/configure-project.cmake` | `cdfc911092051b35fff3108027a1041810964b5f45c977331f45238fcff30a01` |
| `cmake/toolchain-macos-to-windows.cmake` | `7054d3db263776b6e55800c750807dd641c5fb5783128fb4fb561ffa1688584c` |
| `CMakeLists.txt` | `893a72f083d6b3c5ae5bed9083dd13e346f1f386c712427c9aa376489716ac6e` |
| `include/ice/execptions/buffer_error.hpp` | `0c9dad38ca33681d91e80b26fd467670c4647a3467c2c66f33a70614a9c5657a` |
| `include/ice/execptions/instance_build_error.hpp` | `bb49fa42716d5e13e286ef0214ec84498fcf1025293d0138abf76da85c5ebd44` |
| `include/ice/execptions/load_error.hpp` | `ee6e8bfd0fb4e10e2fa8c93da727395eac3c944116ce54dad08321e8ee4fd7bb` |
| `src/CMakeLists.txt` | `a0023cae16bad623eb6be2b28bf5a3d9135b310da5b7fe3939554b1e240e0619` |

2026-09-09 新增只读终核快照：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `include/ice/config/config.hpp` | `2dc494fba54f001c0078f0f49f28585a80fbf51c2a223158f0d4b65c7fd8dbd5` |
| `src/ice/config/config.cpp` | `82524379a490100aac2f813b32142e505d70753703b25210d0b753f02bbfabda` |
| `include/ice/manage/AudioFormat.hpp` | `1eaeaf88224792a6cf0a603d46f202f15a26534dc1690045f1fffb39717ba2dd` |
| `include/ice/manage/dec/IDecoderFactory.hpp` | `2ec4fef4d5c59fa5b7fddccb38b268cb03f951178c03017b230905b32cbfcae4` |
| `include/ice/manage/dec/IDecoderInstance.hpp` | `49ac80c7ed96b4b36532f17635c7c196663950a9ae461c3af1cf2396fea16dac` |
| `include/ice/manage/dec/MediaInfo.hpp` | `4a3b6a0128ff8fce1cb79eb72122c9f05781ec15f55118f62e6f1646eea8b2e1` |
| `include/ice/out/IReceiver.hpp` | `f25f286f4205c4a9d2a29db60a788785daaf99a940e1b6e5b9158bacd2566cbc` |
| `src/ice/out/IReceiver.cpp` | `6034fedabfe0a0dffd44967e13d68fd97f441cb66976a1573f1b0972fe898389` |
| `include/ice/core/PlayCallBack.hpp` | `c2e4f459fbe9ddb0aa1ef77e0277c519a72247399a0d6a926d876a7c29ef45b6` |

2026-09-09 流式、生命周期与效果器新增终核快照：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `cmake/cross/link.exe` | `7c22da2a8338db7f60cf9fa65302ddb2106861adef4e6dc25a3627f50debf638` |
| `include/ice/manage/AudioPool.hpp` | `517a1e0992e5187ae2d3d2b17732fee31e43d79e1da9efc64fea9dd0a8098a18` |
| `include/ice/manage/AudioTrack.hpp` | `14b6ad1f55fb7d4a237f211553757fc40f7942344d80888fcea2548ad26e0a20` |
| `include/ice/manage/dec/StreamingDecoder.hpp` | `9b43002599567794619c3ec1ddc8284be72032bacb787fb68863bf2514953143` |
| `src/ice/manage/AudioTrack.cpp` | `b263540acf806f39d5ca3ce5dfbb2170051ef7654f02c91f59333dd09e0348f0` |
| `src/ice/manage/dec/StreamingDecoder.cpp` | `40952f8d76e21f41fbb3042185975c549886701890b0a10f789c93a8b83d43e7` |
| `src/ice/manage/AudioPool.cpp` | `c7c9463deb68c45b319c5133f0212918a88aedcf10bbd12c35d0b43f8b83868d` |
| `include/ice/manage/dec/IDecoder.hpp` | `3e5c0537fb165f5d232ba73d006dbc0545bb582ef3ac54a8a143bd9a8d34eb82` |
| `include/ice/manage/dec/CachyDecoder.hpp` | `ab491ba192bc77ae84c34b07b101c7ced3a7b86af9b11003e6b5e74d81f92925` |
| `src/ice/manage/dec/CachyDecoder.cpp` | `1dd8a6fa6ac98d9528c717435f9386c665491a343d97e880664de322c86a085d` |
| `include/ice/manage/dec/AlbumArt.hpp` | `d1dea2985df91d827c63011d9c7932f2a0bdc21267b4a0d5201cb7b65aea26c4` |
| `include/ice/thread/ThreadPool.hpp` | `193311a2ac4d917f52e82455eb105e60f28aabbf8f354b989236eac71043cdaf` |
| `src/ice/thread/ThreadPool.cpp` | `809117183abb38a4de7336e0c62c21a16659215918de157dd2ddfb39d984b98c` |
| `include/ice/tool/AllocationTracker.hpp` | `5ffa497aecf888c87be36878aabbf5ed5887092564269d68d7c26118ca8d3d44` |
| `include/ice/core/IAudioNode.hpp` | `470ce9ab6b52680c12aa71bb8c568384fa8c53321c7c05f8084b31c648ec6f5c` |
| `include/ice/core/effect/IEffectNode.hpp` | `a99ebd854ccdb8e643f6357f8c14ced90060ed330d5731d69a252de0e1a230bc` |
| `src/ice/core/effect/IEffectNode.cpp` | `6a4ddb863c5c761bfb69c43d2d3fa5eb9182955528f91381c0324897c27c854f` |
| `include/ice/core/effect/Clipper.hpp` | `a6f75cd60a4b22ff110ce6a0859e59f532b16963f524ed50ecdaf878dce6e65e` |
| `src/ice/core/effect/Clipper.cpp` | `612efda032eda16d24591515635cc8d106efd13f724b47e7bae0616e1ae1197f` |
| `include/ice/core/effect/PitchAlter.hpp` | `6e67e04fb2e074b2a9f5f12d7d153c51e74fdf43d0f7932391efe9ff799f6897` |
| `src/ice/core/effect/PitchAlter.cpp` | `f63faa9f6cda0451b8477a20358069a0b59bee2746d6a4735ed9110d1dcaec82` |
| `include/ice/core/effect/Compresser.hpp` | `df63f62c875bcb62a64d22f751c35c20ff7e8abd947ffaeef888c98ed91c652a` |
| `src/ice/core/effect/Compresser.cpp` | `a9aa624fe1246be03b4ad9d0476e35490c486b0a0268c1126409fdcdb4405ad0` |
| `include/ice/core/effect/filter/BiquadFilter.hpp` | `ec939e6faa6d71cf44b63e7dd3f42490e1f6c910b96ff1a1ee2406ecb3c9947b` |
| `src/ice/core/effect/filter/BiquadFilter.cpp` | `e8842464f9f6b10a81930667bc6efa4744c6c6f7a8aed107237f2b6170fbcb82` |
| `include/ice/core/effect/GraphicEqualizer.hpp` | `09e9f05fa919fb94418556e115acebb474eed17d7d43a3366133750216f3f43c` |
| `src/ice/core/effect/GraphicEqualizer.cpp` | `f3132d9aca21e5cdca254d9147f572035360fd45feb65b4be7df4f19416fc38f` |

2026-09-09 缓冲、FFmpeg 与预编译工具新增终核快照：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `include/ice/manage/AudioBuffer.hpp` | `7bc1313370aa7e4d9fc0fc3501f87220e279b85f836729635629440cef0b6226` |
| `src/ice/manage/AudioBuffer.cpp` | `5fa4a6f2962b6cbcd17939070e3721c17223df91a549fd8119ad75acbbb8de19` |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | `983c7d6d76cf96887f0c9b0bd052ed09e39ef3994ab7bb0dbc81e5626cf6e9df` |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | `5054f990ffc7a779020824d354342d380ac3168a310a9a150e1b618f994a6537` |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | `61ca7248e4350b9b780551b61e81f53ed857ecf1720fd46c8b7c5ae734881021` |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | `7bba3c2fcedb5dbf770fd822907932babb20254c457be89da88bc5d027b3af64` |
| `3rdpty/cmake/modules/FindOpenAL.cmake` | `43d0ca13ec3850c1259476acaf24ffa6657ab0c888f47585180612bd33e7e1bf` |
| `3rdpty/cmake/modules/FindSDL3Static.cmake` | `760bc5bee6de3742f45f5adf04fa44da39776cab56de0b341687c2a3c7ffce67` |
| `3rdpty/cmake/modules/Findffmpeg.cmake` | `32aab4dd5d713b9fca46b1dcd1c403971be8baee6d6c9f4487e05917fec75d40` |
| `3rdpty/cmake/modules/Findfftw.cmake` | `72b9d53730770543fb9d4a9b9c79590d09100ff6b8bfeb21b2aadfc44685e2cf` |
| `3rdpty/cmake/modules/Findfmt.cmake` | `af0fa70127c181e67c721fd2f15cfd429430256020c423237931373f1748c910` |
| `3rdpty/cmake/modules/Findlame.cmake` | `e00c43acb37e6a3262aeaaa17b13b7573e4140cee90e15b29519885d128ee55a` |
| `3rdpty/cmake/modules/Findlibsamplerate.cmake` | `771f0a10fd22a4b78f4c6e89b42bfba39a2ebea92a208db0318c44865d14b7d9` |
| `3rdpty/cmake/modules/Findrubberband.cmake` | `0e25e932a40aad3eb62221b3feaf972b9c4fa29436a6f9f0638e3dd22e5f3838` |
| `3rdpty/cmake/modules/Findspdlog.cmake` | `a330bbe61e15382ca1c0f3cb58377ddcb154aa489172f9103a7ad7a92587cc85` |
| `3rdpty/cmake/modules/Findzlib.cmake` | `1471f2289122ecd4815e25a39e42dee50ae26286c0ded283865b52cf03baa994` |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | `ea43c6450e27cf1fb8d9cb8553bf5fe113c8cfe4b070ebd42d2ab729119b28a7` |
| `cmake/cross/clang-cl-gcc-compatible.sh` | `2a7043be75045949869d0c2e935850d303858b199a7ab98d98045c12edafa027` |
| `cmake/cross/llvm-lib-ar-compatible.sh` | `acdf1af10ae8f13bbf652659c0751275110b79e36dc4ec9828804e1b7b404ed6` |
| `cmake/cross/merge-msvc-archives.sh` | `9294f3a9f4ac1e134acb1f79271aa5443048527d6ae9a954fd6b5b262a058c0c` |
| `cmake/ICEMsvcExternalEnvironment.cmake` | `60dd411162593b32e35497605d81c33f845a988d97685b52e425e6c2932c7528` |
| `3rdpty/sources/cmake/arch-probe.cmake` | `5c05cc3228b384e9ba85629956b038a852bf00e670ddcea46699a2973a54df73` |
| `3rdpty/sources/CMakeLists.txt` | `be733cbd35aab81fe93d93ab890db1a3f24976afc790337a612e645316c59ccd` |

2026-09-09 源码构建、SDL 与示例新增终核快照：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | `50b741c248f5e377284c0b42d29fa6fa0d3527289602571f5ba0ab1fc58e3d30` |
| `3rdpty/sources/cmake/Buildfftw.cmake` | `6d65a044fb3dcdc86a30e58ff9095b0bcea94ea7b87cfdd10be22921a9db8db7` |
| `3rdpty/sources/cmake/Buildlame.cmake` | `6f53de40f2efcc159d22016a56c0cd488b30d315d9f9eba601f48265af7d554b` |
| `3rdpty/sources/cmake/Buildlibsamplerate.cmake` | `afd5df687865bf22abbee6e1db5f63e314d0287fa24e97b2291041f775b7136e` |
| `3rdpty/sources/cmake/Buildopenal.cmake` | `7eee377d93a5172fb3128ada96b80796e323308920db5f416ba898dff7961da5` |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | `25da739042386631eeea85cad57fefd2e096731d38ff413ff06235fe189ced47` |
| `3rdpty/sources/cmake/Buildsdl.cmake` | `e38b992025fcb4d2685e02defaae6e3e83d743f87dee9442af967223a6f196be` |
| `3rdpty/sources/cmake/Buildspdlog.cmake` | `d1d234602c84e6128e26b1a320e25a91944113060a50ca59f3f813065f6fbd2a` |
| `3rdpty/sources/cmake/Buildzlib.cmake` | `c28b0ce0556788530a5d2ccebb7f5be1557aed0fc073d2d79944ca4cefcd83df` |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | `c8790a9e9ec2f473ce0d7266c91a0df7afebee47de1fabf0f069c78a3bce0a21` |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | `b72ad54d4ac2c350776800f9d73176d667819ac87f33f2609144525c1f1b7efb` |
| `src/main.cpp` | `189d8d5d12dfb28b452a59c5e4f1ac7148f023c1adcf694ca345639126a2fe09` |
| `src/sample.cpp` | `28f4439eefab69cb1dc95ea64bec74e60007a0b0fc6ecf212e63ac1c63be4f2c` |

2026-09-09 变速链四文件注释终核（保留旁路容量行为缺陷）：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `include/ice/core/effect/TimeStretcher.hpp` | `9f7b9ef85aded5b40004343a26e5b22876fae8f176d53edf94cf27882605d84c` |
| `src/ice/core/effect/TimeStretcher.cpp` | `18276d3580cd876d81f501df1b6fc9ded6c1616566fe9cd1c3360fc9c944080b` |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | `37cee23591347de10b5750ee3e2d83c0890ffe0a15a352eac673549df987e9c6` |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | `84da7bfeb4f8d3566e213bafe1ddbd5a826344c30773b525351e9d7056fc90e8` |

2026-09-09 文件输出与 OpenAL 四文件只读注释终核：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | `94b0e094ec1606a098a12a3ba186452c8ba0561970521b8b7675afa16ac25369` |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | `4913c757915165f097a42f97a9cbf50d56edf7c59427a1ac02b9c1cdd7a18f4f` |
| `include/ice/out/play/openal/ALPlayer.hpp` | `3cbbddea680b0e9841da9bf7050fcb0d49da532e9e7b3a75c219d1992ed543d3` |
| `src/ice/out/play/openal/ALPlayer.cpp` | `a9e346f8daaa22dc5790870e2fc9e7e0aa3bafc8d4f41dc7f34ed242a0c52f5e` |

2026-09-09 实时测试注释终核：

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `tests/TimeStretcherRealtimeTest.cpp` | `942e8ea96ffb3ce9fdb55adcf7f5df74f469b50e0a3ed6ce02278e57e040a9da` |

后续任何内容或换行变更导致摘要变化时，先检查差异再更新本表；不应重复无差异的整文件补注释。

## 逐文件统计与复核记录定位（2026-09-09 更新）

| ICE 相对路径 | 注释行 | 代码行 | 注释率 | 复核记录定位 |
| --- | ---: | ---: | ---: | --- |
| `3rdpty/cmake/modules/Findffmpeg.cmake` | 36 | 82 | 30.51% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findfftw.cmake` | 15 | 35 | 30.00% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findfmt.cmake` | 16 | 37 | 30.19% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findlame.cmake` | 14 | 31 | 31.11% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findlibsamplerate.cmake` | 16 | 37 | 30.19% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/FindOpenAL.cmake` | 23 | 53 | 30.26% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findrubberband.cmake` | 23 | 52 | 30.67% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/FindSDL3Static.cmake` | 25 | 57 | 30.49% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findspdlog.cmake` | 19 | 43 | 30.65% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/Findzlib.cmake` | 17 | 38 | 30.91% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 182 | 422 | 30.13% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/CMakeLists.txt` | 8 | 17 | 32.00% | 注释终核完成；构建入口与错误类型收尾记录 |
| `3rdpty/sources/cmake/arch-probe.cmake` | 10 | 21 | 32.26% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | 134 | 312 | 30.04% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildfftw.cmake` | 46 | 98 | 31.94% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildfmt.cmake` | 8 | 17 | 32.00% | 注释终核完成；构建入口与错误类型收尾记录 |
| `3rdpty/sources/cmake/Buildlame.cmake` | 58 | 135 | 30.05% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildlibsamplerate.cmake` | 45 | 103 | 30.41% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildopenal.cmake` | 35 | 80 | 30.43% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | 165 | 384 | 30.05% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildsdl.cmake` | 63 | 145 | 30.29% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildspdlog.cmake` | 12 | 28 | 30.00% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/cmake/Buildzlib.cmake` | 44 | 101 | 30.34% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `3rdpty/sources/CMakeLists.txt` | 7 | 12 | 36.84% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `cmake/configure-project.cmake` | 4 | 7 | 36.36% | 注释终核完成；构建入口与错误类型收尾记录 |
| `cmake/cross/clang-cl-gcc-compatible.sh` | 19 | 43 | 30.65% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `cmake/cross/llvm-lib-ar-compatible.sh` | 7 | 14 | 33.33% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `cmake/cross/merge-msvc-archives.sh` | 17 | 39 | 30.36% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `cmake/toolchain-macos-to-windows.cmake` | 11 | 15 | 42.31% | 注释终核完成；构建入口与错误类型收尾记录 |
| `CMakeLists.txt` | 30 | 70 | 30.00% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/config/config.hpp` | 18 | 25 | 41.86% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/core/effect/Clipper.hpp` | 7 | 13 | 35.00% | 注释终核完成；效果器终核记录 |
| `include/ice/core/effect/Compresser.hpp` | 39 | 51 | 43.33% | 注释终核完成；效果器终核记录 |
| `include/ice/core/effect/filter/BiquadFilter.hpp` | 22 | 24 | 47.83% | 注释终核完成；效果器终核记录 |
| `include/ice/core/effect/GraphicEqualizer.hpp` | 75 | 49 | 60.48% | 注释终核完成；效果器终核记录 |
| `include/ice/core/effect/IEffectNode.hpp` | 44 | 38 | 53.66% | 注释终核完成；效果器终核记录 |
| `include/ice/core/effect/PitchAlter.hpp` | 38 | 31 | 55.07% | 注释终核完成；效果器终核记录 |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | 104 | 70 | 59.77% | 注释终核完成；A2：预热、补偿与排尾；行为边界见变速链记录 |
| `include/ice/core/effect/TimeStretcher.hpp` | 186 | 120 | 60.78% | 近单位倍率修复后复核完成 |
| `include/ice/core/IAudioNode.hpp` | 15 | 12 | 55.56% | 注释终核完成；效果器终核记录 |
| `include/ice/core/MixBus.hpp` | 102 | 103 | 49.76% | 注释终核完成；混音与来源节点终核记录 |
| `include/ice/core/PlayCallBack.hpp` | 18 | 15 | 54.55% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/core/SourceNode.hpp` | 168 | 174 | 49.12% | 注释终核完成；混音与来源节点终核记录 |
| `include/ice/execptions/buffer_error.hpp` | 5 | 9 | 35.71% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/execptions/instance_build_error.hpp` | 5 | 9 | 35.71% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/execptions/load_error.hpp` | 5 | 9 | 35.71% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/manage/AudioBuffer.hpp` | 155 | 339 | 31.38% | SIMD 解交错修复后复核完成 |
| `include/ice/manage/AudioFormat.hpp` | 9 | 10 | 47.37% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/manage/AudioPool.hpp` | 74 | 160 | 31.62% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/manage/AudioTrack.hpp` | 48 | 55 | 46.60% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/manage/dec/AlbumArt.hpp` | 21 | 39 | 35.00% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/manage/dec/CachyDecoder.hpp` | 50 | 54 | 48.08% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 8 | 16 | 33.33% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 24 | 24 | 50.00% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `include/ice/manage/dec/IDecoder.hpp` | 26 | 20 | 56.52% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 16 | 20 | 44.44% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 20 | 16 | 55.56% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/manage/dec/MediaInfo.hpp` | 12 | 17 | 41.38% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 38 | 31 | 55.07% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/out/io/cod/IEncoder.hpp` | 0 | 0 | 不适用 | 空文件；无代码可审 |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 116 | 69 | 62.70% | 输出关闭错误传播修复后复核完成 |
| `include/ice/out/IReceiver.hpp` | 28 | 30 | 48.28% | 注释终核完成；配置与基础接口恢复终核记录 |
| `include/ice/out/play/openal/ALPlayer.hpp` | 97 | 63 | 60.62% | 排队失败退出与线程回收修复后复核完成 |
| `include/ice/out/play/openal/ALSource.hpp` | 0 | 0 | 不适用 | 空文件；无代码可审 |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 58 | 62 | 48.33% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `include/ice/thread/ThreadPool.hpp` | 59 | 101 | 36.88% | 注释终核完成；流式与资源生命周期终核记录 |
| `include/ice/tool/AllocationTracker.hpp` | 13 | 10 | 56.52% | 注释终核完成；流式与资源生命周期终核记录 |
| `src/CMakeLists.txt` | 23 | 50 | 31.51% | 注释终核完成；构建入口与错误类型收尾记录 |
| `src/ice/config/config.cpp` | 5 | 10 | 33.33% | 注释终核完成；配置与基础接口恢复终核记录 |
| `src/ice/core/effect/Clipper.cpp` | 3 | 6 | 33.33% | 注释终核完成；效果器终核记录 |
| `src/ice/core/effect/Compresser.cpp` | 29 | 54 | 34.94% | 注释终核完成；效果器终核记录 |
| `src/ice/core/effect/filter/BiquadFilter.cpp` | 21 | 49 | 30.00% | 注释终核完成；效果器终核记录 |
| `src/ice/core/effect/GraphicEqualizer.cpp` | 82 | 189 | 30.26% | 注释终核完成；效果器终核记录 |
| `src/ice/core/effect/IEffectNode.cpp` | 19 | 42 | 31.15% | 注释终核完成；效果器终核记录 |
| `src/ice/core/effect/PitchAlter.cpp` | 27 | 59 | 31.40% | 注释终核完成；效果器终核记录 |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | 142 | 328 | 30.21% | 注释终核完成；A2：预热、补偿与排尾；行为边界见变速链记录 |
| `src/ice/core/effect/TimeStretcher.cpp` | 322 | 740 | 30.32% | 近单位倍率修复后复核完成 |
| `src/ice/core/MixBus.cpp` | 116 | 262 | 30.69% | 注释终核完成；混音与来源节点终核记录 |
| `src/ice/core/SourceNode.cpp` | 127 | 288 | 30.60% | 注释终核完成；混音与来源节点终核记录 |
| `src/ice/manage/AudioBuffer.cpp` | 53 | 121 | 30.46% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `src/ice/manage/AudioPool.cpp` | 8 | 18 | 30.77% | 注释终核完成；流式与资源生命周期终核记录 |
| `src/ice/manage/AudioTrack.cpp` | 50 | 79 | 38.76% | 注释终核完成；流式与资源生命周期终核记录 |
| `src/ice/manage/dec/CachyDecoder.cpp` | 83 | 151 | 35.47% | 注释终核完成；流式与资源生命周期终核记录 |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 69 | 155 | 30.80% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 227 | 499 | 31.27% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 118 | 240 | 32.96% | 注释终核完成；流式与资源生命周期终核记录 |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 370 | 824 | 30.99% | 输出关闭错误传播修复后复核完成 |
| `src/ice/out/IReceiver.cpp` | 3 | 5 | 37.50% | 注释终核完成；配置与基础接口恢复终核记录 |
| `src/ice/out/play/openal/ALPlayer.cpp` | 458 | 1031 | 30.76% | 查询、退队和自动播放错误退出复核完成 |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 108 | 227 | 32.24% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `src/ice/thread/ThreadPool.cpp` | 32 | 74 | 30.19% | 注释终核完成；流式与资源生命周期终核记录 |
| `src/main.cpp` | 75 | 168 | 30.86% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `src/sample.cpp` | 6 | 13 | 31.58% | 注释终核完成；源码构建、SDL 与示例终核记录 |
| `tests/TimeStretcherRealtimeTest.cpp` | 534 | 1243 | 30.05% | SIMD 解交错修复后复核完成 |
| `cmake/ICEMsvcExternalEnvironment.cmake` | 17 | 37 | 31.48% | 注释终核完成；缓冲、FFmpeg 与预编译工具终核记录 |
| `cmake/cross/link.exe` | 2 | 3 | 40.00% | 注释终核完成；流式与资源生命周期终核记录 |

### 新增诊断实现摘要（2026-09-09）

| 文件 | SHA-256 |
| --- | --- |
| `src/diagnostics/AllocationTracker.cpp` | `1263b947a7ce7e2378a3f5c86548de651d498cf5a0fadaa5a8959b00dd0ac4e3` |
