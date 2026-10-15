// ============================================================================
//  http_parser_test.cpp —— HttpContext（HTTP 增量状态机解析器）单元测试
//
//  它是全项目里**唯一一块纯逻辑**——
//  只依赖 Buffer 与 HttpRequest，不碰 epoll / socket / 协程，任何机器上
//  都能跑，且它一旦出错就是"线上直接 400/解析错乱"，最值得被测试锁住。
//
//  覆盖的六类行为：
//    ① 正常路径：GET/POST、路径与 query 分离、HTTP/1.0 与 1.1
//    ② 增量语义：逐字节喂、在每个字节位置切一刀、粘包（一次两个请求）
//    ③ keep-alive：reset 后复用同一个解析器继续解析下一个请求
//    ④ 大小写与空白：头部名不区分大小写、值自动 trim
//    ⑤ 错误路径：400 / 413 / 414 / 431 四类状态码各自的触发条件
//    ⑥ 幂等：kComplete 之后不 reset 再 parse，仍然安全返回 kComplete
//
//  约定：用 EXPECT_* 而不是 ASSERT_*（除"后面断言依赖它"的地方）——
//        一个用例里暴露尽可能多的失败信息，比在第一个断言就停下更有用。
// ============================================================================

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "buffer.hpp"
#include "http_parser.hpp"

namespace {

using coronet::HttpContext;
using coronet::HttpMethod;
using coronet::HttpVersion;
using ParseResult = HttpContext::ParseResult;

// 注意：Buffer 定义在**全局命名空间**（不在 coronet 里），所以不能写成
// `using coronet::Buffer;` —— 它天然可见。

// 一次性把整段报文喂进去
ParseResult feedAll(HttpContext& ctx, Buffer& buf, const std::string& raw) {
    buf.append(raw.data(), raw.size());
    return ctx.parse(&buf);
}

// 从头开始逐字节喂，返回"每次喂入后的返回值"。
// 一旦某一步不是 kIncomplete（完成或出错）就停止 —— 因为再喂也没意义了。
std::vector<ParseResult> feedByteByByte(HttpContext& ctx, Buffer& buf, const std::string& raw) {
    std::vector<ParseResult> history;
    for (size_t i = 0; i < raw.size(); ++i) {
        buf.append(raw.data() + i, 1);
        const ParseResult r = ctx.parse(&buf);
        history.push_back(r);
        if (r != ParseResult::kIncomplete) {
            break;
        }
    }
    return history;
}

}  // namespace

// ---------------------------------------------------------------------------
//  ① 正常路径
// ---------------------------------------------------------------------------

TEST(HttpParserTest, CompleteGetRequest) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET /plain HTTP/1.1\r\nHost: example.com\r\n\r\n"),
              ParseResult::kComplete);

    EXPECT_EQ(ctx.request().method(), HttpMethod::kGet);
    EXPECT_EQ(ctx.request().path(), std::string("/plain"));
    EXPECT_TRUE(ctx.request().query().empty());
    EXPECT_EQ(ctx.request().version(), HttpVersion::kHttp11);
    EXPECT_EQ(ctx.request().headerCount(), 1u);
    EXPECT_EQ(ctx.request().getHeader("Host"), std::string("example.com"));
    EXPECT_TRUE(ctx.request().body().empty());

    EXPECT_TRUE(ctx.gotAll());
    EXPECT_EQ(ctx.errorStatus(), 200);
    // 报文被完整消费：解析器不把已确认的字节留在缓冲区里
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(HttpParserTest, PathAndQueryAreSplit) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET /echo?msg=hi&x=1 HTTP/1.1\r\n\r\n"), ParseResult::kComplete);

    EXPECT_EQ(ctx.request().path(), std::string("/echo"));      // path 不含 '?'
    EXPECT_EQ(ctx.request().query(), std::string("msg=hi&x=1"));
    EXPECT_EQ(ctx.request().getQueryParam("msg"), std::string_view("hi"));
    EXPECT_EQ(ctx.request().getQueryParam("x"), std::string_view("1"));
    EXPECT_TRUE(ctx.request().getQueryParam("nope").empty());   // 不存在 -> 空，不抛异常
}

