// ============================================================================
//  buffer_test.cpp —— Buffer（应用层缓冲区）单元测试
//
//  Buffer 的所有索引运算（readerIndex_ / writerIndex_ /
//  prependable）都是"差一位就静默出错"的类型 —— 越界不崩、只是数据错位，
//  而这类 bug 在压测里表现为"偶发乱码"，定位成本很高。
//
//  覆盖：
//    ① 三个指针的初始状态与基本推进
//    ② retrieve 越界等价于 retrieveAll（不能留下脏索引）
//    ③ 扩容必须是"倍增"（否则连续 append 的均摊成本退化到 O(n²)）
//    ④ 空间复用：读完之后再写，应当先搬移复用前置空闲区，而不是无脑扩容
//    ⑤ findCRLF 的边界（无 CRLF / 只有 '\r' / 就在开头）
//    ⑥ 空闲瘦身：容量曾涨到阈值以上才缩，偶发大包不该永久占内存
//
//  注意：这里**不测** readFd / writeFd —— 它们直接对 fd 做 readv/write，
//        属于"与内核交互"，要测就得用 socketpair，留给集成测试。
// ============================================================================

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "buffer.hpp"

namespace {

// Buffer 构造后内部 vector 的长度 = kCheapPrepend + initialSize，
// 这三个常量决定了下面所有断言的期望值，先钉死它们。
constexpr size_t kInit = Buffer::kInitialSize;            // 1024
constexpr size_t kPre  = Buffer::kCheapPrepend;           // 8
constexpr size_t kCap  = kPre + kInit;                    // 1032

}  // namespace

// ---------------------------------------------------------------------------
//  ① 初始状态与基本推进
// ---------------------------------------------------------------------------

TEST(BufferTest, InitialStateIsEmpty) {
    Buffer buf;

    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_EQ(buf.writableBytes(), kInit);
    EXPECT_EQ(buf.prependableBytes(), kPre);   // 预留头部在 readerIndex 之前
}

TEST(BufferTest, AppendThenRetrieve) {
    Buffer buf;

    buf.append("hello", 5);
    EXPECT_EQ(buf.readableBytes(), 5u);
    EXPECT_EQ(buf.writableBytes(), kInit - 5);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), std::string("hello"));

    buf.retrieve(2);
    EXPECT_EQ(buf.readableBytes(), 3u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), std::string("llo"));
    // 读过的空间变成 prependable，后续可以复用
    EXPECT_EQ(buf.prependableBytes(), kPre + 2);
}

TEST(BufferTest, AppendStringOverload) {
    Buffer buf;
    const std::string s("coronet");

    buf.append(s);
    EXPECT_EQ(buf.readableBytes(), s.size());
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), s);
}

TEST(BufferTest, RetrieveAllResetsIndicesButKeepsCapacity) {
    Buffer buf;

    buf.append("abcdef", 6);
    buf.retrieveAll();

    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_EQ(buf.prependableBytes(), kPre);   // 索引归位（不是留在 6）
    EXPECT_EQ(buf.writableBytes(), kInit);     // 容量没被释放，下次还能直接用
}

// ---------------------------------------------------------------------------
//  ② retrieve 越界 = retrieveAll
// ---------------------------------------------------------------------------

TEST(BufferTest, RetrieveMoreThanReadableBecomesRetrieveAll) {
    Buffer buf;

    buf.append("abc", 3);
    buf.retrieve(100);   // 请求消费的比可读的还多

    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_EQ(buf.prependableBytes(), kPre);   // 必须彻底归位，不能留下负的可读长度
}

// ---------------------------------------------------------------------------
//  ③ 扩容：倍增
// ---------------------------------------------------------------------------

TEST(BufferTest, GrowthIsDoublingNotExactFit) {
    Buffer buf;

    // 写 2000 字节，初始 capacity 1032 不够 -> 倍增到 2064
    const std::string big(2000, 'x');
    buf.append(big);

    EXPECT_EQ(buf.readableBytes(), 2000u);
    // 若实现是"缺多少补多少"，这里会恰好是 0；倍增实现留有余量
    EXPECT_EQ(buf.writableBytes(), kCap * 2 - kPre - 2000);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), big);
}

// ---------------------------------------------------------------------------
//  ④ 空间复用：先读后写应搬移复用，而不是无脑扩容
// ---------------------------------------------------------------------------

