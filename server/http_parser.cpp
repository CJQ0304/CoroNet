#include "http_parser.hpp"

#include <algorithm>
#include <utility>

namespace coronet {

namespace {

// 从 start 起找 "\r\n"，找不到返回 nullptr。
// 为什么不用 Buffer::findCRLF()？它固定从 peek()（readerIndex）开始找，
// 而状态机每处理完一行就 retrieve 掉，peek() 会自动前移，所以这里
// 传 buf->peek() 其实等价 —— 但显式写出"从哪开始找"更清楚，
// 也为将来支持"在已读区内回溯查找"留了口子。
const char* findCRLFFrom(const Buffer* buf, const char* start) {
    const char* end = buf->beginWriteConst();
    const char* p = std::search(start, end, "\r\n", "\r\n" + 2);
    return (p == end) ? nullptr : p;
}

}  // namespace

// ---------------------------------------------------------------------------
//  主状态机
// ---------------------------------------------------------------------------
HttpContext::ParseResult HttpContext::parse(Buffer* buf) {
    while (true) {
        switch (state_) {
            // -------------------------------------------------------------
            //  ① 请求行：GET /index.html?x=1 HTTP/1.1\r\n
            // -------------------------------------------------------------
            case State::kExpectRequestLine: {
                const char* crlf = findCRLFFrom(buf, buf->peek());
                if (crlf == nullptr) {
                    // 还没见到 CRLF 就已经超出上限 —— 典型的"慢速/超长请求行"
                    // 攻击特征，直接拒绝，不必等它把内存撑满。
                    if (buf->readableBytes() > kMaxRequestLineBytes) {
                        fail(414, "request line too long");
                        return ParseResult::kError;
                    }
                    return ParseResult::kIncomplete;
                }
                if (static_cast<size_t>(crlf - buf->peek()) > kMaxRequestLineBytes) {
                    fail(414, "request line too long");
                    return ParseResult::kError;
                }
                if (!parseRequestLine(buf->peek(), crlf)) {
                    fail(400, "malformed request line");
                    return ParseResult::kError;
                }
                // 确认解析成功了才消费字节：失败时数据留着也没意义（要断开），
                // 但"先验证后消费"是解析器不破坏缓冲区的基本纪律。
                buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));
                state_ = State::kExpectHeaders;
                break;
            }

            // -------------------------------------------------------------
            //  ② 头部：Host: x\r\nUser-Agent: y\r\n ... 直到一个空行
            // -------------------------------------------------------------
            case State::kExpectHeaders: {
                const char* crlf = findCRLFFrom(buf, buf->peek());
                if (crlf == nullptr) {
                    // 头部总量超限：同样提前拒绝
                    if (headerBytes_ + buf->readableBytes() > kMaxHeaderBytes) {
                        fail(431, "headers too large");
                        return ParseResult::kError;
                    }
                    return ParseResult::kIncomplete;
                }

                if (crlf == buf->peek()) {
                    // 空行 = 头部结束
                    buf->retrieve(2);
                    if (contentLength_ > 0) {
                        state_ = State::kExpectBody;   // 还要等 body
                        break;
                    }
                    state_ = State::kGotAll;           // 没有 body，请求完成
                    return ParseResult::kComplete;
                }

                headerBytes_ += static_cast<size_t>(crlf + 2 - buf->peek());
                ++headerCount_;
                if (headerBytes_ > kMaxHeaderBytes || headerCount_ > kMaxHeaderCount) {
                    fail(431, "too many or too large headers");
                    return ParseResult::kError;
                }

                if (!parseHeaderLine(buf->peek(), crlf)) {
                    // parseHeaderLine 内部可能已经给出了更精确的状态码
                    // （例如 Content-Length 超限 -> 413），此时不要用 400 覆盖它。
                    if (errorStatus_ == 200) {
                        fail(400, "malformed header");
                    }
                    return ParseResult::kError;
                }
                buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));
                break;
            }

            // -------------------------------------------------------------
            //  ③ body：按 Content-Length 等够字节
            // -------------------------------------------------------------
            case State::kExpectBody: {
                if (buf->readableBytes() < contentLength_) {
                    return ParseResult::kIncomplete;   // 半包：一个字节都不消费，等下次
                }
                request_.setBody(buf->retrieveAsString(contentLength_));
                state_ = State::kGotAll;
                return ParseResult::kComplete;
            }

            // -------------------------------------------------------------
            //  ④ 已完成：上层还没 reset() 就又来 parse，直接告知"已完成"（幂等）
            //     这样调用方可以无条件地"第一次 parse 后第二次再 parse"而不会出错。
            // -------------------------------------------------------------
            case State::kGotAll:
                return ParseResult::kComplete;
        }
    }
}

