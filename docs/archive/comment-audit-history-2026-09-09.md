# 注释治理历史记录（归档，2026-09-09）

本文仅保留历史证据，不再追加，也不是当前执行清单。历史上的“继续审计”“完整验收”等要求已由新的精简计划替代。除非定位特定历史问题，不需通读本文；当前入口为 `../comment-audit-tasks.md`。

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

## OpenAL 查询、退队和自动播放错误退出（2026-09-09）

本批修改 `src/ice/out/play/openal/ALPlayer.cpp`：处理数量、源状态、队列长度各自查询成功后才使用结果；正常退队失败不复用未知 buffer ID；首批自动播放与欠载恢复失败清除运行请求。错误后跳至统一清理，避免继续查询或按默认值操纵队列。控制侧 pause/resume、上下文绑定与清理调用的完整错误策略尚未覆盖，不能解释为所有 AL/ALC 返回路径均已通过。

原探针只要求最终退出，会因旧版后续上传检查捕获遗留错误而通过，不能证明查询后及时退出。增强探针记录错误后至清理前的额外查询，旧版退出 4；修复后分别在第 1/2/3 次查询注入无效参数，均不再继续正常查询，清理退出并可重启，退出 0。探针位于 `build/test_output/comment-audit/query-failure-*`。此前首批/回填排队故障、队列模式及两个暂停探针重新链接，五个场景也全部通过。退队和播放失败本批为代码路径审查，未单独注入相应故障。

主项目和独立 ICE 构建成功，格式化/检查通过，完整现有 CTest 1/1 通过（4.67 秒）。本批修改文件 **350 注释行、801 代码行、30.41%**。全项目 cloc 2.08 复扫 1017 有效、5 空，ICE 94/94 ≥30%，主仓 685/923 不足。统计自维护业务/头/测试/工具和运行时脚本，排除第三方、预编译、生成/代理代码及纯资源；files.csv、under-30.csv 已刷新。当前文件摘要更新，其余 93 个匹配，未提交、未推送。

## OpenAL 排队失败退出与线程回收（2026-09-09）

首批/模式重建的 refillAllBuffers 及正常回填均检查 queueAudioBuffer 返回值，失败清除运行请求并进入统一停源退队出口。上传后独立检查错误，失败时不把旧缓冲再次排队。stop 不再依据 running=false 提前返回，仍 join 已退出或收尾中的线程；start 在重新创建线程对象前 join 旧线程。失败停止不回退图游标，也不自动重试或重放已经拉取的音频。

链接器包装 alSourceQueueBuffers 在指定次调用注入无效源错误，包装 alSourceStop 观测供数清理出口。旧版第 1 次失败后未退出，探针等待上限耗尽并退出 1；修复后第 1 和第 9 次失败均清理退出，查询 running=false，并可直接 start 后 stop，不覆盖未 join 的线程对象。探针位于 `build/test_output/comment-audit/queue-failure-*`。重新链接模式探针及第一次/第九次暂停探针，全部退出 0。测试等待仅为独立探针时限，没有进入业务循环。

| 本批修改的 ICE 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/openal/ALPlayer.cpp` | 344 | 778 | 30.66% |
| `include/ice/out/play/openal/ALPlayer.hpp` | 84 | 63 | 57.14% |

主项目与独立 ICE 构建成功，修改文件格式化/检查通过，现有完整 CTest 1/1 通过（4.61 秒）。全项目 cloc 2.08 复扫：1017 有效、5 空，ICE 94/94 ≥30%，主仓 685/923 不足；自维护业务/头/测试/构建工具/运行时脚本纳入，第三方、预编译、生成/代理代码及纯资源排除。files.csv、under-30.csv 已刷新，两文件摘要更新，其余 92 个匹配。

此次只覆盖上传/入队失败的退出链，未宣称 alGetSourcei、退队、播放调用等所有错误均已传播，亦未验证实体设备或全局上下文隔离。错误路径仍使用既有格式化输出，需要继续治理。未提交、未推送。

## OpenAL 队列模式一致性修复（2026-09-09）

ALBackend 新增供数线程独占的 queuedSpatialOutput。refillAllBuffers 在空队列开始填充时读取控制请求一次，queueAudioBuffer 使用该固定快照；正常回收槽也保持该队列格式。中途模式请求仍由既有重建标志触发下一轮清队列并接收，避免同一队列混用 mono/stereo。快照不是共享控制成员，不新增原子、锁或等待，未改图游标预读策略。

空后端探针在第三次上游取数改变空间模式，包装 alBufferData 观察上传格式。旧版首批八槽混用格式，探针退出 1；修复后扩展检查十六次上传，首批八槽统一原格式，清空后的第二批八槽统一新格式，退出 0。这同时排除“永远忽略模式切换”的错误修补。重新链接此前暂停探针，第一次和第九次取数暂停场景均通过。探针位于 `build/test_output/comment-audit/queue-format-*`，没有把空后端验证解释为全部实体驱动行为通过。

| 本批修改的 ICE 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/openal/ALPlayer.cpp` | 340 | 772 | 30.58% |
| `include/ice/out/play/openal/ALPlayer.hpp` | 82 | 63 | 56.55% |

主项目和独立 ICE 构建成功，修改文件格式化/检查通过，完整现有 CTest 1/1 通过（4.63 秒）。全项目 cloc 2.08 复扫仍为 1017 有效、5 空，ICE 94/94 ≥30%，主仓 685/923 不足。范围包括自维护业务/头/测试/工具与运行时脚本，排除第三方、预编译、生成/代理代码及纯资源；files.csv、under-30.csv 已刷新。两文件当前摘要更新，其余 92 个匹配。排队失败恢复、多实例上下文和其他边界问题仍需继续处理，未提交、未推送。

## OpenAL 自动播放暂停竞态修复（2026-09-09）

本批只修改 `src/ice/out/play/openal/ALPlayer.cpp`：refillAllBuffers 末尾播放与循环欠载恢复，在持 alMutex 时复查 m_running 与 m_paused。pause 的源暂停操作使用相同锁：如果自动播放先取得锁，pause 随后停源；如果 pause 已执行，自动播放读取暂停请求后跳过。stop 会清暂停位，因此运行请求也必须参与判断。保留已有原子序、队列与供数策略，没有引入等待或每块分配。

独立探针链接当前 ICE 与 OpenAL Soft，设置 ALSOFT_DRIVERS=null，包装 alSourcePlay 记录暂停后发生的播放调用。合成上游在第九次取数暂停播放器，再由测试控制侧 stop/join，旧版探针退出 1，修复后退出 0；另在第一次取数暂停，覆盖首次填队末尾播放入口，也退出 0。三秒条件变量等待仅为探针失败时限，未进入业务实现。该探针覆盖指定暂停/停止交错，不证明所有并发时序、恢复播放音质或真实驱动行为。空后端报告的设备环境/优先级警告不影响探针成功。

探针源码和链接参数位于 `build/test_output/comment-audit/pause-race-*`，并未注册新 CTest。主项目及独立 ICE 构建成功，修改文件格式化/检查通过，完整现有 CTest 1/1 通过（4.58 秒）。本批修改文件 **336 注释行、770 代码行、30.38%**。

全项目 cloc 2.08 复扫：1017 有效、5 空，ICE 94/94 达标，主仓 685/923 不足。统计范围仍含自维护业务/测试/构建工具及运行时脚本，排除第三方、预编译、生成/代理代码和纯资源。files.csv、under-30.csv 已刷新，本文件摘要更新，其余 93 个匹配。模式切换期间的队列格式一致性、错误恢复及多实例上下文隔离继续待处理，未提交、未推送。

## 输出关闭错误传播修复（2026-09-09）

