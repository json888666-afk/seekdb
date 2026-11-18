#pragma once
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <deque>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace mini {
using json = nlohmann::json;
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, std::string m) : std::runtime_error(m), code(std::move(c)) {}
};
inline void require(bool b, std::string_view code, std::string_view msg) {
    if (!b)
        throw Error(std::string(code), std::string(msg));
}
inline std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return s;
}
// 词法扫描逐字节判定字符类。std::isspace/isdigit 等在 glibc 里要经
// __ctype_b_loc() 取表，每次调用都是外部函数开销；批量 INSERT 的语句有
// 十几万字符，改成本地内联比较后词法阶段不再是写入热点。
inline bool char_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
inline bool char_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}
inline bool char_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool char_alnum(unsigned char c) {
    return char_alpha(c) || char_digit(c);
}
using Value = std::variant<int64_t, std::string, std::vector<float>, double>;
inline json value_json(const Value &v) {
    return std::visit([](auto &&x) -> json { return x; }, v);
}

// 手写紧凑 JSON 输出，绕开 nlohmann 的通用序列化开销。保证 round-trip
// 精度（double 用 max_digits10）。所有函数操作 std::ostream。

inline std::ostream &write_json_string(std::ostream &out, std::string_view s) {
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[16];
                    int n = std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out.write(buf, n);
                } else
                    out.put(static_cast<char>(c));
        }
    }
    out << '"';
    return out;
}

inline std::ostream &write_json_double(std::ostream &out, double d) {
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), d,
                                 std::chars_format::general,
                                 std::numeric_limits<double>::max_digits10);
    if (ec == std::errc{}) {
        out.write(buf, p - buf);
    } else {
        int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
        out.write(buf, n);
    }
    return out;
}

inline std::ostream &write_json_float(std::ostream &out, float f) {
    return write_json_double(out, static_cast<double>(f));
}

inline std::ostream &write_json_int64(std::ostream &out, int64_t n) {
    char buf[32];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    if (ec == std::errc{})
        out.write(buf, p - buf);
    return out;
}

inline std::ostream &write_value(std::ostream &out, const Value &v) {
    std::visit([&](auto &&x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) {
            write_json_int64(out, x);
        } else if constexpr (std::is_same_v<T, double>) {
            write_json_double(out, x);
        } else if constexpr (std::is_same_v<T, std::string>) {
            write_json_string(out, x);
        } else if constexpr (std::is_same_v<T, std::vector<float>>) {
            out.put('[');
            for (size_t i = 0; i < x.size(); ++i) {
                if (i) out.put(',');
                write_json_float(out, x[i]);
            }
            out.put(']');
        }
    }, v);
    return out;
}

// ===== 极简 JSON 请求解析 + ReqId 序列化 =====
// 只覆盖本题协议需要的固定格式：顶层 object，key 为 id/op/sql/search_mode；
// value 仅需支持 null / integer / string / true|false / float。
// 解析器跳过空白，不解析嵌套 object/array（请求里不会出现）。

struct ReqId {
    enum Type { NONE, INT, STRING, NULL_VAL };
    Type type = NONE;
    int64_t i = 0;
    std::string s;

    // 按 JSON 格式写入 id：null / 整数 / 字符串
    void write_to(std::ostream &out) const {
        switch (type) {
            case INT:  write_json_int64(out, i); break;
            case STRING: write_json_string(out, s); break;
            default:   out << "null"; break;
        }
    }
};

struct Request {
    ReqId id;
    std::string op;
    std::string sql;
    bool has_search_mode = false;
    std::string search_mode;
};

