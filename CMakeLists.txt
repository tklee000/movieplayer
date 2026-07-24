cmake_minimum_required(VERSION 3.15)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(MoviePlayer VERSION 0.2.0.0 LANGUAGES C CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "MoviePlayer is a native Win32 application and must be built on Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MoviePlayer requires a 64-bit build. Generate the project with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Upstream native AI sources contain UTF-8 comments.  The VS2019 compiler
    # otherwise interprets them using the active console code page.  C5105 is
    # an old Windows 10 SDK macro warning fixed in newer SDK/compiler pairs.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:/utf-8>"
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
        "$<$<COMPILE_LANGUAGE:C>:/wd5105>"
        "$<$<COMPILE_LANGUAGE:CXX>:/wd5105>"
    )
endif()

# NVIDIA's supplied SDK library is built against the dynamic MSVC runtime. Use
# the matching runtime and copy the permitted VC142 redistributable DLLs beside
# the executable so end users do not need a system-wide redist installation.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(MOVIEPLAYER_DISPLAY_VERSION
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(MOVIEPLAYER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MOVIEPLAYER_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${MOVIEPLAYER_GENERATED_DIR}/Version.h"
    @ONLY
)

set(RTX_VIDEO_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rtx_video_sdk" CACHE PATH
    "Path to NVIDIA RTX Video SDK 1.1 or newer")
set(WHISPER_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper_cpp" CACHE PATH
    "Path to the pinned whisper.cpp source tree")
set(CTRANSLATE2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctranslate2" CACHE PATH
    "Path to the pinned CTranslate2 source tree")
set(SENTENCEPIECE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sentencepiece" CACHE PATH
    "Path to the pinned SentencePiece source tree")

set(RTX_VIDEO_SDK_REQUIRED_FILES
    "include/nvsdk_ngx_helpers_vsr.h"
    "lib/Windows/x64/nvsdk_ngx_d.lib"
    "lib/Windows/x64/nvsdk_ngx_d_dbg.lib"
    "bin/Windows/x64/rel/nvngx_vsr.dll"
)
foreach(required_file IN LISTS RTX_VIDEO_SDK_REQUIRED_FILES)
    if(NOT EXISTS "${RTX_VIDEO_SDK_ROOT}/${required_file}")
        message(FATAL_ERROR
            "RTX Video SDK is incomplete: ${RTX_VIDEO_SDK_ROOT}/${required_file} was not found. "
            "Run scripts/setup_rtx_video_sdk.ps1 first.")
    endif()
endforeach()

foreach(native_ai_source IN ITEMS
        "${WHISPER_CPP_ROOT}/CMakeLists.txt"
        "${CTRANSLATE2_ROOT}/CMakeLists.txt"
        "${SENTENCEPIECE_ROOT}/CMakeLists.txt")
    if(NOT EXISTS "${native_ai_source}")
        message(FATAL_ERROR
            "Native AI build dependency is missing: ${native_ai_source}. "
            "Run scripts/setup_native_ai_dependencies.ps1 first.")
    endif()
endforeach()

# Build only the native inference libraries used by the subtitle worker.  The
# source revisions are installed and verified by setup_native_ai_dependencies.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_STATIC ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
add_subdirectory("${WHISPER_CPP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/native/whisper_cpp"
                 EXCLUDE_FROM_ALL)

