#pragma once

// ============================================================================
//  log_file.hpp —— 日志「落盘器」
//
//  只被日志后端线程使用，所以**内部完全无锁**（单线程独占）。
//  职责：
//    1. 把一批日志字节写进文件（fwrite + 缓冲，不是每条一次 write）；
//    2. 超过 rollingThreshold 就滚动到新文件（按时间戳 + 序号命名）。
//
//  为什么用 FILE* + fwrite 而不是 ::write？
//    FILE* 自带用户态缓冲，能把后端线程攒的一整块日志合并成少数几次
//    write 系统调用；我们自己已在前端做了缓冲，这里再叠一层，
//    是为了让"落盘"这一步的代价也降到最低。
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>

namespace coronet {

class LogFile {
public:
    LogFile(std::string basename, size_t rollSize);
    ~LogFile();

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    void append(const char* data, size_t len);
    void flush();
    bool rollFile();   // 关闭当前文件，切到新文件；返回是否成功

    size_t writtenBytes() const { return writtenBytes_; }

private:
    const std::string basename_;
    const size_t rollSize_;
    FILE* file_ = nullptr;
    size_t writtenBytes_ = 0;
};

}  // namespace coronet
