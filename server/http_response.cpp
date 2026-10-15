#include "http_response.hpp"

#include <cstdio>

// 复用 http_request.hpp 里的 iequals / 头部工具（本文件只是用，不依赖其类型）
#include "http_request.hpp"

namespace coronet {

const char* httpStatusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

void HttpResponse::setContentType(std::string_view type) {
    // 就地覆盖而不是 addHeader：keep-alive 下每轮都会设一次 Content-Type，
    // 如果每次都 push_back，headers_ 会随请求数一路增长（内存泄漏式的膨胀）。
    for (auto& kv : headers_) {
        if (iequals(kv.first, "Content-Type")) {
            kv.second.assign(type);
            return;
        }
    }
    addHeader("Content-Type", std::string(type));
}

void HttpResponse::appendHeaderTo(std::string* out) const {
    out->clear();
    // 头部通常 200~400 字节，一次 reserve 比让 string 反复翻倍扩容划算。
    out->reserve(256);

    // 状态行。用 snprintf 写进栈上小缓冲，再 append —— 不引入 <sstream>，
    // 也就不需要为格式化付出流对象的构造/析构与 locale 查询。
    char numbuf[16];
    const int n = std::snprintf(numbuf, sizeof(numbuf), "%d", static_cast<int>(statusCode_));
    out->append("HTTP/1.1 ");
    if (n > 0) {
        out->append(numbuf, static_cast<size_t>(n));
    }
    out->push_back(' ');
    out->append(statusMessage_.empty() ? httpStatusText(static_cast<int>(statusCode_))
                                       : statusMessage_);
    out->append("\r\n");

    // 用户显式加过的头部原样输出；同时记住哪些"特殊头"已经有了，
    // 免得下面再自动补一遍导致重复字段（重复的 Content-Length 会让
    // 代理/客户端行为不可预期，属于协议层面的硬错误）。
    bool hasContentLength = false;
    bool hasConnection = false;
    for (const auto& kv : headers_) {
        out->append(kv.first);
        out->append(": ");
        out->append(kv.second);
        out->append("\r\n");
        if (iequals(kv.first, "Content-Length")) {
            hasContentLength = true;
        } else if (iequals(kv.first, "Connection")) {
            hasConnection = true;
        }
    }

    // 自动补 Content-Length：HTTP/1.1 的 keep-alive 必须靠它划分消息边界，
    // 否则客户端根本不知道一个响应在哪里结束（204 除外，它按定义没有体）。
    if (!hasContentLength && statusCode_ != HttpStatus::k204NoContent) {
        char lenbuf[32];
        const int ln = std::snprintf(lenbuf, sizeof(lenbuf), "%zu", body_.size());
        out->append("Content-Length: ");
        if (ln > 0) {
            out->append(lenbuf, static_cast<size_t>(ln));
        }
        out->append("\r\n");
    }

    if (!hasConnection) {
        out->append(close_ ? "Connection: close\r\n" : "Connection: keep-alive\r\n");
    }

    out->append("\r\n");   // 空行：头部结束的标志
}

void HttpResponse::clear(bool close) {
    close_ = close;
    statusCode_ = HttpStatus::k200Ok;
    statusMessage_.clear();
    headers_.clear();   // 保留 capacity（见头文件说明）
    body_.clear();
}

}  // namespace coronet