namespace detail_json {
inline void skip_ws(std::string_view s, size_t &i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        ++i;
}
inline std::string parse_string(std::string_view s, size_t &i) {
    require(i < s.size() && s[i] == '"', "invalid_request", "expected string");
    ++i; // opening "
    std::string out;
    while (i < s.size() && s[i] != '"') {
        char c = s[i++];
        if (c == '\\') {
            require(i < s.size(), "invalid_request", "bad escape");
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    require(i + 4 <= s.size(), "invalid_request", "bad \\u");
                    // 我们不支持完整 unicode 代理对；这里只支持 \uXXXX 中 BMP 内的
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else require(false, "invalid_request", "bad \\u hex");
                    }
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: require(false, "invalid_request", "unsupported escape");
            }
        } else
            out.push_back(c);
    }
    require(i < s.size() && s[i] == '"', "invalid_request", "unterminated string");
    ++i; // closing "
    return out;
}

inline int64_t parse_integer(std::string_view s, size_t &i) {
    size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    require(i < s.size() && (s[i] >= '0' && s[i] <= '9'), "invalid_request", "expected integer");
    int64_t v = 0;
    bool neg = (s[start] == '-');
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + (s[i] - '0');
    }
    return neg ? -v : v;
}

inline bool match_keyword(std::string_view s, size_t &i, const char *kw) {
    size_t k = 0;
    while (kw[k]) {
        if (i + k >= s.size() || s[i + k] != kw[k]) return false;
        ++k;
    }
    i += k;
    return true;
}
} // namespace detail_json

