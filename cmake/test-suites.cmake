# 测试入口按模块合并，但 CTest 用例仍逐项启动独立进程。 此文件只管理测试目标与分派，不改变各模块原有的断言和资源输入。
# 测试对象库保留旧目标名，使模块内已有的 include、宏和链接声明继续生效。 最终程序目标集中生成，避免新增测试时无意突破二进制数量上限。
# 此文件不处理测试资源路径，资源参数仍由各模块逐项传给 CTest。 配置隔离入口留在根工程，套件分派只决定运行哪个原始测试。

# 按依赖边界安排二进制；含测试替身的目标不能和真实 UI 实现同进程。
function(mmm_test_suite_name MODULE_NAME TARGET_NAME RESULT_VARIABLE)
  # Markdown 渲染用例替换了浏览器函数，与完整 UI 库同进程会重定义该符号。
  if(TARGET_NAME STREQUAL "MarkdownRendererTest")
    set(SUITE_GROUP Foundation)
    # 替身函数只在 Foundation 中存在，不与 UI 生产实现同链。
  elseif(TARGET_NAME STREQUAL "WelcomeViewTest")
    # WelcomeView 引入 Game，单独链接以避免扩大其余 UI 测试的静态库闭包。
    set(SUITE_GROUP UI_App)
    # 该例外占用第十个程序名额，新增分组须先检查数量上限。
  elseif(MODULE_NAME STREQUAL "Common" OR MODULE_NAME STREQUAL "Graphic")
    # 基础图形用例共享较轻的依赖闭包，不需要单独链接二进制。
    set(SUITE_GROUP Foundation)
    # 基础测试保持较轻的业务依赖闭包。
  elseif(
    MODULE_NAME STREQUAL "Network"
    OR MODULE_NAME STREQUAL "Collaboration"
    OR MODULE_NAME STREQUAL "CollaborationServer")
    # 联机客户端和服务端的测试共享一个程序，但仍逐条在独立进程执行。
    set(SUITE_GROUP Network)
    # 网络测试的端口与全局状态仍通过进程边界隔离。
  else()
    # 其他模块沿用目录分组，方便定位链接冲突和测试失败。
    set(SUITE_GROUP "${MODULE_NAME}")
    # 分组名取模块参数，源码路径调整不会改变 CTest 分派。
  endif()
  # 向调用方返回目标名，避免 helper 的局部变量泄漏到模块作用域。
  set(${RESULT_VARIABLE}
      "MMMTestSuite_${SUITE_GROUP}"
      PARENT_SCOPE)
endfunction()

