#!/usr/bin/env bash
# 输出 Linux 挂载的 MSVC、Windows SDK 与 Vulkan SDK 关键目录和文件探针。
# 默认仅诊断；--strict 将任何缺失项转换为非零退出，供 CI 前置门禁使用。
# 列表条目设置上限，避免大型 SDK 目录淹没 CI 日志或触发日志大小限制。
# 环境覆盖与 cross-msvc.cmake 使用相同变量名，便于对照实际配置来源。
# 脚本只读工具链挂载，不创建代理、不下载组件，也不修改构建缓存。
set -euo pipefail

# 打印命令行选项及所有受支持的工具链路径覆盖变量。
showUsage() {
    # 单引号 heredoc 禁止当前 shell 展开示例中的变量表达式。
    cat <<'EOF'
Usage: scripts/ci/cross/list-msvc-toolchain-layout.sh [options]

List key Windows MSVC cross toolchain directories before configure/build.

Options:
  --max-entries <count>  Max entries printed per directory. Default: 120
  --strict              Exit non-zero when a critical directory/file is missing
  -h, --help            Show this help

Environment overrides:
  WINDOWS_CROSS_ROOT    Default: /mnt/cross/windows
  MSVC_BASE             Default: ${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231
  WINSDK_BASE           Default: ${WINDOWS_CROSS_ROOT}/Program Files (x86)/Windows Kits/10
  WINSDK_VER            Default: 10.0.26100.0
  VULKAN_SDK            Default: ${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0
EOF
}

# 每个目录最多打印的直属条目数。
maxEntries=120
# 严格模式默认关闭，允许开发者先观察不完整挂载。
strict=0
# 所有目录和文件探针共享累计缺失计数。
missingCount=0

# 解析选项时立即消费对应参数，未知项统一返回用法错误。
while (( $# > 0 )); do
    case "$1" in
        --max-entries)
            # 该选项必须紧跟一个正整数值。
            if (( $# < 2 )); then
                printf "error: --max-entries requires a value\n" >&2
                exit 1
            fi
            maxEntries="$2"
            # 同时移除选项和值。
            shift 2
            ;;
        --strict)
            # 布尔开关只消费当前参数。
            strict=1
            shift
            ;;
        -h | --help)
            # 帮助属于成功终止，不继续访问工具链路径。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数同时输出错误和完整用法，便于 CI 诊断调用脚本。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

if [[ ! "${maxEntries}" =~ ^[0-9]+$ ]] || (( maxEntries < 1 )); then
    # sed 范围要求至少一项，零和非数字均拒绝。
    printf "error: --max-entries must be a positive integer\n" >&2
    exit 1
fi

# 路径默认值与交叉工具链文件保持同步，环境变量优先。
WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
# MSVC toolset 版本目录同时包含编译头、运行库和 ATL/MFC。
MSVC_BASE="${MSVC_BASE:-${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231}"
# SDK 根与具体版本分离，便于列出挂载中可用的其他版本。
WINSDK_BASE="${WINSDK_BASE:-${WINDOWS_CROSS_ROOT}/Program Files (x86)/Windows Kits/10}"
WINSDK_VER="${WINSDK_VER:-10.0.26100.0}"
# Vulkan SDK 单独挂载并同时提供 Include 与 Lib。
VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

# 用统一视觉分隔打印每个诊断区域标题。
printHeader() {
    printf "\n========== %s ==========\n" "$1"
}

# 记录一个缺失探针，最终统一决定是否严格失败。
markMissing() {
    missingCount=$(( missingCount + 1 ))
}

# 打印目录直属条目；缺失时改列父目录帮助定位版本或层级错误。
# 第一个参数是显示标签，第二个参数是待检查绝对路径。
listDir() {
    local label="$1"
    local dir="$2"

    # 每个目录独立成段，并首先回显未转义的真实路径。
    printHeader "${label}"
    printf "path: %s\n" "${dir}"
    if [[ ! -d "${dir}" ]]; then
        # 缺失目录计入严格模式结果，同时尝试展示其父级候选。
        printf "missing directory\n"
        markMissing
        local parent
        # dirname 仅用于诊断，不改变后续探针路径。
        parent="$(dirname "${dir}")"
        if [[ -d "${parent}" ]]; then
            # 父目录列表常能直接暴露版本号或大小写不匹配。
            printf "parent listing: %s\n" "${parent}"
            find "${parent}" -maxdepth 1 -mindepth 1 -printf '%M %10s %TY-%Tm-%Td %TH:%TM %p\n' | sort | sed -n "1,${maxEntries}p"
        else
            # 父级也不存在时停止向上递归，保持输出边界明确。
            printf "missing parent: %s\n" "${parent}"
        fi
        return
    fi

    # 排序后的固定字段同时显示权限、大小、时间和完整路径。
    find "${dir}" -maxdepth 1 -mindepth 1 -printf '%M %10s %TY-%Tm-%Td %TH:%TM %p\n' | sort | sed -n "1,${maxEntries}p"
    local total
    # 总数独立统计，即使展示因 maxEntries 截断也能看到目录规模。
    total="$(find "${dir}" -maxdepth 1 -mindepth 1 | wc -l)"
    printf "entry count: %s\n" "${total}"
}

# 检查单个关键文件并把缺失状态汇入全局计数。
checkFile() {
    local label="$1"
    local file="$2"

    if [[ -f "${file}" ]]; then
        # 成功输出包含标签和真实路径，便于核对来源版本。
        printf "ok: %s -> %s\n" "${label}" "${file}"
    else
        # 普通模式仍继续检查其余文件，集中给出完整缺失清单。
        printf "missing: %s -> %s\n" "${label}" "${file}"
        markMissing
    fi
}

# 首段回显所有最终路径变量，作为后续目录清单的解释上下文。
printHeader "MSVC cross toolchain environment"
printf "WINDOWS_CROSS_ROOT=%s\n" "${WINDOWS_CROSS_ROOT}"
printf "MSVC_BASE=%s\n" "${MSVC_BASE}"
printf "WINSDK_BASE=%s\n" "${WINSDK_BASE}"
printf "WINSDK_VER=%s\n" "${WINSDK_VER}"
printf "VULKAN_SDK=%s\n" "${VULKAN_SDK}"

# 先展示挂载根和 Visual Studio 层级，确认容器卷是否完整。
listDir "Windows cross root" "${WINDOWS_CROSS_ROOT}"
# Program Files 层确认挂载保留带空格的 Windows 原始目录名。
listDir "Program Files (x86)" "${WINDOWS_CROSS_ROOT}/Program Files (x86)"
# Visual Studio 根用于观察可用的主版本与产品版本。
listDir "Visual Studio root" "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio"
# MSVC versions 直接列出可替换默认 14.51 路径的候选目录。
listDir "MSVC versions" "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC"

# MSVC toolset 分别检查标准头、x64 库与可选 ATL/MFC 组件。
listDir "MSVC include" "${MSVC_BASE}/include"
# x64 静态与导入库必须和所选 toolset headers 同版本。
listDir "MSVC lib x64" "${MSVC_BASE}/lib/x64"
# ATL/MFC 头目录用于依赖探针，即使主应用不直接引用也需观察。
listDir "MSVC ATL/MFC include" "${MSVC_BASE}/atlmfc/include"
# ATL/MFC x64 库目录与对应 include 成对检查。
listDir "MSVC ATL/MFC lib x64" "${MSVC_BASE}/atlmfc/lib/x64"

# SDK 根与版本目录先列出，便于发现默认 WINSDK_VER 是否已经过期。
listDir "Windows SDK base" "${WINSDK_BASE}"
# Include 根展示挂载中所有 Windows SDK 版本。
listDir "Windows SDK include versions" "${WINSDK_BASE}/Include"
# selected include 确认 WINSDK_VER 对应目录真实存在。
listDir "Windows SDK selected include" "${WINSDK_BASE}/Include/${WINSDK_VER}"
# 四类 include 子目录对应 C runtime、共享定义、Win32 API 与 WinRT。
listDir "Windows SDK ucrt include" "${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt"
listDir "Windows SDK shared include" "${WINSDK_BASE}/Include/${WINSDK_VER}/shared"
listDir "Windows SDK um include" "${WINSDK_BASE}/Include/${WINSDK_VER}/um"
listDir "Windows SDK winrt include" "${WINSDK_BASE}/Include/${WINSDK_VER}/winrt"
# 库目录重点检查目标版本下的 UCRT 与 UM x64 导入库。
listDir "Windows SDK lib versions" "${WINSDK_BASE}/Lib"
# selected lib 必须与 selected include 使用相同 SDK 版本号。
listDir "Windows SDK selected lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}"
listDir "Windows SDK ucrt lib x64" "${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64"
listDir "Windows SDK um lib x64" "${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64"

# Vulkan 目录确认头层级与 Windows loader 导入库同时存在。
listDir "Vulkan SDK root" "${VULKAN_SDK}"
listDir "Vulkan SDK include" "${VULKAN_SDK}/Include"
listDir "Vulkan SDK vulkan include" "${VULKAN_SDK}/Include/vulkan"
listDir "Vulkan SDK lib" "${VULKAN_SDK}/Lib"

# 文件探针比目录存在性更严格，覆盖实际编译和链接所需代表项。
printHeader "Critical file probes"
# MSVC STL 与运行时头验证 toolset include 内容不是空挂载。
checkFile "MSVC STL vector" "${MSVC_BASE}/include/vector"
# vcruntime.h 验证编译器 ABI 基础声明完整。
checkFile "MSVC vcruntime.h" "${MSVC_BASE}/include/vcruntime.h"
# yvals_core.h 验证 MSVC STL 配置核心未缺失。
checkFile "MSVC yvals_core.h" "${MSVC_BASE}/include/yvals_core.h"
# 静态 C/C++ 运行库与 vcruntime 导入确保 /MT 链接可用。
checkFile "MSVC libcpmt.lib" "${MSVC_BASE}/lib/x64/libcpmt.lib"
# msvcprt.lib 覆盖动态 C++ runtime 导入场景。
checkFile "MSVC msvcprt.lib" "${MSVC_BASE}/lib/x64/msvcprt.lib"
# vcruntime.lib 是 C++ ABI 辅助符号的链接基础。
checkFile "MSVC vcruntime.lib" "${MSVC_BASE}/lib/x64/vcruntime.lib"
# 同时探测 Windows.h 原始大小写与小写代理需求。
checkFile "Windows SDK Windows.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/um/Windows.h"
checkFile "Windows SDK windows.h lowercase probe" "${WINSDK_BASE}/Include/${WINSDK_VER}/um/windows.h"
# shared 与 ucrt 代表头确认基础类型和 C runtime 均已挂载。
checkFile "Windows SDK windef.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/shared/windef.h"
checkFile "Windows SDK corecrt.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt/corecrt.h"
checkFile "Windows SDK stdio.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt/stdio.h"
# kernel32、user32 和 ucrt 覆盖主程序最基础的系统链接闭包。
checkFile "Windows SDK kernel32.lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64/kernel32.lib"
checkFile "Windows SDK user32.lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64/user32.lib"
checkFile "Windows SDK ucrt.lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64/ucrt.lib"
# Vulkan C/C++ 头与 loader import lib 必须属于同一 SDK 根。
checkFile "Vulkan vulkan.h" "${VULKAN_SDK}/Include/vulkan/vulkan.h"
checkFile "Vulkan vulkan.hpp" "${VULKAN_SDK}/Include/vulkan/vulkan.hpp"
checkFile "Vulkan loader import lib" "${VULKAN_SDK}/Lib/vulkan-1.lib"

# 汇总只依赖累计计数，前面的所有探针都会执行完毕。
printHeader "MSVC toolchain layout summary"
if (( missingCount > 0 )); then
    # 非严格模式报告数量但仍返回成功，适合交互式探索不完整环境。
    printf "missing probes: %s\n" "${missingCount}"
    if (( strict )); then
        # 严格模式把同一诊断结果转换为 CI 门禁失败。
        exit 1
    fi
else
    # 零缺失同时证明所有目录与关键文件探针通过。
    printf "all probed directories/files are present\n"
fi
