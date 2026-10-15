#include "log_file.hpp"

#include <cstdint>
#include <ctime>
#include <utility>

namespace coronet {

namespace {
// 同一秒内可能多次滚动（比如 rollSize 很小 + 流量很大），加序号避免重名覆盖
std::atomic<uint32_t> s_rollSeq{0};
}  // namespace

LogFile::LogFile(std::string basename, size_t rollSize)
    : basename_(std::move(basename)), rollSize_(rollSize) {
    rollFile();
}

LogFile::~LogFile() {
    if (file_ != nullptr) {
        ::fflush(file_);
        ::fclose(file_);
        file_ = nullptr;
    }
}

void LogFile::append(const char* data, size_t len) {
    if (file_ == nullptr) {
        // 文件打不开（权限/磁盘满）：退化为 stderr，宁可难看也不能丢日志
        ::fwrite(data, 1, len, stderr);
        return;
    }
    const size_t n = ::fwrite(data, 1, len, file_);
    writtenBytes_ += n;
    if (writtenBytes_ >= rollSize_) {
        rollFile();
    }
}

void LogFile::flush() {
    if (file_ != nullptr) {
        ::fflush(file_);
    }
}

bool LogFile::rollFile() {
    if (file_ != nullptr) {
        ::fflush(file_);
        ::fclose(file_);
        file_ = nullptr;
    }

    ::time_t now = ::time(nullptr);
    struct tm tmv;
    ::localtime_r(&now, &tmv);

    char stamp[32];
    ::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

    char name[512];
    ::snprintf(name, sizeof(name), "%s.%s.%u.log", basename_.c_str(), stamp,
               s_rollSeq.fetch_add(1) + 1);

    file_ = ::fopen(name, "a");
    writtenBytes_ = 0;
    return file_ != nullptr;
}

}  // namespace coronet
