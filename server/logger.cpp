#include "logger.hpp"

#include <cstdio>
#include <memory>

#include "async_logging.hpp"

namespace coronet {

const char* levelName(LogLevel lv) {
    switch (lv) {
        case kDebug:
            return "DEBUG";
        case kInfo:
            return "INFO ";
        case kWarn:
            return "WARN ";
        case kError:
            return "ERROR";
        default:
            return "?????";
    }
}

void formatTime(char* buf, size_t len) {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    struct tm tmv;
    ::localtime_r(&tv.tv_sec, &tmv);   // localtime_r 是线程安全版本（localtime 不是）
    ::snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%06ld", tmv.tm_year + 1900, tmv.tm_mon + 1,
               tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<long>(tv.tv_usec));
}

// ---------------------------------------------------------------------------
//  LogMessage
// ---------------------------------------------------------------------------

LogMessage::LogMessage(const char* file, int line, LogLevel level)
    : level_(level), line_(line), basename_(file) {
    // 只留文件名，去掉路径：日志里出现一长串绝对路径既占地方又泄露目录结构
    const char* slash = std::strrchr(file, '/');
    basename_ = (slash != nullptr) ? slash + 1 : file;

    char tbuf[40];
    formatTime(tbuf, sizeof(tbuf));

    // 头部：时间 + 级别
    stream_ << tbuf << ' ' << levelName(level_) << ' ';
}

LogMessage::~LogMessage() {
    // 尾部：来源位置。放在析构里补，是为了让用户侧只写"消息体"。
    stream_ << " - " << basename_ << ':' << line_ << '\n';
    Logger::append(stream_.data(), stream_.size());
}

// ---------------------------------------------------------------------------
//  Logger
// ---------------------------------------------------------------------------

namespace {
// 用裸指针 + 原子读写：热路径（append）只需一次原子 load，
// 不必每条日志都去抢一把全局互斥锁。
std::atomic<AsyncLogging*> g_async{nullptr};
std::atomic<bool> g_inited{false};
}  // namespace

// 无参重载：等价于 init(Options{})。
// 放在 .cpp 里是因为此处 Options 已是完整类型，可以安全地使用它的默认成员初始化器
// （放在头文件里做默认参数则会触发"默认成员初始化器在所属类结束前不可用"的硬错误）。
void Logger::init() {
    init(Options{});
}

void Logger::init(const Options& opt) {
    if (g_inited.exchange(true)) {
        return;   // 只允许初始化一次（重复 init 直接忽略）
    }
    AsyncLogging* al = new AsyncLogging(opt.basename, opt.rollSize, opt.flushInterval, opt.console);
    al->start();
    g_async.store(al, std::memory_order_release);   // 发布：此后所有线程都能看到
}

void Logger::shutdown() {
    AsyncLogging* al = g_async.exchange(nullptr, std::memory_order_acq_rel);
    if (al == nullptr) {
        g_inited.store(false);
        return;
    }
    // stop() 会 join 后端线程，并在退出前把残留日志全部落盘。
    al->stop();
    g_inited.store(false);

    // 这里**故意不 delete al**：进程退出过程中可能仍有 IO 线程在收尾打日志，
    // 若此刻析构对象，正在 append 的线程会踩到已释放内存（use-after-free）。
    // 正确做法是先优雅停掉所有线程再 shutdown；本项目为了退出流程简单，
    // 选择让这一块内存随进程一起归还操作系统（这是有意为之的取舍）。
}

void Logger::append(const char* data, size_t len) {
    AsyncLogging* al = g_async.load(std::memory_order_acquire);
    if (al != nullptr) {
        al->append(data, len);   // 异步路径：只 memcpy，不碰 I/O
        return;
    }
    // 还没 init（或已经 shutdown）：退化成直接写 stderr —— 一条日志都不丢
    ::fwrite(data, 1, len, stderr);
}

void Logger::flush() {
    AsyncLogging* al = g_async.load(std::memory_order_acquire);
    if (al != nullptr) {
        al->flush();
    } else {
        ::fflush(stderr);
    }
}

bool Logger::inited() {
    return g_inited.load();
}

}  // namespace coronet
