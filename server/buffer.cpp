#include "buffer.hpp"

#include <cerrno>
#include <sys/uio.h>
#include <unistd.h>

// readv 分散读：一次系统调用把数据读进"缓冲区可写区 + 栈上临时区"两段内存。
// 好处：
//   1. 缓冲区还有空间时，数据直接进缓冲区，零额外拷贝；
//   2. 数据超过可写区时，多出来的部分先进 64KB 栈空间，再 append 进来，
//      避免因为"怕不够"而每次都扩容。
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];
    struct iovec vec[2];

    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writerIndex_ += static_cast<size_t>(n);
    } else {
        writerIndex_ = buffer_.size();
        append(extrabuf, static_cast<size_t>(n) - writable);
    }
    return n;
}

// 一次 write 把可读区尽量发出去。返回 >0 表示"写走了一段"，
// 但仍**可能没写完**（内核发送缓冲满了）——调用方靠 readableBytes()
// 是否归零来判断，而不是靠返回值。
ssize_t Buffer::writeFd(int fd, int* savedErrno) {
    const size_t readable = readableBytes();
    if (readable == 0) {
        return 0;
    }
    const ssize_t n = ::write(fd, peek(), readable);
    if (n < 0) {
        *savedErrno = errno;
        return n;
    }
    retrieve(static_cast<size_t>(n));
    return n;
}
