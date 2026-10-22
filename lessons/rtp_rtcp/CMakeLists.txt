cmake_minimum_required(VERSION 3.20)
project(rtp_rtcp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network Widgets)
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg SDK containing include, lib and bin")
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
  PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)

add_library(rtp_protocol STATIC rtp.cpp rtcp.cpp)
target_link_libraries(rtp_protocol PUBLIC Qt5::Core)
target_include_directories(rtp_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(rtp_sender sender.cpp)
add_executable(rtp_receiver receiver.cpp video_decoder.cpp ../video_player/video_view.cpp)
target_include_directories(rtp_receiver PRIVATE ../video_player)
target_link_libraries(rtp_sender PRIVATE rtp_protocol Qt5::Network)
target_link_libraries(rtp_receiver PRIVATE rtp_protocol Qt5::Network Qt5::Widgets)

foreach(target rtp_sender rtp_receiver)
  target_include_directories(${target} SYSTEM PRIVATE "${FFMPEG_INCLUDE_DIR}")
  foreach(component avformat avcodec avutil swscale)
    find_library(FFMPEG_${component}_LIBRARY NAMES ${component}
      PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(${target} PRIVATE "${FFMPEG_${component}_LIBRARY}")
  endforeach()
endforeach()

add_executable(protocol_check EXCLUDE_FROM_ALL protocol_check.cpp)
target_link_libraries(protocol_check PRIVATE rtp_protocol)
foreach(target rtp_protocol rtp_sender rtp_receiver protocol_check)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

if(WIN32)
  file(GLOB ffmpeg_runtime "${FFMPEG_ROOT}/bin/*.dll")
  foreach(target rtp_sender rtp_receiver)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${ffmpeg_runtime}
        "$<TARGET_FILE:Qt5::Core>" "$<TARGET_FILE:Qt5::Network>"
        "$<TARGET_FILE:Qt5::Gui>" "$<TARGET_FILE:Qt5::Widgets>"
        "$<TARGET_FILE_DIR:${target}>" COMMAND_EXPAND_LISTS VERBATIM)
  endforeach()
  add_custom_command(TARGET rtp_receiver POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:rtp_receiver>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:Qt5::QWindowsIntegrationPlugin>" "$<TARGET_FILE:Qt5::QOffscreenIntegrationPlugin>"
      "$<TARGET_FILE_DIR:rtp_receiver>/platforms" VERBATIM)
endif()
if(MINGW)
  get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(target rtp_sender rtp_receiver)
    foreach(runtime libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${compiler_bin}/${runtime}"
          "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    endforeach()
  endforeach()
endif()
