
# 准备生成可执行文件的所有源文件并且生成
aux_source_directory(. SRC_LIST)
aux_source_directory(./db DB_LIST)
aux_source_directory(./redis REDIS_LIST)
add_executable(ChatServer ${SRC_LIST} ${DB_LIST} ${REDIS_LIST})
# 直播模块显式登记，使既有构建目录在接入模块时重新扫描源文件。
target_sources(ChatServer PRIVATE live_service.cpp live_rtc.cpp db/live_repository.cpp)

# 指定链接时依赖的库文件（移除 mysqlclient，使用 Boost::mysql 头文件库）
target_link_libraries(ChatServer chat_media ${OpenCV_LIBS} ${Boost_LIBRARIES} hiredis pthread ssl crypto)

# 预编译头：boost::mysql 和 boost::asio 模板极重，PCH 只解析一次，所有 TU 共享
target_precompile_headers(ChatServer PRIVATE
    <boost/mysql.hpp>
    <boost/asio.hpp>
)