// ---------------------------------------------------------------------------
//  请求行解析
// ---------------------------------------------------------------------------
bool HttpContext::parseRequestLine(const char* begin, const char* end) {
    // 用 string_view 在原始字节切片上"零拷贝"地定位各字段，
    // 只在最后落库时才分配（setPath/setQuery 各一次）。
    const std::string_view line(begin, static_cast<size_t>(end - begin));

    const size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        return false;
    }
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        return false;
    }
    // 只允许恰好两个空格：多出来的空格会让"路径里能不能含空格"变得歧义，
    // 严格拒绝比"猜"更安全（真实客户端也不会那么发）。
    if (line.find(' ', sp2 + 1) != std::string_view::npos) {
        return false;
    }

    const std::string_view methodStr = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view versionStr = trim(line.substr(sp2 + 1));

    const HttpMethod method = stringToMethod(methodStr);
    if (method == HttpMethod::kInvalid) {
        return false;
    }
    request_.setMethod(method);

    // 只支持 origin-form（以 '/' 开头）。绝对形式（GET http://host/path）与
    // authority 形式（CONNECT host:port）不处理 —— 代理场景才需要。
    if (target.empty() || target[0] != '/') {
        return false;
    }

    const size_t qmark = target.find('?');
    if (qmark == std::string_view::npos) {
        request_.setPath(std::string(target));
        request_.setQuery(std::string());
    } else {
        request_.setPath(std::string(target.substr(0, qmark)));
        request_.setQuery(std::string(target.substr(qmark + 1)));
    }

    if (versionStr == "HTTP/1.1") {
        request_.setVersion(HttpVersion::kHttp11);
    } else if (versionStr == "HTTP/1.0") {
        request_.setVersion(HttpVersion::kHttp10);
    } else {
        return false;   // 不认识 HTTP/0.9 或其他版本
    }

    return true;
}

// ---------------------------------------------------------------------------
//  头部行解析：Key: Value
// ---------------------------------------------------------------------------
bool HttpContext::parseHeaderLine(const char* begin, const char* end) {
    const std::string_view line(begin, static_cast<size_t>(end - begin));

    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;   // 没有冒号，或冒号在开头（空字段名）
    }

    const std::string_view key = trim(line.substr(0, colon));
    const std::string_view value = trim(line.substr(colon + 1));
    if (key.empty()) {
        return false;
    }

    if (iequals(key, "content-length")) {
        // 手工解析十进制，**不用 std::stoi**：解析器里靠异常做流程控制既慢
        // 又难查（stoi 会因超范围抛 out_of_range），而且我们要在超限的第一
        // 时间就拒绝，而不是先算出一个巨大的数。
        if (value.empty() || value.size() > 19) {
            return false;
        }
        size_t len = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') {
                return false;
            }
            len = len * 10 + static_cast<size_t>(c - '0');
            if (len > kMaxBodyBytes) {
                fail(413, "body too large");
                return false;
            }
        }
        contentLength_ = len;
    }

    request_.addHeader(std::string(key), std::string(value));
    return true;
}

// ---------------------------------------------------------------------------
//  reset：keep-alive 的关键
// ---------------------------------------------------------------------------
void HttpContext::reset() {
    state_ = State::kExpectRequestLine;
    request_.clear();   // 保留 capacity，不重新分配
    contentLength_ = 0;
    headerBytes_ = 0;
    headerCount_ = 0;
    errorStatus_ = 200;
    errorMessage_.clear();
}

}  // namespace coronet
