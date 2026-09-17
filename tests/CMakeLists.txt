# ABOUTME: Builds repository tests against the server's Boost.MySQL dependency.
# ABOUTME: Keeps database verification independent of the running chat service.
cmake_minimum_required(VERSION 3.16)
project(chat_repository_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Boost 1.82 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
add_executable(media_repository_test
    media_repository_test.cpp ../src/server/db/media_repository.cpp)
target_include_directories(media_repository_test PRIVATE
    ../include/server ../media/include ../thirdparty ${Boost_INCLUDE_DIRS})
target_link_libraries(media_repository_test PRIVATE Boost::system OpenSSL::SSL
    OpenSSL::Crypto Threads::Threads)
target_compile_options(media_repository_test PRIVATE -Wall -Wextra -Wpedantic)
