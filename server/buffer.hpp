#pragma once

// ============================================================================
//  Buffer —— 应用层缓冲区
//
//  为什么需要它？TCP 是"字节流"，没有消息边界：
//    - 读：一次 read 可能只读到半条消息（半包），也可能读到多条（粘包）；
//    - 写：一次 write 可能写不完（内核发送缓冲区满了），剩余的要缓存起来等可写。
//  所以每个连接都需要一个"输入缓冲 + 输出缓冲"。
//
//  内存布局（readerIndex_ / writerIndex_ 把 vector 切成三段）：
//    +----------------+-----------------+------------------+
//    |  prependable   |    readable     |     writable     |
//    +----------------+-----------------+------------------+
//    0            readerIndex_      writerIndex_        size()
// ============================================================================

#include <algorithm>
#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

class Buffer {
public:
    // 注意用 `static constexpr` 而不是 `static const`：
    // C++17 起 constexpr 静态成员是**隐式 inline** 的，无需类外再给定义；
    // 而 static const 整型一旦被"取地址/绑引用"（例如把 kShrinkThreshold
    // 传进 EXPECT_EQ 这类模板函数）就是 ODR-use，会报 undefined reference。
    static constexpr size_t kCheapPrepend = 8;    // 预留头部，方便前插（HTTP 组装时用）
    static constexpr size_t kInitialSize = 1024;

    // 两个阈值（扩容上限与收缩门槛）：
    //   kMaxCapacity     —— 倍增扩容的上限，超过后改为"按需精确增长"，
    //                       防止一个超大包把单连接内存吃到失控；
    //   kShrinkThreshold —— 空闲时若容量曾经涨到这么大以上，就缩回初始大小，
    //                       否则"偶发一个大包"会让每个连接永久占着大块内存。
    static constexpr size_t kMaxCapacity = 64 * 1024 * 1024;
    static constexpr size_t kShrinkThreshold = 512 * 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }

    // 查找 "\r\n"（解析 HTTP 请求行时用得上）
    const char* findCRLF() const {
        const char* crlf = std::search(peek(), beginWriteConst(), "\r\n", "\r\n" + 2);
        return crlf == beginWriteConst() ? nullptr : crlf;
    }

    // 消费掉 len 字节
    void retrieve(size_t len) {
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
        // 读完顺手"瘦身"。连接的生命周期很长，一旦某次收到了大消息
        // 把 buffer_ 撑到几 MB，不缩回去就会一直占着——十万连接就是几十 GB
        // 的隐形开销。这里只在**空闲**状态缩，并加阈值避免频繁 realloc。
        if (buffer_.size() > kShrinkThreshold) {
            buffer_.resize(kCheapPrepend + kInitialSize);
            buffer_.shrink_to_fit();
        }
    }

    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

    std::string retrieveAsString(size_t len) {
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
    }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::copy(data, data + len, beginWrite());
        writerIndex_ += len;
    }

    void append(const std::string& str) { append(str.data(), str.size()); }

    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWriteConst() const { return begin() + writerIndex_; }
    void hasWritten(size_t len) { writerIndex_ += len; }

    // 核心：从 fd 读数据到缓冲区（用 readv 分散读，避免小块堆分配）
    // 返回读到的字节数；<0 时把 errno 写入 *savedErrno
    ssize_t readFd(int fd, int* savedErrno);

    // 把可读区数据写给 fd（一次 write），返回写入字节数；<0 时写 *savedErrno。
    // 与 readFd 对称，把"跟内核打交道"的细节收拢在 Buffer 内部；
    // 调用方（TcpConnection）只关心"还剩多少没写出去"。
    //
    // 为什么这里不用 writev？因为待发数据在缓冲区里本来就是**连续**的一段，
    // 一次 write 就够。writev 的价值在于"发送分散的几段"（例如 HTTP 响应头 +
    // 响应体、或"上次没写完的残留 + 本次新数据"），解析 HTTP 时用得上。
    ssize_t writeFd(int fd, int* savedErrno);

private:
    char* begin() { return buffer_.data(); }
    const char* begin() const { return buffer_.data(); }

    // 空间不足时：要么扩容，要么把已读数据挪到前面复用
    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            // 需要真正的扩容：按 2 倍增长。若"缺多少补多少"，连续小块追加会
            // 反复触发 resize + 整块拷贝，均摊成本会退化到 O(n²)。
            size_t newCap = buffer_.size() * 2;
            if (newCap < writerIndex_ + len) {
                newCap = writerIndex_ + len;   // 一次就要很大，直接给够
            }
            if (newCap > kMaxCapacity) {
                newCap = writerIndex_ + len;   // 触顶后不再倍增，改为精确增长
            }
            buffer_.resize(newCap);
        } else {
            const size_t readable = readableBytes();
            std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};
