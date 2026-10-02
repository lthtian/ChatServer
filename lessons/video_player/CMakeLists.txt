cmake_minimum_required(VERSION 3.20)
project(video_player LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Multimedia Test)
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg SDK containing include, lib and bin")
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
    PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

add_library(playback STATIC video_decoder.cpp video_decoder.h player_window.cpp player_window.h video_view.cpp video_view.h)
target_include_directories(playback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(playback SYSTEM PUBLIC "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(playback PUBLIC Qt5::Widgets)
foreach(component avformat avcodec avutil swscale)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
        PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(playback PUBLIC "${FFMPEG_${component}_LIBRARY}")
endforeach()
if(MSVC)
    target_compile_options(playback PUBLIC /W4 /utf-8)
else()
    target_compile_options(playback PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(silent_player WIN32 main.cpp)
target_link_libraries(silent_player PRIVATE playback)
add_executable(playback_test tests/playback_test.cpp)
target_link_libraries(playback_test PRIVATE playback Qt5::Test)
add_dependencies(playback_test silent_player)

add_library(media_playback STATIC media_decoder.cpp media_decoder.h
    audio_output.cpp audio_output.h media_window.cpp media_window.h video_view.cpp video_view.h)
target_include_directories(media_playback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(media_playback SYSTEM PUBLIC "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(media_playback PUBLIC Qt5::Widgets Qt5::Multimedia)
foreach(component avformat avcodec avutil swscale swresample)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
        PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(media_playback PUBLIC "${FFMPEG_${component}_LIBRARY}")
endforeach()
if(MSVC)
    target_compile_options(media_playback PUBLIC /W4 /utf-8)
else()
    target_compile_options(media_playback PUBLIC -Wall -Wextra -Wpedantic)
endif()
add_executable(media_player WIN32 media_main.cpp)
target_link_libraries(media_player PRIVATE media_playback)
add_executable(media_smoke tests/media_smoke.cpp)
target_link_libraries(media_smoke PRIVATE media_playback Qt5::Test)
add_dependencies(media_smoke media_player)

if(WIN32)
    file(GLOB ffmpeg_runtime "${FFMPEG_ROOT}/bin/*.dll")
    add_custom_command(TARGET silent_player POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${ffmpeg_runtime}
            "$<TARGET_FILE:Qt5::Core>" "$<TARGET_FILE:Qt5::Gui>" "$<TARGET_FILE:Qt5::Widgets>"
            "$<TARGET_FILE_DIR:silent_player>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:silent_player>/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Qt5::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:silent_player>/platforms"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_command(TARGET playback_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:Qt5::Test>"
            "$<TARGET_FILE_DIR:playback_test>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Qt5::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:playback_test>/platforms" VERBATIM)
    add_custom_command(TARGET media_player POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${ffmpeg_runtime}
            "$<TARGET_FILE:Qt5::Core>" "$<TARGET_FILE:Qt5::Gui>" "$<TARGET_FILE:Qt5::Widgets>"
            "$<TARGET_FILE:Qt5::Multimedia>" "$<TARGET_FILE:Qt5::Network>" "$<TARGET_FILE:Qt5::Test>"
            "$<TARGET_FILE_DIR:media_player>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:media_player>/platforms"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:media_player>/audio"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:Qt5::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:media_player>/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:Qt5::QWindowsAudioPlugin>"
            "$<TARGET_FILE_DIR:media_player>/audio"
        COMMAND_EXPAND_LISTS VERBATIM)
endif()
if(MINGW)
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        add_custom_command(TARGET silent_player POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${compiler_bin}/${runtime}" "$<TARGET_FILE_DIR:silent_player>" VERBATIM)
        add_custom_command(TARGET media_player POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${compiler_bin}/${runtime}" "$<TARGET_FILE_DIR:media_player>" VERBATIM)
    endforeach()
endif()
