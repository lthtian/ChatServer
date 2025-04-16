
# 准备生成可执行文件的所有源文件并且生成
aux_source_directory(. SRC_LIST)
aux_source_directory(./db DB_LIST)
aux_source_directory(./redis REDIS_LIST)
add_executable(ChatServer ${SRC_LIST} ${DB_LIST} ${REDIS_LIST})

# 指定链接时依赖的库文件
target_link_libraries(ChatServer ${OpenCV_LIBS} muduo_net muduo_base mysqlclient hiredis pthread ssl crypto)