#!/usr/bin/env bash
set -euo pipefail

showUsage() {
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

maxEntries=120
strict=0
missingCount=0

while (( $# > 0 )); do
    case "$1" in
        --max-entries)
            if (( $# < 2 )); then
                printf "error: --max-entries requires a value\n" >&2
                exit 1
            fi
            maxEntries="$2"
            shift 2
            ;;
        --strict)
            strict=1
            shift
            ;;
        -h | --help)
            showUsage
            exit 0
            ;;
        *)
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

if [[ ! "${maxEntries}" =~ ^[0-9]+$ ]] || (( maxEntries < 1 )); then
    printf "error: --max-entries must be a positive integer\n" >&2
    exit 1
fi

WINDOWS_CROSS_ROOT="${WINDOWS_CROSS_ROOT:-/mnt/cross/windows}"
MSVC_BASE="${MSVC_BASE:-${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/14.51.36231}"
WINSDK_BASE="${WINSDK_BASE:-${WINDOWS_CROSS_ROOT}/Program Files (x86)/Windows Kits/10}"
WINSDK_VER="${WINSDK_VER:-10.0.26100.0}"
VULKAN_SDK="${VULKAN_SDK:-${WINDOWS_CROSS_ROOT}/VulkanSDK/1.4.350.0}"

printHeader() {
    printf "\n========== %s ==========\n" "$1"
}

markMissing() {
    missingCount=$(( missingCount + 1 ))
}

listDir() {
    local label="$1"
    local dir="$2"

    printHeader "${label}"
    printf "path: %s\n" "${dir}"
    if [[ ! -d "${dir}" ]]; then
        printf "missing directory\n"
        markMissing
        local parent
        parent="$(dirname "${dir}")"
        if [[ -d "${parent}" ]]; then
            printf "parent listing: %s\n" "${parent}"
            find "${parent}" -maxdepth 1 -mindepth 1 -printf '%M %10s %TY-%Tm-%Td %TH:%TM %p\n' | sort | sed -n "1,${maxEntries}p"
        else
            printf "missing parent: %s\n" "${parent}"
        fi
        return
    fi

    find "${dir}" -maxdepth 1 -mindepth 1 -printf '%M %10s %TY-%Tm-%Td %TH:%TM %p\n' | sort | sed -n "1,${maxEntries}p"
    local total
    total="$(find "${dir}" -maxdepth 1 -mindepth 1 | wc -l)"
    printf "entry count: %s\n" "${total}"
}

checkFile() {
    local label="$1"
    local file="$2"

    if [[ -f "${file}" ]]; then
        printf "ok: %s -> %s\n" "${label}" "${file}"
    else
        printf "missing: %s -> %s\n" "${label}" "${file}"
        markMissing
    fi
}

printHeader "MSVC cross toolchain environment"
printf "WINDOWS_CROSS_ROOT=%s\n" "${WINDOWS_CROSS_ROOT}"
printf "MSVC_BASE=%s\n" "${MSVC_BASE}"
printf "WINSDK_BASE=%s\n" "${WINSDK_BASE}"
printf "WINSDK_VER=%s\n" "${WINSDK_VER}"
printf "VULKAN_SDK=%s\n" "${VULKAN_SDK}"

listDir "Windows cross root" "${WINDOWS_CROSS_ROOT}"
listDir "Program Files (x86)" "${WINDOWS_CROSS_ROOT}/Program Files (x86)"
listDir "Visual Studio root" "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio"
listDir "MSVC versions" "${WINDOWS_CROSS_ROOT}/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC"

listDir "MSVC include" "${MSVC_BASE}/include"
listDir "MSVC lib x64" "${MSVC_BASE}/lib/x64"
listDir "MSVC ATL/MFC include" "${MSVC_BASE}/atlmfc/include"
listDir "MSVC ATL/MFC lib x64" "${MSVC_BASE}/atlmfc/lib/x64"

listDir "Windows SDK base" "${WINSDK_BASE}"
listDir "Windows SDK include versions" "${WINSDK_BASE}/Include"
listDir "Windows SDK selected include" "${WINSDK_BASE}/Include/${WINSDK_VER}"
listDir "Windows SDK ucrt include" "${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt"
listDir "Windows SDK shared include" "${WINSDK_BASE}/Include/${WINSDK_VER}/shared"
listDir "Windows SDK um include" "${WINSDK_BASE}/Include/${WINSDK_VER}/um"
listDir "Windows SDK winrt include" "${WINSDK_BASE}/Include/${WINSDK_VER}/winrt"
listDir "Windows SDK lib versions" "${WINSDK_BASE}/Lib"
listDir "Windows SDK selected lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}"
listDir "Windows SDK ucrt lib x64" "${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64"
listDir "Windows SDK um lib x64" "${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64"

listDir "Vulkan SDK root" "${VULKAN_SDK}"
listDir "Vulkan SDK include" "${VULKAN_SDK}/Include"
listDir "Vulkan SDK vulkan include" "${VULKAN_SDK}/Include/vulkan"
listDir "Vulkan SDK lib" "${VULKAN_SDK}/Lib"

printHeader "Critical file probes"
checkFile "MSVC STL vector" "${MSVC_BASE}/include/vector"
checkFile "MSVC vcruntime.h" "${MSVC_BASE}/include/vcruntime.h"
checkFile "MSVC yvals_core.h" "${MSVC_BASE}/include/yvals_core.h"
checkFile "MSVC libcpmt.lib" "${MSVC_BASE}/lib/x64/libcpmt.lib"
checkFile "MSVC msvcprt.lib" "${MSVC_BASE}/lib/x64/msvcprt.lib"
checkFile "MSVC vcruntime.lib" "${MSVC_BASE}/lib/x64/vcruntime.lib"
checkFile "Windows SDK Windows.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/um/Windows.h"
checkFile "Windows SDK windows.h lowercase probe" "${WINSDK_BASE}/Include/${WINSDK_VER}/um/windows.h"
checkFile "Windows SDK windef.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/shared/windef.h"
checkFile "Windows SDK corecrt.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt/corecrt.h"
checkFile "Windows SDK stdio.h" "${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt/stdio.h"
checkFile "Windows SDK kernel32.lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64/kernel32.lib"
checkFile "Windows SDK user32.lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64/user32.lib"
checkFile "Windows SDK ucrt.lib" "${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64/ucrt.lib"
checkFile "Vulkan vulkan.h" "${VULKAN_SDK}/Include/vulkan/vulkan.h"
checkFile "Vulkan vulkan.hpp" "${VULKAN_SDK}/Include/vulkan/vulkan.hpp"
checkFile "Vulkan loader import lib" "${VULKAN_SDK}/Lib/vulkan-1.lib"

printHeader "MSVC toolchain layout summary"
if (( missingCount > 0 )); then
    printf "missing probes: %s\n" "${missingCount}"
    if (( strict )); then
        exit 1
    fi
else
    printf "all probed directories/files are present\n"
fi
