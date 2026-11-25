include_guard(GLOBAL)

# 本文件只负责解析预编译包目录和库文件路径，不创建任何具体目标。 每个库的目标 必须在自己的 Findxxx.cmake 中显式声明。

# 初始化主项目预编译库目录选择变量。头文件集中放在 <root>/headers/<包>/include， 二进制集中放在
# <root>/binaries/<platform>/<包> 下，便于继续扩展 macOS 等平台。 静态偏好布局为：
# <root>/binaries/<platform>/<包>/libs/<arch>/<toolchain>/<compiler-tag>/<config>
# 动态偏好布局为：
# <root>/binaries/<platform>/<包>/libs/<arch>/<toolchain>/<compiler-tag>/shared/<config>
# <root>/binaries/<platform>/<包>/bin/<arch>/<toolchain>/<compiler-tag>/shared/<config>
# 生成自动推导编译器标签的回退候选。当前主版本优先，后续依次尝试更旧主版本， 用于兼容 clang22 使用 clang19、gcc15 使用 gcc14
# 这类预编译库复用场景。

# * 输入是工具链前缀和已解析的主版本；调用者不传完整编译器版本字符串。
# * 最具体标签先进入候选，因此新增当前版本归档无需移除较旧目录。
# * 较旧标签只代表搜索顺序，实际可复用性仍取决于制品发布时的 ABI 约定。
# * 非数字输入只使用无版本前缀，不能交给 CMake 算术表达式直接求值。
# * 空候选用前缀兜底，使没有主版本信息的工具链仍有确定搜索位置。
function(prebuilt_compiler_tag_fallback_candidates out_var tag_prefix
         compiler_major)
  set(_candidates "")
  # * 用整串匹配拒绝带后缀版本，避免误将小数版本解释成 CMake 算式。
  # * 主版本从当前向下递减，不能尝试比本机工具链更新的归档。
  if(compiler_major MATCHES "^[0-9]+$")
    set(_candidate_major "${compiler_major}")
    # * 停在零之前，避免出现在发行布局中无意义的 gcc0 或 clang0。
    # * 每轮只生成同一前缀的标签，GCC 和 Clang 的归档不会相互回退。
    while(_candidate_major GREATER 0)
      list(APPEND _candidates "${tag_prefix}${_candidate_major}")
      math(EXPR _candidate_major "${_candidate_major} - 1")
    endwhile()
  endif()
  # * 兼容不能报告编译器主版本的调用环境。
  # * 兜底保留一个明确候选，诊断时仍可追溯搜索路径。
  if(_candidates STREQUAL "")
    set(_candidates "${tag_prefix}")
  endif()
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# * 每个 Find 模块都可先调用本函数，本轮配置用户已提供的覆盖取值继续有效。
# * 先缓存链接、根目录及目标平台，之后所有包看到同一布局。
# * 交叉编译用 CMake 目标平台标识判断，不能借用执行配置程序的宿主平台。
# * 公开缓存变量保存用户选择，内部候选列表随当前选择重新生成。
# * 本函数只验证平台目录；头文件和库的逐包诊断由后续入口给出。
function(prebuilt_init default_root)
  if(NOT DEFINED PROJECT_LINKAGE OR PROJECT_LINKAGE STREQUAL "")
    set(PROJECT_LINKAGE
        "static"
        CACHE STRING "Third-party dependency linkage preference.")
  endif()
  # * 大小写归一化后各包可直接根据枚举字符串分支。
  # * 非法值在公共入口拒绝，防止下载或链接走到另一种文件布局才失败。
  string(TOLOWER "${PROJECT_LINKAGE}" PROJECT_LINKAGE)
  if(NOT PROJECT_LINKAGE MATCHES "^(static|shared)$")
    message(FATAL_ERROR "PROJECT_LINKAGE 只能是 static 或 shared。")
  endif()
  # * 缓存公开值写回标准拼写，后续工程配置界面可看到实际生效的模式。
  # * FORCE 只用于归一化已接受的枚举，不能用来换掉用户选择的 shared。
  set(PROJECT_LINKAGE
      "${PROJECT_LINKAGE}"
      CACHE STRING "Third-party dependency linkage preference." FORCE)

  # * 显式根优先，可对独立暂存目录做离线包验收。
  # * 环境给出空根与未指定相同，从脚本所属仓库解析包。
  if(NOT DEFINED PROJECT_PREBUILT_ROOT OR PROJECT_PREBUILT_ROOT STREQUAL "")
    set(PROJECT_PREBUILT_ROOT
        "${default_root}"
        CACHE PATH "Root directory containing project prebuilt packages.")
  endif()

  # 平台目录名固定为仓库约定，避免直接暴露 CMake 系统名。
  if(NOT DEFINED PROJECT_PREBUILT_PLATFORM OR PROJECT_PREBUILT_PLATFORM
                                              STREQUAL "")
    # * 探测结果是预编译包目录名，保留项目的三平台目录约定。
    # * Windows 交叉工具链和原生构建都进入这条分支。
    if(WIN32)
      set(_platform "windows")
    elseif(APPLE)
      set(_platform "macos")
    else()
      set(_platform "linux")
    endif()
    # * 自动平台推导完成时才写缓存，显式目标平台保留。
    # * 覆盖应与当前工具链目标一致，以免构建时取得无法加载的库。
    set(PROJECT_PREBUILT_PLATFORM
        "${_platform}"
        CACHE STRING "Prebuilt platform directory name.")
  endif()

  # macOS 默认使用 Apple Silicon 预编译布局；其它平台继续按指针宽度选择。 交叉构建或 Intel macOS 构建可显式设置
  # PROJECT_PREBUILT_ARCH 覆盖默认值。
  if(NOT DEFINED PROJECT_PREBUILT_ARCH OR PROJECT_PREBUILT_ARCH STREQUAL "")
    # * Apple 默认与现存 arm64 制品匹配；Intel 平台须传显式构建覆盖。
    # * 指针宽度只区分项目已维护的 x86 两种默认布局，其他架构不能凭宽度推断。
    if(APPLE)
      set(_arch "arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_arch "x86_64")
    else()
      set(_arch "x86")
    endif()
    # * 留下架构缓存供预编译配置和诊断使用。
    # * 不用宿主处理器名称决定交叉构建的预编译架构。
    set(PROJECT_PREBUILT_ARCH
        "${_arch}"
        CACHE STRING "Prebuilt architecture directory name.")
  endif()

  # 工具链目录用于区分 MSVC、MinGW 等二进制格式。
  if(NOT DEFINED PROJECT_PREBUILT_TOOLCHAIN OR PROJECT_PREBUILT_TOOLCHAIN
                                               STREQUAL "")
    # * MSVC 和 MinGW 都产出 Windows 文件，但导入库和运行时契约不同。
    # * Linux/macOS 的 Clang 和 GNU 候选各进自己工具链目录，标签回退不会越界。
    if(MSVC)
      set(_toolchain "msvc")
    elseif(MINGW)
      set(_toolchain "mingw")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      set(_toolchain "gcc")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(_toolchain "clang")
    else()
      string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _toolchain)
    endif()
    # * 同一目标平台的不同工具链独占目录，不能通过调整编译器标签跨工具链回退。
    # * 缓存标签显式供打包构建复用，不依赖系统编译器输出拼写。
    set(PROJECT_PREBUILT_TOOLCHAIN
        "${_toolchain}"
        CACHE STRING "Prebuilt toolchain directory name.")
  endif()

  # 编译器标签用于区分同一工具链下的 ABI 或运行库版本。
  if(NOT DEFINED PROJECT_PREBUILT_COMPILER_TAG OR PROJECT_PREBUILT_COMPILER_TAG
                                                  STREQUAL "")
    # * MSVC 标签跟随本仓分发的 VS 工具链代际名称，不取 cl.exe 的内部版本数字。
    # * MinGW Clang 和 GCC 的运行时布局不同；不能因都使用 UCRT 就混用 C++ 库。
    if(MSVC)
      set(_compiler_tag "2026")
    elseif(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # Windows MinGW Clang 使用 CLANG64 布局；其 C runtime 是 UCRT，但 C++ runtime 是
      # libc++。
      set(_compiler_tag "clang64")
      # * UCRT64 是当前 MinGW GCC 的默认分发布局。
      # * 另有固定 GCC 变体时，由调用方设置精确标签，无需推断系统安装目录。
    elseif(MINGW)
      set(_compiler_tag "ucrt64")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # Linux GCC 预编译库按主版本区分 ABI 和 libstdc++ 版本，例如 gcc14。
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major)
        set(_compiler_tag "gcc${_compiler_major}")
        prebuilt_compiler_tag_fallback_candidates(_compiler_tag_candidates
                                                  "gcc" "${_compiler_major}")
      else()
        set(_compiler_tag "gcc")
        set(_compiler_tag_candidates "${_compiler_tag}")
      endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # Linux Clang 预编译库按主版本区分前端与标准库组合，例如 clang19。
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major)
        set(_compiler_tag "clang${_compiler_major}")
        prebuilt_compiler_tag_fallback_candidates(_compiler_tag_candidates
                                                  "clang" "${_compiler_major}")
      else()
        set(_compiler_tag "clang")
        set(_compiler_tag_candidates "${_compiler_tag}")
      endif()
    else()
      string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _compiler_tag)
      set(_compiler_tag_candidates "${_compiler_tag}")
    endif()
    # * MSVC、MinGW 和未识别编译器没有主版本候选，明确固定一个目录。
    # * 自动推导路径缓存首选标签，整个降级序列另由内部变量保存。
    if(NOT DEFINED _compiler_tag_candidates)
      set(_compiler_tag_candidates "${_compiler_tag}")
    endif()
    # * 使用对用户可见的首选标签，而非把候选组合写进目录变量。
    # * 修改该值可交换 ABI 严格布局；旧版本尝试由候选列表单独处理。
    set(PROJECT_PREBUILT_COMPILER_TAG
        "${_compiler_tag}"
        CACHE STRING "Prebuilt compiler/runtime directory name.")
    # * 用户显式选择非默认标签，例如特定 MinGW 线程模型时，只尊重该标签。
    # * 缓存里的值也可能是上次自动推导所得；若恰好等于当前自然标签则重建降级序列。
    # * 这使重新进入同一构建树时，后续 Find 模块仍能找到已有旧版本归档。
  else()
    set(_compiler_tag_candidates "${PROJECT_PREBUILT_COMPILER_TAG}")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major AND PROJECT_PREBUILT_COMPILER_TAG STREQUAL
                             "gcc${_compiler_major}")
        prebuilt_compiler_tag_fallback_candidates(_compiler_tag_candidates
                                                  "gcc" "${_compiler_major}")
      endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major AND PROJECT_PREBUILT_COMPILER_TAG STREQUAL
                             "clang${_compiler_major}")
        prebuilt_compiler_tag_fallback_candidates(_compiler_tag_candidates
                                                  "clang" "${_compiler_major}")
      endif()
    endif()
  endif()
  # * 缓存内部列表供作用域互不相同的各 Find 模块读取。
  # * 用 FORCE 刷新派生值，避免换编译器组合后继续沿用上一次标签候选。
  set(PROJECT_PREBUILT_COMPILER_TAG_CANDIDATES
      "${_compiler_tag_candidates}"
      CACHE INTERNAL "Prebuilt compiler/runtime directory fallback names."
            FORCE)

  # * 先只验证平台组件，缺包错误可以在各 Find 模块带上包名报告。
  # * 预编译模式不能静默退回源码，防止 CI 因缺依赖而改变发布制品来源。
  set(_platform_root
      "${PROJECT_PREBUILT_ROOT}/binaries/${PROJECT_PREBUILT_PLATFORM}")
  if(NOT IS_DIRECTORY "${_platform_root}")
    message(
      FATAL_ERROR
        "SOURCES_BUILD=OFF 需要预编译库目录：${_platform_root}。请提供预编译库，或显式开启 SOURCES_BUILD。"
    )
  endif()
