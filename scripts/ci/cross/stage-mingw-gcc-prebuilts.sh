#!/usr/bin/env bash
set -euo pipefail

showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/stage-mingw-gcc-prebuilts.sh [options]

Copy MinGW GCC source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_cross_mingw_gcc_sources
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: ucrt64
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

copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local sourceRelativePath="$3"
    local outputName="$4"

    local sourcePath="${buildDir}/${sourceRelativePath}"
    local outputPath="${prebuiltRoot}/windows/${packageName}/libs/x86_64/mingw/${compilerTag}/${buildType}/${outputName}"

    if [[ ! -f "${sourcePath}" ]]; then
        printf "error: source library not found: %s\n" "${sourcePath}" >&2
        exit 1
    fi

    install -D -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

copyMainLib() {
    if [[ "${stageScope}" = "ice" ]]; then
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/prebuilts" "$@"
}

copyIceLib() {
    if [[ "${stageScope}" = "main" ]]; then
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

buildDir="build_cross_mingw_gcc_sources"
buildType="RelWithDebInfo"
compilerTag="${MINGW_GCC_PREBUILT_COMPILER_TAG:-ucrt64}"
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
        --build-type)
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
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

if [[ -z "${compilerTag}" ]]; then
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

case "${stageScope}" in
    all | main | ice)
        ;;
    *)
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    printf "error: build directory not found: %s\n" "${buildDir}" >&2
    exit 1
fi

fmtLib="lib/libfmt.a"
fmtOutputName="libfmt.a"
freetypeLib="lib/libfreetype.a"
freetypeOutputName="libfreetype.a"
spdlogLib="lib/libspdlog.a"
spdlogOutputName="libspdlog.a"

case "${buildType}" in
    Debug | debug)
        fmtLib="lib/libfmtd.a"
        fmtOutputName="libfmtd.a"
        freetypeLib="lib/libfreetyped.a"
        freetypeOutputName="libfreetyped.a"
        spdlogLib="lib/libspdlogd.a"
        spdlogOutputName="libspdlogd.a"
        ;;
esac

copyMainLib "ImGuiFileDialog" "lib/libImGuiFileDialog.a" "libImGuiFileDialog.a"
copyMainLib "IonCachyEngine" "lib/libIonCachyEngine-static.a" "libIonCachyEngine-static.a"
copyMainLib "curl" "lib/libcurl.a" "libcurl.a"
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a" "libavcodec.a"
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a" "libavformat.a"
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a" "libavutil.a"
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a" "libswresample.a"
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a" "libswscale.a"
copyMainLib "fftw" "3rdpty/fftw_inst/lib/libfftw3.a" "libfftw3.a"
copyMainLib "fmt" "${fmtLib}" "${fmtOutputName}"
copyMainLib "freetype" "${freetypeLib}" "${freetypeOutputName}"
copyMainLib "glfw" "lib/libglfw3.a" "libglfw3.a"
copyMainLib "imgui" "lib/libimgui-static.a" "libimgui-static.a"
copyMainLib "implot" "lib/lib3rd_implot.a" "lib3rd_implot.a"
copyMainLib "lame" "3rdpty/lame_inst/lib/libmp3lame.a" "libmp3lame.a"
copyMainLib "libsamplerate" "3rdpty/libsamplerate/libsamplerate.a" "libsamplerate.a"
copyMainLib "luajit" "luajit/src/libluajit.a" "libluajit.a"
copyMainLib "lunasvg" "lib/liblunasvg.a" "liblunasvg.a"
copyMainLib "lunasvg" "lib/libplutovg.a" "libplutovg.a"
copyMainLib "miniz" "lib/lib3rd_miniz.a" "lib3rd_miniz.a"
copyMainLib "nativefiledialog-extended" "lib/libnfd.a" "libnfd.a"
copyMainLib "openal" "lib/libOpenAL32.a" "libOpenAL32.a"
copyMainLib "rubberband" "rb_inst/lib/librubberband.a" "librubberband.a"
copyMainLib "sdl" "lib/libSDL3.a" "libSDL3.a"
copyMainLib "spdlog" "${spdlogLib}" "${spdlogOutputName}"
copyMainLib "zlib" "3rdpty/zlib_inst/lib/libz.a" "libz.a"

copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a" "libavcodec.a"
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a" "libavformat.a"
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a" "libavutil.a"
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a" "libswresample.a"
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a" "libswscale.a"
copyIceLib "fftw" "3rdpty/fftw_inst/lib/libfftw3.a" "libfftw3.a"
copyIceLib "fmt" "${fmtLib}" "${fmtOutputName}"
copyIceLib "lame" "3rdpty/lame_inst/lib/libmp3lame.a" "libmp3lame.a"
copyIceLib "libsamplerate" "3rdpty/libsamplerate/libsamplerate.a" "libsamplerate.a"
copyIceLib "openal" "lib/libOpenAL32.a" "libOpenAL32.a"
copyIceLib "rubberband" "rb_inst/lib/librubberband.a" "librubberband.a"
copyIceLib "sdl" "lib/libSDL3.a" "libSDL3.a"
copyIceLib "spdlog" "${spdlogLib}" "${spdlogOutputName}"
copyIceLib "zlib" "3rdpty/zlib_inst/lib/libz.a" "libz.a"
