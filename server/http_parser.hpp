#pragma once

// ============================================================================
//  HttpContext —— HTTP 请求的「增量状态机」解析器
//
//  ── 为什么必须用状态机，不能用"等收全了再正则/字符串查找一次解析"？
//
//  1. **TCP 是字节流，没有消息边界。** 一次 read 可能只给你半个请求
//     （"GET /ind"），也可能一次给你三个请求（流水线）。所以解析器必须能
//     "吃多少算多少、剩下的等下次"，而正则那种"一次性匹配"模式做不到。
//
//  2. **不能凭"收到 1 个字节就先攒着"来简化。** 那样每个请求都要在内存里
//     攒完整份报文，且遇到恶意客户端（发了 10MB 头部再不发完）会把内存吃光。
//     状态机边解析边消费（`buf->retrieve`），内存占用只跟"当前处理的这一行"
//     有关，且能对超长行/超多头**及时**返回 414/431 并断开。
//
//  3. **状态机天然可恢复。** 挂起点（协程 co_await）随时可能发生，函数必须
//     能"停在中间、下次从断点继续"，这正是状态机的语义。
//
//  ── 三个状态与转移
//
//      kExpectRequestLine --(见到 \r\n)--> kExpectHeaders
//      kExpectHeaders     --(见到空行 \r\n)--> [有 Content-Length ? kExpectBody : kGotAll]
//      kExpectBody        --(收满 Content-Length 字节)--> kGotAll
//      kGotAll            --(上层处理完并 reset())--> kExpectRequestLine
//
//  ── 返回值三态（比 bool 多一态，上层才好区分"继续读"和"该报错了"）
//      kComplete   解析出一个完整请求，request() 可用
//      kIncomplete 数据不够，什么都没消费完，等更多数据
//      kError      报文非法/超限，用 errorStatus() + errorMessage() 拼错误响应
//
//  ── 与 Buffer 的配合
//      解析器只从 Buffer 里 `retrieve` 掉"已确认解析完毕"的字节，
//      所以调用方可以放心地继续 `append` 新数据，不会打乱解析位置。
// ============================================================================

#include <cstddef>
#include <string>

#include "buffer.hpp"
#include "http_request.hpp"

namespace coronet {

class HttpContext {
public:
    enum class ParseResult {
        kComplete,
        kIncomplete,
        kError,
    };

    // ---- 防滥用限制 ----------------------------------------------------
    // 这些都是"必须存在"的东西：没有它们，一个恶意客户端只要发一行
    // 无限长的请求行，就能让服务端把内存吃满。真实项目里还应配合
    // "N 秒内必须发完整个头部"的时间限制。
    static constexpr size_t kMaxRequestLineBytes = 8 * 1024;          // 请求行上限
    static constexpr size_t kMaxHeaderBytes = 64 * 1024;              // 所有头部合计
    static constexpr size_t kMaxHeaderCount = 128;                    // 头部条数
    static constexpr size_t kMaxBodyBytes = 16 * 1024 * 1024;         // body 上限

    HttpContext() = default;

    // 增量解析：从 buf 里"能吃多少吃多少"。
    // 返回 kIncomplete 时只消费了能确定的部分，剩余字节仍留在 buf 里。
    ParseResult parse(Buffer* buf);

    // 是否已经解析出一个完整请求（kComplete 之后、reset() 之前为 true）
    bool gotAll() const { return state_ == State::kGotAll; }

    const HttpRequest& request() const { return request_; }

    // 出错时给上层参考的状态码与原因（400 / 413 / 414 / 431）
    int errorStatus() const { return errorStatus_; }
    const std::string& errorMessage() const { return errorMessage_; }

    // 一个请求处理完毕，准备解析同一连接上的下一个请求（keep-alive）。
    // 注意：要**保留 capacity**，所以用 clear() 而不是重建对象。
    void reset();

private:
    enum class State {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll,
    };

    // 解析请求行："GET /path?query HTTP/1.1"（范围 [begin, end)，不含 CRLF）
    bool parseRequestLine(const char* begin, const char* end);
    // 解析一个头部行："Key: Value"
    bool parseHeaderLine(const char* begin, const char* end);

    void fail(int status, std::string message) {
        errorStatus_ = status;
        errorMessage_ = std::move(message);
    }

    State state_ = State::kExpectRequestLine;
    HttpRequest request_;
    size_t contentLength_ = 0;   // 从 Content-Length 解析出来，决定 kExpectBody 要等多少字节
    size_t headerBytes_ = 0;     // 已解析的头部累计字节数（配合 kMaxHeaderBytes）
    size_t headerCount_ = 0;     // 已解析的头部条数
    int errorStatus_ = 200;      // 200 表示"还没有错误"
    std::string errorMessage_;
};

}  // namespace coronet
