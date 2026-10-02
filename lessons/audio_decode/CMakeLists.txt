cmake_minimum_required(VERSION 3.20)
project(audio_decode LANGUAGES CXX)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core)
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg SDK containing include, lib and bin")
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
    PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

add_executable(audio_decode main.cpp)
target_compile_features(audio_decode PRIVATE cxx_std_17)
set_target_properties(audio_decode PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(audio_decode SYSTEM PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(audio_decode PRIVATE Qt5::Core)
foreach(component avformat avcodec avutil swresample)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
        PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(audio_decode PRIVATE "${FFMPEG_${component}_LIBRARY}")
endforeach()
if(MSVC)
    target_compile_options(audio_decode PRIVATE /W4 /utf-8)
else()
    target_compile_options(audio_decode PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    file(GLOB ffmpeg_runtime "${FFMPEG_ROOT}/bin/*.dll")
    add_custom_command(TARGET audio_decode POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${ffmpeg_runtime}
            "$<TARGET_FILE:Qt5::Core>" "$<TARGET_FILE_DIR:audio_decode>"
        COMMAND_EXPAND_LISTS VERBATIM)
endif()
if(MINGW)
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        add_custom_command(TARGET audio_decode POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${compiler_bin}/${runtime}" "$<TARGET_FILE_DIR:audio_decode>"
            VERBATIM)
    endforeach()
endif()
