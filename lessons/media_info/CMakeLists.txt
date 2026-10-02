cmake_minimum_required(VERSION 3.20)
project(media_info LANGUAGES CXX)

set(FFMPEG_ROOT "" CACHE PATH "FFmpeg development package containing include, lib and bin")
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
    PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

add_executable(media_info main.cpp)
target_compile_features(media_info PRIVATE cxx_std_17)
set_target_properties(media_info PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(media_info SYSTEM PRIVATE "${FFMPEG_INCLUDE_DIR}")
foreach(component avformat avcodec avutil swscale)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
        PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(media_info PRIVATE "${FFMPEG_${component}_LIBRARY}")
endforeach()

if(MSVC)
    target_compile_options(media_info PRIVATE /W4 /utf-8)
else()
    target_compile_options(media_info PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_compile_definitions(media_info PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    file(GLOB ffmpeg_runtime "${FFMPEG_ROOT}/bin/*.dll")
    add_custom_command(TARGET media_info POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${ffmpeg_runtime} "$<TARGET_FILE_DIR:media_info>"
        COMMAND_EXPAND_LISTS VERBATIM)
endif()
if(MINGW)
    target_link_options(media_info PRIVATE -municode)
    get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        add_custom_command(TARGET media_info POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${compiler_directory}/${runtime}" "$<TARGET_FILE_DIR:media_info>"
            VERBATIM)
    endforeach()
endif()
