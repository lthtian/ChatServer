#pragma once
#include <iostream>

// RAII log helper - adds newline on destruction (compatible with muduo::Logger API)
class LogStream {
public:
    LogStream(std::ostream &os, const char *level) : os_(os) {
        os_ << "[" << level << "] ";
    }
    ~LogStream() { os_ << std::endl; }

    template <typename T>
    LogStream &operator<<(const T &val) {
        os_ << val;
        return *this;
    }

private:
    std::ostream &os_;
};

#define LOG_INFO LogStream(std::cout, "INFO")
#define LOG_WARN LogStream(std::cout, "WARN")
#define LOG_ERROR LogStream(std::cerr, "ERROR")
