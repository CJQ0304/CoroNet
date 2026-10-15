#pragma once

// ============================================================================
//  http_response.hpp —— 服务端要发出去的 HTTP 响应
//
//  ── 设计要点一：头部与体**分开持有**（appendHeaderTo 只组头部，body 单独取）
//
//  一个 HTTP 响应在字节上天然是两块：
//      "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\n"   +   "hello world\n"
//  如果把两段拼成一个 std::string 再 write()，就多出一次 O(体长) 的 memcpy；
//  响应体越大（几 MB 的文件下载）这次拷贝越亏。
//  本类刻意**不提供**"拼成一整串"的接口，逼着调用方走 writev 两段聚集写。
//
//  ── 设计要点二：对象可复用，keep-alive 下稳态零分配
//
//  clear() 用的是 string::clear() / vector::clear()，它们**只销毁元素、保留
//  capacity**。于是一个连接处理 100 万个请求，稳态下 headers_ 的 vector 与
//  body_ / statusMessage_ 的 string 都不再触发堆分配。
//  这也是"对象池"的一种轻量做法：不需要全局池 + 锁，**每连接一个实例**就够了
//  （因为一个连接固定在一个 IO 线程上，天然没有竞争）。
// ============================================================================

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coronet {

// 只收录本项目真正会返回的状态码。写全 HTTP 状态码表没有意义——
// 用不到的状态码不会出现，多一行就多一处可能抄错的地方。
enum class HttpStatus {
    k200Ok = 200,
    k204NoContent = 204,
    k301MovedPermanently = 301,
    k400BadRequest = 400,
    k404NotFound = 404,
    k405MethodNotAllowed = 405,
    k408RequestTimeout = 408,
    k413PayloadTooLarge = 413,
    k414UriTooLong = 414,
    k431RequestHeaderFieldsTooLarge = 431,
    k500InternalServerError = 500,
};

// 状态码 -> 原因短语（"404" -> "Not Found"）；未知码返回 "Unknown"
const char* httpStatusText(int code);

class HttpResponse {
public:
    explicit HttpResponse(bool close = false) : close_(close) {}

    // ---- 状态行 ----
    void setStatusCode(HttpStatus code) { statusCode_ = code; }
    HttpStatus statusCode() const { return statusCode_; }
    // 覆盖默认原因短语（一般不需要）
    void setStatusMessage(std::string msg) { statusMessage_ = std::move(msg); }

    // ---- 连接管理 ----
    void setCloseConnection(bool on) { close_ = on; }
    bool closeConnection() const { return close_; }

    // ---- 头部 ----
    void addHeader(std::string key, std::string value) {
        headers_.emplace_back(std::move(key), std::move(value));
    }
    void setContentType(std::string_view type);
    // Content-Type: text/plain; charset=utf-8

    // ---- body ----
    void setBody(std::string body) { body_ = std::move(body); }
    void setBody(const char* cstr) { body_.assign(cstr); }
    void setBody(const char* data, size_t len) { body_.assign(data, len); }

    const std::string& body() const { return body_; }

    // 把 body 移动出来（零拷贝）。调用时机必须在 appendHeaderTo 之后——
    // 因为 Content-Length 是从 body_.size() 算出来的。
    // 移动后 body_ 处于"有效但未指定"状态，调用方随后应 clear()。
    std::string takeBody() { return std::move(body_); }

    // 组装"状态行 + 头部"，写进 out（**不含 body**）。
    // out 会被清空并预留 ~256 字节（头部就那么大），避免多次扩容。
    void appendHeaderTo(std::string* out) const;

    // keep-alive 里复用：清空内容、保留 capacity、重设连接语义。
    // 不能省掉这一步（会有上一个请求的头部残留）。
    void clear(bool close);

private:
    bool close_;
    HttpStatus statusCode_ = HttpStatus::k200Ok;
    std::string statusMessage_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

}  // namespace coronet