endfunction()

# 获取当前编译器标签的回退候选列表。候选顺序从当前主版本递减，确保 clang22 可自动使用 clang19 等较旧预编译库。

# * 优先用本轮派生出的有序列表。
# * 调用方只设置首选而未生成内部缓存时，仍可按单个精确标签工作。
# * 返回值保留候选优先级；下游不做字典排序。
function(prebuilt_compiler_tag_candidates out_var)
  if(DEFINED PROJECT_PREBUILT_COMPILER_TAG_CANDIDATES
     AND NOT PROJECT_PREBUILT_COMPILER_TAG_CANDIDATES STREQUAL "")
    set(_candidates ${PROJECT_PREBUILT_COMPILER_TAG_CANDIDATES})
  elseif(DEFINED PROJECT_PREBUILT_COMPILER_TAG)
    set(_candidates "${PROJECT_PREBUILT_COMPILER_TAG}")
  else()
    set(_candidates "")
  endif()
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# 获取指定包的根目录。

# * 这里只接受指定平台包根，不查找 host 的安装路径。
# * 具体库名不进入包目录解析，允许一个包携带多个相互配套的归档。
# * 缺根是交付配置错误，不能等链接器碰到旧版系统库才发现。
function(prebuilt_package_root out_var package)
  set(_root
      "${PROJECT_PREBUILT_ROOT}/binaries/${PROJECT_PREBUILT_PLATFORM}/${package}"
  )
  # * 包根必须为目录，误把归档文件放到包名位置同样失败。
  # * 若正在配置纯头文件包，调用其头目录入口，不经此处要求空二进制包。
  if(NOT IS_DIRECTORY "${_root}")
    message(FATAL_ERROR "缺少预编译包目录：${_root}")
  endif()
  set(${out_var}
      "${_root}"
      PARENT_SCOPE)