TEST(HttpParserTest, PostWithBody) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf,
                      "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhello world"),
              ParseResult::kComplete);

    EXPECT_EQ(ctx.request().method(), HttpMethod::kPost);
    EXPECT_EQ(ctx.request().body(), std::string("hello world"));
    EXPECT_EQ(ctx.request().getHeader("Content-Length"), std::string("11"));
    EXPECT_EQ(ctx.request().headerCount(), 2u);   // Host + Content-Length
}

TEST(HttpParserTest, Http10RequestWithoutHostIsComplete) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET / HTTP/1.0\r\n\r\n"), ParseResult::kComplete);

    EXPECT_EQ(ctx.request().version(), HttpVersion::kHttp10);
    EXPECT_EQ(ctx.request().path(), std::string("/"));
    EXPECT_EQ(ctx.request().headerCount(), 0u);
}

TEST(HttpParserTest, ContentLengthZeroCompletesWithoutBody) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n"),
              ParseResult::kComplete);

    EXPECT_TRUE(ctx.request().body().empty());
    EXPECT_EQ(buf.readableBytes(), 0u);
}

// ---------------------------------------------------------------------------
//  ② 增量语义 —— 解析器最核心的价值：半包 / 粘包都不出错
// ---------------------------------------------------------------------------

TEST(HttpParserTest, ByteByByteFeedStaysIncompleteUntilLastByte) {
    HttpContext ctx;
    Buffer buf;
    const std::string raw = "GET /plain HTTP/1.1\r\nHost: h\r\n\r\n";

    const std::vector<ParseResult> history = feedByteByByte(ctx, buf, raw);

    ASSERT_EQ(history.size(), raw.size()) << "应喂完恰好 raw.size() 个字节";
    for (size_t i = 0; i + 1 < history.size(); ++i) {
        EXPECT_EQ(history[i], ParseResult::kIncomplete)
            << "第 " << (i + 1) << " 个字节喂入后不该出结果";
    }
    EXPECT_EQ(history.back(), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().path(), std::string("/plain"));
}

TEST(HttpParserTest, SplitAtEveryByteBoundaryStillParses) {
    // 这是最值钱的一个用例：把报文在**每一个可能的位置**切成两段，
    // 覆盖所有"半包边界"。手写用例不可能穷举这些边界，循环可以。
    const std::string raw =
        "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";

    for (size_t cut = 1; cut < raw.size(); ++cut) {
        HttpContext ctx;
        Buffer buf;

        buf.append(raw.data(), cut);
        const ParseResult first = ctx.parse(&buf);
        if (first == ParseResult::kComplete) {
            continue;   // 这一刀切在报文末尾之后（不会发生），保守跳过
        }
        ASSERT_EQ(first, ParseResult::kIncomplete) << "cut=" << cut;

        buf.append(raw.data() + cut, raw.size() - cut);
        ASSERT_EQ(ctx.parse(&buf), ParseResult::kComplete) << "cut=" << cut;

        EXPECT_EQ(ctx.request().method(), HttpMethod::kPost) << "cut=" << cut;
        EXPECT_EQ(ctx.request().path(), std::string("/echo")) << "cut=" << cut;
        EXPECT_EQ(ctx.request().body(), std::string("hello")) << "cut=" << cut;
        EXPECT_EQ(buf.readableBytes(), 0u) << "cut=" << cut;
    }
}

