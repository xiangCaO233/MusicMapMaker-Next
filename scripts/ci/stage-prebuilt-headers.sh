#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/stage-prebuilt-headers.sh [options]

Copy development headers into the prebuilt header layout.

Options:
  --build-dir <path>      Source build directory that contains generated installs.
                          Default: build_linux_sources
  --scope <all|main|ice>  Staging scope. Default: all
  -h, --help              Show this help
EOF
}

projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        printf "%s\n" "${inputPath}"
    else
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

requireDir() {
    local directoryPath="$1"

    if [[ ! -d "${directoryPath}" ]]; then
        printf "error: directory not found: %s\n" "${directoryPath}" >&2
        exit 1
    fi
}

requireFile() {
    local filePath="$1"

    if [[ ! -f "${filePath}" ]]; then
        printf "error: file not found: %s\n" "${filePath}" >&2
        exit 1
    fi
}

resetIncludeDir() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local includeDir="${prebuiltRoot}/headers/${packageName}/include"

    case "${includeDir}" in
        "${projectRoot}/3rdpty/prebuilts/headers/"* | "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/headers/"*)
            ;;
        *)
            printf "error: refusing to remove unexpected include directory: %s\n" "${includeDir}" >&2
            exit 1
            ;;
    esac

    rm -rf -- "${includeDir}"
    mkdir -p "${includeDir}"
}

copyDirContents() {
    local sourceDir="$1"
    local destinationDir="$2"

    requireDir "${sourceDir}"
    mkdir -p "${destinationDir}"
    rsync -a --exclude='.git' "${sourceDir}/" "${destinationDir}/"
}

copyDirAsChild() {
    local sourceDir="$1"
    local destinationParent="$2"
    local childName="$3"

    copyDirContents "${sourceDir}" "${destinationParent}/${childName}"
}

copyFile() {
    local sourceFile="$1"
    local destinationDir="$2"

    requireFile "${sourceFile}"
    install -D -m 0644 "${sourceFile}" "${destinationDir}/$(basename "${sourceFile}")"
}

copyGlob() {
    local sourceDir="$1"
    local destinationDir="$2"
    local pattern="$3"
    local copied=0
    local sourceFile

    requireDir "${sourceDir}"
    mkdir -p "${destinationDir}"
    while IFS= read -r -d '' sourceFile; do
        install -D -m 0644 "${sourceFile}" "${destinationDir}/$(basename "${sourceFile}")"
        copied=1
    done < <(find "${sourceDir}" -maxdepth 1 -type f -name "${pattern}" -print0)

    if (( ! copied )); then
        printf "error: no files matched %s in %s\n" "${pattern}" "${sourceDir}" >&2
        exit 1
    fi
}

stageDirectoryPackage() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local sourceDir="$3"

    resetIncludeDir "${prebuiltRoot}" "${packageName}"
    copyDirContents "${sourceDir}" "${prebuiltRoot}/headers/${packageName}/include"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/${packageName}/include"
}