endfunction()

# 获取指定包的 include 目录。头文件统一收拢到 headers 根目录下，避免和平台二进制目录混放。

# * 公共头与库一起发布，但头文件与 Debug 和发布配置无关。
# * 纯头包也通过本入口验证，不要求该包存在平台二进制子目录。
# * 将绝对路径返回调用者，再由具体 target 决定公开或系统包含。
function(prebuilt_include_dir out_var package)
  set(_include_dir "${PROJECT_PREBUILT_ROOT}/headers/${package}/include")
  # * 校验只覆盖目录存在性，具体必要公共头的完整性由编译及制品检查保证。
  # * 即使系统存在兼容的同名包，也不利用该事实掩盖发布缺漏。
  if(NOT IS_DIRECTORY "${_include_dir}")
    message(FATAL_ERROR "缺少预编译头文件目录：${_include_dir}")
  endif()
  set(${out_var}
      "${_include_dir}"
      PARENT_SCOPE)
endfunction()

# 获取当前生成器需要的构建配置列表。

# * 多配置生成器须一次校验所有已启用配置，因为用户可在构建时再选择。
# * 单配置生成器只验证当前选中的构建类型，避免无关平台产物阻挡配置。
# * 没有显式配置时挑发布族作缺省，目标属性仍需由包暴露无配置映射。
function(prebuilt_target_configs out_var)
  # * 直接尊重生成器配置列表，不擅自追加仓库不准备提供的构建模式。
  # * 映射到真实文件目录发生在之后，可让不同消费配置按发布政策共享同一库。
  if(CMAKE_CONFIGURATION_TYPES)
    set(_configs ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    set(_configs "${CMAKE_BUILD_TYPE}")
  else()
    set(_configs "Release")
  endif()
  set(${out_var}
      ${_configs}
      PARENT_SCOPE)
endfunction()

# 将 CMake 配置名映射到预编译目录候选名。

# * 候选顺序保留发布组织的目录拼写，也兼容历史小写目录。
# * Debug 只在自己的大小写集合内搜索，不能因缺文件借用发布版。
# * Config 代表消费端的构建类型，目录候选代表提供方真实归档组织。
# * 返回值可能含多个名字，调用方只能按顺序选择第一个存在的目录。
function(prebuilt_config_dir_candidates out_var config)
  string(TOLOWER "${config}" _config_lower)
  # * Debug 归档可能使用不同运行库和断言模式，不能以发布配置兜底。
  # * 大小写别名用于兼容既有目录拼写，链接语义仍同一配置。
  if(_config_lower STREQUAL "debug")
    set(_candidates "Debug" "debug")
    # * 空配置和 CMake 的 NOCONFIG 归入发布族。
    # * 先取保留符号的 RelWithDebInfo；缺失才借用 Release。
  elseif(config STREQUAL "" OR _config_lower STREQUAL "noconfig")
    set(_candidates "RelWithDebInfo" "relwithdebinfo" "Release" "release")
  elseif(
    # * 发布族在布局中共享归档，不因消费端 MinSizeRel 强制生成独立第三方库。
    # * 大小写目录的先后在区分大小写的文件系统上可确定。
    _config_lower STREQUAL "release"
    OR _config_lower STREQUAL "relwithdebinfo"
    OR _config_lower STREQUAL "minsizerel")
    set(_candidates "RelWithDebInfo" "relwithdebinfo" "Release" "release")
  else()
    # * 自定义配置先找同名，以便精确安装定制归档。
    # * 未提供同名才走发布候选；不从 Debug 抽取对象。
    set(_candidates "${config}" "${_config_lower}" "RelWithDebInfo"
                    "relwithdebinfo" "Release" "release")
  endif()
  # * 原名与小写名相同时去重，保留初次出现顺序。
  # * 诊断和目录探测都只需报告同一路径一次。
  list(REMOVE_DUPLICATES _candidates)
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# 在编译器标签候选和配置候选中选择第一个实际存在的目录。

# * 这是布局共用的选择入口，链接库、运行库及 PDB 各传自己的基目录和后缀。
# * 每个编译器候选内部先尝试配置候选，工具链兼容优先于换用更老标签的对应配置。
# * 此处查找目录；各 Find 模块还须验证实际库或运行库文件名。
# * 返回的附带列表记录找到之前所有尝试路径，供不同调用方生成有范围的错误。
function(prebuilt_select_config_dir out_var tagged_base_dir tagged_suffix
         config)
  prebuilt_compiler_tag_candidates(_compiler_tag_candidates)
  prebuilt_config_dir_candidates(_config_candidates "${config}")
  set(_searched_dirs "")
  # * 外层遍历使当前编译器标签下的发布回退优先于旧编译器标签。
  # * 空标签保留传统无版本布局，避免构造多余路径层。
  foreach(_compiler_tag IN LISTS _compiler_tag_candidates)
    if(_compiler_tag STREQUAL "")
      set(_config_base_dir "${tagged_base_dir}${tagged_suffix}")
      # * 标签后缀按共享布局加在标签与配置之间。
      # * 静态库调用传空后缀，既有静态目录层次不会改变。
    else()
      set(_config_base_dir
          "${tagged_base_dir}/${_compiler_tag}${tagged_suffix}")
    endif()
    # * 多配置生成器的每次解析仅追加当前消费配置的实际路径。
    # * 即使当前路径不存在也记录，出错时需要说明具体缺哪一个目录。
    foreach(_config_dir_name IN LISTS _config_candidates)
      set(_candidate_dir "${_config_base_dir}/${_config_dir_name}")
      list(APPEND _searched_dirs "${_candidate_dir}")
      # * 优先级确定后立即返回；不以文件系统遍历顺序覆盖先前选择。
      # * 调用层收到同一个实际目录再寻找库名，不对配置目录重新排序。
      if(IS_DIRECTORY "${_candidate_dir}")
        set(${out_var}
            "${_candidate_dir}"
            PARENT_SCOPE)
        set(${out_var}_SEARCHED_DIRS
            ${_searched_dirs}
            PARENT_SCOPE)
        return()
      endif()
    endforeach()
  endforeach()

  set(${out_var}
      ""
      PARENT_SCOPE)
  set(${out_var}_SEARCHED_DIRS
      ${_searched_dirs}
      PARENT_SCOPE)
endfunction()

# 获取指定包在指定配置下的库目录。

# * 将目标架构与工具链固化在编译器标签前，回退只能改变以后层级。
# * 共享布局的 libs 放导入库或链接对象，运行时文件仍由独立入口解析。
# * 对链接依赖没有静默跳过，缺目录会在配置结束前报告。
function(prebuilt_library_dir out_var package config)
  prebuilt_package_root(_package_root "${package}")
  set(_library_tagged_base_dir
      "${_package_root}/libs/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}"
  )
  set(_library_tagged_suffix "")
  # * shared 目录独立于静态归档，防止同时存在时选错产物。
  # * 后缀位置由通用选择器处理，配置名始终在最后一层。
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(_library_tagged_suffix "/shared")
  endif()

  prebuilt_select_config_dir(_library_dir "${_library_tagged_base_dir}"
                             "${_library_tagged_suffix}" "${config}")
  # * 只返回已存在的配置目录，具体库名仍由消费方给出。
  # * 失败路径需要完整记录标签候选及实际试过的位置。
  if(_library_dir)
    set(${out_var}
        "${_library_dir}"
        PARENT_SCOPE)
    return()
  endif()

  message(
    FATAL_ERROR
      "缺少 ${package} 的 ${config} 预编译库目录。编译器标签候选：${PROJECT_PREBUILT_COMPILER_TAG_CANDIDATES}；已尝试：${_library_dir_SEARCHED_DIRS}"
  )
endfunction()

# 获取指定包在指定配置下的运行时目录。仅动态链接偏好需要此目录。

# * 仅动态加载文件使用 bin 目录，归档或导入库不从该目录查找。
# * 运行时同样限制架构和工具链，避免复制名称相同但无法加载的 DLL。
# * 没有运行时目录时直接失败，保证可执行文件不会以依赖不完整的状态交付。
function(prebuilt_runtime_dir out_var package config)
  prebuilt_package_root(_package_root "${package}")
  set(_runtime_tagged_base_dir
      "${_package_root}/bin/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}"
  )
  # * 始终明确共享模式后缀，此入口不负责为静态库寻找部署文件。
  # * 配置转换与链接库共用规则，避免配置名在两个入口得到不同拼写。
  prebuilt_select_config_dir(_runtime_dir "${_runtime_tagged_base_dir}"
                             "/shared" "${config}")
  # * 与对应运行时配置匹配后直接交给文件查找步骤。
  # * 查库名仍由包模块控制，不把目录内第一个文件当成有效运行时。
  if(_runtime_dir)
    set(${out_var}
        "${_runtime_dir}"
        PARENT_SCOPE)
    return()
  endif()

  message(
    FATAL_ERROR
      "缺少 ${package} 的 ${config} 动态运行时目录。编译器标签候选：${PROJECT_PREBUILT_COMPILER_TAG_CANDIDATES}；已尝试：${_runtime_dir_SEARCHED_DIRS}"
  )
endfunction()

# 获取指定包在指定配置下的 PDB 符号目录。符号文件不参与链接，缺失时返回空值。

# * 外置符号目录和导入库使用相同的配置及编译器候选规则。
# * 发布调试符号可能随平台采用其他形式，空结果不影响链接。
# * 调用者只有对支持外置 PDB 的工具链才需要进一步枚举文件。
function(prebuilt_symbol_dir out_var package config)
  prebuilt_package_root(_package_root "${package}")
  set(_symbol_tagged_base_dir
      "${_package_root}/symbols/${PROJECT_PREBUILT_ARCH}/${PROJECT_PREBUILT_TOOLCHAIN}"
  )
  set(_symbol_tagged_suffix "")
  # * 共享 DLL 和静态归档的调试文件不能相互替代。
  # * 遵守对应产物的静态或共享目录隔离。
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(_symbol_tagged_suffix "/shared")
  endif()

  prebuilt_select_config_dir(_symbol_dir "${_symbol_tagged_base_dir}"
                             "${_symbol_tagged_suffix}" "${config}")
  if(_symbol_dir)
    set(${out_var}
        "${_symbol_dir}"
        PARENT_SCOPE)
    return()
  endif()

  set(${out_var}
      ""
      PARENT_SCOPE)
endfunction()

# 查找指定包在当前配置下已经提供的 PDB 符号文件。

# * 无符号目录时不执行 glob，避免错误地读取宿主根目录。
# * 只返回当前选中符号目录的 .pdb，不把其他配置或版本的符号一起打包。
# * CONFIGURE_DEPENDS 让新增外置 PDB 时重新生成已有工程的安装图。
function(prebuilt_find_symbol_files out_var package config)
  prebuilt_symbol_dir(_symbol_dir "${package}" "${config}")
  if(_symbol_dir STREQUAL "")
    set(${out_var}
        ""
        PARENT_SCOPE)
    return()
  endif()

  # * 调试符号可有多个对象归档配套文件，因此返回列表。
  # * glob 只用于可选调试元数据，不参与判定库导入是否有效。
  file(GLOB _symbol_files CONFIGURE_DEPENDS "${_symbol_dir}/*.pdb")
  set(${out_var}
      ${_symbol_files}
      PARENT_SCOPE)
endfunction()

# 记录 shared 预编译包的运行时 DLL，供可执行文件构建后统一复制。

# * 仅 Windows 共享库需要此处的旁置 DLL 复制清单。
# * 从 Find 模块收集绝对路径，后处理无需重新计算工具链和配置。
# * 全局属性跨 CMake 子目录累积，重复发现同一个依赖不会增加拷贝项。
function(prebuilt_record_runtime_file runtime_file)
  if(NOT PROJECT_LINKAGE STREQUAL "shared"
     OR NOT WIN32
     OR runtime_file STREQUAL "")
    return()
  endif()

  # * 同一 DLL 可能同时被 UI 和全局基础库依赖。
  # * 添加后去重，保留首次解析到的路径而不依赖目标的构建先后。
  get_property(_runtime_files GLOBAL PROPERTY PROJECT_PREBUILT_RUNTIME_FILES)
  list(APPEND _runtime_files "${runtime_file}")
  list(REMOVE_DUPLICATES _runtime_files)
  set_property(GLOBAL PROPERTY PROJECT_PREBUILT_RUNTIME_FILES
                               "${_runtime_files}")
endfunction()

# 读取当前配置已解析出的 shared 运行时 DLL 清单。

# * 任意调用目标都可获取共享 DLL 已登记清单。
# * 空清单用于明确跳过 POST_BUILD，防止执行只有目的路径的复制命令。
# * 返回后不清空全局属性，因为多个程序可能需各自部署同一批依赖。
function(prebuilt_get_runtime_files out_var)
  get_property(_runtime_files GLOBAL PROPERTY PROJECT_PREBUILT_RUNTIME_FILES)
  if(_runtime_files)
    list(REMOVE_DUPLICATES _runtime_files)
  endif()
  set(${out_var}
      ${_runtime_files}
      PARENT_SCOPE)
endfunction()

# 将预编译 DLL 复制到指定可执行目标旁边，保证 bin 目录可直接运行和诊断依赖。

# * 部署绑定具体运行目标，将库放在该配置的实际可执行目录。
# * 复制只发生在构建阶段，配置检查不写入 bin 目录。
# * 静态与非 Windows 目标跳过注册，不为此入口产生空自定义命令。
function(prebuilt_copy_runtime_files target_name)
  if(NOT PROJECT_LINKAGE STREQUAL "shared" OR NOT WIN32)
    return()
  endif()

  # * 消费配置阶段已登记的清单，拷贝命令不在 build 时重新扫依赖根。
  # * 清单可用于每个目标多次注册部署，无需重新解析包。
  prebuilt_get_runtime_files(_runtime_files)
  if(NOT _runtime_files)
    return()
  endif()

  # * 生成器表达式在构建时解析目标目录，避免多配置共用写入位置。
  # * 文件内容相同时不覆盖，减少增量链接后的无效 I/O。
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_runtime_files}
            "$<TARGET_FILE_DIR:${target_name}>"
    COMMENT "Copying prebuilt runtime DLLs for ${target_name}")