inline Request parse_request(std::string_view s) {
    using namespace detail_json;
    size_t i = 0;
    skip_ws(s, i);
    require(i < s.size() && s[i] == '{', "invalid_request", "expected object");
    ++i;
    Request req;
    bool has_id = false, has_op = false, has_sql = false;
    skip_ws(s, i);
    if (i < s.size() && s[i] == '}') { ++i; goto done; }
    while (true) {
        skip_ws(s, i);
        std::string key = parse_string(s, i);
        skip_ws(s, i);
        require(i < s.size() && s[i] == ':', "invalid_request", "expected colon");
        ++i;
        skip_ws(s, i);
        if (key == "id") {
            // id 可以是 null / integer / string
            require(i < s.size(), "invalid_request", "expected id value");
            if (s[i] == 'n' && match_keyword(s, i, "null")) {
                req.id.type = ReqId::NULL_VAL;
            } else if (s[i] == '"') {
                req.id.s = parse_string(s, i);
                req.id.type = ReqId::STRING;
            } else {
                // integer（允许前导 +/-）
                req.id.i = parse_integer(s, i);
                req.id.type = ReqId::INT;
            }
            has_id = true;
        } else if (key == "op") {
            req.op = parse_string(s, i);
            has_op = true;
        } else if (key == "sql") {
            req.sql = parse_string(s, i);
            has_sql = true;
        } else if (key == "search_mode") {
            req.search_mode = parse_string(s, i);
            req.has_search_mode = true;
        } else {
            // 跳过未知 key 的 value：string / null / bool / number
            if (s[i] == '"') parse_string(s, i);
            else if (s[i] == 'n' && match_keyword(s, i, "null")) {}
            else if (s[i] == 't' && match_keyword(s, i, "true")) {}
            else if (s[i] == 'f' && match_keyword(s, i, "false")) {}
            else if (s[i] == '{' || s[i] == '[') {
                // 跳过嵌套 object/array（协议里不会有，防万一）
                char open = s[i++];
                char close = (open == '{') ? '}' : ']';
                int depth = 1;
                while (i < s.size() && depth > 0) {
                    if (s[i] == '"') { parse_string(s, i); continue; }
                    if (s[i] == open) ++depth;
                    else if (s[i] == close) --depth;
                    ++i;
                }
            } else {
                // number：跳过到非数字字符
                while (i < s.size() && (s[i] == '-' || s[i] == '+' || (s[i] >= '0' && s[i] <= '9') ||
                                        s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
            }
        }
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        break;
    }
    skip_ws(s, i);
    require(i < s.size() && s[i] == '}', "invalid_request", "unterminated object");
    ++i;
    skip_ws(s, i);
done:
    require(has_id && has_op, "invalid_request", "id/op required");
    require(req.op == "execute" ? has_sql : true, "invalid_request", "execute requires SQL string");
    if (req.op == "execute") {
        auto m = req.has_search_mode ? req.search_mode : "exact";
        require(m == "exact" || m == "ann", "invalid_mode", "search_mode must be exact or ann");
    }
    return req;
}

struct Column {
    std::string name, type;
    uint32_t size = 0;
};
struct Token {
    std::string_view text;
    bool quoted = false, identifier = false;
};
class Parser {
    // SQL must outlive the parser. Escaped values have stable storage even when
    // the token vector grows or another escaped literal is decoded.
    std::deque<std::string> decoded;
    std::vector<Token> tokens;
    size_t pos = 0;

    static bool equal_keyword(std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            unsigned char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z')
                x += 'a' - 'A';
            if (y >= 'A' && y <= 'Z')
                y += 'a' - 'A';
            if (x != y)
                return false;
        }
        return true;
    }

  public:
    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;
    explicit Parser(const std::string &sql) {
        const std::string_view input(sql);
        for (size_t i = 0; i < sql.size();) {
            unsigned char c = sql[i];
            if (char_space(c)) {
                ++i;
                continue;
            }
            if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
                i = sql.find('\n', i + 2);
                if (i == std::string::npos)
                    break;
                continue;
            }
            if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                size_t e = sql.find("*/", i + 2);
                require(e != std::string::npos, "syntax_error", "unterminated comment");
                i = e + 2;
                continue;
            }
            if (c == '\'' || c == '`' || c == '"') {
                char quote = c;
                bool closed = false;
                std::string text;
                size_t start = ++i, part = start;
                bool escaped = false;
                while (i < sql.size()) {
                    char x = sql[i];
                    if (x == quote) {
                        if (i + 1 < sql.size() && sql[i + 1] == quote) {
                            text.append(sql, part, i - part);
                            text += quote;
                            escaped = true;
                            i += 2;
                            part = i;
                        } else {
                            std::string_view value;
                            if (escaped) {
                                text.append(sql, part, i - part);
                                decoded.push_back(std::move(text));
                                value = decoded.back();
                            } else {
                                value = input.substr(start, i - start);
                            }
                            tokens.push_back({value, quote == '\'', quote != '\''});
                            ++i;
                            closed = true;
                            break;
                        }
                    } else if (x == '\\' && quote == '\'' && i + 1 < sql.size()) {
                        text.append(sql, part, i - part);
                        escaped = true;
                        ++i;
                        char esc = sql[i++];
                        switch (esc) {
                        case 'n':
                            text += '\n';
                            break;
                        case 'r':
                            text += '\r';
                            break;
                        case 't':
                            text += '\t';
                            break;
                        case '0':
                            text += '\0';
                            break;
                        default:
                            text += esc;
                        }
                        part = i;
                    } else {
                        ++i;
                    }
                }
                require(closed, "syntax_error", "unterminated quoted token");
                continue;
            }
            if (char_digit(c) || c == '.') {
                size_t start = i++;
                while (i < sql.size() &&
                       (char_digit(static_cast<unsigned char>(sql[i])) || sql[i] == '.'))
                    ++i;
                if (i < sql.size() && (sql[i] == 'e' || sql[i] == 'E')) {
                    ++i;
                    if (i < sql.size() && (sql[i] == '+' || sql[i] == '-'))
                        ++i;
                    while (i < sql.size() && char_digit(static_cast<unsigned char>(sql[i])))
                        ++i;
                }
                tokens.push_back({input.substr(start, i - start)});
                continue;
            }
            if (char_alpha(c) || c == '_' || c >= 128) {
                size_t start = i++;
                while (i < sql.size()) {
                    unsigned char x = sql[i];
                    if (!char_alnum(x) && x != '_' && x < 128)
                        break;
                    ++i;
                }
                tokens.push_back({input.substr(start, i - start), false, true});
                continue;
            }
            size_t start = i++;
            if (i < sql.size() && (c == '<' || c == '>' || c == '!') &&
                (sql[i] == '=' || (c == '<' && sql[i] == '>')))
                ++i;
            tokens.push_back({input.substr(start, i - start)});
        }
    }
    bool done() const {
        return pos == tokens.size();
    }
    bool is(std::string_view s) const {
        return !done() && !tokens[pos].quoted && equal_keyword(tokens[pos].text, s);
    }
    bool take(std::string_view s) {
        if (is(s)) {
            ++pos;
            return true;
        }
        return false;
    }
    void expect(std::string_view s) {
        if (!take(s))
            throw Error("syntax_error", "expected " + std::string(s));
    }
    Token next() {
        require(!done(), "syntax_error", "unexpected end of SQL");
        return tokens[pos++];
    }
    std::string identifier(bool fold = true) {
        Token t = next();
        require(t.identifier, "syntax_error", "expected identifier");
        return fold ? lower(std::string(t.text)) : std::string(t.text);
    }
    void finish() {
        take(";");
        require(done(), "unsupported_sql", "unsupported trailing SQL");
    }
    std::string number() {
        std::string sign;
        if (take("-"))
            sign = "-";
        else
            take("+");
        Token t = next();
        require(!t.quoted && !t.identifier, "type_error", "expected number");
        sign.append(t.text);
        return sign;
    }
    int64_t integer() {
        bool negative = take("-");
        if (!negative)
            take("+");
        Token t = next();
        require(!t.quoted && !t.identifier, "type_error", "expected number");
        uint64_t magnitude = 0;
        auto parsed = std::from_chars(t.text.data(), t.text.data() + t.text.size(), magnitude);
        uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
        require(parsed.ec == std::errc() && magnitude <= limit, "value_out_of_range",
                "BIGINT out of range");
        require(parsed.ptr == t.text.data() + t.text.size(), "type_error",
                "expected BIGINT integer");
        if (negative && magnitude == uint64_t(INT64_MAX) + 1)
            return INT64_MIN;
        return negative ? -int64_t(magnitude) : int64_t(magnitude);
    }
    uint32_t size_parameter() {
        int64_t v = integer();
        require(v > 0 && v <= 16777216, "value_out_of_range", "invalid type dimension");
        return static_cast<uint32_t>(v);
    }
    std::string vector_text() {
        if (!done() && tokens[pos].quoted)
            return std::string(next().text);
        expect("[");
        std::string text = "[";
        if (!is("]")) {
            do {
                if (text.size() > 1)
                    text += ",";
                text += number();
            } while (take(","));
        }
        expect("]");
        return text + "]";
    }
    // 直接消费 token 流解析向量，跳过 vector_text() 的字符串拼装。
    std::vector<float> vector_literal(uint32_t dim) {
        if (!done() && tokens[pos].quoted) {
            return vector_value(next().text, dim);
        }
        expect("[");
        std::vector<float> v;
        v.reserve(dim);
        if (!is("]")) {
            do {
                bool negative = take("-");
                if (!negative)
                    take("+");
                require(pos < tokens.size(), "type_error", "unexpected end of SQL");
                const Token &t = tokens[pos++];
                require(!t.quoted && !t.identifier, "type_error", "expected number");
                double d = 0;
                auto parsed = std::from_chars(t.text.data(), t.text.data() + t.text.size(), d);
                require(parsed.ec == std::errc() && parsed.ptr == t.text.data() + t.text.size(),
                        "type_error", "expected number");
                if (negative)
                    d = -d;
                float f = static_cast<float>(d);
                require(std::isfinite(d) && std::isfinite(f), "invalid_vector",
                        "vector must contain finite float32 values");
                v.push_back(f);
            } while (take(","));
        }
        expect("]");
        require(v.size() == dim, "invalid_vector", "vector dimension mismatch");
        return v;
    }
    static std::vector<float> vector_value(std::string_view text, uint32_t dim) {
        // 直接扫描 `[a,b,...]` 文本：批量 INSERT 的每个分量都走一次
        // nlohmann JSON 解析是解析热点，这里改为定长扫描 + from_chars。
        const char *p = text.data(), *e = p + text.size();
        auto skip_space = [&] {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                ++p;
        };
        skip_space();
        require(p < e && *p == '[', "invalid_vector", "invalid vector array");
        ++p;
        skip_space();
        std::vector<float> v;
        v.reserve(dim);
        if (p < e && *p == ']') {
            ++p;
        } else {
            while (true) {
                skip_space();
                bool negative = false;
                if (p < e && (*p == '+' || *p == '-')) {
                    negative = *p == '-';
                    ++p;
                }
                double d = 0;
                auto parsed = std::from_chars(p, e, d);
                require(parsed.ec == std::errc() && parsed.ptr != p, "invalid_vector",
                        "invalid vector array");
                p = parsed.ptr;
                if (negative)
                    d = -d;
                float f = static_cast<float>(d);
                require(std::isfinite(d) && std::isfinite(f), "invalid_vector",
                        "vector must contain finite float32 values");
                v.push_back(f);
                skip_space();
                require(p < e && (*p == ',' || *p == ']'), "invalid_vector",
                        "invalid vector array");
                if (*p == ']') {
                    ++p;
                    break;
                }
                ++p;
            }
        }
        skip_space();
        require(p == e, "invalid_vector", "invalid vector array");
        require(v.size() == dim, "invalid_vector", "vector dimension mismatch");
        return v;
    }
    Value literal(const Column &col) {
        if (col.type == "bigint")
            return integer();
        if (col.type == "varchar") {
            Token t = next();
            require(t.quoted, "type_error", "expected string");
            // Parsing through JSON validates UTF-8; count Unicode scalar values, not bytes.
            try {
                (void)json(t.text).dump();
            } catch (...) {
                throw Error("type_error", "invalid UTF-8");
            }
            size_t chars = 0;
            for (unsigned char c : t.text)
                if ((c & 0xc0) != 0x80)
                    ++chars;
            require(chars <= col.size, "value_too_long", "VARCHAR exceeds character limit");
            return std::string(t.text);
        }
        if (col.type == "vector") {
            return vector_literal(col.size);
        }
        throw Error("type_error", "unknown type");
    }
};
inline int compare(const Value &a, const Value &b) {
    require(a.index() == b.index(), "type_error", "incompatible comparison");
    if (auto p = std::get_if<int64_t>(&a)) {
        auto q = std::get<int64_t>(b);
        return *p < q ? -1 : *p > q ? 1 : 0;
    }
    if (auto p = std::get_if<std::string>(&a)) {
        auto &q = std::get<std::string>(b);
        int c = p->compare(q);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (auto p = std::get_if<double>(&a)) {
        auto q = std::get<double>(b);
        return *p < q ? -1 : *p > q ? 1 : 0;
    }
    throw Error("unsupported_sql", "vector comparison is unsupported");
}
inline double l2(const std::vector<float> &a, const std::vector<float> &b) {
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = double(a[i]) - double(b[i]);
        sum += d * d;
    }
    return std::sqrt(sum);
}
// 直接按行体内的小端 float 计算，避免为剪枝构造整行 Value；归约顺序与上面一致。
inline double l2(const char *a, const std::vector<float> &b, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        float x;
        std::memcpy(&x, a + i * sizeof(float), sizeof(x));
        double d = double(x) - double(b[i]);
        sum += d * d;
    }
    return std::sqrt(sum);
}
// 行体内 float32 与 double 查询向量的平方距离。查询向量在每次检索前转成
// double，热点循环即可用 AVX2/FMA 一次算 4 个分量；标量版保持 4 路归约，
// 与 SIMD 版的舍入差异远小于 benchmark 的 1e-10 容差。
namespace l2_kernel {
inline double scalar(const float *f, const double *q, size_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        double d0 = double(f[i]) - q[i], d1 = double(f[i + 1]) - q[i + 1];
        double d2 = double(f[i + 2]) - q[i + 2], d3 = double(f[i + 3]) - q[i + 3];
        s0 += d0 * d0;
        s1 += d1 * d1;
        s2 += d2 * d2;
        s3 += d3 * d3;
    }
    double s = (s0 + s1) + (s2 + s3);
    for (; i < n; ++i) {
        double d = double(f[i]) - q[i];
        s += d * d;
    }
    return s;
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) inline double avx2(const float *f, const double *q, size_t n) {
    __m256d s0 = _mm256_setzero_pd(), s1 = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256d x0 = _mm256_cvtps_pd(_mm_loadu_ps(f + i));
        __m256d d0 = _mm256_sub_pd(x0, _mm256_loadu_pd(q + i));
        s0 = _mm256_fmadd_pd(d0, d0, s0);
        __m256d x1 = _mm256_cvtps_pd(_mm_loadu_ps(f + i + 4));
        __m256d d1 = _mm256_sub_pd(x1, _mm256_loadu_pd(q + i + 4));
        s1 = _mm256_fmadd_pd(d1, d1, s1);
    }
    if (i + 4 <= n) {
        __m256d x0 = _mm256_cvtps_pd(_mm_loadu_ps(f + i));
        __m256d d0 = _mm256_sub_pd(x0, _mm256_loadu_pd(q + i));
        s0 = _mm256_fmadd_pd(d0, d0, s0);
        i += 4;
    }
    __m256d sum = _mm256_add_pd(s0, s1);
    alignas(32) double lane[4];
    _mm256_store_pd(lane, sum);
    double s = (lane[0] + lane[1]) + (lane[2] + lane[3]);
    for (; i < n; ++i) {
        double d = double(f[i]) - q[i];
        s += d * d;
    }
    return s;
}
__attribute__((target("avx512f"))) inline double avx512(const float *f, const double *q, size_t n) {
    __m512d s0 = _mm512_setzero_pd(), s1 = _mm512_setzero_pd();
    __m512d s2 = _mm512_setzero_pd(), s3 = _mm512_setzero_pd();
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m512d d0 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i)), _mm512_loadu_pd(q + i));
        s0 = _mm512_fmadd_pd(d0, d0, s0);
        __m512d d1 =
            _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i + 8)), _mm512_loadu_pd(q + i + 8));
        s1 = _mm512_fmadd_pd(d1, d1, s1);
        __m512d d2 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i + 16)),
                                   _mm512_loadu_pd(q + i + 16));
        s2 = _mm512_fmadd_pd(d2, d2, s2);
        __m512d d3 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i + 24)),
                                   _mm512_loadu_pd(q + i + 24));
        s3 = _mm512_fmadd_pd(d3, d3, s3);
    }
    for (; i + 8 <= n; i += 8) {
        __m512d d0 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i)), _mm512_loadu_pd(q + i));
        s0 = _mm512_fmadd_pd(d0, d0, s0);
    }
    __m512d sum = _mm512_add_pd(_mm512_add_pd(s0, s1), _mm512_add_pd(s2, s3));
    alignas(64) double lane[8];
    _mm512_store_pd(lane, sum);
    double s =
        ((lane[0] + lane[1]) + (lane[2] + lane[3])) + ((lane[4] + lane[5]) + (lane[6] + lane[7]));
    for (; i < n; ++i) {
        double d = double(f[i]) - q[i];
        s += d * d;
    }
    return s;
}
#endif
using Fn = double (*)(const float *, const double *, size_t);
inline Fn choose() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f"))
        return avx512;
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        return avx2;
#endif
    return scalar;
}
inline Fn fn() {
    static const Fn picked = choose();
    return picked;
}
} // namespace l2_kernel
inline double squared_l2(const char *a, const double *q, size_t n) {
    return l2_kernel::fn()(reinterpret_cast<const float *>(a), q, n);
}
} // namespace mini