TEST(HttpParserTest, HalfArrivedBodyIsNotConsumedUntilComplete) {
    HttpContext ctx;
    Buffer buf;

    buf.append("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    ASSERT_EQ(ctx.parse(&buf), ParseResult::kIncomplete);

    // 关键：body 一个字节都不该被"预消费"，否则重试时会丢数据
    EXPECT_FALSE(ctx.gotAll());
    EXPECT_EQ(buf.readableBytes(), 3u);

    buf.append("defghij");
    ASSERT_EQ(ctx.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().body(), std::string("abcdefghij"));
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(HttpParserTest, PipelinedRequestsOnOneConnection) {
    HttpContext ctx;
    Buffer buf;

    buf.append("GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n");

    ASSERT_EQ(ctx.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().path(), std::string("/a"));

    // keep-alive：reset 后同一个对象继续解析第二个请求（而不是重建解析器）
    ctx.reset();
    ASSERT_EQ(ctx.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().path(), std::string("/b"));
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(HttpParserTest, IncompleteWhenOnlyCarriageReturnArrived) {
    // "\r" 到了但 "\n" 没到 —— 这是 findCRLF 实现最容易踩的边界：
    // std::search 在只有 "\r" 时必须返回 end（nullptr），不能越界读。
    HttpContext ctx;
    Buffer buf;

    buf.append("GET /x HTTP/1.1\r");
    EXPECT_EQ(ctx.parse(&buf), ParseResult::kIncomplete);

    buf.append("\n\r\n");
    EXPECT_EQ(ctx.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().path(), std::string("/x"));
}

// ---------------------------------------------------------------------------
//  ③ 头部：大小写不敏感、值 trim
// ---------------------------------------------------------------------------

TEST(HttpParserTest, HeaderLookupIsCaseInsensitive) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET / HTTP/1.1\r\nConTent-LengTH: 0\r\nX-Y: z\r\n\r\n"),
              ParseResult::kComplete);

    EXPECT_TRUE(ctx.request().hasHeader("content-length"));
    EXPECT_TRUE(ctx.request().hasHeader("CONTENT-LENGTH"));
    EXPECT_EQ(ctx.request().getHeader("Content-Length"), std::string("0"));
    EXPECT_EQ(ctx.request().getHeader("x-y"), std::string("z"));
    EXPECT_FALSE(ctx.request().hasHeader("x-z"));
    // 不存在的头部返回空串引用（不是 nullptr、不抛异常）
    EXPECT_TRUE(ctx.request().getHeader("nope").empty());
}

TEST(HttpParserTest, HeaderValueIsTrimmed) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET / HTTP/1.1\r\nHost:   example.com \t\r\n\r\n"),
              ParseResult::kComplete);

    EXPECT_EQ(ctx.request().getHeader("Host"), std::string("example.com"));
}

// ---------------------------------------------------------------------------
//  ④ keep-alive 生命周期
// ---------------------------------------------------------------------------

TEST(HttpParserTest, ResetRestoresInitialState) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "POST /x HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc"),
              ParseResult::kComplete);
    ASSERT_TRUE(ctx.gotAll());

    ctx.reset();

    EXPECT_FALSE(ctx.gotAll());
    EXPECT_EQ(ctx.errorStatus(), 200);
    EXPECT_EQ(ctx.request().headerCount(), 0u);
    EXPECT_TRUE(ctx.request().path().empty());
    EXPECT_TRUE(ctx.request().body().empty());
    EXPECT_TRUE(ctx.errorMessage().empty());

    // 复用同一个对象继续解析下一个请求（这正是不重建解析器的意义）
    ASSERT_EQ(feedAll(ctx, buf, "GET /next HTTP/1.1\r\n\r\n"), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().path(), std::string("/next"));
}

TEST(HttpParserTest, ParseIsIdempotentAfterComplete) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET /a HTTP/1.1\r\n\r\n"), ParseResult::kComplete);
    // 没有 reset 就再解析：应当安全地再次告知"已完成"，而不是崩或改成错误
    EXPECT_EQ(ctx.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(ctx.request().path(), std::string("/a"));
}

// ---------------------------------------------------------------------------
//  ⑤ 错误路径（400 报文非法 / 413 body 过大 / 414 行过长 / 431 头过大）
// ---------------------------------------------------------------------------

TEST(HttpParserTest, Error400OnMalformedRequestLine) {
    {
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "GET\r\n\r\n"), ParseResult::kError);   // 缺目标与版本
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "GET /plain\r\n\r\n"), ParseResult::kError);  // 缺版本
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        // 两个连续空格："GET /plain  HTTP/1.1" —— 路径里能不能含空格是歧义的，
        // 严格拒绝比"猜"更安全
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "GET /plain  HTTP/1.1\r\n\r\n"), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
}