endfunction()

# 在指定包和配置目录中查找库文件。

# * 将包名、消费配置和可能的产物名称分别交给布局解析器。
# * 调用者按本包发布命名传入候选列表，函数不推断任意上游名称。
# * 主输出始终是可供链接的文件路径；运行时与符号文件通过附加变量提供。
# * 不在这里定义 CMake 目标，以免不同接口误用通用链接属性。
function(prebuilt_find_library out_var package config)
  # shared 布局必须提供 DLL 运行时文件，缺失时配置阶段直接报错。

  # * 缺候选名表示包脚本配置错误，不能让查找起始于无通用名字的路径。
  # * 候选顺序沿用 Find 模块定义，匹配前已限定包和构建配置。
  if(ARGN)
    set(_prebuilt_library_names ${ARGN})
  else()
    message(FATAL_ERROR "prebuilt_find_library 缺少库候选名：${package}")
  endif()

  prebuilt_library_dir(_library_dir "${package}" "${config}")
  # * 禁止默认搜索路径，避免缺 LFS 制品时错误采用系统安装版本。
  # * 不跨包复用 CMake 搜索缓存，库位置每次随当前目录及候选计算。
  find_library(
    _prebuilt_library
    NAMES ${_prebuilt_library_names}
    PATHS "${_library_dir}"
    NO_DEFAULT_PATH NO_CACHE)
  # * 包目录存在并不能证明指定配置的归档齐全。
  # * 错误信息留下实际目录和库名候选，可区别路径缺失与发布改名。
  if(NOT _prebuilt_library)
    message(
      FATAL_ERROR
        "缺少 ${package} 的 ${config} 预编译库。搜索目录：${_library_dir}；候选名：${_prebuilt_library_names}"
    )
  endif()
  set(${out_var}
      "${_prebuilt_library}"
      PARENT_SCOPE)

  # * Windows 动态加载需要 DLL 本体，链接器的导入库通常不是同一个文件。
  # * 运行时查找限于库所在平台包对应配置，不从 PATH 提取同名 DLL。
  if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
    prebuilt_runtime_dir(_runtime_dir "${package}" "${config}")
    set(_runtime_names "")
    # * 优先找与归档候选对应的未带 ABI 后缀的文件名。
    # * 扩展名直接复用当前工具链设置，不在文件名规则中硬编码 Windows 后缀。
    foreach(_library_name IN LISTS _prebuilt_library_names)
      list(APPEND _runtime_names
           "${_library_name}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    endforeach()
    # * 精确 DLL 名优先，避免同目录存在版本后缀时偏离实际导入库名称。
    # * 清除跨配置缓存影响，使 Debug 和发布模式分别收集自身运行库。
    find_file(
      _prebuilt_runtime
      NAMES ${_runtime_names}
      PATHS "${_runtime_dir}"
      NO_DEFAULT_PATH NO_CACHE)
    # * 某些发布将 ABI 版本拼入 DLL 文件名，精确匹配失败后才尝试该形式。
    # * 对版本后缀只在当前运行时目录展开，不能跨配置捡旧库。
    if(NOT _prebuilt_runtime)
      set(_runtime_glob_matches "")
      foreach(_runtime_name IN LISTS _runtime_names)
        get_filename_component(_runtime_stem "${_runtime_name}" NAME_WE)
        file(GLOB _runtime_versioned_matches CONFIGURE_DEPENDS
             "${_runtime_dir}/${_runtime_stem}-*${CMAKE_SHARED_LIBRARY_SUFFIX}")
        list(APPEND _runtime_glob_matches ${_runtime_versioned_matches})
      endforeach()
      # * 排序取得稳定首项，使文件系统枚举顺序不影响复制结果。
      # * 这里不是动态推断最高兼容 ABI；应由包发布者限制同目录制品集合。
      if(_runtime_glob_matches)
        list(SORT _runtime_glob_matches)
        list(GET _runtime_glob_matches 0 _prebuilt_runtime)
      endif()
    endif()
    # * 即使导入库已找到，缺真实运行时依然阻止交付。
    # * 诊断会给出 DLL 目录而非导入库目录，便于补齐完整包。
    if(NOT _prebuilt_runtime)
      message(
        FATAL_ERROR
          "缺少 ${package} 的 ${config} 动态运行时文件。搜索目录：${_runtime_dir}；候选名：${_runtime_names}"
      )
    endif()
    set(${out_var}_RUNTIME
        "${_prebuilt_runtime}"
        PARENT_SCOPE)
    # * 同时记录应用部署所需路径；调用者不必再走一遍库名探测。
    # * 返回变量用于包定义目标属性，全局登记服务构建后拷贝。
    prebuilt_record_runtime_file("${_prebuilt_runtime}")
  endif()

  # * MSVC 单独发布 PDB；其他工具链的调试信息不从此文件列表获取。
  # * 仅 Debug 和 RelWithDebInfo 对应可提交符号的消费配置。
  # * 找到符号时连同链接路径返回，缺符号不改变已成立的库解析结果。
  if(MSVC)
    string(TOLOWER "${config}" _prebuilt_config_lower)
    if(_prebuilt_config_lower STREQUAL "debug" OR _prebuilt_config_lower
                                                  STREQUAL "relwithdebinfo")
      prebuilt_find_symbol_files(_prebuilt_symbol_files "${package}"
                                 "${config}")
      if(_prebuilt_symbol_files)
        set(${out_var}_PDBS
            ${_prebuilt_symbol_files}
            PARENT_SCOPE)
      endif()
    endif()
  endif()
endfunction()