stageMainSourceHeaders() {
    local prebuiltRoot="$1"
    local sourceRoot="${projectRoot}/3rdpty/sources"
    local includeDir

    stageDirectoryPackage "${prebuiltRoot}" "curl" "${sourceRoot}/curl/include"
    resetIncludeDir "${prebuiltRoot}" "entt"
    copyDirAsChild "${sourceRoot}/entt/src/entt" "${prebuiltRoot}/headers/entt/include" "entt"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/entt/include"
    stageDirectoryPackage "${prebuiltRoot}" "freetype" "${sourceRoot}/freetype/include"
    stageDirectoryPackage "${prebuiltRoot}" "glfw" "${sourceRoot}/glfw/include"
    stageDirectoryPackage "${prebuiltRoot}" "nativefiledialog-extended" "${sourceRoot}/nativefiledialog-extended/src/include"
    stageDirectoryPackage "${prebuiltRoot}" "nlohmann_json" "${sourceRoot}/nlohmann_json/include"
    stageDirectoryPackage "${prebuiltRoot}" "sol2" "${sourceRoot}/sol2/include"
    stageDirectoryPackage "${prebuiltRoot}" "IonCachyEngine" "${sourceRoot}/IonCachyEngine/include"

    resetIncludeDir "${prebuiltRoot}" "ImGuiFileDialog"
    includeDir="${prebuiltRoot}/headers/ImGuiFileDialog/include"
    copyGlob "${sourceRoot}/ImGuiFileDialog" "${includeDir}" "ImGuiFileDialog*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/ImGuiFileDialog/include"

    resetIncludeDir "${prebuiltRoot}" "clay"
    copyFile "${sourceRoot}/clay/clay.h" "${prebuiltRoot}/headers/clay/include"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/clay/include"

    resetIncludeDir "${prebuiltRoot}" "concurrentqueue"
    includeDir="${prebuiltRoot}/headers/concurrentqueue/include"
    copyFile "${sourceRoot}/concurrentqueue/concurrentqueue.h" "${includeDir}"
    copyFile "${sourceRoot}/concurrentqueue/blockingconcurrentqueue.h" "${includeDir}"
    copyFile "${sourceRoot}/concurrentqueue/lightweightsemaphore.h" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/concurrentqueue/include"

    resetIncludeDir "${prebuiltRoot}" "glm"
    copyDirAsChild "${sourceRoot}/glm/glm" "${prebuiltRoot}/headers/glm/include" "glm"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/glm/include"

    resetIncludeDir "${prebuiltRoot}" "imgui"
    includeDir="${prebuiltRoot}/headers/imgui/include"
    copyGlob "${sourceRoot}/imgui" "${includeDir}" "*.h"
    copyDirContents "${sourceRoot}/imgui/backends" "${includeDir}/backends"
    find "${includeDir}/backends" -type f ! -name '*.h' -delete
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/imgui/include"

    resetIncludeDir "${prebuiltRoot}" "implot"
    copyGlob "${sourceRoot}/implot" "${prebuiltRoot}/headers/implot/include" "*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/implot/include"

    resetIncludeDir "${prebuiltRoot}" "luajit"
    copyGlob "${sourceRoot}/luajit/src" "${prebuiltRoot}/headers/luajit/include" "*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/luajit/include"

    resetIncludeDir "${prebuiltRoot}" "lunasvg"
    includeDir="${prebuiltRoot}/headers/lunasvg/include"
    copyDirContents "${sourceRoot}/lunasvg/include" "${includeDir}"
    copyDirContents "${sourceRoot}/lunasvg/plutovg/include" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/lunasvg/include"

    resetIncludeDir "${prebuiltRoot}" "miniz"
    includeDir="${prebuiltRoot}/headers/miniz/include"
    copyGlob "${sourceRoot}/miniz" "${includeDir}" "miniz*.h"
    copyFile "${buildDir}/3rdpty/sources/miniz_export.h" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/miniz/include"

    resetIncludeDir "${prebuiltRoot}" "stb"
    copyGlob "${sourceRoot}/stb" "${prebuiltRoot}/headers/stb/include" "*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/stb/include"
}

stageIceDependencyHeaders() {
    local prebuiltRoot="$1"
    local sourceRoot="${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/sources"
    local includeDir

    stageDirectoryPackage "${prebuiltRoot}" "ffmpeg" "${buildDir}/3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/include"
    stageDirectoryPackage "${prebuiltRoot}" "fftw" "${buildDir}/3rdpty/fftw_inst/include"
    stageDirectoryPackage "${prebuiltRoot}" "fmt" "${sourceRoot}/fmt/include"
    stageDirectoryPackage "${prebuiltRoot}" "lame" "${buildDir}/3rdpty/lame_inst/include"
    stageDirectoryPackage "${prebuiltRoot}" "openal" "${sourceRoot}/openal/include"
    stageDirectoryPackage "${prebuiltRoot}" "rubberband" "${buildDir}/rb_inst/include"
    stageDirectoryPackage "${prebuiltRoot}" "sdl" "${sourceRoot}/sdl/include"
    stageDirectoryPackage "${prebuiltRoot}" "spdlog" "${sourceRoot}/spdlog/include"
    stageDirectoryPackage "${prebuiltRoot}" "zlib" "${buildDir}/3rdpty/zlib_inst/include"

    resetIncludeDir "${prebuiltRoot}" "libsamplerate"
    includeDir="${prebuiltRoot}/headers/libsamplerate/include"
    copyFile "${sourceRoot}/libsamplerate/src/samplerate.h" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/libsamplerate/include"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

buildDir="build_linux_sources"
stageScope="all"

while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --scope)
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
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

case "${stageScope}" in
    all | main | ice)
        ;;
    *)
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

buildDir="$(projectPath "${buildDir}")"
requireDir "${buildDir}"

if [[ "${stageScope}" = "all" || "${stageScope}" = "main" ]]; then
    stageMainSourceHeaders "${projectRoot}/3rdpty/prebuilts"
    stageIceDependencyHeaders "${projectRoot}/3rdpty/prebuilts"
fi

if [[ "${stageScope}" = "all" || "${stageScope}" = "ice" ]]; then
    stageIceDependencyHeaders "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts"
fi