TEST(BufferTest, WriteReusesPrependSpaceInsteadOfGrowing) {
    Buffer buf;

    buf.append(std::string(1000, 'a'));
    buf.retrieve(900);                  // 前置区腾出 900 字节
    ASSERT_EQ(buf.readableBytes(), 100u);
    ASSERT_EQ(buf.prependableBytes(), kPre + 900);

    buf.append(std::string(900, 'b'));  // 这 900 字节应该塞进前置区

    EXPECT_EQ(buf.readableBytes(), 1000u);
    // 搬移复用后，可读区被搬到最前面，prependable 退回最小值
    EXPECT_EQ(buf.prependableBytes(), kPre);
    // 没扩容：容量仍是初始值，所以可写区恰好是"初始容量 - 已用"
    EXPECT_EQ(buf.writableBytes(), kCap - kPre - 1000);
}

// ---------------------------------------------------------------------------
//  ⑤ findCRLF 边界
// ---------------------------------------------------------------------------

TEST(BufferTest, FindCRLFFindsFirstOccurrence) {
    Buffer buf;
    buf.append("abc\r\ndef\r\n", 10);

    const char* crlf = buf.findCRLF();
    ASSERT_NE(crlf, nullptr);
    EXPECT_EQ(static_cast<size_t>(crlf - buf.peek()), 3u);
}

TEST(BufferTest, FindCRLFReturnsNullWhenAbsent) {
    Buffer buf;
    buf.append("abcdef", 6);

    EXPECT_EQ(buf.findCRLF(), nullptr);
}

TEST(BufferTest, FindCRLFReturnsNullWhenOnlyCarriageReturnPresent) {
    // 只有一个 '\r' 就到头了 —— 这是最容易越界读的一个边界
    Buffer buf;
    buf.append("abc\r", 4);

    EXPECT_EQ(buf.findCRLF(), nullptr);
}

TEST(BufferTest, FindCRLFAtStartIsPeek) {
    Buffer buf;
    buf.append("\r\n", 2);

    const char* crlf = buf.findCRLF();
    ASSERT_NE(crlf, nullptr);
    EXPECT_EQ(crlf, buf.peek());
}

// ---------------------------------------------------------------------------
//  ⑥ 取走数据：retrieveAsString / retrieveAllAsString
// ---------------------------------------------------------------------------

TEST(BufferTest, RetrieveAllAsStringEmptiesBuffer) {
    Buffer buf;
    buf.append("hello", 5);

    EXPECT_EQ(buf.retrieveAllAsString(), std::string("hello"));
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(BufferTest, RetrieveAsStringPartial) {
    Buffer buf;
    buf.append("hello world", 11);

    // 注意先把结果存下来再断言：retrieveAsString 有副作用（消费数据），
    // 直接写进 EXPECT_EQ 的实参位置虽然 gtest 也能正确处理，
    // 但"断言里藏副作用"是坏味道，写出来读的人会以为没消费。
    const std::string got = buf.retrieveAsString(5);

    EXPECT_EQ(got, std::string("hello"));
    EXPECT_EQ(buf.readableBytes(), 6u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), std::string(" world"));
}

// ---------------------------------------------------------------------------
//  ⑦ 空闲瘦身：只有涨到阈值以上才缩
// ---------------------------------------------------------------------------

TEST(BufferTest, ShrinksAfterHugeBufferBecomesIdle) {
    Buffer buf;

    // 600 KiB > kShrinkThreshold(512 KiB)，读完应当缩回初始大小，
    // 否则"偶发一个大包"会让这个连接永久占着 600 KiB
    buf.append(std::string(600 * 1024, 'z'));
    ASSERT_GT(buf.readableBytes(), Buffer::kShrinkThreshold);

    buf.retrieveAll();

    EXPECT_EQ(buf.writableBytes(), kInit);   // 已缩回
}

TEST(BufferTest, DoesNotShrinkWhenBelowThreshold) {
    Buffer buf;

    // 100 KiB < 512 KiB：不缩，避免"反复 resize + 拷贝"变成常态开销
    buf.append(std::string(100 * 1024, 'z'));
    buf.retrieveAll();

    EXPECT_GE(buf.writableBytes(), 100u * 1024);
}