# 旧测试的 main 被包装为唯一入口，额外源码和目标级编译设置保持原样。
function(mmm_add_test_executable MODULE_NAME TARGET_NAME)
  # 第一个源文件必须是含 main 的测试入口；其余源文件不进行符号改名。
  set(TEST_SOURCES ${ARGN})
  # 入口源文件不再作为普通源文件重复编译，否则 main 会再次出现。
  list(POP_FRONT TEST_SOURCES PRIMARY_SOURCE)
  if(NOT IS_ABSOLUTE "${PRIMARY_SOURCE}")
    # 模块源码使用相对路径登记，生成的包装文件必须引用确定的绝对路径。
    set(PRIMARY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${PRIMARY_SOURCE}")
  endif()
  # 包装文件由构建树生成，源码树中的测试文件不需机械重写 main。
  set(WRAPPER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_TestEntry.cpp")
  # 宏只覆盖被包含测试文件中的 main，不改动该文件的其它外部符号。
  set(WRAPPER_CONTENT
      "#include \"${CMAKE_SOURCE_DIR}/tests/TestCaseEntry.h\"\n#define main mmmEntry_${TARGET_NAME}\n#include \"${PRIMARY_SOURCE}\"\n#undef main\nint mmmRun_${TARGET_NAME}(int argc, char** argv) { return MMM::Test::invokeTestMain(&mmmEntry_${TARGET_NAME}, argc, argv); }\n"
  )
  file(
    GENERATE
    OUTPUT "${WRAPPER_SOURCE}"
    CONTENT "${WRAPPER_CONTENT}")
  # file(GENERATE) 在配置完成后落盘，避免多配置生成器重写源文件。 OBJECT 库不会生成额外可执行文件，仍允许模块独立配置编译与链接需求。
  if(BUILD_TESTING)
    add_library(${TARGET_NAME} OBJECT "${WRAPPER_SOURCE}" ${TEST_SOURCES})
  else()
    # 禁用测试时保留可显式构建的目标，不纳入默认构建。
    add_library(${TARGET_NAME} OBJECT EXCLUDE_FROM_ALL "${WRAPPER_SOURCE}"
                                      ${TEST_SOURCES})
  endif()
  mmm_test_suite_name("${MODULE_NAME}" "${TARGET_NAME}" SUITE_TARGET)
  # 所属模块用于恢复旧配置根目录，不与套件分组混用。 将映射留在目标属性上，CTest 登记无需重复维护分组名单。
  set_property(TARGET ${TARGET_NAME} PROPERTY MMM_TEST_SUITE "${SUITE_TARGET}")
  set_property(TARGET ${TARGET_NAME} PROPERTY MMM_TEST_MODULE "${MODULE_NAME}")
  set_property(GLOBAL APPEND PROPERTY MMM_TEST_SUITE_NAMES "${SUITE_TARGET}")
  # 每个套件的对象目标按模块声明顺序加入分派器，保持构建可重复。
  set_property(GLOBAL APPEND PROPERTY "MMM_TEST_CASES_${SUITE_TARGET}"
                                      "${TARGET_NAME}")
endfunction()

# 保留原 CTest 名称、参数及每个用例的独立配置根。
function(mmm_register_test_case)
  # 仅登记真正的 C++ 用例；脚本型 CTest 仍直接使用 add_test。
  cmake_parse_arguments(PARSE_ARGV 0 CASE "" "NAME;COMMAND;CONFIG_ROOT" "")
  if(NOT CASE_NAME OR NOT TARGET "${CASE_COMMAND}")
    message(FATAL_ERROR "测试用例必须提供名称和已声明的测试目标")
  endif()
  # 无效目标在配置时拒绝，避免出现运行期才暴露的空分派入口。
  get_target_property(SUITE_TARGET "${CASE_COMMAND}" MMM_TEST_SUITE)
  # 配置目录继续以旧测试目标为粒度，而非共享套件级目录。
  get_target_property(CASE_MODULE "${CASE_COMMAND}" MMM_TEST_MODULE)
  if(CASE_CONFIG_ROOT)
    # 少数配置隔离测试显式指定根目录，不能被默认规则覆盖。
    set(CONFIG_ROOT "${CASE_CONFIG_ROOT}")
  else()
    # 未覆盖的用例沿用原来的模块与目标双层目录。
    set(CONFIG_ROOT
        "${CMAKE_BINARY_DIR}/test_output/config/${CASE_MODULE}/${CASE_COMMAND}")
  endif()
  # 传参保持原顺序，资源目录或输出参数由原测试自身解析。 每项 CTest 都启动新的套件进程，避免测试间共享单例和 UI 上下文。
  add_test(
    NAME "${CASE_NAME}"
    COMMAND
      "${CMAKE_COMMAND}" -E env "MMM_CONFIG_ROOT=${CONFIG_ROOT}"
      "$<TARGET_FILE:${SUITE_TARGET}>" "${CASE_COMMAND}"
      ${CASE_UNPARSED_ARGUMENTS})
endfunction()

# 模块注册结束后集中生成套件入口，并在配置期约束程序数量。
function(mmm_finalize_test_suites)
  # 全部子目录创建目标后才能知道最终的模块分组和目标数量。
  get_property(TEST_SUITE_NAMES GLOBAL PROPERTY MMM_TEST_SUITE_NAMES)
  list(REMOVE_DUPLICATES TEST_SUITE_NAMES)
  # CTest 条目不受此限制，用户仍可逐项选择和定位失败。
  list(LENGTH TEST_SUITE_NAMES TEST_SUITE_COUNT)
  if(TEST_SUITE_COUNT GREATER 10)
    message(FATAL_ERROR "测试套件可执行文件不得超过 10 个")
  endif()
  # 约束只统计目标，不把细分 CTest 用例误认为独立程序。
  foreach(SUITE_TARGET IN LISTS TEST_SUITE_NAMES)
    # 分派表由已登记目标自动生成，避免与 CTest 名称列表手工同步。
    get_property(CASE_TARGETS GLOBAL PROPERTY "MMM_TEST_CASES_${SUITE_TARGET}")
    # <string_view> 仅供入口选择器使用，不引入测试业务头文件。
    set(SUITE_SOURCE "#include <string_view>\n")
    foreach(CASE_TARGET IN LISTS CASE_TARGETS)
      # 所有入口都具有统一签名，参数列表由公共包装器传给原始 main。
      string(APPEND SUITE_SOURCE "int mmmRun_${CASE_TARGET}(int, char**);\n")
    endforeach()
    # argc 和 argv 左移一位，让原测试看见自己的旧目标名作为 argv[0]。
    string(
      APPEND
      SUITE_SOURCE
      "int main(int argc, char** argv) {\n  if (argc < 2) return 2;\n  const std::string_view selected(argv[1]);\n"
    )
    foreach(CASE_TARGET IN LISTS CASE_TARGETS)
      # 选择器使用旧目标名，保留已有 CTest 名称与重命名空间。
      string(
        APPEND
        SUITE_SOURCE
        "  if (selected == \"${CASE_TARGET}\") return mmmRun_${CASE_TARGET}(argc - 1, argv + 1);\n"
      )
    endforeach()
    string(APPEND SUITE_SOURCE "  return 2;\n}\n")
    # 文件位于构建树，源码树不会出现自动生成的入口文件。 未知选择器返回非零，使配置错误在 CTest 中表现为失败。
    set(SUITE_SOURCE_PATH "${CMAKE_BINARY_DIR}/generated/${SUITE_TARGET}.cpp")
    file(
      GENERATE
      OUTPUT "${SUITE_SOURCE_PATH}"
      CONTENT "${SUITE_SOURCE}")
    # 所有标准构建配置共用目标声明，由根工程决定输出目录。
    if(BUILD_TESTING)
      # 显式开启测试时套件参与普通构建和 CTest 运行。
      add_executable(${SUITE_TARGET} "${SUITE_SOURCE_PATH}")
    else()
      # 关闭测试时不增加默认构建开销，但目标仍可显式构建。
      add_executable(${SUITE_TARGET} EXCLUDE_FROM_ALL "${SUITE_SOURCE_PATH}")
    endif()
    # 链接对象库时保留各测试原有的传递依赖，不手工合并链接清单。 对象目标继续提供原用例的 include、定义和链接需求。
    mmm_set_test_target(Suites ${SUITE_TARGET})
    target_link_libraries(${SUITE_TARGET} PRIVATE ${CASE_TARGETS})
  endforeach()
endfunction()