TEST(HttpParserTest, Error400OnUnknownMethodAndBadTargetAndVersion) {
    {
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "PATCH /x HTTP/1.1\r\n\r\n"), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        // 只支持 origin-form（以 '/' 开头），绝对形式是给代理用的
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "GET http://example.com/ HTTP/1.1\r\n\r\n"),
                  ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "GET /x HTTP/2.0\r\n\r\n"), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
}

TEST(HttpParserTest, Error400OnBadHeaderLine) {
    {
        HttpContext ctx; Buffer buf;   // 没有冒号
        ASSERT_EQ(feedAll(ctx, buf, "GET / HTTP/1.1\r\nBadHeader\r\n\r\n"), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        HttpContext ctx; Buffer buf;   // 冒号在开头 = 空字段名
        ASSERT_EQ(feedAll(ctx, buf, "GET / HTTP/1.1\r\n: value\r\n\r\n"), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        HttpContext ctx; Buffer buf;   // 冒号前只有空白，trim 后为空
        ASSERT_EQ(feedAll(ctx, buf, "GET / HTTP/1.1\r\n   : v\r\n\r\n"), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
}

TEST(HttpParserTest, Error400OnBadContentLength) {
    {
        HttpContext ctx; Buffer buf;   // 含非数字
        ASSERT_EQ(feedAll(ctx, buf, "POST / HTTP/1.1\r\nContent-Length: 12a\r\n\r\n"),
                  ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        HttpContext ctx; Buffer buf;   // 空值
        ASSERT_EQ(feedAll(ctx, buf, "POST / HTTP/1.1\r\nContent-Length: \r\n\r\n"),
                  ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
    {
        // 20 位数字会在 size_t 上溢出 —— 所以必须**先看位数再看数值**，
        // 这里期望 400（位数超限直接拒绝），而不是算出一个溢出后的怪数
        HttpContext ctx; Buffer buf;
        ASSERT_EQ(feedAll(ctx, buf, "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"),
                  ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 400);
    }
}

TEST(HttpParserTest, Error413WhenBodyTooLarge) {
    HttpContext ctx;
    Buffer buf;

    // kMaxBodyBytes = 16 MiB，17 MiB 应被拒
    ASSERT_EQ(feedAll(ctx, buf, "POST / HTTP/1.1\r\nContent-Length: 17825792\r\n\r\n"),
              ParseResult::kError);
    EXPECT_EQ(ctx.errorStatus(), 413);
}

TEST(HttpParserTest, Error414WhenRequestLineTooLong) {
    {
        // 情况一：还没见到 CRLF 就已经超限 —— 慢速攻击的典型特征，
        // 必须"提前"拒绝，不能等它把内存撑满
        HttpContext ctx; Buffer buf;
        buf.append(std::string(HttpContext::kMaxRequestLineBytes + 1, 'a'));
        ASSERT_EQ(ctx.parse(&buf), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 414);
    }
    {
        // 情况二：CRLF 到了，但这一行本身就超限
        HttpContext ctx; Buffer buf;
        const std::string raw =
            "GET /" + std::string(HttpContext::kMaxRequestLineBytes, 'a') + " HTTP/1.1\r\n\r\n";
        ASSERT_EQ(feedAll(ctx, buf, raw), ParseResult::kError);
        EXPECT_EQ(ctx.errorStatus(), 414);
    }
}

TEST(HttpParserTest, Error431WhenTooManyHeaders) {
    HttpContext ctx;
    Buffer buf;

    std::string raw = "GET / HTTP/1.1\r\n";
    for (size_t i = 0; i < HttpContext::kMaxHeaderCount + 1; ++i) {
        raw += "X: y\r\n";
    }
    raw += "\r\n";

    ASSERT_EQ(feedAll(ctx, buf, raw), ParseResult::kError);
    EXPECT_EQ(ctx.errorStatus(), 431);
}

TEST(HttpParserTest, ErrorMessageIsProvidedForErrors) {
    HttpContext ctx;
    Buffer buf;

    ASSERT_EQ(feedAll(ctx, buf, "GET /x HTTP/9.9\r\n\r\n"), ParseResult::kError);
    EXPECT_FALSE(ctx.errorMessage().empty());
    EXPECT_FALSE(ctx.gotAll());
}
