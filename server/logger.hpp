#pragma once

// ============================================================================
//  logger.hpp —— 日志「前端」（异步日志的生产端）
//
//  同步日志（每条直接 << std::cout）的问题：cout 内部有锁、还可能触发 write
//  系统调用，**IO 线程打一条日志 = 一次潜在的阻塞**，这是服务端的大忌。
//  异步日志要达到的目标：
//
//      业务线程打日志的开销 ≈ 一次 memcpy（+ 一次极短的加锁）
//      磁盘 I/O 全部交给「日志后端线程」批量完成
//
//  三个角色：
//    LogStream  —— 定长栈缓冲的顺序化输出，替代 std::ostringstream。
//                  不必每条日志来一次堆分配 + 一次 locale 加锁。
//    LogMessage —— RAII：构造时写头（时间 / 级别），析构时补尾（文件:行）
//                  并把整行交给 Logger（异步入队）。
//    Logger     —— 前端门面：init() 拉起后端；append() 入队；没 init 时
//                  退化为直接写 stderr，保证日志一条都不丢。
//
//  ⚠️ 用法：进程启动最开始（**在创建任何线程之前**）调用一次
//        coronet::Logger::init(...);
//      进程退出前调用 coronet::Logger::shutdown()（把残留日志刷盘）。
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <thread>
#include <time.h>
#include <type_traits>

namespace coronet {

// ---------------------------------------------------------------------------
//  日志级别：名字定长 5 字符，输出时天然对齐
// ---------------------------------------------------------------------------
enum LogLevel { kDebug = 0, kInfo, kWarn, kError };

const char* levelName(LogLevel lv);

// 把当前时间写进 buf："2026-10-13 18:56:45.123456"
void formatTime(char* buf, size_t len);

// ---------------------------------------------------------------------------
//  LogStream —— 定长缓冲的输出流
//
//  为什么不用 std::ostringstream？因为它每条日志都要：
//    1. 构造/析构（堆分配）；
//    2. 处理 locale（内部有锁）；
//    3. 最终还要 .str() 拷一份 string。
//  在自己项目里，日志内容是「数字 + 短字符串」为主，查表法/手写整数转
//  字符完全够用，快得多，也顺便讲清楚"为什么不用现成的"。
// ---------------------------------------------------------------------------
class LogStream {
public:
    static constexpr size_t kMaxLine = 4096;   // 单条日志上限（超长截断，绝不越界）

    LogStream() = default;
    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    const char* data() const { return buf_; }
    size_t size() const { return cur_; }
    void reset() { cur_ = 0; }

    void append(const char* data, size_t len) {
        const size_t avail = kMaxLine - cur_;
        if (len > avail) {
            len = avail;   // 截断保护：宁可丢尾巴，也不能写越界
        }
        std::memcpy(buf_ + cur_, data, len);
        cur_ += len;
    }

    void append(const std::string& s) { append(s.data(), s.size()); }

    void push(char c) {
        if (cur_ < kMaxLine) {
            buf_[cur_++] = c;
        }
    }

    // ---- 常用类型重载 ----
    LogStream& operator<<(const char* s) {
        if (s != nullptr) {
            append(s, std::strlen(s));
        } else {
            append("(null)", 6);
        }
        return *this;
    }

    LogStream& operator<<(const std::string& s) {
        append(s);
        return *this;
    }

    LogStream& operator<<(char c) {
        push(c);
        return *this;
    }

    LogStream& operator<<(bool v) {
        if (v) {
            append("true", 4);
        } else {
            append("false", 5);
        }
        return *this;
    }

    // 项目里大量用 tid 验证"连接落在哪个线程"，所以专门给 thread::id 一个重载
    LogStream& operator<<(std::thread::id id) {
        std::ostringstream oss;
        oss << id;
        append(oss.str());
        return *this;
    }

    // 整型（不含 bool / char，它们已有上面的精确重载）
    template <typename T,
              typename std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                                            !std::is_same_v<T, char>,
                                        int> = 0>
    LogStream& operator<<(T v) {
        appendInteger(v);
        return *this;
    }

    // 枚举：必须单独给一个重载。否则 LogStream << SomeEnum 会在
    // operator<<(char) / operator<<(bool) / 整型模板之间产生二义性，直接编译失败。
    template <typename T, typename std::enable_if_t<std::is_enum_v<T>, int> = 0>
    LogStream& operator<<(T v) {
        appendInteger(static_cast<std::underlying_type_t<T>>(v));
        return *this;
    }