按 [FFmpeg avio_closep 官方契约](https://ffmpeg.org/doxygen/7.1/avio_8h.html) 检查关闭返回码。close 在无已有错误时保存关闭失败，继续释放容器资源；start 关闭后结合错误字符串计算最终返回值，避免 trailer 成功但最后 IO 失败仍报告成功。已有编码错误或协作取消优先，不被清理失败覆盖。

链接当前 ICE 静态构建的独立探针用 --wrap=avio_closep 调用真实关闭释放资源，再返回负错误码。旧实现测试退出 1，确认失败未传播；修复后精确检查关闭诊断、再次导出并由进度回调 stop 验证取消文本保留，退出 0。未包装的正常导出也成功。探针位于 `build/test_output/comment-audit/close-error-*`；故障是模拟返回码，不表示实际磁盘出现故障，也不是新注册 CTest。

| 本批修改的 ICE 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 326 | 747 | 30.38% |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 111 | 68 | 62.01% |

主项目和独立 ICE 构建成功，修改文件格式化和格式检查通过；相关 CTest 1/1 通过。全项目 cloc 2.08 复扫仍为 1017 有效、5 空，ICE 94/94 ≥30%，主仓 685/923 不足。范围仍含自维护业务/头/测试/构建工具/运行时脚本，排除第三方、预编译、生成/代理代码及纯资源。files.csv、under-30.csv 已刷新，两文件摘要更新，其余 92 个匹配。其他审计缺陷继续保留，未提交、未推送。

## 编码帧多声道指针表修复（2026-09-09）

本批仅修改 `src/ice/out/io/FFmpegFileReceiver.cpp` 的 FIFO 输出指针表与对应说明：由 frame->data 改为 frame->extended_data。[FFmpeg 官方 AVFrame 文档](https://www.ffmpeg.org/doxygen/7.1/structAVFrame.html) 要求超过固定 data 容量的平面声道使用 extended_data；该表也适用于交错格式，因此无需按声道数分支。布局由 av_frame_get_buffer 成功建立后才进入 FIFO 读取，所有权与 PTS 逻辑保持不变。

尝试用十六声道生成 WavPack/AAC/FLAC：当前引擎预编译包缺少 WavPack muxer，AAC 不支持 9.1.6 布局，FLAC 上限八声道。这些是验证场景不可用，不是指针表修复失败，也不是已复现旧版越界。独立探针链接当前 ICE 构建产物，成功生成二声道 AAC/m4a、八声道 FLAC/ogg、十六声道 PCM/wav；系统 FFmpeg 解码均成功，各得到 1024 帧。输入为空源静音，未验证各声道信号身份或音质。超过八声道平面编码的端到端验证缺口仍保留。

探针源码、二进制、媒体与解码结果均位于 `build/test_output/comment-audit/multichannel-*`，没有写入源码资源目录，也没有把一次性探针伪装为新增 CTest。修改文件 clang-format 与检查通过，主项目和独立 ICE 构建成功；相关实时 CTest 1/1 通过（4.57 秒），该测试不验证文件导出。

本批修改文件统计：`src/ice/out/io/FFmpegFileReceiver.cpp`，**323 注释行、744 代码行、30.27%**。全项目 cloc 复扫 1017 有效、5 空，ICE 94/94 达标，主仓 685/923 不足。沿用自维护业务/测试/构建工具/运行时脚本范围，排除第三方、预编译、生成/代理代码和纯资源；files.csv 与 under-30.csv 已刷新。当前文件摘要更新，其余 93 个匹配。错误关闭传播、整数容量及设备竞态等问题仍待处理，未提交、未推送。

## Linux SIMD 解交错边界修复（2026-09-09）

修复 write_interleaved_stereo 的四帧步长/八浮点存储冲突和错误声道重排。每组用两次 128 位非对齐加载读四个立体声帧，分别提取偶数/奇数样本，每声道恰写四浮点；循环以实际有效帧为界，最多三帧标量尾部不触碰输入填充或目标后缀。保留 SIMD 路径，未改成纯标量实现。增加零帧、空源和非双声道保护；源目标仍要求不重叠。

调用签名和掩码选择已核对 [Clang 官方 xmmintrin 文档](https://clang.llvm.org/doxygen/xmmintrin_8h.html)。新增 Linux 专属回归覆盖 0、1、3、4、5、7、8、9、17 帧，独立逐帧核对两声道和后缀，并检查受监测堆操作。旧实现运行新增用例时 CTest 发生 SegFault（3.18 秒）；修复后通过。测试源为固定最大数组，不能据此宣称已经用保护页或 ASan 验证所有源越界读取；不读填充同时由循环边界审查支撑。

| 本批修改的 ICE 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 124 | 289 | 30.02% |
| `tests/TimeStretcherRealtimeTest.cpp` | 490 | 1142 | 30.02% |

主项目和独立 ICE 当前源码构建成功，修改文件 clang-format 及只读检查通过；最终相关 CTest 1/1 通过，Linux 当前含 14 个用例。没有验证其他 CPU 目标或性能吞吐，也没有修改第三方代码。

全项目 cloc 复扫并合并最终测试说明重计：1017 有效、5 空；ICE 94/94 ≥30%，主仓 685/923 不足。范围包含自维护业务/头/测试/工具和运行时脚本，排除第三方、预编译、生成/代理文件和纯资源。files.csv、under-30.csv 已刷新，两个修改文件摘要更新，其余 92 个匹配。其他行为缺陷仍待修复；本轮没有提交或推送。

## 近单位倍率容量缺陷修复（2026-09-09）

本批进入行为修复：TimeStretcher 仅在速度精确为 1.0 且半音为 0.0 时旁路，其余参数经已有预热状态处理。输入小数累计不再进入一对一复制路径，保留非单位时钟语义；同时不再忽略接近零但非零的音高配置。仅修改本地选择条件，没有新增第三方 API 调用。

新增 testNearUnityOutputBounds：倍率 0.9991/1.0009，512 帧输出连续 32 块，比较独立总时长公式得到的累计上游输入；两声道各留 8 帧容量后缀，检查合法全容量清零后没有额外音频写入；同时要求容量拒绝计数与受监测分配/释放为零。首次测试把后缀误设为必须保留非零，核对 AudioBuffer::clear 的全容量契约后修正为零后缀检查。修正后的测试在旧实现上明确失败一次（输出后缀），应用精确旁路修复后通过；不是靠放宽期望使旧实现通过。

| 本批修改的 ICE 路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/effect/TimeStretcher.hpp` | 186 | 120 | 60.78% |
| `src/ice/core/effect/TimeStretcher.cpp` | 320 | 735 | 30.33% |
| `tests/TimeStretcherRealtimeTest.cpp` | 472 | 1101 | 30.01% |

修改文件已格式化并通过检查。主项目与独立 ICE 构建成功，完整实时 CTest 1/1 通过，现含 13 个用例。最终只补实现说明后再次构建和测试。测试验证本次局部边界，不宣称所有音质、第三方内部内存或并发交错都被覆盖。

全项目 cloc 复扫并对最后补充说明的测试文件重计合并：1017 有效、5 空；ICE 94/94 ≥30%，主仓 685/923 不足。统计范围仍含自维护业务/测试/构建工具/运行时脚本及头模板，排除第三方、预编译、生成/代理文件及纯数据。全明细和其他不足清单已刷新 files.csv、under-30.csv；三个受影响摘要更新，其余 91 个匹配。其他既有行为缺陷仍待修复，未提交、未推送。

## 实时测试终核与行为修复入口（2026-09-09）

本批完成最后一个测试文件的逐段注释终核，累计 **94/94**，2 空文件不适用；94 份当前 SHA-256 均已匹配，验收索引已新增当前门槛表并明确旧表的历史属性。注释终核完成不代表用户要求的全部行为审计已经通过，目标仍继续。

核对了两个合成源、固定脚本 provider、静音/极值/可闻帧观察器、TLS 计数、普通 new/delete 替换、Linux C 堆包装，以及 main 调用的全部 12 个测试。测试覆盖状态接管、重置、final、拒绝超容量、暂停、输入小数余量、极值参数、延迟补偿、边界和并发发布。现有局限已经核对：部分窗口不检查 C free；计数不包含所有分配 API 或共享库内部；静音不排除 NaN；极值不证明信号次序；可闻帧计数不等于有效输出长度；并发调度不保证全部交错或最终状态接管。

本批修改 `tests/TimeStretcherRealtimeTest.cpp`：**451 注释行、1047 代码行、30.11%**。修正 final 完成含义，明确精确单位倍率旁路与超容量入口测试不能发现合法输出块内部的近单位倍率越界。只改注释，非注释 token 与 HEAD 一致；clang-format 及只读检查通过。主项目和独立 ICE 构建成功，相关 CTest 1/1 通过（4.46 秒）。

全项目 cloc 2.08 复扫仍为 1017 有效、5 空；ICE 94/94 数值达标，主仓 685/923 不足。范围含自维护业务/头/测试/构建工具/运行时 Lua/着色器/头模板，排除第三方、预编译、生成目录、代理头、纯资源与翻译表。最新逐文件明细及其他不足清单为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

下一阶段先修复已确认的 TimeStretcher 近单位倍率旁路容量缺陷，补充覆盖倍率两侧、跨块累计及输出边界的回归，再检查尚存的内存边界、失败传播和播放器状态问题。用户持续要求全部 ICE 代码通过审计，因此后续不能只重复注释统计或将已记录行为缺陷视为豁免；修复必须保持局部范围并重新构建、测试及更新摘要。当前未提交、未推送。

## 文件输出与 OpenAL 终核记录（2026-09-09）

本批只读完成四文件注释终核，累计 **93/94**；未修改源码，已有 89 份内容摘要全部匹配，新增四份摘要及原逐文件计数见验收索引。剩余 `tests/TimeStretcherRealtimeTest.cpp` 已开始检查夹具、provider 与分配计数范围，尚未完成全部用例终核。

- FFmpegFileReceiver 头与实现：核对按扩展名选择容器/编码器、采样率/布局回退、半初始化清理、输入时钟进度、重采样 FIFO、帧 PTS、send/receive、trailer 与取消流程。注释与当前实现一致：start 同步且不支持并发进入，stop 不 join，错误/取消保留部分文件，普通进度与诊断不跨线程同步；send EAGAIN 不重试，短尾不补零，FIFO 用 frame->data，块长窄化和累计容量仍有限制。
- 文件输出行为边界补充记录：close 忽略 avio_closep 返回值，start 的成功不能证明关闭阶段没有 IO 错误；重采样容量查询为零时直接成功返回而未消费输入，容量窄化与多声道平面处理仍须专项修复/验证。没有运行真实文件编码，不标记这些路径运行通过。
- ALPlayer 头与实现：核对进程环境/日志、枚举与逐设备回退、上下文与源/缓冲失败回滚、start/stop/join、暂停恢复、空间参数、下混和定长队列。注释已说明基类图绑定不参与内部源锁、轮询休眠与错误日志、失败不回滚图游标、多个播放器上下文隔离及暂停竞态。格式转换依赖固定有效输入，不把浮点转换或驱动上传视为任意数据安全。
- OpenAL 行为边界补充记录：refillAllBuffers 不消费 stop/pause，每槽独立读取空间开关，切换期间可能向尚未清空的旧格式队列提交新格式；排队失败返回值被调用者忽略，槽失败后没有可靠恢复流程。自动欠载恢复不复查 pause，可覆盖控制侧刚发出的暂停。当前均未修复，未运行实体设备或跨实例播放专项测试。

四文件 clang-format 只读检查通过；本批仅修改审计文档，沿用上批主项目/独立 ICE 构建成功和实时 CTest 1/1 的源码快照证据，没有重复运行无关测试。没有代码变更，逐文件 cloc 仍沿用最近全项目复扫：1017 有效、5 空，ICE 94/94 达到 30%，主仓 685/923 不足。范围及排除规则未变，其他不足完整清单见 `build/test_output/comment-audit/under-30.csv`。注释终核与全部行为验收分开，已发现缺陷仍阻止宣称目标全部通过。

## 变速链终核记录（2026-09-09）

本批完成 TimeStretcher/RStretcher 四文件的注释与契约终核，累计 **89/94**，剩余 5 个有效文件。新增 SHA-256 登记于验收索引，已有 85 个摘要全部匹配。

- RStretcher：核对构造最终参数、固定指针工作区、分块提交、启动填充与丢弃、三阶段静音预热、final 与 drain、reset 和兼容 setter。明确补偿量固定于构造，setter 不重算或预热；处理入口不检查约定容量，不记录单独的 final 已提交状态，输入/输出不可重叠，调用者须限制积压并维持一次 final 后仅 drain 的协议。无分配说明受这些前提约束，不是任意输入的保证。
- TimeStretcher：核对暂停优先级、单控制写入者、pending 独占移交与退役回收、有限次 provider 配置读取、手动定位与显式边界、跨块旧段排尾及输入余量、最终输出帧预算。修正 finalDrained 的物理排空歧义，补充原子字段的读写线程及用途。provider 配置不稳定时回退整段并不证明真实边界完整；不同原子查询不构成整体快照。
- **未修复的行为缺陷：近单位倍率旁路可能越过输出边界。** `should_bypass` 接受与 1 相差小于 0.001 的倍率，输入预算仍按该倍率计算，`process_bypass_segments` 用输入帧数直接复制且不裁剪输出余量。按实现公式复算，倍率 1.0009、每块输出 512 帧，前三块输入为 512、512、513。第三块超过输出活动范围；若容量没有额外余量则存在存储越界风险。本轮保留执行语义，只在接口、复制点和本记录明确问题，不能标为全部行为审计通过。该复算不是内存检测器运行证据，既有实时测试通过也不排除该缺陷。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/effect/TimeStretcher.hpp` | 186 | 120 | 60.78% |
| `src/ice/core/effect/TimeStretcher.cpp` | 318 | 736 | 30.17% |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | 100 | 68 | 59.52% |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | 136 | 315 | 30.16% |

四文件已 clang-format 并通过只读格式检查，非注释 token 与 HEAD 一致。主项目 `cmake --build build --parallel 12` 成功；独立 ICE Debug 静态源码构建成功，`IonTimeStretcherRealtimeTest` 1/1 通过（4.46 秒）。没有修改测试逻辑或执行第三方源码构建。

全项目 cloc 2.08 逐文件复扫：1017 有效、5 空，ICE 94/94 达到 30%，主仓 685/923 不足。范围含自维护头/实现、测试、构建工具、运行时 Lua/着色器和三份头模板，排除第三方、预编译、生成目录、代理头、纯数据/翻译表。最新全明细和其他不足清单见 `build/test_output/comment-audit/files.csv`、`under-30.csv`；没有跨模块补注释。剩余 FFmpegFileReceiver/ALPlayer 四文件及实时测试尚需终核，未提交、未推送。

## 源码构建、SDL 与示例终核记录（2026-09-09）

本批完成 13 个文件的注释终核，累计 **85/94**，剩余 9 个有效文件；新增摘要和逐文件统计见验收索引。

- 九个源码依赖脚本：核对构建类型与 CRT、工具和 SDK 环境传递、源码时间戳、配置哈希、安装及失败路径、Meson/CMake/Autotools 目标闭包。FFmpeg/LAME/zlib 仍固定静态，部分 MSVC 分支固定静态 CRT；非 MSVC Rubber Band 已有 build.ninja 可跳过 setup、LTO 分支覆盖链接参数等局限与现有说明一致，没有因说明完整而宣称实现满足所有链接偏好规则。
- SDL 头和实现：核对初始化、枚举、打开失败、显式 close、停止 join、暂停和主动供数循环。初始化原子不保护全局生命周期，基类 set_source 不参与 source_mutex；查询/提交错误处理和队列阈值均保持已记录边界。没有运行实体设备。
- main/sample：核对图连接、实际输出根、硬编码路径、默认策略与流式策略、分离线程及退出码。发现旧注释把不同策略请求描述为缓存复用，修正为按策略独立保活；未把手工示例和未接入构建的占位入口当成 CTest。

本批唯一新增源码修改 `src/main.cpp`：**67 注释行、154 代码行、30.32%**；仅替换两行注释，非注释 token 与 HEAD 一致。其他 12 个文件未修改。C++ clang-format 与九个 CMake 的格式检查通过，主项目构建成功，独立 ICE 构建成功并重链示例；CTest 1/1 通过。没有执行第三方源码构建、实体设备或手工试听，不作这些运行通过声明。

修改后全项目 cloc 2.08 逐文件复扫仍为 1017 有效、5 空；ICE 94/94 数值达标，主仓 685/923 不足。统计包含自维护头/实现、测试、工具/构建脚本、运行时 Lua/着色器和头文件模板，排除第三方、预编译、生成目录、代理头、纯资源/翻译表。完整明细及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。最后 9 文件为 TimeStretcher/RStretcher、ALPlayer、FFmpegFileReceiver 的头与实现，以及 TimeStretcherRealtimeTest.cpp；仍需终核，未提交、未推送。

## 缓冲、FFmpeg 与预编译工具终核记录（2026-09-09）

本批新增 23 个有效文件的注释终核，累计 **72/94**，剩余 **22**；每个路径及当前 SHA-256 已登记验收索引。核对内容包括：

- AudioBuffer 两平台的活动长度/容量/固定跨度、移动所有权、清零范围及 SIMD 入口限制。已记录的拆分步长/写宽冲突、尺寸溢出与异常边界仍存在，不能将 Linux 构建成功解释为未调用 SIMD 入口已安全。
- FFmpeg 工厂/实例的元信息回退、所有权清理、目标帧定位、坏包补偿、重采样及 EOF 余量。发现公开头的丢尾说明落后于 a3d5ab6，实现已保留排尾余量，现修正并说明零帧定位重建上下文。初始化失败裸资源回滚、多声道平面容量、非精确寻道与错误短读仍需要专项行为审查。
- 十个 Find 模块与 PrebuiltLayout 的目标名、配置映射、缺包失败、系统链接闭包、运行时登记和符号查找。说明与实现一致：只验证头目录，配置目录存在但缺库时不继续候选目录，DLL 与导入库独立选目录，同名多配置 DLL 可覆盖，PDB 缺失并非强制失败。未把这些限制写成打包规范已满足。
- 三个交叉包装脚本及 MSVC 环境 helper 的参数边界、宏探测分支、工具覆盖、归档成员和失败清理；架构探测及源码入口的目标平台分支和依赖顺序。未实际执行第三方源码构建。

本批唯一新增源码修改：`include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp`，**18 注释行、23 代码行、43.90%**；其余 22 文件只读终核。头文件非注释 token 与 HEAD 一致；clang-format、CMake `--check`、Bash `-n` 通过。主项目 `cmake --build build --parallel 12` 成功，当前 ICE 独立源码/示例/测试重建成功，CTest 1/1 通过（4.47 秒）。实时测试没有覆盖真实媒体排尾、任意寻道或多声道格式，亦未验证 Windows/MSVC 多配置部署，不作相应专项通过声明。

修改后全项目 cloc 2.08 逐文件复扫沿用既定范围：1017 有效、5 空；ICE 94/94 达到 30%，主仓 685/923 不足。三份 `.h.in` 单独按 C/C++ Header 计数；排除第三方、预编译、生成目录、代理头、纯资源及翻译表，保留运行时 Lua/着色器、测试和自维护构建工具。完整明细与其他不足清单见 `build/test_output/comment-audit/files.csv`、`under-30.csv`；范围与排除证据为 files.txt、excluded.json。尚余源码构建脚本、变速链、播放器/文件输出与示例/实时测试终核，未提交、未推送。

## 流式与资源生命周期、效果器终核记录（2026-09-09）

本批通读并对照当前调用链完成 27 个文件的注释终核，累计 49/94，剩余 45 个有效文件。流式创建、页缓存锁与请求环、短读长度更新、缓存/流式策略隔离、封面复制移动、线程池排空及分配计数均已按实际代码核对。节点基类的准备/静音分支、Clipper 空实现、PitchAlter 惰性分配及旁路累加、Compressor 包络与参数脏标记、Biquad 递推与响应、均衡器单读取者 hazard 发布回收均已核对；没有把旧行为差异当成已修复。

本批修正的说明：流式缓存竞争时不入队；短读统一缩短报告长度，无法区分 EOF 与错误；估算偏小不保证自动探索完整文件；声道指针验证可留下已清零前缀；工厂异常不统一转换为空；AudioTrack 流式长度为估算且 origin 不提供视图；缓存写锁内可同步首读或等待旧流式线程回收。补充 link.exe 转发脚本的环境入口及参数/退出状态契约。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `cmake/cross/link.exe` | 2 | 3 | 40.00% |
| `include/ice/manage/AudioPool.hpp` | 70 | 153 | 31.39% |
| `include/ice/manage/AudioTrack.hpp` | 47 | 61 | 43.52% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 24 | 26 | 48.00% |
| `src/ice/manage/AudioTrack.cpp` | 24 | 46 | 34.29% |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 102 | 213 | 32.38% |

其余 21 个文件只读终核，具体路径、统计和当前摘要见验收索引。6 个修改文件 C++ token/脚本命令比较与 HEAD 一致，未改行为；C++ clang-format、全部新增终核 C++ 的只读格式检查、Bash 语法和两层 diff 检查通过。

验证：Linux Clang 22、Debug、SOURCES_BUILD=OFF、ICE_LINKAGE=static 独立配置成功；当前 ICE 源码、示例与实时测试重建成功，CTest 1/1 通过（4.47 秒，启用 C 堆包装）。独立目录为 `build/test_output/ice-comment-audit-build`。主项目 `cmake --build build --parallel 12` 再次成功。相关测试不覆盖流式真实媒体、线程池失败/停机或压缩/变调的音频结果，未报告这些专项运行通过。

全项目 cloc 2.08 逐文件复扫：1017 有效、5 空；ICE 94/94 达到 30%，主仓 685/923 不足。包含自维护源码/头、测试、构建/工具脚本、运行时 Lua/着色器和三份头文件模板；排除第三方、预编译、生成目录、代理头和纯资源/翻译表。当前明细及其他不足清单为 `build/test_output/comment-audit/files.csv`、`under-30.csv`，范围与排除原因见 files.txt、excluded.json。审计文档不计入注释率。下一步继续剩余构建脚本、缓冲、FFmpeg、播放器与实时变速链的终核，未提交、未推送。

## 配置与基础接口恢复终核记录（2026-09-09）

用户已明确恢复 ICE 审计。本批恢复既有注释与契约审计范围，只修改任务单和验收索引，未修改业务实现、构建脚本或第三方源码。

- 当前 ICE HEAD 为 `3a4a3eb`；相对上次审计提交 `658224d`，流式解码及 MSVC 构建工作改变了 10 个文件。旧记录中的“流式占位”不再适用于当前实现，相关文件已在索引标记待重新复核。
- 已登记的 13 个文件 SHA-256 全部匹配。新增终核配置三文件、解码基础接口三文件、接收端与通知三文件，累计 **22/94**，剩余 **72** 个有效文件待终核；两份空文件仍不适用。
- 配置契约已核对 AudioTrack 创建、SourceNode 格式检查与时间换算：全局配置必须在运行期间稳定。解码接口已核对 FFmpeg 工厂、实例包装与缓存消费者：创建存在历史异常通道，格式查询返回目标 PCM 格式，长度仅为估算，失败探测不保证输出回滚。
- 接收端契约已核对 SDL/OpenAL 生命周期、文件接收端和 SourceNode 通知位置：基类不保存格式，设置来源需要停止拉取，返回共享指针引用不保活；循环通知使用归零后的下一次读取游标，不证明设备尾音播放完成。

| ICE 相对路径（本批只读终核） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/config/config.hpp` | 18 | 27 | 40.00% |
| `src/ice/config/config.cpp` | 5 | 10 | 33.33% |
| `include/ice/manage/AudioFormat.hpp` | 9 | 12 | 42.86% |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 15 | 22 | 40.54% |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 19 | 18 | 51.35% |
| `include/ice/manage/dec/MediaInfo.hpp` | 12 | 15 | 44.44% |
| `include/ice/out/IReceiver.hpp` | 28 | 30 | 48.28% |
| `src/ice/out/IReceiver.cpp` | 3 | 5 | 37.50% |
| `include/ice/core/PlayCallBack.hpp` | 18 | 17 | 51.43% |

范围校正：使用 cloc 2.08 的 `--by-file --skip-uniqueness` 复扫 ICE 自维护代码和构建/工具脚本，排除内部第三方、预编译、生成目录及纯资源。加入新增 `cmake/ICEMsvcExternalEnvironment.cmake`，补入旧索引漏列的 `cmake/cross/link.exe`（Bash 链接器转发脚本）。现有 **94 个有效文件、2 个空文件，93 个达到 30%**。唯一不足文件为 `cmake/cross/link.exe`：0 注释行、3 代码行、0%。未为该发现扩大源码修改范围。当前逐文件完整数据为 `build/test_output/comment-audit/ice-files.json`、`ice-files.csv`，持久化数值表见验收索引。

验证：九文件 `clang-format --dry-run --Werror` 通过；本机 Linux、Clang 22 的 `cmake --build build --parallel 12` 成功完成 110 个构建步骤。当前为 `SOURCES_BUILD=OFF`，使用预编译 ICE；本次未独立重建 ICE 源码，不能替代该验证门槛。`ctest --test-dir build -N` 返回 **0 项测试**，不报告测试通过。历史 Windows libc++ 构建失败保留为原环境记录，不代表本次 Linux 构建仍失败。

旧 build 审计产物在本机不存在，已重建本批 ICE 数据；本批未修改自维护代码，未重新全项目扫描，旧主仓 685 个不足及其清单仅是历史记录，不能作为当前全项目统计。两份修改文件为 Markdown 审计文档，不参与代码注释率统计。全量审计仍未完成；下一批优先复核已变化的流式解码、音轨/缓存池及新增构建脚本。未提交、未推送。

## 提交记录（2026-09-08）

按用户“提交一下”的要求，ICE 子仓已提交为 `658224d`（`docs(ice): 补充音频引擎注释与构建契约说明`），包含 74 个文件的注释及格式变更；主仓随本记录提交子仓引用、任务单与验收索引。提交前重新通过 C++ 去注释一致性、构建脚本词法一致性及 diff 检查，全量复扫仍为 ICE 92/92 数值达标。未推送；本次只提交已保存工作，不恢复审计，也不将提交视为全部验收通过。以下暂停交接记录中的“未提交”描述的是暂停当时状态。

## 本次暂停交接（2026-09-08）

用户要求“记录下进度，这次到此为止”，已停止继续审计，等待用户明确恢复；不把暂停标记为目标完成或环境阻塞。

- ICE 92 个有效自维护文件全部达到逐文件 30%，另 2 个空文件不适用。主仓仍有 685 个不足文件，未扩大修改范围。
- [验收索引](ice-comment-audit-index.md) 已登记全部 94 个文件。注释终核与 SHA-256 快照累计完成 13 个有效文件，剩余 79 个仍需依据当前内容确认；历史复核记录不能直接当作最终通过。
- 本轮新增确认九文件：顶层、src、依赖入口三个 CMakeLists；configure-project、macOS 交叉工具链、Buildfmt 三脚本；execptions 目录下三个错误类型头。均只读复核，cmake-format --check 与 clang-format --dry-run --Werror 通过，未新增源码改动。
- 最近一次独立 ICE Clang64 构建成功，CTest 1/1 通过（1.45 秒）。主项目本轮重新执行 `cmake --build build --parallel 12`，进程已以退出码 1 结束，仍因本机 libc++ 缺少原子 shared_ptr 特化失败，不是修复提交被回退。未修改工具链或主项目逻辑以绕过该门槛。
- 工作区改动均保留，未提交、未推送。无本轮构建进程仍在运行。统计及不足清单位于 `build/test_output/comment-audit/`，清理 build 会删除这些产物；关键统计与进度已保存至任务单及验收索引。
- 恢复后从验收索引中未登记终核的 79 个有效文件继续；先校验已登记摘要，只审查变化内容。全部质量证据、最终验证门槛尚未满足，不能宣布所有审计通过。

## 基线与口径

2026-09-07，主仓基线 `f2fc908b`；包含当前工作区与 ICE 已跟踪的自维护源码，未修改业务实现。使用 cloc 2.08，开启 `--by-file --skip-uniqueness`，相同内容的不同文件仍分别计数。

- 有效文件 **1009** 个，**758** 个注释率低于 30%，**251** 个仅在数值上达到要求，尚不能认定注释质量全部合格。
- 主项目 **918** 个文件中 **682** 个不足；ICE **91** 个文件中 **76** 个不足。
- `.h/.hpp`：412 个中 176 个不足；`.cpp/.cc/.cxx`：431 个中 425 个不足。此分类不含模板 `.in`、Objective-C++ 等其他后缀。
- 全部有效文件共有注释 31582 行、代码 219580 行；整体比例不是验收指标，不能抵扣单文件不足。
- 5 个空源文件单独标记不适用。包含构建脚本、测试代码、运行时 Lua/着色器、可执行的插件示例及自维护 `.in` 模板；排除外部第三方、预编译包、生成目录、代理头、纯翻译表、外来谱面测试资源。
- ICE 自维护的 `3rdpty/cmake` 和 `3rdpty/sources/cmake` 计入；其内部第三方库源码不读取、不修改。

完整逐文件明细：`build/test_output/comment-audit/files.csv` 与 `files.json`；范围清单：`files.txt`；排除原因：`excluded.json`；模块汇总：`summary.json`。`baseline-files.csv`、`baseline-files.json` 和 `baseline-summary.json` 保留首次补注释前的统计；无前缀文件为本批完成后重扫结果。这些是构建目录中的本次审查产物，清理构建目录会移除它们。

**证据边界**：逐文件行数进行了全量统计；函数体语义和注释质量进行了风险导向抽查，并非已经逐个审阅所有函数。不能把数值达标等同于函数内注释完整。

## 模块分布

| 范围 | 文件数 | 数值不足文件 |
| --- | ---: | ---: |
| Game/UI | 249 | 181 |
| Game/Logic | 146 | 97 |
| ICE | 91 | 76 |
| Game/Graphic | 67 | 41 |
| Game/Canvas | 57 | 41 |
| MMM 谱面模型/格式 | 48 | 42 |
| Network | 46 | 32 |
| Audio | 42 | 29 |
| Config | 52 | 35 |
| Event | 42 | 36 |
| 主仓自维护 3rdpty 构建脚本 | 56 | 52 |
| scripts | 30 | 30 |
| cmake | 17 | 15 |

其他范围见完整明细，不能因为未出现在此摘要表就免检。

## 抽查发现：需要补哪里、重写哪里

1. **头文件契约不能代替实现注释**：ICE `include/ice/core/MixBus.hpp` 为 46.97%，但 `src/ice/core/MixBus.cpp` 为 0.38%。`process` 中的分块、失败静音和快照释放路径，以及 `acquireSourceSnapshot` / `reclaimRetiredSourcesLocked` 的 hazard 保护与回收时序，几乎没有实现侧说明。需要解释不变量与时序，不能复制头文件描述填满开头。
2. **实时处理的状态协议缺说明**：ICE `RStretcher.hpp` 为 56.69%，对应 `.cpp` 为 0.94%。应在 `process`、`prewarm`、起始 padding、延迟丢弃和 final drain 附近解释帧数、结束状态和预分配约束。
3. **高比例也可能是假象**：ICE `src/sample.cpp` 为 88.33%，主体却是注释掉的旧 FFmpeg 示例代码。应先确认样例是否还使用，再清理/替换成有效示例说明；不能把这些废弃代码当合格文档。此任务不授权顺带删除文件或修改运行行为。
4. **已达标实现仍有注释重构空间**：`VKRenderPass.cpp` 为 31.18%。同步依赖段解释了约束，但不少附件设置注释仅重复 setter 名称；应保留同步原因，将浅层复述改成 load/clear、初末布局及调用方契约说明。
5. **协议与格式转换**：`BeatmapDocumentCodec.cpp` 为 3.71%，`SaveMMMMap.hpp` 为 4.76%。已有少量格式/字段标题，需要补外层封装、长度边界、压缩回退、格式版本、路径及事件映射的语义，避免逐字段重复赋值内容。
6. **刚完成的演练模块也未达标**：`WelcomeView.cpp` 0.96%、`WalkthroughModel.cpp` 1.73%、`WalkthroughService.cpp` 2.07%、`WalkthroughPage.cpp` 0.46%。重点为声明校验和依赖环、临时文件/备份回滚、事件信号与真实完成条件、停靠/焦点恢复、标签状态及 ImGui 栈配对。不能因为功能刚通过测试而豁免。
7. **大型实现与测试**：`ToolbarView.cpp` 1.61%、`VKContextImguiImpl.cpp` 2.90%、`CanvasCameraTest.cpp` 3.20%、`Basic2DCanvas_Interaction.cpp` 5.88%。先按函数/场景梳理职责，再在算法、状态切换和断言附近补理由。测试需要说明构造场景为何能覆盖回归和断言代表什么约束，而不是给每个断言机械加一句“检查结果”。

## 执行规则

- 一批以一个职责为边界，通常处理 1 至 3 个文件；头文件和实现文件独立验收，不能互相抵扣。
- 先通读实现和调用者，再写函数头契约与函数体关键说明；不改业务行为、原子序、异常策略、文件格式或第三方代码。
- 每个文件检查 `comment / (comment + code) >= 30%`，同时人工检查函数体注释位置及内容；不通过行数目标自动生成填充注释。
- 明细中的 `minimum_additional_comment_lines` 仅是保持代码行数不变时的数学差额，不是要求填充的注释配额。全量差额约 69439 行，说明这是长期治理任务，不能一次堆注释解决。
- 修改 C++ 后执行 clang-format 和限 60% CPU 的构建、相关测试；若格式化造成无关改动，应收敛差异。每批记录前后比例与验证结果。
- 主仓与 ICE 的改动按仓库和职责分开；只有用户要求时才提交，不自动推送。

## 待办批次

- [x] A0：建立主仓与 ICE 的逐文件基线，区分数值不足和质量问题，完成风险抽查。
- [x] A1：ICE `MixBus.cpp` 实现注释，复核 `MixBus.hpp` 的线程/生命周期约束；首批完成，验证边界见下文。
- [x] A2：ICE `RStretcher.cpp` 与对应头文件，解释预热、padding、delay、final 输入和尾部取出。
- [x] A3：ICE `TimeStretcher.cpp` 与头文件，解释重建、参数生效和实时路径边界；验证见 A3 完成记录。
- [x] A4：ICE `SourceNode.cpp`、`ALPlayer.cpp` 按各自职责分批处理，补来源切换、队列与播放状态；A4.2 的主项目构建限制见完成记录。
- [x] A5：ICE `FFmpegFileReceiver.cpp`、`FFmpegDecoderInstance.cpp` 注释及配套头契约复核完成，已说明状态、重采样、EOF 与回收；行为缺陷与测试边界仍见记录。
- [x] A6：ICE `TimeStretcherRealtimeTest.cpp` 注释与观测口径复核完成；`sample.cpp` 已独立复核并清理历史注释代码。运行覆盖限制见实时测试收尾记录，不代表全部行为规范通过。
- [ ] B1：演练 `WalkthroughModel.h/.cpp`，补章节顺序、占位主题、步骤 DAG 校验和进度归约。
- [ ] B2：演练 `WalkthroughService.h/.cpp`，补线程队列、只读恢复保护和写入失败回滚。
- [ ] B3：`WelcomeView.h/.cpp`、`WalkthroughPage.h/.cpp` 分两批，补页面生命周期与布局约束。
- [ ] B4：`WalkthroughTest.cpp`、`WelcomeViewTest.cpp`，补场景/预期/回归含义。
- [ ] C1：`EditorEngine.cpp`、`ProjectController.cpp` 及对应头文件逐组处理，补项目切换、会话恢复和副作用顺序。
- [ ] C2：`ActionController_Editing.cpp`、`BeatmapSession_Commands.cpp` 分批，补编辑指令、不变量和撤销语义。
- [ ] C3：画布交互、抓取工具、时间线弹窗分批，补坐标转换、吸附、焦点和拖放生命周期。
- [ ] C4：渲染系统和 `VKContextImguiImpl.cpp` 分批；复核已达标的 `VKRenderPass.cpp`，补同步与资源生命周期说明。
- [ ] D1：Network 协作房间和文档编解码分批，补协议边界、顺序、冲突处理和断线恢复。
- [ ] D2：MMM 的 Load/Save 头文件按格式分批，补版本兼容、时间单位、路径和有损映射。
- [ ] D3：Audio、Config、Common、Event、Main、Runtime、Updater、Log 等剩余代码，按完整明细逐文件推进。
- [ ] E1：工具栏、设置、元数据、音频管理等 UI 实现按职责拆批，补状态联动和布局约束。
- [ ] E2：剩余测试按被测模块归批；优先大型回归测试，说明边界条件和断言依据。
- [ ] F1：主仓 CMake、工具脚本、预编译布局脚本分批；ICE 构建脚本单独归批。
- [ ] F2：运行时 Lua、着色器和插件示例，补输入输出、坐标/颜色约定、皮肤覆盖行为。
- [ ] G1：复查所有数值达标文件的注释质量，清理无意义堆砌/过时说明，再执行最终全量逐文件验收。

## 首批完成记录：A1

仅修改 ICE `src/ice/core/MixBus.cpp`，补充控制线程/音频线程边界、分块混音、失败静音、快照保护与回收、声道路由及显示电平语义。头文件已复核，本批未修改。

| 文件 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| ICE `src/ice/core/MixBus.cpp` | 1 → 115 | 262 → 262 | 0.38% → 30.50% |
| ICE `include/ice/core/MixBus.hpp`（只读复核） | 93 → 93 | 105 → 105 | 46.97% → 46.97% |

验证：clang-format、主项目 `cmake --build build --parallel 12`、ICE 此翻译单元的 `clang++ -std=c++23 -fsyntax-only` 均通过；差异检查确认全部 114 行变更均为新增注释，无删除行或业务代码变更。

当前 `SOURCES_BUILD=OFF`，`MixBusRealtimeSafetyTest` 只在源码构建时注册，因此本次没有运行该测试，也没有重建 ICE 预编译库。主项目构建不能当成 ICE 运行时验证；本批为注释变更，已单独验证 ICE 源码编译。

首批重扫后仍有 **757/1009** 个文件不足，其中 ICE 为 **75/91**。这是 A1 完成时的历史记录，最新结果见下文。
## ICE 后续批次记录（2026-09-07）

本轮只推进 ICE 注释治理，不改变业务代码、同步操作、异常策略或预编译二进制。以下统计包含前一批 MixBus 的工作区修改。

- 当前 ICE 有效文件 91 个，**55 个数值达标，36 个仍不足**；相较首轮基线 76 个不足，累计减少 40 个，其中本轮减少 39 个。
- 工作区修改的 **44 个 ICE 文件均逐文件达到 30%**。新增说明分布在接口契约、函数体边界、状态转换、缓冲容量与生命周期处；没有用头文件抵扣实现文件。
- `sample.cpp` 移除了不可执行的注释代码，改为说明占位入口与正式实现归属；该文件没有加入构建目标，也没有补造运行行为。
- 尚未完成 ICE 全量验收；原本数值达标但未完整检查的文件仍保留质量复核任务。

### 已完成的职责批次

- [x] A2：RubberBand 包装的预热、起始填充、延迟丢弃、final 输入和尾部取出；修正倍率并发访问约束。
- [x] A4.1：SourceNode 块内起播、游标推进、循环/结束回调、参考时钟 hazard 获取与控制侧回收。
- [x] A5.1：FFmpeg 探测工厂及实例公开接口；解释时长估算、元信息复制、封面所有权及失败语义。实例实现仍待处理。
- [x] A6.1：清理 sample.cpp 的废弃注释代码，明确它不属于可运行验证。
- [x] A7：AudioTrack、Cachy/StreamingDecoder 及基础接口；明确首次等待、占位策略和追加借用视图语义。
- [x] A8：IEffectNode、Compressor、PitchAlter、Clipper 的已有实现和限制；未把历史分配或空实现描述为已实时化。
- [x] A9：AudioBuffer 头文件及两种平台存储实现；解释逻辑帧数、容量、固定存储跨度、移动及 SIMD 前置条件。
- [x] A10：配置、错误类型、输出接收基类、分配统计工具及线程池空翻译单元。
- [x] F1.1：ICE 顶层、src、3rdpty 三个构建入口；其余 Find、布局、源码依赖和跨编译脚本仍待审查。

### 逐文件结果

下表路径均相对于 ICE 根目录；代码行数因格式化可能变化，验证还检查了去注释、空白后的文本一致性。

| 文件 | 注释行：基线 → 当前 | 代码行：基线 → 当前 | 注释率：基线 → 当前 |
| --- | ---: | ---: | ---: |
| `3rdpty/CMakeLists.txt` | 3 → 8 | 17 → 17 | 15.00% → 32.00% |
| `CMakeLists.txt` | 11 → 30 | 70 → 70 | 13.58% → 30.00% |
| `include/ice/config/config.hpp` | 5 → 12 | 27 → 27 | 15.63% → 30.77% |
| `include/ice/core/effect/Clipper.hpp` | 3 → 7 | 15 → 15 | 16.67% → 31.82% |
| `include/ice/core/effect/Compresser.hpp` | 13 → 23 | 53 → 53 | 19.70% → 30.26% |
| `include/ice/core/effect/IEffectNode.hpp` | 31 → 32 | 40 → 40 | 43.66% → 44.44% |
| `include/ice/core/effect/PitchAlter.hpp` | 14 → 21 | 33 → 33 | 29.79% → 38.89% |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | 89 → 90 | 68 → 68 | 56.69% → 56.96% |
| `include/ice/core/IAudioNode.hpp` | 4 → 6 | 14 → 14 | 22.22% → 30.00% |
| `include/ice/core/PlayCallBack.hpp` | 5 → 8 | 17 → 17 | 22.73% → 32.00% |
| `include/ice/execptions/buffer_error.hpp` | 0 → 5 | 11 → 11 | 0.00% → 31.25% |
| `include/ice/execptions/instance_build_error.hpp` | 0 → 5 | 11 → 11 | 0.00% → 31.25% |
| `include/ice/execptions/load_error.hpp` | 0 → 5 | 11 → 11 | 0.00% → 31.25% |
| `include/ice/manage/AudioBuffer.hpp` | 84 → 122 | 283 → 283 | 22.89% → 30.12% |
| `include/ice/manage/AudioFormat.hpp` | 0 → 6 | 13 → 12 | 0.00% → 33.33% |
| `include/ice/manage/AudioTrack.hpp` | 14 → 23 | 53 → 52 | 20.90% → 30.67% |
| `include/ice/manage/dec/AlbumArt.hpp` | 16 → 24 | 54 → 52 | 22.86% → 31.58% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 10 → 18 | 42 → 41 | 19.23% → 30.51% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 2 → 8 | 18 → 18 | 10.00% → 30.77% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 4 → 10 | 23 → 23 | 14.81% → 30.30% |
| `include/ice/manage/dec/IDecoder.hpp` | 5 → 12 | 21 → 21 | 19.23% → 36.36% |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 4 → 10 | 22 → 22 | 15.38% → 31.25% |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 5 → 8 | 18 → 18 | 21.74% → 30.77% |
| `include/ice/manage/dec/MediaInfo.hpp` | 2 → 7 | 16 → 15 | 11.11% → 31.82% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 5 → 12 | 27 → 27 | 15.63% → 30.77% |
| `include/ice/out/IReceiver.hpp` | 19 → 21 | 30 → 30 | 38.78% → 41.18% |
| `include/ice/tool/AllocationTracker.hpp` | 4 → 18 | 40 → 40 | 9.09% → 31.03% |
| `src/CMakeLists.txt` | 6 → 22 | 50 → 50 | 10.71% → 30.56% |
| `src/ice/config/config.cpp` | 0 → 5 | 10 → 10 | 0.00% → 33.33% |
| `src/ice/core/effect/Clipper.cpp` | 2 → 3 | 6 → 6 | 25.00% → 33.33% |
| `src/ice/core/effect/Compresser.cpp` | 26 → 29 | 70 → 54 | 27.08% → 34.94% |
| `src/ice/core/effect/IEffectNode.cpp` | 0 → 18 | 42 → 42 | 0.00% → 30.00% |
| `src/ice/core/effect/PitchAlter.cpp` | 6 → 12 | 33 → 28 | 15.38% → 30.00% |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | 3 → 135 | 315 → 315 | 0.94% → 30.00% |
| `src/ice/core/MixBus.cpp` | 1 → 115 | 262 → 262 | 0.38% → 30.50% |
| `src/ice/core/SourceNode.cpp` | 8 → 124 | 288 → 288 | 2.70% → 30.10% |
| `src/ice/manage/AudioBuffer.cpp` | 12 → 41 | 94 → 94 | 11.32% → 30.37% |
| `src/ice/manage/AudioTrack.cpp` | 2 → 20 | 46 → 41 | 4.17% → 32.79% |
| `src/ice/manage/dec/CachyDecoder.cpp` | 32 → 54 | 126 → 126 | 20.25% → 30.00% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 31 → 67 | 156 → 156 | 16.58% → 30.04% |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 4 → 12 | 28 → 28 | 12.50% → 30.00% |
| `src/ice/out/IReceiver.cpp` | 0 → 3 | 5 → 5 | 0.00% → 37.50% |
| `src/ice/thread/ThreadPool.cpp` | 0 → 2 | 4 → 4 | 0.00% → 33.33% |
| `src/sample.cpp` | 106 → 6 | 14 → 13 | 88.33% → 31.58% |

### 本轮验证与边界

- 修改的 C++/头文件执行 clang-format，三个 CMake 文件执行 cmake-format；两层仓库 `git diff --check` 通过。
- 44 个修改文件与 ICE HEAD 比较，去除注释和空白后的文本全部一致；这项检查覆盖 C++ 与 CMake，不替代运行测试。
- 在 `build/test_output/ice-comment-audit-build` 独立配置 ICE：`SOURCES_BUILD=OFF`、`ICE_BUILD_TESTS=ON`、`ICE_LINKAGE=static`、`RelWithDebInfo`。此模式编译当前 ICE 自有源码并链接已有依赖包，没有重建任何外部第三方源码。
- ICE 自身源码、示例程序、实时安全测试编译链接通过；`IonTimeStretcherRealtimeTest` 通过（1/1）。
- 主项目 `cmake --build build --parallel 12` 通过；该构建仍使用 ICE 预编译包，不能替代上面的 ICE 独立源码验证。
- AllocationTracker.hpp 另做单独语法检查通过；直接以主输入编译头文件产生 pragma-once 提示，不是引擎构建失败。
- 未验证 Windows/macOS 构建、实体音频设备或每个遗留效果器的实时行为；未同步或重建 ICE 发布二进制。
- 未提交、未推送。

### 发现但未擅自修改的行为问题

以下是注释审查发现的独立修复候选，并非已修复或通过运行回归验证的行为：

- StreamingDecoder 与 Clipper 仍是占位实现；非空对象不能证明可播放或已限幅。
- Compressor 的参数 setter 写入非原子脏标记，系数更新分支可能扩容；PitchAlter 仍有容量调整、共享指针复制及首次后端创建。
- FFmpegDecoderFactory 的历史 AVCALL_CHECK 宏将比较结果赋给 ret，不能保留原始负错误码；其 probe 主路径不使用该宏。
- AudioBuffer 的历史 SIMD 立体声拆分入口存在四帧步长与八浮点对齐写入冲突，旧的“已完成解交错”注释已收敛，需单独验证并修复。
- 既有异常类、原始分配及诊断输出只是保留原行为，新增注释不意味着授权新增这些用法。

### ICE 下一轮逐文件任务

下列清单保留上一批的 36 个待处理文件及其后续进度；A3 完成后仍有 35 个未达到数值门槛。继续处理输出后端和解码实现，再处理测试与构建脚本。除此之外，ThreadPool 等原有数值达标文件仍须人工质量复核。

- [ ] `tests/TimeStretcherRealtimeTest.cpp`：10.51%。
- [x] `src/ice/out/play/openal/ALPlayer.cpp`：6.79% → 30.22%，A4.2 完成注释审查，验证限制见下文。
- [x] `src/ice/core/effect/TimeStretcher.cpp`：5.88% → 30.04%，A3 完成。
- [x] `src/ice/out/io/FFmpegFileReceiver.cpp`：6.55% → 30.27%，实现注释与配套头契约一致性已复核。
- [x] `3rdpty/cmake/modules/PrebuiltLayout.cmake`：7.27% → 30.13%，公共接口与实现分支已复核，布局探针通过。
- [x] `3rdpty/sources/cmake/Buildrubberband.cmake`：7.07% → 30.06%，Meson 参数、元数据、平台分支与缓存说明已复核。
- [x] `3rdpty/sources/cmake/Buildffmpeg.cmake`：7.25% → 30.23%，参数构造、功能清单、缓存与安装边界已复核。
- [x] `src/ice/core/effect/GraphicEqualizer.cpp`：1.05% → 30.26%，实现和头文件已复核。
- [x] `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：20.89% → 30.24%，A5.2 已复核。
- [x] `3rdpty/sources/cmake/Buildsdl.cmake`：7.05% → 30.29%，平台裁剪、缓存及包装目标已复核。
- [x] `3rdpty/sources/cmake/Buildlame.cmake`：6.77% → 30.34%，交叉参数、归档补齐与缓存边界已复核。
- [x] `src/main.cpp`：13.48% → 30.32%，实际图连接、手工演示边界及线程生命周期说明已复核。
- [x] `3rdpty/sources/cmake/Buildzlib.cmake`：2.88% → 30.34%，已复核注释与配置文本。
- [x] `3rdpty/sources/cmake/Buildlibsamplerate.cmake`：3.81% → 30.41%，已复核注释与配置文本。
- [x] `3rdpty/cmake/modules/Findffmpeg.cmake`：1.16% → 30.51%，已复核。
- [x] `src/ice/out/play/sdl/SDLPlayer.cpp`：18.50% → 30.64%，已复核。
- [x] `3rdpty/sources/cmake/Buildfftw.cmake`：10.09% → 31.94%，外部构建、CRT、缓存与产物接口已复核。
- [x] `3rdpty/cmake/modules/FindSDL3Static.cmake`：3.39% → 30.49%，已复核。
- [x] `3rdpty/sources/cmake/Buildopenal.cmake`：13.98% → 30.43%，平台裁剪、链接与消费宏已复核。
- [x] `3rdpty/cmake/modules/Findrubberband.cmake`：1.89% → 30.67%，已复核。
- [x] `3rdpty/cmake/modules/FindOpenAL.cmake`：5.26% → 30.26%，已复核。
- [x] `3rdpty/cmake/modules/Findspdlog.cmake`：4.44% → 30.65%，已复核。
- [x] `cmake/cross/clang-cl-gcc-compatible.sh`：4.44% → 30.65%，已复核并检查 shell 语法。
- [x] `include/ice/manage/AudioPool.hpp`：24.06% → 30.05%，已复核。
- [x] `3rdpty/cmake/modules/Findzlib.cmake`：2.56% → 30.91%，已复核。
- [x] `cmake/cross/merge-msvc-archives.sh`：4.88% → 30.36%，已复核并检查 shell 语法。
- [x] `3rdpty/cmake/modules/Findfmt.cmake`：2.63% → 30.19%，已复核。
- [x] `3rdpty/cmake/modules/Findlibsamplerate.cmake`：2.63% → 30.19%，已复核。
- [x] `3rdpty/cmake/modules/Findfftw.cmake`：2.78% → 30.00%，已复核。
- [x] `src/ice/core/effect/filter/BiquadFilter.cpp`：14.04% → 30.00%，实现和头文件已复核。
- [x] `3rdpty/cmake/modules/Findlame.cmake`：3.13% → 31.11%，已复核。
- [x] `src/ice/manage/AudioPool.cpp`：9.09% → 32.00%，已复核。
- [x] `include/ice/out/play/sdl/SDLPlayer.hpp`：24.64% → 43.96%，已复核。
- [x] `cmake/cross/llvm-lib-ar-compatible.sh`：6.67% → 33.33%，已复核并检查 shell 语法。
- [x] `3rdpty/sources/cmake/Buildspdlog.cmake`：22.22% → 30.00%，已复核注释与配置文本。
- [x] `3rdpty/sources/cmake/arch-probe.cmake`：27.59% → 32.26%，已复核注释与配置文本。

该批复扫：主仓加 ICE 仍有 **718/1009** 个文件不足；主仓仍为 682/918，未在该批混入业务模块注释改动。

## A3 完成记录：TimeStretcher（2026-09-07）

本批只处理变速节点的头文件与实现，保留此前工作区修改，不改算法、参数、原子序或控制流程。

- 补充控制侧预热发布、音频侧激活、退役链回收的所有权与线程边界。
- 在函数体内说明暂停冻结请求、输入小数余量、连续段切分、定位重置、显式边界旧段排尾音、跨块输入计划保留，以及 final 提交与预算交付的区别。
- 明确 provider 配置仅做有限次读取，不阻塞等待配置稳定；配置序号不保护外部 context 生命周期，控制写入者必须串行。
- 修正公开查询语义：实际倍率是诊断值，激活状态时可先发布设定值；final drained 表示应用层预算已交付，不保证后端物理队列为空；拒绝计数也包含格式不匹配。

| ICE 相对路径 | 注释行：前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/ice/core/effect/TimeStretcher.cpp` | 46 → 316 | 736 → 736 | 5.88% → 30.04% |
| `include/ice/core/effect/TimeStretcher.hpp` | 155 → 172 | 120 → 120 | 56.36% → 58.90% |

验证：clang-format、两层仓库 diff 检查、去除注释与空白后的代码一致性检查、ICE 独立源码构建、`IonTimeStretcherRealtimeTest`（1/1）及主项目构建均通过。ICE 独立构建沿用 `build/test_output/ice-comment-audit-build`，只编译引擎自有源码并链接现有第三方预编译依赖；未构建外部依赖源码、未更新预编译二进制。

全量复扫仍按逐文件口径，包括主项目与 ICE 自维护头文件、实现、测试及脚本，排除外部第三方和生成/纯数据文件。当前 ICE **56/91** 个文件数值达标，**35** 个仍不足；全项目 **717/1009** 个仍不足。数值已达标的其他文件仍需质量复核，不能据此宣称 ICE 全量审计完成。

下一批优先处理 `ALPlayer.cpp`、`FFmpegFileReceiver.cpp` 和 `FFmpegDecoderInstance.cpp`，随后推进实时测试与构建脚本。未提交、未推送。

## A4.2 完成记录：ALPlayer（2026-09-08）

本批处理 ICE `ALPlayer.cpp/.hpp`，补充默认设备回退、分阶段失败回收、图绑定与句柄生命周期、队列退回与欠载恢复、暂停与空间化切换、平面音频下混及上传格式约束。函数体内说明锁顺序、队列重建丢弃旧块但不回退图游标，以及日志截断和错误查询的边界。英文接口说明改为中文 Doxygen。

| ICE 相对路径 | 注释行：前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/openal/ALPlayer.cpp` | 56 → 333 | 769 → 769 | 6.79% → 30.22% |
| `include/ice/out/play/openal/ALPlayer.hpp` | 58 → 82 | 63 → 63 | 47.93% → 56.55% |

### 验证结果与边界

- 两个文件已执行 clang-format；两层仓库 `git diff --check` 通过；与 ICE HEAD 比较，识别字符串后去除注释和空白的业务文本一致。
- 使用本机实际存在的 `C:\msys64\msys2_shell.cmd -clang64 -shell fish -defterm -no-start -here` 进入 clang64；编译器为 Clang 22.1.8，并行度为 12（20 个逻辑处理器的 60%）。
- ICE 独立源码构建使用 `build/test_output/ice-comment-audit-build`，配置为 `SOURCES_BUILD=OFF`、`ICE_LINKAGE=static`、`ICE_BUILD_TESTS=ON`、`RelWithDebInfo`。引擎自有源码与示例、测试编译链接通过，`IonTimeStretcherRealtimeTest` 通过（1/1）。日志位于 `build/test_output/comment-audit/ice-build.log` 与 `ice-test.log`。
- 主项目 `cmake --build build --parallel 12` **未通过**。`BeatmapSession.h`、`RenderSyncRegistry.h`、`SessionRegistry.h` 中的 `std::atomic<std::shared_ptr<...>>` 在当前 clang64 libc++ 中触发普通 `atomic<T>` 的 trivially-copyable 静态断言，日志见 `build/test_output/comment-audit/main-build.log`。
- 已核对 `ac301275`（`fix(logic): 迁移原子共享指针接口`）属于当前 develop 历史，修复未丢失；该提交将旧式原子自由函数迁移为 `std::atomic<std::shared_ptr<...>>`。本机 libc++ 的 `version` 头中 `__cpp_lib_atomic_shared_ptr` 未启用，当前失败是迁移后接口与本地标准库的兼容问题。本批未修改这些业务文件或编译参数来绕过错误。
- ICE 回归测试不涉及实体 OpenAL 设备，因此不作为设备播放、暂停竞态或多实例上下文安全的验证。没有重建外部第三方源码，也没有更新发布预编译包。

### 当前全量复扫

本机没有上一轮构建目录中的基线产物，因此重新依据两层 Git 跟踪路径生成范围清单，并用 cloc 2.08 `--by-file --skip-uniqueness` 逐文件统计。范围包括主项目与 ICE 的业务头文件、实现、测试、自维护 CMake/工具/CI 脚本、运行时 Lua、着色器和可执行插件示例；排除外部第三方、预编译包、代理头、构建产物、纯配置数据模板、文档、纯翻译表及外来测试数据。`.cmake.in` 按 CMake 计数，头文件模板按 C++ 计数，5 个空文件单列为不适用。

当前有效文件 **1012** 个，**719** 个不足；主仓 **921** 个中 **685** 个不足，ICE **91** 个中 **34** 个不足。该重建清单比历史基线多 3 个有效文件，不把总数差异直接解释为本批业务代码变化；本批 ICE 数值不足减少 1 个，其余达标文件仍保留质量复核任务。

完整明细为 `build/test_output/comment-audit/files.csv`、`files.json`；其他未达标文件逐项列在 `under-30.csv`；范围、排除理由和汇总分别为 `files.txt`、`excluded.json`、`summary.json`。这些产物在构建目录中，清理构建目录会移除它们；本轮未重建或覆盖历史基线快照。

### 注释审查确认的现有约束

- `m_sourceMutex` 不参与基类 `set_source` 写入，图绑定仍必须在停止拉取后替换。
- 供数循环末尾的欠载恢复没有再次检查暂停，可能覆盖同轮控制侧暂停；空间开关与队列重建异步完成，重建会丢弃已预读的旧块。
- 供数线程现有互斥量、1ms 轮询休眠及排队失败时的格式化输出不满足无阻塞实时约束；新增说明没有把这些行为描述为已修复。

下一批继续 A5 的 `FFmpegFileReceiver.cpp` 或 `FFmpegDecoderInstance.cpp`，各按独立职责处理。未提交、未推送。

## ICE 全量审计持续推进记录（2026-09-08）

目标为全部 ICE 自维护文件完成比例与质量审计，当前仍在进行。A4.2 后依次处理四个职责批次：FFmpeg 解码实例、SDL 输出、音轨池、图形均衡器与双二阶滤波。各批次只补充或修正说明并格式化，不改变业务文本。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 16 | 23 | 41.03% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 192 | 443 | 30.24% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 40 | 51 | 43.96% |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 72 | 163 | 30.64% |
| `include/ice/manage/AudioPool.hpp` | 61 | 142 | 30.05% |
| `src/ice/manage/AudioPool.cpp` | 8 | 17 | 32.00% |
| `include/ice/core/effect/filter/BiquadFilter.hpp` | 22 | 26 | 45.83% |
| `src/ice/core/effect/filter/BiquadFilter.cpp` | 21 | 49 | 30.00% |
| `include/ice/core/effect/GraphicEqualizer.hpp` | 75 | 51 | 59.52% |
| `src/ice/core/effect/GraphicEqualizer.cpp` | 82 | 189 | 30.26% |

复扫沿用 A4.2 重建范围：主仓和 ICE 共 1012 个有效文件，5 个空文件单列；712 个数值不足，其中主仓 685/921、ICE **27/91**。ICE 当前数值达标 **64/91**，本轮减少 7 个不足文件。全量明细和剩余清单仍为 `build/test_output/comment-audit/files.csv` 与 `under-30.csv`。本轮的比例及函数体复核只覆盖上表职责，不能把其余原有达标文件视作已全量质量验收。

验证：修改文件全部 clang-format；当前 12 个工作区 C++/头文件（含此前 ALPlayer 两个文件）与 ICE HEAD 去除注释和空白后文本一致。clang64 下 ICE 独立源码构建及 `IonTimeStretcherRealtimeTest`（1/1）通过。主项目仍保留 A4.2 的原子共享指针标准库兼容阻塞，未声称全项目构建通过。

均衡器专项测试 `Modules/Audio/tests/GraphicEqualizerRealtimeTest.cpp` 只在主项目源码依赖模式注册。本轮尝试用 clang64 直接将原测试链接当前 ICE 独立库，但测试自身第 287 行的 `std::aligned_alloc` 在本机 libc++/MinGW 环境不可用，编译未通过，不能报告该专项测试运行通过。未修改测试内存分配策略或切换工具链掩盖这一限制。

实现侧审查新增约束与独立修复候选：

- FFmpeg 实例构造仍有历史异常和未完整回滚的裸句柄；寻道未裁剪到样本精确位置；EOF 排尾未保留超出本次请求的重采样帧；负重采样返回值未单独报告。本次注释明确短读与初始化失败语义，不将它们描述为已修复。
- SDL `stop` 先清 running，再调用要求 running 为真的 `resume`，该调用实际不恢复设备；供数退出依靠主动轮询。暂停设备后供数仍可填到队列阈值，类也没有自动 close 的析构实现。旧的“恢复设备是退出关键”说明已重写。
- AudioPool 的文件状态签名不是内容哈希或原子文件快照；缓存命中不更换加载策略，COREAUDIO 仍无工厂实现。
- 均衡器的控制侧预构造避免音频侧分配，但 hazard 获取在持续发布下可能重试，不保证有界等待；新状态历史清零，没有系数渐变。双二阶滤波状态按声道独占且跨块保留。

剩余重点：`FFmpegFileReceiver.cpp`、`TimeStretcherRealtimeTest.cpp`、`src/main.cpp`、预编译布局及 Find/源码构建/跨编译脚本；随后逐个复核其余数值达标文件的质量。全量目标保持未完成，未提交、未推送。

## ICE 构建脚本审计推进记录（2026-09-08）

本批完成 10 个独立 Find 模块、3 个跨编译 shell 脚本、3 个源码构建脚本、架构探测及源码模式入口的注释复核。补充配置映射、平台库名兼容、静态链接闭包、运行时 DLL 检查、工具参数转换、临时归档生命周期、增量构建缓存及配置头生成约束。未改动命令参数或执行逻辑，未读取或修改外部第三方源码。

### 范围校正与逐文件结果

本次发现 ICE 自维护的 `3rdpty/sources/CMakeLists.txt` 被原第三方路径过滤规则漏计，已补入统计并完成注释审查。有效范围由 1012 增至 **1013**（主仓 921、ICE **92**），另有 5 个空文件不适用；没有将新增计数解释为新增业务代码。其余统计与排除口径沿用 A4.2，仍逐文件使用 cloc 2.08 的 `comment / (comment + code)`。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `3rdpty/cmake/modules/Findffmpeg.cmake` | 36 | 82 | 30.51% |
| `3rdpty/cmake/modules/Findfftw.cmake` | 15 | 35 | 30.00% |
| `3rdpty/cmake/modules/Findfmt.cmake` | 16 | 37 | 30.19% |
| `3rdpty/cmake/modules/Findlame.cmake` | 14 | 31 | 31.11% |
| `3rdpty/cmake/modules/Findlibsamplerate.cmake` | 16 | 37 | 30.19% |
| `3rdpty/cmake/modules/FindOpenAL.cmake` | 23 | 53 | 30.26% |
| `3rdpty/cmake/modules/Findrubberband.cmake` | 23 | 52 | 30.67% |
| `3rdpty/cmake/modules/FindSDL3Static.cmake` | 25 | 57 | 30.49% |
| `3rdpty/cmake/modules/Findspdlog.cmake` | 19 | 43 | 30.65% |
| `3rdpty/cmake/modules/Findzlib.cmake` | 17 | 38 | 30.91% |
| `3rdpty/sources/CMakeLists.txt` | 7 | 12 | 36.84% |
| `3rdpty/sources/cmake/Buildlibsamplerate.cmake` | 45 | 103 | 30.41% |
| `3rdpty/sources/cmake/Buildspdlog.cmake` | 12 | 28 | 30.00% |
| `3rdpty/sources/cmake/Buildzlib.cmake` | 44 | 101 | 30.34% |
| `3rdpty/sources/cmake/arch-probe.cmake` | 10 | 21 | 32.26% |
| `cmake/cross/clang-cl-gcc-compatible.sh` | 19 | 43 | 30.65% |
| `cmake/cross/llvm-lib-ar-compatible.sh` | 7 | 14 | 33.33% |
| `cmake/cross/merge-msvc-archives.sh` | 17 | 39 | 30.36% |

### 验证及尚未满足的门槛

- 所有修改的 CMake 文件已 cmake-format 并通过 `--check`。词法检查现已覆盖 `CMakeLists.txt`，剔除注释与空白后的 CMake token 序列与 ICE HEAD 一致；shell 非注释命令行一致，3 个脚本均通过 clang64 环境下 `bash -n`。
- 已有 12 个 C++/头文件再次通过去注释与空白的文本一致性检查。两层仓库 `git diff --check` 通过。
- 指定 clang64/fish 环境中，ICE 独立构建 `cmake --build C:/MusicMapMaker-Next/build/test_output/ice-comment-audit-build --parallel 12` 通过，`IonTimeStretcherRealtimeTest` 1/1 通过（最终一轮 1.23 秒）。本批 Find 修改触发重新配置成功，之后 Ninja 无待编译项。
- 独立构建保持 `SOURCES_BUILD=OFF`；源码模式脚本和跨编译包装只完成静态审查及语法/词法核验，未通过实际构建外部第三方来验证。shell 脚本没有执行归档覆盖或删除操作。
- 主项目完整构建仍有 A4.2 记录的 libc++ 原子共享指针兼容阻塞，均衡器专项测试仍有 `std::aligned_alloc` 兼容阻塞，均未声称已通过。

### 实现边界记录

- zlib 源码入口固定构建静态库，不随 `ICE_LINKAGE` 切换；源码戳只比较时间，不检测安装库删除、源码删除或保留 mtime 的替换，也不直接识别编译参数变化。多配置使用及路径中的 POSIX 单引号仍需独立审查。
- libsamplerate 探针使用通用 `HAVE_*` 缓存名；舍入探针临时设置 `CMAKE_REQUIRED_LIBRARIES` 后直接 unset，而非恢复父值；版本元数据固定为 0.1.9。注释没有将这些局限描述为已修复。
- 跨编译归档脚本的输出覆盖失败不恢复原归档；调用者必须隔离输入输出，并注意临时目录切换后的相对输入路径。当前合并方式也不能据此保证同一输入归档中的重复成员名完整保留。

当前 ICE 数值达标 **82/92**，不足 **10**；全项目不足 **695/1013**，其中主仓不足 685/921。其余原有达标文件仍需完成质量复核，不得将数字达标等同于全量审计通过。

ICE 剩余不足清单：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 33 | 421 | 7.27% |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | 24 | 307 | 7.25% |
| `3rdpty/sources/cmake/Buildfftw.cmake` | 11 | 98 | 10.09% |
| `3rdpty/sources/cmake/Buildlame.cmake` | 9 | 124 | 6.77% |
| `3rdpty/sources/cmake/Buildopenal.cmake` | 13 | 80 | 13.98% |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | 26 | 342 | 7.07% |
| `3rdpty/sources/cmake/Buildsdl.cmake` | 11 | 145 | 7.05% |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 52 | 742 | 6.55% |
| `src/main.cpp` | 24 | 154 | 13.48% |
| `tests/TimeStretcherRealtimeTest.cpp` | 123 | 1047 | 10.51% |

全项目明细及其他不足文件逐项清单已刷新到 `build/test_output/comment-audit/files.csv` 和 `under-30.csv`；该构建目录中的产物仍会随清理丢失。任务目标保持未完成，未提交、未推送。

## ICE 预编译布局 helper 审计记录（2026-09-08）

本批完整复核 `3rdpty/cmake/modules/PrebuiltLayout.cmake` 的 16 个 helper，补充中文接口契约与实现附近说明。覆盖缓存默认值、目标平台与架构推断、编译器标签候选、配置映射、路径选择与失败返回、可选 PDB、运行时登记及构建后部署。修正原有“当前配置 DLL 清单”和“保证直接运行”等超出实现保证的说明，不改变任何配置命令或参数。

| ICE 相对路径 | 注释行：前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 33 → 182 | 421 → 422 | 7.27% → 30.13% |

代码行增加来自格式化换行，CMake 词法 token 序列与 ICE HEAD 一致。已执行 cmake-format 并通过 `--check`；全体工作区构建脚本词法/命令文本检查以及既有 12 个 C++ 文件去注释文本检查再次通过。

### 已验证的边界与未修复事项

- 标签回退仅按编译器主版本递减，不验证标准库、ABI 或运行库兼容；显式标签恰好等于当前 gccN/clangN 时仍会产生旧版本候选。
- 目录选择先标签后配置，较新标签的 Release 可以优先于较旧标签的 RelWithDebInfo。首个目录存在但内部缺库时，文件查找直接失败，不继续尝试其余目录。
- 库目录与 DLL 目录独立解析，文件存在不能证明两者版本配对；版本化 DLL 回退按字典序首项选择，不是选择语义版本最大值。
- PDB 查找是可选行为，缺文件不报错；仅 MSVC 且请求 Debug/RelWithDebInfo 时返回符号，Release 请求即使使用含符号目录也不进入该分支。现有逻辑不能替代打包符号完整性验收。
- DLL 登记使用不分配置的全局路径集合。不同目录同名文件都可留下，部署时可能覆盖；未检查间接 DLL 依赖。构建后复制不会因仅替换源 DLL 而保证重新执行。
- 默认架构使用 Apple arm64 或指针宽度推断，非 Apple 的其他 64 位 ISA 需要显式覆盖；默认缓存也不能代替换工具链时的干净配置。

在 `build/test_output/comment-audit/prebuilt-layout-probe.cmake` 增加并运行临时诊断探针，11 项断言全部通过：标签递减、未知版本回退、Debug 隔离、发布含符号优先、自定义配置去重、多配置/无配置输出、标签优先的目录选择、动态缺目录返回空、运行时按完整路径去重及静态登记无操作。测试目录仅位于 `build/test_output/comment-audit/layout-probe`，未读取第三方源码或操作真实预编译包。该探针不覆盖跨编译实际链接、DLL 配对、PDB 标识或构建后复制行为，不能用作这些行为已验证的证据。

指定 clang64/fish 环境下，ICE 独立构建重新配置成功并通过，`IonTimeStretcherRealtimeTest` 1/1 通过（1.31 秒）。仍保持 `SOURCES_BUILD=OFF`，不构建外部第三方、不更新预编译包。主项目原子共享指针及均衡器专项测试的标准库兼容阻塞保持前述记录，未声称解决。

全量逐文件复扫范围仍为 1013 个有效自维护文件及 5 个空文件；ICE 当前 **83/92** 数值达标、**9** 个不足，全项目仍有 **694/1013** 不足（主仓 685/921）。剩余 ICE 清单为上一节表格去除 `PrebuiltLayout.cmake` 后的 9 项；完整主仓与 ICE 不足清单已刷新到 `build/test_output/comment-audit/under-30.csv`。其他原有达标文件仍待质量复核，全量审计未完成，未提交、未推送。

## ICE 输出后端构建脚本审计记录（2026-09-08）

本批复核 OpenAL 与 SDL 的自维护源码构建入口，只修正和补充中文注释，不读取或修改上游第三方源码。说明配置缓存、静态/动态选择、平台功能裁剪、目标包装、系统依赖传播及构建与运行时验证的区别。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildopenal.cmake` | 13 → 35 | 80 | 13.98% → 30.43% |
| `3rdpty/sources/cmake/Buildsdl.cmake` | 11 → 63 | 145 | 7.05% → 30.29% |

两文件均已 cmake-format 并通过 `--check`，CMake 词法 token 与 ICE HEAD 一致；所有工作区脚本和既有 C++ 的注释外文本核验通过，两层 `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功，Ninja 无待编译项，`IonTimeStretcherRealtimeTest` 1/1 通过（1.28 秒）。仍使用 `SOURCES_BUILD=OFF`，因此本批源码模式脚本只完成静态审查及语法/词法核验，不声称实际构建了 OpenAL/SDL 或验证了设备播放。

审查确认的现有边界：

- OpenAL 后端裁剪条件为 `CMAKE_CROSSCOMPILING OR WIN32`，交叉到 Linux/macOS 也会关闭所列后端；后文链接 CoreAudio 框架不重新启用该后端。
- OpenAL 的非 Apple/Windows 分支只显式补 `dl`，没有显式添加 `pthread`，已修正旧注释。静态内部日志符号宏只向静态消费者传播。
- SDL 的后端 SHARED 选项表示动态加载服务库，与 SDL 自身库类型不同；选项为 ON 不能证明配置探测成功或运行时服务存在。
- 两脚本存在仅在某个平台分支强制写入、没有反向恢复的缓存选项，跨工具链复用构建目录可能残留旧值；注释要求隔离构建树，未改变现有配置行为。
- SDL 共享目标兼容分支按名字查找，不另行验证聚合目标的库类型；静态分支要求上游提供 `SDL3-static`。构建成功不能替代实际音频设备测试。

全量复扫仍覆盖 1013 个有效自维护文件及 5 个空文件，排除第三方、预编译、生成代码和纯数据。ICE 当前 **85/92** 数值达标，仍有 **7** 个不足；全项目不足 **692/1013**，其中主仓不足 685/921。主项目构建与均衡器专项测试的标准库兼容阻塞仍未解除。

剩余 ICE 比例不足清单：`Buildffmpeg.cmake`、`Buildfftw.cmake`、`Buildlame.cmake`、`Buildrubberband.cmake`（均位于 `3rdpty/sources/cmake`），以及 `src/ice/out/io/FFmpegFileReceiver.cpp`、`src/main.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。它们的逐文件数值保持此前表格所列；全项目完整未达标清单已刷新至 `build/test_output/comment-audit/under-30.csv`，明细为 `files.csv`。其余数值达标文件仍需质量复核，全量审计未完成，未提交、未推送。

## ICE FFTW 构建脚本审计记录（2026-09-08）

本批完整复核自维护的 FFTW 构建入口，补充外部构建生命周期、源码时间戳的失效边界、平台产物名称、MSVC CRT 传递、线程子库与实际消费库的区别、交叉编译工具传递及安装产物依赖说明。仅注释和格式变化，不读取或修改 FFTW 上游源码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildfftw.cmake` | 11 → 46 | 98 | 10.09% → 31.94% |

验证：cmake-format 与 `--check` 通过，CMake token 序列与 ICE HEAD 一致；全体工作区脚本及既有 C++ 注释外文本检查通过，两层 `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功、Ninja 无待编译项，实时测试 1/1 通过（1.26 秒）。独立构建仍使用 `SOURCES_BUILD=OFF`，因此没有实际构建 FFTW，不能把 ICE 回归测试当成该源码入口的跨平台构建验证。

记录但未修复的边界：非 MSVC 动态分支用共享库前后缀直接拼 lib 下路径，没有单独描述 MinGW 导入库与 bin 下 DLL；线程子库开关为 ON 不意味着包装目标链接线程子库；源码戳不检测安装库删除或参数变化；未提供 MSVC CRT 偏好时后备为静态 CRT，不单独根据 ICE_LINKAGE 推导；非 MSVC 分支未显式传递父级各配置 C flags。后续实际打包验证应逐项覆盖，不能从注释达标推断这些行为已正确。

全量 cloc 复扫沿用 1013 个有效自维护文件及 5 个空文件的范围，排除第三方、预编译、生成代码及纯数据。ICE 当前 **86/92** 数值达标，**6** 个仍不足；全项目不足 **691/1013**，主仓仍不足 685/921。全项目明细与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 不足文件为 `3rdpty/sources/cmake/Buildffmpeg.cmake`、`3rdpty/sources/cmake/Buildlame.cmake`、`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`src/main.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。原有数值达标文件仍需质量复核；主项目构建与均衡器专项测试的兼容阻塞保持前述记录。全量目标未完成，未提交、未推送。

## ICE LAME 构建脚本审计记录（2026-09-08）

本批完整复核 LAME 自维护构建入口，补充 Autotools 参数环境、交叉 MSVC 包装器、静态归档合并、安装戳、构建并行度及稳定消费接口说明。只修改中文注释与格式，未读取或修改上游 LAME 源码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildlame.cmake` | 9 → 54 | 124 | 6.77% → 30.34% |

cmake-format、`--check`、CMake token 序列一致性及两层 `git diff --check` 通过；既有工作区脚本和 C++ 的注释外文本检查再次通过。指定 clang64/fish 环境下 ICE 独立构建成功，Ninja 无待编译项，实时测试 1/1 通过（1.32 秒）。保持 `SOURCES_BUILD=OFF`，没有实际执行 LAME 源码构建或归档合并，不能报告其跨平台构建行为已验证。

确认但未修复的边界：当前入口固定静态 LAME；交叉 MSVC 分支整体覆盖前面继承的 C flags，固定 /MT 或 /MTd 及 x86_64 host；子 make 按 ProcessorCount 自选并行度，不受外层 Ninja 上限直接约束；源码时间戳不识别安装库删除或编译参数变化；Windows 配置阶段查找的 shell 不复用于后续直接调用 sh 的构建命令。以上已在对应实现附近说明，不将注释完成当作行为修复。

全量逐文件复扫仍包含 1013 个有效自维护文件及 5 个空文件，排除范围保持不变。ICE **87/92** 数值达标，仍有 **5** 个不足；全项目不足 **690/1013**（主仓 685/921）。完整统计与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 比例不足文件：`3rdpty/sources/cmake/Buildffmpeg.cmake`、`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`src/main.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。原有达标文件仍待质量复核，主项目及均衡器专项测试兼容阻塞仍在；全量目标未完成，未提交、未推送。

## ICE 手工示例入口审计记录（2026-09-08）

本批复核 `src/main.cpp`，新增入口和演示 lambda 的中文 Doxygen，并在函数体内解释缓存策略、真实图连接、未使用节点、固定等待及失败退出语义。移除注释掉的旧调用片段，改用对当前行为的说明；未改变任何可执行代码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/main.cpp` | 24 → 67 | 154 | 13.48% → 30.32% |

审查确认的现有局限：第二音源没有加入总线，压缩器也未作为输出根，因此当前输出不包含二轨混合或压缩保护；两个扫参 lambda 均未调用；TimeStretcher 没有在入口显式 prepare；设备 open 状态未检查，加载失败仍返回 0。固定输入为原开发机器路径，非 Apple 分支也不提供 Windows 资源选择。分离诊断线程按引用捕获局部变量，500ms 延时不保证对象存活或首块处理完成，短音轨情况下存在生命周期风险。这些仅说明，未修复；示例不能替代自动化回归用例或线程安全模板。

验证：clang-format、13 个工作区 C++/头文件去注释与空白后的文本一致性、两层 `git diff --check` 通过。指定 clang64/fish 中 ICE 独立构建重新编译并链接 `testIonCachyEngin.exe` 成功，实时测试 1/1 通过（1.54 秒）。没有运行该手工示例，不声称验证实体设备播放或分离线程安全。构建仍保持 `SOURCES_BUILD=OFF`，未构建外部第三方。

全量复扫仍覆盖 1013 个有效自维护文件及 5 个空文件，范围与排除口径不变；ICE 当前 **88/92** 数值达标，仍有 **4** 个不足，全项目不足 **689/1013**（主仓 685/921）。逐文件明细和其他不足清单已刷新到 `build/test_output/comment-audit/files.csv` 与 `under-30.csv`。

ICE 剩余不足文件为 `3rdpty/sources/cmake/Buildffmpeg.cmake`、`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。其余数值达标文件仍需质量复核；主项目及均衡器专项测试兼容门槛仍未满足。全量审计未完成，未提交、未推送。

## ICE FFmpeg 构建入口审计记录（2026-09-08）

完整阅读并补注释复核自维护 FFmpeg 构建入口，覆盖宿主/目标平台区别、交叉包装器、依赖探测、编解码器与容器/解析器/位流过滤器清单、调试符号策略、配置哈希、安装清理及静态链接闭包。修正把静态归档后缀称为动态库后缀的旧说明，不改变配置参数或命令。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | 24 → 133 | 307 | 7.25% → 30.23% |

验证：cmake-format 与 `--check` 通过，所有工作区构建脚本的 CMake token/命令文本一致性检查通过，既有 13 个 C++/头文件注释外文本检查通过，两层 `git diff --check` 通过。指定 clang64/fish 中 ICE 独立构建成功，Ninja 无待编译项，实时测试 1/1 通过（1.46 秒）。保持 `SOURCES_BUILD=OFF`，未执行 FFmpeg 外部源码构建、安装或其清理命令，未读取上游第三方源码。

确认但未修复的实现边界：

- 入口固定静态 FFmpeg；MSVC 固定 /MT 或 /MTd，不按父级 CRT 偏好推导。非 MSVC 不直接继承父级 C flags，但继承通用 EXE 链接 flags，父级需避免混入业务 PGO。
- 配置哈希只包含参数清单，不覆盖工具二进制、PATH/pkg-config 环境或 zlib/LAME 库内容。源码检查仅比较 mtime，不识别删除和安装库缺失。
- 参数清单最终逐项套双引号变成 sh 字符串，未完整转义内嵌 shell 特殊字符；列表形式不等于任意路径均安全。
- 缓存失效会清理私有安装前缀，失败不回滚旧安装；安装成功后清理 share 并更新戳。本批没有执行这些操作。
- 编解码与封装清单表示构建请求，不证明全部功能探测或媒体运行验证通过；file 协议与系统套接字链接项不能混为网络协议支持。
- 子 make 使用调用环境中的 PROCESSOR_COUNT，本文件没有验证它或使之等于外层并行度。

全量逐文件范围仍为 1013 个有效自维护文件及 5 个空文件，排除口径不变；ICE **89/92** 数值达标，仍有 **3** 个不足，全项目不足 **688/1013**（主仓 685/921）。完整明细及其他不足文件清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 比例不足文件：`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。其他数值达标文件仍需质量复核，既有主项目构建与均衡器专项测试兼容阻塞仍在。全量目标未完成，未提交、未推送。

## ICE Rubber Band 构建入口审计记录（2026-09-08）

完整复核自维护 Rubber Band 构建脚本，补充 Meson 类型映射、编译/链接参数、生成 pkg-config 元数据、CRT、交叉机器描述、安装产物及增量缓存的中文说明。未修改执行内容，也未读取或修改上游 Rubber Band 源码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | 26 → 147 | 342 | 7.07% → 30.06% |

验证：重复格式化至稳定后 cmake-format `--check` 通过，CMake token 与 ICE HEAD 一致；所有工作区脚本及既有 13 个 C++/头文件注释外文本检查通过，两层 `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功，Ninja 无待编译项，实时测试 1/1 通过（1.29 秒）。保持 `SOURCES_BUILD=OFF`，本轮未执行 Meson 或外部第三方源码构建；只能据此报告注释/语法复核和 ICE 回归结果，不能报告该入口跨平台构建已经验证。

确认但未修复的边界：

- LTO 追加编译 flags，却覆盖之前的链接 flags，可能丢掉 target、sysroot 或运行库参数；脚本仍引用外层命名的 LTO 开关，独立项目变量归属需要另行治理。
- 生成的 pkg-config 版本固定为 FFTW 3.3.10 与 libsamplerate 0.1.9，不验证源码或归档版本；路径为当前构建绝对位置，不是可迁移发布元数据。
- 非 MSVC 的所有交叉分支都写固定 Windows x86_64 little-endian 机器描述；MSVC cross-file 路径则依赖顶层源码目录，native-file=NUL 还包含宿主平台假设。
- 非 MSVC 已有 build.ninja 时跳过 setup，源码戳也不覆盖参数、依赖库和安装库缺失；MSVC 分支不使用这个时间戳缓存，两者不能合并描述。
- 非 MSVC shared 路径没有单列 MinGW 导入库和 bin 下 DLL；构建产物列表也未完整描述旁路符号，不能代替预编译包完整性验收。
- `MESON_ENV`、`RUBBERBAND_BUILD_TYPE` 等历史变量没有进入最终对应命令，不能把它们的设置当作实际行为；shell 字符串和生成 INI 的特殊字符引用也仍有约束。

全项目逐文件复扫范围保持 1013 个有效自维护文件及 5 个空文件，排除范围不变。ICE 当前 **90/92** 数值达标、**2** 个不足；全项目不足 **687/1013**（主仓 685/921）。完整统计及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 比例不足文件为 `src/ice/out/io/FFmpegFileReceiver.cpp`（52 行注释、742 行代码，6.55%）与 `tests/TimeStretcherRealtimeTest.cpp`（123 行注释、1047 行代码，10.51%）。原有数值达标文件仍需质量复核；既有主项目构建和均衡器专项测试兼容门槛仍未满足。全量审计未完成，未提交、未推送。

## ICE FFmpeg 文件输出分段审计记录一（2026-09-08）

已完整阅读文件输出实现和公开头，本批先补充格式协商 helper、采样数组释放、配置访问、open/close/start/stop 生命周期部分。中文 Doxygen 与函数内说明强调优选 codec 回退、能力表不可用的尝试语义、采样率首项回退、声道数匹配而非声道身份匹配、输入进度与输出持久化的区别、停止与资源释放的同步边界。英文 helper 文档已译为中文，不改业务文本。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 | 状态 |
| --- | ---: | ---: | ---: | --- |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 52 → 140 | 742 | 6.55% → 15.87% | 分段进行中，未达 30% |

该文件尚未完成，后续继续处理编码器初始化、重采样输出、FIFO 分帧、发送取包与排尾；不能把本批记作整文件通过，也不能仅堆积函数外说明补齐比例。

已确认的接口边界：frames_written 是送入编码链路的累计输入帧，不是磁盘持久化帧数；采样率无法精确匹配时选能力表首项而非最近值；声道布局按数量匹配，不保证声道身份相同；running 的 load/store 不提供 start 的互斥获取，close 也不能与正在执行的编码循环并发。stop 只请求块边界取消，不能中断当前编码、文件 IO 或回调；取消/失败路径不保证补 trailer，也不删除部分输出。头文件相关接口说明仍需与后续整文件复核一起检查一致性，本批没有声称修复这些行为。

验证：clang-format、14 个工作区 C++/头文件去注释与空白文本一致性、两层 `git diff --check` 均通过。指定 clang64/fish 中已重新编译 FFmpegFileReceiver、链接 ICE 静态库与示例/测试，实时测试 1/1 通过（1.74 秒）。该测试不覆盖文件编码、取消输出或容器有效性，不能用作这些行为的运行验证；主项目完整构建与均衡器专项测试兼容阻塞仍在。

全量统计范围仍为 1013 个有效自维护文件及 5 个空文件，排除口径保持不变。ICE 仍为 **90/92** 数值达标、**2** 个不足；全项目仍 **687/1013** 不足（主仓 685/921）。另一个 ICE 不足文件为 `tests/TimeStretcherRealtimeTest.cpp`，仍为 123 行注释、1047 行代码、10.51%。最新逐文件明细与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量目标保持进行中，未提交、未推送。

## ICE FFmpeg 文件输出分段审计记录二（2026-09-08）

继续完成编码器初始化、重采样、FIFO 分帧、结束排尾、编码包写入和错误保存的 Doxygen 与实现附近注释。说明输入/输出时间基、固定帧长与短尾处理、临时布局及样本所有权、错误不回滚、包接收终止条件和实际文件写入边界。没有改变任何业务表达式或控制流程。

| ICE 相对路径 | 注释行：本批前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 140 → 321 | 742 → 744 | 15.87% → 30.14% |

代码行变化来自 clang-format 的换行；14 个工作区 C++/头文件去注释与空白后的文本均与 ICE HEAD 一致。clang-format、两层 `git diff --check` 通过。指定 clang64/fish 下重新编译输出实现并链接引擎、示例、测试成功，实时测试 1/1 通过（1.49 秒）。该测试不覆盖音频文件编码、取消文件有效性或高声道数；不能把它作为这些行为已验证的证据。

新增记录的现有约束与修复候选：初始 FIFO 块大小窄化及容量累加未完整检查整数上界；输出容量查询为零时直接返回且不调用转换器；固定长度编码器的尾帧不补零，由编码器接受或拒绝；FIFO 使用 frame->data 而非 extended_data，较多平面声道需要另行验证；样本出队与 PTS 推进后发送失败不回滚；send 的 EAGAIN 被当成失败，没有先 drain 再重试分支；排尾期间不检查 stop，不能保证及时取消。上述均未实施行为修复。

本实现的数值与实现说明复核已完成，但公开头 `include/ice/out/io/FFmpegFileReceiver.hpp` 仍需统一输入进度、持久化含义与跨线程调用契约，A5 批次暂不整体勾选。全量逐文件统计范围保持 1013 个有效自维护文件及 5 个空文件，排除口径不变；ICE **91/92** 数值达标，唯一比例不足为 `tests/TimeStretcherRealtimeTest.cpp`（123 行注释、1047 行代码，10.51%）。全项目不足 **686/1013**，其中主仓 685/921；完整明细及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

其余原有数值达标文件仍需质量复核，主项目构建及均衡器专项测试兼容门槛仍未满足。全量审计目标保持进行中，未提交、未推送。

## ICE 文件输出公开契约一致性复核（2026-09-08）

本批对照完整实现更新 `FFmpegFileReceiver.hpp`，统一输入预算/进度与编码输出的单位区别、同步回调线程、取消请求、错误文本借用及资源回收约束。原先“close 总会写 trailer”及“运行状态 false 表示完成”的潜在误读已消除；同时修正实现与头中 start 的前置失败路径说明：前置检查失败直接返回，只有进入编码循环后才统一收尾关闭。没有修改接口签名、原子操作或执行逻辑。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 110 | 68 | 61.80% |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 323 | 744 | 30.27% |

验证：两文件均 clang-format；当前 15 个工作区 C++/头文件去注释与空白文本一致性通过；两层 `git diff --check` 通过。指定 clang64/fish 下重新编译接收端并链接 ICE、示例和测试成功，实时测试 1/1 通过（最终一轮 1.67 秒）。仍不将该测试解释为文件编码有效性、取消输出或高声道数覆盖；未执行外部第三方源码构建。

全量逐文件复扫范围保持 1013 个有效自维护文件及 5 个空文件，排除口径不变。ICE 仍 **91/92** 数值达标，唯一比例不足为 `tests/TimeStretcherRealtimeTest.cpp`（123 行注释、1047 行代码，10.51%）；全项目仍 **686/1013** 不足，主仓 685/921。最新明细与其他不足文件清单位于 `build/test_output/comment-audit/files.csv`、`under-30.csv`。下一步处理实时测试注释及其余原有达标文件质量复核，既有构建/专项测试兼容门槛仍未满足，全量审计未完成，未提交、未推送。

## ICE 实时测试分段审计记录一（2026-09-08）

补充 `tests/TimeStretcherRealtimeTest.cpp` 前半段的夹具契约、函数内实现说明和测试覆盖边界：固定容量边界脚本、单线程消费、借用 epoch 生命周期、release/acquire 通知、relaxed 观测计数、块内代际切换、分配统计窗口，以及状态切换、排尾、容量保护、暂停恢复和小数帧余量用例。只修改注释与格式，没有修改测试条件或业务行为。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 | 状态 |
| --- | ---: | ---: | ---: | --- |
| `tests/TimeStretcherRealtimeTest.cpp` | 123 → 204 | 1047 | 10.51% → 16.31% | 分段审计进行中，未达 30% |

本批明确了证据限制：线程局部分配统计不能证明其他线程或未接入钩子的入口没有堆操作；部分早期用例仅检查 C++ 计数；静音比较不拒绝 NaN；首声道极值与可闻帧统计不证明多声道一致或算法有效帧数；暂停用例为串行状态观察，不能代替设备及并发测试。上述是现有测试边界，不是本批修复完成的缺陷。后半段极端参数、边界用例、并发测试和分配钩子实现仍待继续复核。

验证：clang-format 完成；16 个工作区 C++/头文件去注释及空白后的文本与 ICE HEAD 一致；两层 `git diff --check` 通过。指定 clang64/fish 环境下重新构建 ICE 独立测试成功，`IonTimeStretcherRealtimeTest` 1/1 通过（2.22 秒）。主项目完整构建的 libc++ 原子共享指针阻塞和均衡器专项测试的 aligned_alloc 阻塞仍按前文记录，不作通过声明。

全量逐文件复扫保持 1013 个有效自维护文件、5 个空文件及既有排除口径；ICE 91/92 数值达标，唯一比例不足仍为上述测试文件。全项目不足 686/1013，其中主仓 685/921。完整逐文件数据及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计目标仍进行中，未提交、未推送。

## ICE 实时测试分段审计记录二（2026-09-08）

继续补充极端状态、待发布状态与 final 排尾、起始延迟补偿、循环边界、provider Final、旁路和并发发布用例；为分配器包装、全局分配/释放入口和测试入口添加中文 Doxygen，并在关键实现附近说明计数及所有权边界。测试行为和断言未改变。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 | 状态 |
| --- | ---: | ---: | ---: | --- |
| `tests/TimeStretcherRealtimeTest.cpp` | 204 → 321 | 1047 | 16.31% → 23.46% | 仍不足 30%，继续复核 |

进一步明确：极端参数不是完整组合覆盖；循环边界极值只能证明两侧信号出现，不能证明逐采样连续；Final 可闻帧上界不证明低幅度尾音未丢失；并发测试没有强制每次发布与音频读取重叠；C 包装依赖链接选项，不保证捕获共享库内部调用；realloc 计数为保守的调用记录，不等于实际搬迁/释放次数；普通 new 钩子未在此实现过对齐入口。部分 reset 用例及并发汇总仍未检查 C free，未通过修改测试掩盖这些覆盖边界。

验证：clang-format、16 个工作区 C++/头文件去注释与空白文本一致性、ICE `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功，实时测试 1/1 通过（2.32 秒）。全量复扫仍为 1013 个有效自维护文件和 5 个空文件；ICE 91/92 数值达标，唯一不足仍为此测试文件；全项目 686/1013 不足（主仓 685/921），最新明细及不足清单仍在 `build/test_output/comment-audit/files.csv`、`under-30.csv`。尚需测试文件整体质量和比例复核、其余原有达标文件质量复核；主项目构建及均衡器专项测试的既有兼容门槛未消除。全量目标保持进行中，未提交、未推送。

## ICE 测试说明校正与基类质量复核（2026-09-08）

对照 `TimeStretcher.cpp` 的 `roundedOutputFrames` 与终止段预算计算，修正测试中误写的 floor 说明：实现对累计输出预算四舍五入，37 / 0.75 在两种取整方式下恰好都得到 49，因此当前用例无法区分这两种规则。对照 `src/CMakeLists.txt` 及当前 Ninja 构建规则，确认 **Windows Clang64 未启用 ICE_TEST_WRAP_MALLOC 或 --wrap**；该配置中 C 堆计数为零没有提供 C 堆观测证据。此前实时测试通过记录只能按实际启用的入口解释，不能扩展为 C 堆也已覆盖。

同时完整阅读并复核 `IAudioNode.hpp`、`IEffectNode.hpp`、`IEffectNode.cpp`：补充接口析构、同步借用、单实例串行处理、无环上游链、无效准备、效果输出完整性和缓冲引用约束。实现侧的保护、预清零和失败静音说明已有覆盖，本批未修改实现。只读复核 `src/sample.cpp`，确认其已不再保留注释掉的旧示例，仍为未接入构建且不应调用的无返回占位；不能作为运行验证。上述四个文件的本次语义阅读为质量复核证据，不代表剩余 ICE 文件均已复核。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/IAudioNode.hpp` | 15 | 14 | 51.72% |
| `include/ice/core/effect/IEffectNode.hpp` | 44 | 40 | 52.38% |
| `tests/TimeStretcherRealtimeTest.cpp` | 325 | 1047 | 23.69%（未达标） |

三个文件均执行 clang-format，18 个工作区 C++/头文件去注释及空白文本与 ICE HEAD 一致。指定 clang64/fish 下重新构建受影响 ICE 源码、示例和测试成功，实时测试 1/1 通过（2.21 秒）。全项目逐文件复扫范围仍为 1013 个有效自维护文件与 5 个空文件，排除口径不变；ICE 91/92 数值达标，唯一不足仍为实时测试；全项目 686/1013 不足（主仓 685/921）。最新逐文件表及不足清单位于 `build/test_output/comment-audit/files.csv`、`under-30.csv`。主项目构建及专项测试兼容门槛仍未满足，全量审计继续进行，未提交、未推送。

## ICE 实时测试夹具观察口径复核（2026-09-08）

继续补充零预算区间消费、零帧拉取对首段序号的影响、统计帧与样本单位、标签借用，以及静音/极值/可闻帧观察的有效范围。未改变夹具或断言行为。

`tests/TimeStretcherRealtimeTest.cpp`：409 行注释、1047 行代码，**28.09%**，仍未达 30%。clang-format、41 个工作区 C++/头文件去注释与空白文本一致性通过；指定 clang64/fish 下正式测试构建成功，CTest 1/1 通过（2.20 秒）。全量复扫保持 1013 个有效自维护文件、5 个空文件；ICE 91/92 比例达标，全项目不足 686/1013，主仓 685/921。完整统计与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计未完成，未提交、未推送。

## ICE 实时测试排尾与并发收尾复核（2026-09-08）

继续补充待发布状态接管与终态保持的组合断言、起始预填与延迟预算的单位区别、双声道偏移哨兵、并发线程的 TLS 汇总和停止/析构边界。明确最后一个发布状态不保证被消费、无启动握手可能使回调次数断言失败，以及部分末段只验证行为而未开启分配统计。未改变测试行为。

`tests/TimeStretcherRealtimeTest.cpp` 当前 391 行注释、1047 行代码，**27.19%**，仍未达 30%。clang-format、41 个工作区 C++/头文件去注释及空白文本一致性通过；指定 clang64/fish 下正式测试构建成功，CTest 1/1 通过（2.30 秒）。全量统计继续覆盖 1013 个有效自维护文件、5 个空文件；ICE 91/92 比例达标，全项目不足 686/1013，主仓 685/921，最新明细和其他不足清单已更新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计仍未完成，未提交、未推送。

## ICE 实时测试边界断言复核（2026-09-08）

补充容量保护、暂停恢复、小数预算、循环两侧、provider Final 和旁路终态的构造目的与断言边界。明确容量测试未设非零输出哨兵、暂停只检查最后块内容、最终累计预算不证明逐块分布，以及旁路“立即排空”标签实际在第二次 process 后观察状态。未修改断言或增加不真实的覆盖承诺。

本批修改 `tests/TimeStretcherRealtimeTest.cpp`：367 行注释、1047 行代码，**25.95%**，仍未达 30%。clang-format、41 个工作区 C++/头文件去注释与空白文本一致性通过；指定 clang64/fish 下正式测试构建成功，CTest 1/1 通过（2.56 秒）。全量复扫口径保持 1013 个有效自维护文件、5 个空文件；ICE 91/92 比例达标，全项目不足 686/1013（主仓 685/921），完整表及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计仍进行中，未提交、未推送。

## ICE 实时测试阶段契约补充（2026-09-08）

继续补充正式实时测试中参数连续发布、旁路排除、时间线借用、代际递增与终态断言的依据。极端用例说明三个 setter 并非整组原子提交，准备中间状态可混合新倍率和上一组参数；明确第三组倍率变更仍沿用 +24 半音与 Balanced。未修改断言、处理调用或统计窗口。

本批修改 `tests/TimeStretcherRealtimeTest.cpp`：注释由 325 增至 348 行，代码仍 1047 行，比例由 23.69% 增至 **24.95%**，尚未达到 30%。clang-format、41 个工作区 C++/头文件去注释及空白文本一致性、ICE diff 检查通过。指定 clang64/fish 下正式测试重新构建并运行，CTest 1/1 通过（2.48 秒）。原有 C 堆包装平台限制保持不变。

全量复扫仍为 1013 个有效自维护文件、5 个空文件，排除口径不变；ICE 91/92 数值达标，唯一不足仍为实时测试；全项目不足 686/1013，主仓 685/921。最新逐文件表及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量质量复核、测试比例与既有主项目构建门槛尚未完成，未提交、未推送。

## ICE 混音与来源节点注释终核（2026-09-08）

重新完整对照 MixBus、SourceNode 的声明与实现及 IAudioNode 契约。混音头补充无环图与单来源串行处理前置条件、控制侧增删/清空的分配回收边界、每块路由读取的原子警告，以及左右静音模式互斥和读后写非事务。来源节点补充禁止复制/移动声明、暂停不等待在途处理、增益定义域、空回调/空音轨约束、时间转换前置条件和调度/定位多原子写的覆盖风险；移除未经证明的 lock-free 表述。实现中修正 hazard 对裸指针/引用捕获的生命周期保证，补充 provider 查询与重试的热路径说明。不改变算法、内存序或失败处理。

| ICE 相对路径 | 本批处理 | 注释行 | 代码行 | 注释率 |
| --- | --- | ---: | ---: | ---: |
| `include/ice/core/MixBus.hpp` | 修改注释 | 102 | 105 | 49.28% |
| `include/ice/core/SourceNode.hpp` | 修改注释 | 168 | 176 | 48.84% |
| `src/ice/core/SourceNode.cpp` | 修改注释 | 127 | 288 | 30.60% |
| `src/ice/core/MixBus.cpp` | 只读终核 | 115 | 262 | 30.50% |

三个修改文件 clang-format；44 个工作区 C++/头文件去注释与空白文本一致，ICE diff 检查通过。Clang64 以 12 并行重建混音、来源节点、引擎库和示例成功，CTest 1/1 通过（1.45 秒）。该实时变速测试不能代替 MixBus/SourceNode 专项运行覆盖，不据此声称并发定位等既有风险已修复。

验收索引为四个完整复核文件登记“注释终核完成”和 SHA-256 内容快照，后续据差异决定是否重审，不再仅以历史批次名称表示状态。其他文件仍按清单继续终核，整体目标未完成。全项目复扫范围保持 1013 个有效文件、5 个空文件；ICE 92/92 数值达标，2 空文件不适用；主仓仍有 685 个不足，明细见 `build/test_output/comment-audit/files.csv`、`under-30.csv`。未提交、未推送。

## ICE 顶层与目标构建说明终审（2026-09-08）

完整对照顶层、`src/CMakeLists.txt` 和依赖入口；核对共享导出宏的实际使用与安装规则。顶层修正“标准选择保证 ABI 可链接”、模块路径隔离、静态依赖可独立分发等过度承诺；说明链接参数按子串替换而非命令行分词，仅覆盖通用缓存项，不包含配置/目标级选项，且本入口未剥离继承的 PGO 参数。目标入口明确安装接口不等于安装规则、Windows 不启用 C 堆包装以及 CTest 未设置超时。依赖入口只读复核，不读取外部第三方实现。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `CMakeLists.txt` | 30 | 70 | 30.00% |
| `src/CMakeLists.txt` | 22 | 50 | 30.56% |

两个修改文件已 cmake-format；30 个修改的构建/工具脚本非注释语义文本与 HEAD 一致，41 个 C++/头文件去注释与空白文本一致。Clang64 独立 ICE 目录重新配置与构建成功，CTest 1/1 通过（1.33 秒）；ICE 和文档 diff 检查通过。未改变构建命令、开关默认值或运行时逻辑，未构建外部依赖源码。

新增 [ICE 注释审计验收索引](ice-comment-audit-index.md)，包含全部 94 个范围内文件的逐文件统计和职责复核记录定位，并分列数值、质量、格式、构建、测试和交付门槛。表中记录定位不等同于最终质量通过，仍须核对其证据覆盖当前文件，不能把索引存在当成终审完成。

全项目复扫保持 1013 个有效自维护文件及 5 个空文件；ICE 92/92 数值达标，2 个空文件不适用，主仓仍有 685 个不足。完整与不足清单仍为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。主项目 Clang64 构建门槛和其余证据终核未完成，目标保持进行中，未提交、未推送。

## ICE 交叉工具链注释质量复核（2026-09-08）

完整复核 `cmake/toolchain-macos-to-windows.cmake`，保留原有工具链及路径设置，仅重写注释。移除英文分隔标题和注释掉的窗口链接参数，解释宿主工具/目标库分离、固定 x86_64 与开发机绝对系统根、普通变量覆盖及 ABI 不校验。根据 [CMake 查找文档](https://cmake.org/cmake/help/latest/command/find_package.html) 修正“所有 Find 模块只能在系统根中找到”和“保证没有宿主依赖泄漏”的过度表述：模块加载与配置模式包查找不是同一过程，显式路径及调用参数仍可绕开默认根路径模式。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `cmake/toolchain-macos-to-windows.cmake` | 11 | 15 | 42.31% |

文件已 cmake-format；28 个修改的构建/工具脚本非注释语义文本与 HEAD 一致，41 个 C++/头文件去注释及空白文本一致，ICE diff 检查通过。`cmake -P` 解析执行成功，只验证设置语句和输出，不探测 macOS 编译器或进行交叉链接。Clang64 独立 ICE 构建成功（无待编译项），CTest 1/1 通过（1.27 秒），不能将本机构建当作 macOS 交叉编译验证。

全量复扫仍包含主仓与 ICE 自维护代码、构建及工具脚本，共 1013 个有效文件及 5 个空文件，排除口径不变；ICE 92/92 数值达标，另外 2 个空文件不适用。主仓 685 个不足文件保留在 `build/test_output/comment-audit/under-30.csv`，完整数据为 `files.csv`。剩余需将全部文件的质量证据逐一与清单对齐，并复查顶层/目标构建说明及验证门槛，不将已知未修复行为或未运行的平台分支标为通过。目标保持进行中，未提交、未推送。

## ICE 小型构建入口与历史错误类型质量复核（2026-09-08）

通读三个 `include/ice/execptions/` 头并核对 AudioBuffer、CachyDecoder、FFmpeg 的实际使用点，确认现有说明对应历史失败通道，类型没有额外数据成员；只读复核，不调整异常策略。另通读顶层、src、依赖入口和小型构建脚本，发现数值达标仍未解决的浅层或错误说明，按职责先完成下列两文件。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `cmake/configure-project.cmake` | 4 | 7 | 36.36% |
| `3rdpty/sources/cmake/Buildfmt.cmake` | 8 | 17 | 32.00% |

指令集入口移除注释掉的 AVX/消毒器配置，解释目录作用域、空平台分支及无条件 AVX2 的部署约束。fmt 入口替换 IDE 显示控制、强制相同标准及固定 `-fPIC` 等不准确表述，说明缓存覆盖、依赖触发构建、标准版本继承和平台相关 PIC。未读取 fmt 第三方源码、未修改命令。

两文件 cmake-format；27 个修改的构建/工具脚本非注释语义文本与 HEAD 一致，41 个 C++/头文件去注释与空白文本一致，ICE diff 检查通过。Clang64 独立目录重新配置及构建成功，CTest 1/1 通过（1.26 秒）。当前 `SOURCES_BUILD=OFF`，未执行 fmt 源码构建，不将此构建结果当作该依赖分支运行验证。

全量统计仍为 1013 个有效文件和 5 个空文件；ICE 92/92 数值达标、2 个空文件不适用；其他不足均在主仓，共 685 个，清单见 `build/test_output/comment-audit/under-30.csv`。继续保留 macOS 交叉工具链脚本的绝对化查找保证等说明复核，以及全量证据归档和主项目验证门槛；目标未完成，未提交、未推送。

## ICE 实时测试注释收尾与数值门槛清零（2026-09-08）

完成测试入口、普通及带大小分配/释放替换函数、TLS 统计、脚本输入单位及观察助手的契约说明。修正数组释放说明：元素析构次数不计入最终存储释放次数，但析构函数内部若调用已接入的堆入口，仍可能被全局钩子观察。补充空声道指针、极值输出别名与 NaN/无穷大判定的前置条件和覆盖边界。只修改中文注释，不改变用例、同步或断言。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `tests/TimeStretcherRealtimeTest.cpp` | 449 | 1047 | 30.01% |

再次只读确认 `src/sample.cpp` 是未接入构建的空历史入口，不含废弃注释代码，不能用作文件解码验证。A6 的注释工作据此完成。

验证：测试文件 clang-format；41 个工作区 C++/头文件去注释与空白文本一致；ICE `git diff --check` 通过。指定 clang64/fish 环境以 12 并行构建独立 ICE 审计目录成功，CTest `IonTimeStretcherRealtimeTest` 1/1 通过（2.49 秒）。Windows 配置未启用 C 分配链接包装，不能宣称 C 堆零分配已被证明；测试也不覆盖所有设备后端、文件解码和效果器。

全项目复扫包含 1013 个有效自维护文件及 5 个空文件，排除外部第三方、预编译/生成目录和纯数据，保留自维护构建脚本及资源目录中的程序脚本。ICE 92/92 有效文件数值达标，另 2 个空文件不适用；主仓仍有 685/921 个不足，因此全项目仍有 685/1013 个不足。完整逐文件数据及其他不足清单为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余验收：逐一归档全量质量复核证据、核对热路径说明及已有验证门槛。主项目 Clang64 原子共享指针兼容问题仍未修复，既有行为缺陷也未按注释任务扩大修改。数值清零不等于所有审计通过，目标保持进行中，未提交、未推送。

## ICE 极端参数扩容警告定位（2026-09-08）

本轮针对上次日志中的 RubberBand 扩容警告完成只读源码分析和隔离运行诊断，不修改任何已跟踪测试或业务源码，也不读取第三方源码。构建目录新增临时诊断入口 `build/test_output/comment-audit/realtime-case-probe.cpp`，沿用 Ninja 中原测试的 Clang64 编译选项及库依赖，逐进程调用原有 12 个测试函数：全部返回零，只有 `testExtremePreparedStates` 输出该警告。

为细分阶段，在 `realtime-phase-snapshot.cpp` 保存当前原测试的诊断副本，仅在极端用例的倍率、音高、质量、首次处理、重置请求、重置处理前加入可选终止检查点，未改原测试。检查点 0 至 12 的执行前缀无警告，13 首次出现同一扩容警告。检查点 12 是第三组参数的 `set_playback_ratio(0.05)` 之前，13 是其之后、`set_pitch_semitones` 之前；此时沿用前组的 +24 半音和 Balanced 质量。将终止改为 `_Exit` 跳过析构，再次运行 12/13 得到相同结果，排除是局部对象收尾产生警告。

因此已确认本次警告发生于第三组倍率 setter 内，**在堆统计窗口和该组 process 之前**，并非本次回调内扩容证据。自有代码中该 setter 会调用 `publish_prepared_state`，后端包装构造包含预热，与控制侧准备行为相符。仍不能据此证明所有回调无 C 堆分配：当前 Windows 测试没有启用 C 堆包装，既有覆盖限制保持不变。

临时诊断副本的提前退出仅用于阶段定位，不是完整用例通过证据；12 个原函数的独立完整执行结果与阶段前缀结果已明确区分。临时文件位于构建输出目录，排除于自维护逐文件统计，清理构建目录会移除；没有改 CMake、正式测试断言或第三方库。上一轮官方 CTest 仍为 1/1 通过（2.49 秒）。本轮仅更新诊断证据与文档，原逐文件统计不变：ICE 91/92 比例达标，实时测试 325/1047 行、23.69%，全项目不足清单仍见 `build/test_output/comment-audit/under-30.csv`。全量审计仍未完成，未提交、未推送。

## ICE 音频缓冲质量复核（2026-09-08）

完整复核 AudioBuffer 两个平台分支与移动实现，补充活动范围扩大不清零、Linux clear_from 不清对齐尾部、尺寸算术未完整防溢出、扩容失败不整体回滚，以及复制禁用和移动所有权契约。移除 Linux 历史 SIMD 转换段未经完整验证的中间结果推断，保留明确的四帧步长与八浮点写入冲突警告。未修改样本运算或内存管理行为。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 125 | 283 | 30.64% |
| `src/ice/manage/AudioBuffer.cpp` | 41 | 94 | 30.37% |

两文件 clang-format，41 个工作区 C++/头文件去注释和空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建当前引擎、示例及测试成功，实时测试 1/1 通过（2.49 秒）。Windows 构建只覆盖非 Linux 缓冲分支，不能证明 Linux SIMD 路径可用。

本轮 LastTest.log 同时输出 RubberBand 输出缓冲被迫从 131072 增至 262144 的警告。日志没有标明发生于哪个测试统计窗口，不能推断是准备阶段还是回调阶段；结合 Windows 未启用 C 堆包装，此次测试通过不能作为整个后端零扩容的证明。需在后续测试质量复核中定位，未擅自改测试或后端来掩盖警告。

全量复扫仍为 1013 个有效自维护文件、5 个空文件，排除口径不变；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。完整表及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。质量复核、测试比例和既有构建门槛尚未全部完成，未提交、未推送。

## ICE 接收端与播放通知契约复核（2026-09-08）

完整复核 IReceiver 头与构造实现、PlayCallBack 头，核对 SourceNode 注册与实际通知调用。补充基类不保存格式、默认析构不调用 close、后端 start/stop 同步性不同、get_source 引用不额外保活等边界。播放完成通知表示输入末尾，不是设备尾音播放完成；帧进度是下一次读取游标，循环时可归零；纳秒仅为时间表示单位。两份空头 ALSource.hpp 和 IEncoder.hpp 保持不适用，不计为达标文件。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/PlayCallBack.hpp` | 18 | 17 | 51.43% |
| `include/ice/out/IReceiver.hpp` | 28 | 30 | 48.28% |

两文件 clang-format，39 个工作区 C++/头文件去注释和空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下受影响引擎、示例和测试构建成功，实时测试 1/1 通过（1.29 秒）；不扩展为实体设备停机或通知线程专项测试。未修改执行逻辑。

全量复扫仍为 1013 个有效自维护文件、5 个空文件，排除口径不变；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。完整逐文件数据及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量审计、测试比例及既有构建门槛未完成，未提交、未推送。

## ICE 配置与音频格式质量复核（2026-09-08）

完整复核配置头、配置定义和 AudioFormat，并检索配置消费位置。补充构造时快照与 SourceNode 处理时读取全局值并存的约束，明确运行期间必须保持配置稳定；默认块长的零值保护因消费端而异。格式类型补充公开字段无校验、声道数量不等于布局身份等边界，不改变默认值或运行行为。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/config/config.hpp` | 18 | 27 | 40.00% |
| `include/ice/manage/AudioFormat.hpp` | 9 | 12 | 42.86% |

两文件 clang-format，37 个工作区 C++/头文件去注释及空白文本一致性通过。指定 clang64/fish 下重建受影响引擎源码、示例和测试成功，实时测试 1/1 通过（2.37 秒）。未将测试解释为运行中更改全局配置安全，此行为仍不受支持。

全量复扫保持 1013 个有效自维护文件、5 个空文件及既有排除口径；ICE 91/92 比例达标，实时测试仍为唯一不足（325/1047 行，23.69%），全项目不足 686/1013，主仓 685/921。完整逐文件数据及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。剩余质量复核、测试文件比例和既有主项目构建门槛未完成，未提交、未推送。

## ICE 封面所有权质量复核（2026-09-08）

完整核对 `AlbumArt.hpp` 与 FFmpeg 封面复制调用，补充公开裸指针的所有权不变量、复制失败状态、移动前后借用地址与目标旧视图失效，以及 isValid 只检查非空的边界。替换旧的“窃取资源”等浅层说明，不修改数组分配、释放或异常行为。

本批修改文件 `include/ice/manage/dec/AlbumArt.hpp`：36 行注释、52 行代码，40.91%。clang-format、35 个工作区 C++/头文件去注释及空白文本一致性、ICE diff 检查通过。指定 clang64/fish 下受影响引擎、示例及实时测试构建成功，CTest 1/1 通过（1.36 秒）；不代表封面分配失败、错误别名或图像格式有效性已作专项运行验证。

全量复扫范围不变：1013 个有效自维护文件和 5 个空文件；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。最新完整明细及其他不足清单已更新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量审计、测试比例及既有构建门槛继续保留，未提交、未推送。

## ICE 工厂、实例与媒体信息契约复核（2026-09-08）

复核 `IDecoderFactory.hpp`、`IDecoderInstance.hpp`、`MediaInfo.hpp`，并对照 FFmpeg 工厂头、创建实现及格式/长度查询。修正工厂基类“失败仅返回空”的错误承诺，现明确历史实现还可抛出异常；补充实例定位精度、源/输出格式区别、帧单位与读取阻塞边界，以及元信息标量默认初始化不赋初值的约束。未改变代码行为。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 15 | 22 | 40.54% |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 19 | 18 | 51.35% |
| `include/ice/manage/dec/MediaInfo.hpp` | 12 | 15 | 44.44% |

三个头文件 clang-format，34 个工作区 C++/头文件去注释与空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下受影响引擎、示例及测试构建成功，实时测试 1/1 通过（1.44 秒）；不将其解释为工厂失败或文件解码专项覆盖。

全量复扫仍覆盖 1013 个有效自维护文件及 5 个空文件，ICE 91/92 比例达标，唯一不足为实时测试（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。最新逐文件统计及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量复核、测试比例及既有主项目构建门槛未完成，未提交、未推送。

## ICE 独立变调与限幅占位质量复核（2026-09-08）

完整阅读 `PitchAlter.hpp/.cpp` 与 `Clipper.hpp/.cpp`。变调接口补充倍率定义域、半音换算、默认顺序一致原子读写、重复上游共享句柄读取和覆盖基类保护的边界；说明近单位倍率旁路使用累加、保留旧后端历史，首次后端创建未按当前块长重新准备。只读确认 Clipper 的头和实现已明确空处理不裁剪也不直通，本批不修改该占位类型，不把类型存在当作限幅功能通过。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/effect/PitchAlter.hpp` | 37 | 33 | 52.86% |
| `src/ice/core/effect/PitchAlter.cpp` | 21 | 28 | 42.86% |

两文件 clang-format，31 个工作区 C++/头文件去注释及空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建当前引擎、示例和测试成功，实时测试 1/1 通过（1.45 秒），不覆盖 PitchAlter 的旁路切换或 Clipper 功能。原有热路径扩容、共享复制、状态延续和参数校验问题只记录未修复。

全量复扫范围不变：1013 个有效自维护文件、5 个空文件；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）。全项目 686/1013 不足，主仓 685/921；明细与其他不足清单已更新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。剩余质量复核、测试比例及主项目构建门槛未完成，未提交、未推送。

## ICE 压缩效果器质量复核（2026-09-08）

完整复核 `Compresser.hpp/.cpp`，修正“上升一定更快”等不成立的参数假设，明确 dB 域平滑、各声道独立包络、零 dB 初值、resize 保留已有状态、补偿缓存未参与实际处理和最终输出不限幅。补充原子参数写入者/读取者、默认顺序一致及非原子脏标记约束，不改变同步操作或算法。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/effect/Compresser.hpp` | 39 | 53 | 42.39% |
| `src/ice/core/effect/Compresser.cpp` | 29 | 54 | 34.94% |

现有风险未修复：更新分支在音频调用栈中可能扩容；单独改变声道数不触发包络 resize；参数不校验有限性与范围；补偿逐样本原子读取；格式不匹配时直接保留输出；不存在立体声联动或最终限幅。注释复核不代表这些行为已满足实时规范。

两文件 clang-format，29 个工作区 C++/头文件去注释及空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建引擎、示例及测试成功，实时测试 1/1 通过（1.37 秒）；此测试不覆盖 Compressor 音频结果或参数切换。

全量复扫口径仍为 1013 个有效自维护文件及 5 个空文件；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）。全项目 686/1013 不足，主仓 685/921，完整明细及其他不足清单已更新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。剩余质量复核、测试比例与既有构建门槛未完成，目标保持进行中，未提交、未推送。

## ICE 音轨封装质量复核（2026-09-08）

完整对照 `AudioTrack.hpp/.cpp`，修正“绝对路径”的旧说明，实际为原样保存输入字符串；补充策略枚举、构造前置条件、源元信息与输出格式区别、read 缓冲容量、origin 浮点转整数及借用生命周期。非法策略会留下空解码器、origin 丢弃读取帧数且不检查转换范围等现有边界均已明确，未修改行为或接口。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 42 | 52 | 44.68% |
| `src/ice/manage/AudioTrack.cpp` | 23 | 41 | 35.94% |

两文件 clang-format，27 个工作区 C++/头文件去注释及空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建受影响引擎、示例及实时测试成功，CTest 1/1 通过（1.38 秒），不将此视为文件解码或 origin 边界专项验证。

全量统计范围不变：1013 个有效自维护文件、5 个空文件；ICE 91/92 数值达标，唯一不足仍为实时测试（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。完整统计及不足文件清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量复核、测试比例和既有主项目构建门槛仍未完成；未提交、未推送。

## ICE 解码接口与流式占位质量复核（2026-09-08）

完整核对 `IDecoder.hpp`、`StreamingDecoder.hpp/.cpp`，对照缓存实现统一返回值与未覆盖声道的表述，补充准备等待、借用视图与容量契约。流式策略当前确为占位：没有文件探测、定位、预读或状态保存，所有读取返回零；创建对象仍分配并复制共享句柄。注释明确这些现状，不实现新的流式功能，也不把非空对象或正常返回当成可播放证明。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/IDecoder.hpp` | 26 | 21 | 55.32% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 21 | 27 | 43.75% |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 16 | 28 | 36.36% |

三个文件 clang-format；25 个工作区 C++/头文件去注释及空白文本一致性通过；ICE diff 检查通过。指定 clang64/fish 下重建受影响引擎、示例及实时测试成功，CTest 1/1 通过（1.31 秒）。测试不覆盖流式播放，也不能证明尚未实现的流式功能完成。

全量逐文件复扫继续覆盖 1013 个有效自维护文件与 5 个空文件，排除口径不变；ICE 91/92 比例达标，唯一不足为实时测试（325/1047 行，23.69%）；全项目不足 686/1013，其中主仓 685/921。完整表及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量复核、测试文件比例和既有主项目构建门槛仍未完成；未提交、未推送。

## ICE 缓存解码质量复核（2026-09-08）

完整对照 `CachyDecoder.hpp/.cpp`，替换逐句复述的旧注释，补充后台任务捕获与取消边界、实例格式、容量预估、read 返回约束、call_once 的唯一结果提取、std::exception 失败缓存、声道映射及 span 借用契约。修正 decode 返回值说明：零目标声道仍可返回非零切片长度，不能将返回值当成所有目标声道已写入的证明。未改变业务表达式、异常策略或控制流程。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/CachyDecoder.hpp` | 43 | 41 | 51.19% |
| `src/ice/manage/dec/CachyDecoder.cpp` | 66 | 126 | 34.38% |

既有行为限制仍保留：非空工厂为前置条件；整文件缓存没有应用层内存上限；创建强制 seek(0)；read 返回零不区分 EOF 与失败；多声道裁剪不是混音；首次 get_data 可阻塞；origin 可能扩容且不保活解码器。历史异常和原始分配仍未按注释任务擅自替换，不能据此声称全部行为规范通过。

两文件 clang-format，22 个工作区 C++/头文件去注释和空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建当前 ICE 源码、示例及测试成功，实时测试 1/1 通过（1.34 秒）；该测试不覆盖缓存文件解码及失败资源，本批不作专项运行通过声明。

全量复扫保持 1013 个有效自维护文件、5 个空文件及既有排除口径；ICE 91/92 数值达标，唯一不足仍为实时测试（325 注释行、1047 代码行、23.69%）。全项目不足 686/1013，主仓 685/921；最新逐文件统计及其他不足清单为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量质量审计和既有构建门槛仍未完成，未提交、未推送。

## ICE 线程池与分配跟踪质量复核（2026-09-08）

完整阅读 `ThreadPool.hpp`、空实现编译入口 `ThreadPool.cpp`、`AllocationTracker.hpp`，并核对缓存解码的任务提交与示例包含位置。线程池原有高比例注释大量逐句复述语法且引用错误的 stop_/tasks_ 成员名，现重写为中文 Doxygen 和实现附近的队列协议、所有权、停止排空及并发约束。分配跟踪工具补充统计扰动、全进程入口范围、原子读取/复位以及历史异常和输出接口边界。未改变运行逻辑。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/thread/ThreadPool.hpp` | 54 | 97 | 35.76% |
| `include/ice/tool/AllocationTracker.hpp` | 35 | 40 | 46.67% |

现有行为限制：线程数下限实际为四；队列无界且不支持取消；析构先排空任务再 join，没有截止时间；不能从本池任务销毁池，也不能依赖 stop 检查使析构与提交并发安全；同池嵌套 future 等待可能耗尽工作线程；普通任务异常没有 packaged_task 结果通道。分配工具仍包含历史 throw 和 cout，原子计数自身也会影响被测时延。这些未按注释任务擅自改为另一错误策略或调度实现，不能宣称通过所有行为规范检查。

验证：两文件 clang-format；20 个工作区 C++/头文件去注释和空白文本一致性通过；ICE `git diff --check` 通过。指定 clang64/fish 环境重新编译受影响引擎与示例成功，实时测试 1/1 通过（1.30 秒）；该测试不覆盖线程池任务失败/停机或该全局分配跟踪器，构建成功不代替这些运行验证。

全量逐文件复扫口径不变：1013 个有效自维护文件、5 个空文件，ICE 91/92 数值达标；唯一不足为实时测试的 325 行注释、1047 行代码、23.69%。全项目不足 686/1013，其中主仓 685/921，其他不足清单及完整明细已刷新至 `build/test_output/comment-audit/under-30.csv`、`files.csv`。仍需继续测试文件与其余质量复核，既有主项目构建/专项测试兼容门槛不变，目标未完成，未提交、未推送。