# CTranslate2 is shared on Windows so it uses the same /MD runtime as the main
# application and the NVIDIA SDK.  All optional acceleration backends are off;
# this worker intentionally provides a portable CPU inference path.
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries" FORCE)
set(WITH_MKL OFF CACHE BOOL "" FORCE)
set(WITH_DNNL OFF CACHE BOOL "" FORCE)
set(WITH_ACCELERATE OFF CACHE BOOL "" FORCE)
set(WITH_OPENBLAS OFF CACHE BOOL "" FORCE)
set(WITH_RUY ON CACHE BOOL "" FORCE)
set(WITH_CUDA OFF CACHE BOOL "" FORCE)
set(WITH_CUDNN OFF CACHE BOOL "" FORCE)
set(WITH_HIP OFF CACHE BOOL "" FORCE)
set(WITH_TENSOR_PARALLEL OFF CACHE BOOL "" FORCE)
set(WITH_FLASH_ATTN OFF CACHE BOOL "" FORCE)
set(ENABLE_CPU_DISPATCH OFF CACHE BOOL "" FORCE)
set(OPENMP_RUNTIME NONE CACHE STRING "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CTRANSLATE2_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/native/ctranslate2"
                 EXCLUDE_FROM_ALL)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(SPM_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(SPM_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(SPM_ENABLE_MSVC_MT_BUILD OFF CACHE BOOL "" FORCE)
add_subdirectory("${SENTENCEPIECE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/native/sentencepiece"
                 EXCLUDE_FROM_ALL)

# Codec code is intentionally split by responsibility. Container and audio
# modules do not depend on the video backend, so the subtitle worker can reuse
# MP4/AAC without pulling in the video decoder backends.
add_library(MovieCodecCore STATIC
    src/codec/core/BitReader.cpp
    src/codec/core/BitReader.h
    src/codec/core/ByteReader.cpp
    src/codec/core/ByteReader.h
    src/codec/core/CodecTypes.h
    src/codec/core/RandomAccessFile.cpp
    src/codec/core/RandomAccessFile.h
    src/codec/core/ZlibInflater.cpp
    src/codec/core/ZlibInflater.h
    src/codec/subtitle/TextSubtitleDecoder.cpp
    src/codec/subtitle/TextSubtitleDecoder.h
    src/codec/subtitle/VobSubDecoder.cpp
    src/codec/subtitle/VobSubDecoder.h
    src/codec/container/MediaDemuxer.cpp
    src/codec/container/MediaDemuxer.h
    src/codec/container/avi/AviDemuxer.cpp
    src/codec/container/avi/AviDemuxer.h
    src/codec/container/mkv/MkvDemuxer.cpp
    src/codec/container/mkv/MkvDemuxer.h
    src/codec/container/mp4/Mp4Demuxer.cpp
    src/codec/container/mp4/Mp4Demuxer.h
)
target_include_directories(MovieCodecCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(MovieCodecCore PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00)

add_library(MovieCodecAudio STATIC
    src/codec/audio/AudioDecoder.h
    src/codec/audio/aac/AacHuffman.cpp
    src/codec/audio/aac/AacHuffman.h
    src/codec/audio/aac/AacLcDecoder.cpp
    src/codec/audio/aac/AacLcDecoder.h
    src/codec/audio/mp3/MfMp3Decoder.cpp
    src/codec/audio/mp3/MfMp3Decoder.h
)
target_link_libraries(MovieCodecAudio PUBLIC MovieCodecCore mfplat mfuuid
    wmcodecdspuuid ole32)

add_library(MovieCodecVideo STATIC
    src/codec/video/VideoDecoder.h
    src/codec/video/h264/MfH264Decoder.cpp
    src/codec/video/h264/MfH264Decoder.h
    src/codec/video/hevc/HevcStructures.h
    src/codec/video/hevc/HevcBitstreamParser.cpp
    src/codec/video/hevc/HevcBitstreamParser.h
    src/codec/video/hevc/D3D11HevcDecoder.cpp
    src/codec/video/hevc/D3D11HevcDecoder.h
)
target_link_libraries(MovieCodecVideo PUBLIC MovieCodecCore d3d11 dxgi dxguid
    mfplat mfuuid wmcodecdspuuid ole32)

if(MSVC)
    foreach(codec_target IN ITEMS MovieCodecCore MovieCodecAudio MovieCodecVideo)
        target_compile_options(${codec_target} PRIVATE /W4 /permissive- /utf-8 /EHsc /MP)
    endforeach()
endif()

add_executable(MoviePlayerSubtitleWorker
    src/SubtitleWorker.cpp
    src/SubtitleWorker.rc
)
target_include_directories(MoviePlayerSubtitleWorker PRIVATE
    "${MOVIEPLAYER_GENERATED_DIR}"
    "${SENTENCEPIECE_ROOT}/src"
)
target_compile_definitions(MoviePlayerSubtitleWorker PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
)
target_link_libraries(MoviePlayerSubtitleWorker PRIVATE
    MovieCodecAudio
    whisper
    ctranslate2
    sentencepiece-static
)
if(MSVC)
    target_compile_options(MoviePlayerSubtitleWorker PRIVATE
        /W4 /permissive- /utf-8 /EHsc /MP)
endif()
add_custom_command(TARGET MoviePlayerSubtitleWorker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ctranslate2>"
            "$<TARGET_FILE_DIR:MoviePlayerSubtitleWorker>"
    VERBATIM)

add_executable(MoviePlayer WIN32
    src/Main.cpp
    src/Localization.cpp
    src/Localization.h
    src/FileAssociations.cpp
    src/FileAssociations.h
    src/MediaFileTools.cpp
    src/MediaFileTools.h
    src/PlayerEngine.cpp
    src/PlayerEngine.h
    src/AudioOutput.cpp
    src/AudioOutput.h
    src/D3DRenderer.cpp
    src/D3DRenderer.h
    src/RtxVideoVsr.cpp
    src/RtxVideoVsr.h
    src/SubtitleTrack.cpp
    src/SubtitleTrack.h
    src/WhisperSubtitleJob.cpp
    src/WhisperSubtitleJob.h
    src/Utilities.cpp
    src/Utilities.h
    src/MoviePlayer.rc
    src/app.manifest
)

target_include_directories(MoviePlayer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MOVIEPLAYER_GENERATED_DIR}"
)
target_include_directories(MoviePlayer SYSTEM PRIVATE "${RTX_VIDEO_SDK_ROOT}/include")

target_link_directories(MoviePlayer PRIVATE
    "${RTX_VIDEO_SDK_ROOT}/lib/Windows/x64"
)

target_compile_definitions(MoviePlayer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(MoviePlayer PRIVATE /W4 /permissive- /utf-8 /EHsc /MP)
    # The application manifest is embedded explicitly by MoviePlayer.rc.
    target_link_options(MoviePlayer PRIVATE /SUBSYSTEM:WINDOWS,6.02 /MANIFEST:NO)

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    foreach(msvc_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        foreach(runtime_target IN ITEMS MoviePlayer MoviePlayerSubtitleWorker)
            add_custom_command(TARGET ${runtime_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${msvc_runtime}" "$<TARGET_FILE_DIR:${runtime_target}>"
                VERBATIM)
        endforeach()
    endforeach()
endif()

target_link_libraries(MoviePlayer PRIVATE
    MovieCodecAudio
    MovieCodecVideo
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    gdi32
    uxtheme
    xaudio2
    comctl32
    comdlg32
    shell32
    shlwapi
    advapi32
    ole32
    uuid
    $<$<CONFIG:Debug>:nvsdk_ngx_d_dbg>
    $<$<NOT:$<CONFIG:Debug>>:nvsdk_ngx_d>
)

add_custom_command(TARGET MoviePlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTX_VIDEO_SDK_ROOT}/bin/Windows/x64/rel/nvngx_vsr.dll"
            "$<TARGET_FILE_DIR:MoviePlayer>"
    VERBATIM)

add_custom_command(TARGET MoviePlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/languages"
            "$<TARGET_FILE_DIR:MoviePlayer>/languages"
    VERBATIM)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT MoviePlayer)
set_target_properties(MoviePlayer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:MoviePlayer>"
)

add_executable(MovieCodecSmoke EXCLUDE_FROM_ALL tests/CodecSmoke.cpp)
target_link_libraries(MovieCodecSmoke PRIVATE MovieCodecAudio MovieCodecVideo d3d10 d3d11)
if(MSVC)
    target_compile_options(MovieCodecSmoke PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