    // 浮点
    template <typename T, typename std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    LogStream& operator<<(T v) {
        char tmp[40];
        const int n = ::snprintf(tmp, sizeof(tmp), "%g", static_cast<double>(v));
        if (n > 0) {
            append(tmp, static_cast<size_t>(n));
        }
        return *this;
    }

private:
    // 整数转字符：从低位往高位填，最后倒着输出（避免记录位数的额外一趟）
    template <typename T>
    void appendInteger(T v) {
        using U = std::make_unsigned_t<T>;
        char tmp[24];
        size_t n = 0;
        U u = 0;
        bool neg = false;
        if constexpr (std::is_signed_v<T>) {
            if (v < 0) {
                neg = true;
                u = static_cast<U>(0) - static_cast<U>(v);   // 取绝对值（对最小负数也安全）
            } else {
                u = static_cast<U>(v);
            }
        } else {
            u = static_cast<U>(v);
        }
        do {
            tmp[n++] = static_cast<char>('0' + static_cast<int>(u % 10));
            u /= 10;
        } while (u != 0);
        if (neg) {
            push('-');
        }
        while (n > 0) {
            push(tmp[--n]);
        }
    }

    char buf_[kMaxLine];
    size_t cur_ = 0;
};

// ---------------------------------------------------------------------------
//  LogMessage —— 一条日志的 RAII 载体
//
//  LOG_INFO("a" << b) 展开后是：
//      LogMessage msg(__FILE__, __LINE__, kInfo);
//      msg.stream() << "a" << b;
//  于是「构造函数写头 + 用户写体 + 析构函数补尾并提交」形成完整一行。
//  把提交放在析构里，是为了**不需要**用户写任何结尾标记。
// ---------------------------------------------------------------------------
class LogMessage {
public:
    LogMessage(const char* file, int line, LogLevel level);
    ~LogMessage();

    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;

    LogStream& stream() { return stream_; }

private:
    LogStream stream_;
    LogLevel level_;
    int line_;
    const char* basename_;   // 指向 file 的 basename（不拷贝，file 是字面量，生命周期足够）
};

// ---------------------------------------------------------------------------
//  Logger —— 前端门面（全局唯一）
// ---------------------------------------------------------------------------
class Logger {
public:
    struct Options {
        std::string basename = "coronet";                        // 日志文件基名
        size_t rollSize = static_cast<size_t>(64) * 1024 * 1024;  // 单文件上限，超过即滚动
        double flushInterval = 2.0;                               // 后端最长落盘间隔（秒）
        bool console = false;   // 是否同时把日志镜像到 stderr（调试方便，压测时关掉）
    };

    // ⚠️ 这里**不能**写成 init(const Options& opt = Options{})。
    //    Options 是 Logger 的嵌套类，而默认参数出现在 Logger 的定义体内部；
    //    要构造 Options{} 就得用到它的「默认成员初始化器」
    //    （basename = "coronet" 等），但 C++ 规定默认成员初始化器在
    //    **所属类完全结束之前不可用**，且默认参数不属于"完整类上下文"。
    //    GCC 会直接报：
    //      default member initializer for 'coronet::Logger::Options::basename'
    //      required before the end of its enclosing class
    //    又因为 common.hpp 被几乎所有翻译单元包含，整个工程会一起编译失败。
    //    改为两个重载：无参版本在 logger.cpp 里定义 —— 那里 Options 已是完整类型。
    static void init(const Options& opt);
    static void init();   // 等价于 init(Options{})
    static void shutdown();
    static void append(const char* data, size_t len);   // 供 LogMessage 调用
    static void flush();                                // 请求后端尽快落盘
    static bool inited();
};

}  // namespace coronet

// ---------------------------------------------------------------------------
//  日志宏：调用形态与同步日志完全一致，所以全项目几十个调用点一处都不用改
// ---------------------------------------------------------------------------
#define LOG_DEBUG(...)                                                              \
    do {                                                                            \
        ::coronet::LogMessage _coro_log_msg(__FILE__, __LINE__, ::coronet::kDebug);  \
        _coro_log_msg.stream() << __VA_ARGS__;                                       \
    } while (0)

#define LOG_INFO(...)                                                               \
    do {                                                                            \
        ::coronet::LogMessage _coro_log_msg(__FILE__, __LINE__, ::coronet::kInfo);   \
        _coro_log_msg.stream() << __VA_ARGS__;                                       \
    } while (0)

#define LOG_WARN(...)                                                               \
    do {                                                                            \
        ::coronet::LogMessage _coro_log_msg(__FILE__, __LINE__, ::coronet::kWarn);   \
        _coro_log_msg.stream() << __VA_ARGS__;                                       \
    } while (0)

#define LOG_ERROR(...)                                                              \
    do {                                                                            \
        ::coronet::LogMessage _coro_log_msg(__FILE__, __LINE__, ::coronet::kError);  \
        _coro_log_msg.stream() << __VA_ARGS__;                                       \
    } while (0)
