#pragma once

// ============================================================================
//  http_request.hpp —— 一个"已解析完成"的 HTTP 请求
//
//  为什么方法/版本用枚举而不是字符串？
//    1) 解析时做一次"字符串 -> 枚举"的映射，之后所有逻辑比较的是整数，
//       不必再到处处理大小写、拼写差异（`get` / `GET` / `Get` 只认一个）；
//    2) 分发表（路由）可以直接 switch，比字符串比较便宜得多。
//
//  为什么 headers 用 vector<pair> 而不是 map？
//    - 头部数量极少（通常 < 20），线性查找比 map 的哈希/树更划算，也更好缓存；
//    - 保留原始顺序（某些协议场景下顺序有意义），便于原样回显/调试；
//    - 没有额外节点分配，配合 clear() 保留 capacity，keep-alive 下稳态零分配。
//
//  注意：getHeader() 返回的是**引用**，指向本请求内部存储；
//       只在"本次请求处理期间"有效，不要存到下一个请求之后。
// ============================================================================

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coronet {

// ---------------------------------------------------------------------------
//  HTTP 方法
// ---------------------------------------------------------------------------
enum class HttpMethod { kInvalid, kGet, kHead, kPost, kPut, kDelete, kOptions };

inline const char* methodToString(HttpMethod m) {
    switch (m) {
        case HttpMethod::kGet:     return "GET";
        case HttpMethod::kHead:    return "HEAD";
        case HttpMethod::kPost:    return "POST";
        case HttpMethod::kPut:     return "PUT";
        case HttpMethod::kDelete:  return "DELETE";
        case HttpMethod::kOptions: return "OPTIONS";
        case HttpMethod::kInvalid: break;
    }
    return "INVALID";
}

inline HttpMethod stringToMethod(std::string_view s) {
    if (s == "GET")     return HttpMethod::kGet;
    if (s == "HEAD")    return HttpMethod::kHead;
    if (s == "POST")    return HttpMethod::kPost;
    if (s == "PUT")     return HttpMethod::kPut;
    if (s == "DELETE")  return HttpMethod::kDelete;
    if (s == "OPTIONS") return HttpMethod::kOptions;
    return HttpMethod::kInvalid;
}

enum class HttpVersion { kUnknown, kHttp10, kHttp11 };

inline const char* versionToString(HttpVersion v) {
    switch (v) {
        case HttpVersion::kHttp10: return "HTTP/1.0";
        case HttpVersion::kHttp11: return "HTTP/1.1";
        case HttpVersion::kUnknown: break;
    }
    return "HTTP/?";
}

// ---------------------------------------------------------------------------
//  两个解析器要用的小工具
// ---------------------------------------------------------------------------

// 大小写不敏感比较。HTTP 的"头部字段名"和 Connection 的值都不区分大小写，
// 所以不能用 ==。手写 A-Z 归一化而不用 std::tolower：避免 <cctype> 的
// locale 依赖（多线程下 locale 相关的 ctype 表操作是有代价的）。
inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// 去掉首尾的空白（HTTP 头部 `Key: Value` 冒号后的空格必须处理）
inline std::string_view trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
//  HttpRequest
// ---------------------------------------------------------------------------
class HttpRequest {
public:
    HttpRequest() = default;

    // ---- 请求行 ----
    HttpMethod method() const { return method_; }
    void setMethod(HttpMethod m) { method_ = m; }

    HttpVersion version() const { return version_; }
    void setVersion(HttpVersion v) { version_ = v; }

    // path 不含 query（/echo?a=1 的 path 是 "/echo"）
    const std::string& path() const { return path_; }
    void setPath(std::string p) { path_ = std::move(p); }

    const std::string& query() const { return query_; }
    void setQuery(std::string q) { query_ = std::move(q); }

    // ---- body（长度由 Content-Length 决定）----
    const std::string& body() const { return body_; }
    void setBody(std::string b) { body_ = std::move(b); }

    // ---- 头部 ----
    void addHeader(std::string key, std::string value) {
        headers_.emplace_back(std::move(key), std::move(value));
    }

    size_t headerCount() const { return headers_.size(); }

    // 找不到时返回一个空串（静态对象，安全返回引用）；用 hasHeader 区分"空值"与"不存在"
    const std::string& getHeader(std::string_view field) const {
        for (const auto& kv : headers_) {
            if (iequals(kv.first, field)) {
                return kv.second;
            }
        }
        return emptyString();
    }

    bool hasHeader(std::string_view field) const {
        for (const auto& kv : headers_) {
            if (iequals(kv.first, field)) {
                return true;
            }
        }
        return false;
    }

    // 取 query 参数：GET /echo?msg=hi&x=1 -> getQueryParam("msg") == "hi"
    // 返回 string_view 指向 query_ 内部，同样只在本次请求期间有效。
    // 说明：这里故意**不做 URL 解码**（%XX / '+'），因为服务端只需演示解析骨架；
    //       真实项目要补 urlDecode，否则中文、'&' 之类的参数会解析错。
    std::string_view getQueryParam(std::string_view key) const {
        const std::string_view q(query_);
        size_t pos = 0;
        while (pos <= q.size()) {
            const size_t amp = q.find('&', pos);
            const size_t end = (amp == std::string_view::npos) ? q.size() : amp;
            const std::string_view pair = q.substr(pos, end - pos);
            const size_t eq = pair.find('=');
            if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
                return pair.substr(eq + 1);
            }
            if (amp == std::string_view::npos) {
                break;
            }
            pos = amp + 1;
        }
        return {};
    }

    // keep-alive：处理完一个请求后复用同一个对象，清空内容但保留 capacity，
    // 于是"100 个请求"在稳态下几乎不再发生堆分配。
    void clear() {
        method_ = HttpMethod::kInvalid;
        version_ = HttpVersion::kUnknown;
        path_.clear();
        query_.clear();
        body_.clear();
        headers_.clear();   // vector::clear 不释放 capacity
    }

private:
    static const std::string& emptyString() {
        static const std::string kEmpty;
        return kEmpty;
    }

    HttpMethod method_ = HttpMethod::kInvalid;
    HttpVersion version_ = HttpVersion::kUnknown;
    std::string path_;
    std::string query_;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> headers_;
};

}  // namespace coronet
