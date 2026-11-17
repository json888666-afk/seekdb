#pragma once
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <charconv>
#include <cstring>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace mini {
using json = nlohmann::json;
struct Error : std::runtime_error {
    std::string code;
    Error(std::string c, std::string m) : std::runtime_error(m), code(std::move(c)) {}
};
inline void require(bool b, const std::string &code, const std::string &msg) {
    if (!b)
        throw Error(code, msg);
}
inline std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return s;
}
using Value = std::variant<int64_t, std::string, std::vector<float>, double>;
inline json value_json(const Value &v) {
    return std::visit([](auto &&x) -> json { return x; }, v);
}
struct Column {
    std::string name, type;
    uint32_t size = 0;
};
struct Token {
    std::string text;
    bool quoted = false, identifier = false;
};
class Parser {
    std::vector<Token> tokens;
    size_t pos = 0;

  public:
    explicit Parser(const std::string &sql) {
        for (size_t i = 0; i < sql.size();) {
            unsigned char c = sql[i];
            if (std::isspace(c)) {
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
                ++i;
                while (i < sql.size()) {
                    char x = sql[i++];
                    if (x == quote) {
                        if (i < sql.size() && sql[i] == quote) {
                            text += quote;
                            ++i;
                        } else {
                            closed = true;
                            break;
                        }
                    } else if (x == '\\' && quote == '\'' && i < sql.size()) {
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
                    } else
                        text += x;
                }
                require(closed, "syntax_error", "unterminated quoted token");
                tokens.push_back({text, quote == '\'', quote != '\''});
                continue;
            }
            if (std::isdigit(c) || c == '.') {
                size_t start = i++;
                while (i < sql.size() &&
                       (std::isdigit(static_cast<unsigned char>(sql[i])) || sql[i] == '.'))
                    ++i;
                if (i < sql.size() && (sql[i] == 'e' || sql[i] == 'E')) {
                    ++i;
                    if (i < sql.size() && (sql[i] == '+' || sql[i] == '-'))
                        ++i;
                    while (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i])))
                        ++i;
                }
                tokens.push_back({sql.substr(start, i - start)});
                continue;
            }
            if (std::isalpha(c) || c == '_' || c >= 128) {
                size_t start = i++;
                while (i < sql.size()) {
                    unsigned char x = sql[i];
                    if (!std::isalnum(x) && x != '_' && x < 128)
                        break;
                    ++i;
                }
                tokens.push_back({sql.substr(start, i - start), false, true});
                continue;
            }
            std::string op(1, sql[i++]);
            if (i < sql.size() && (op == "<" || op == ">" || op == "!") &&
                (sql[i] == '=' || (op == "<" && sql[i] == '>')))
                op += sql[i++];
            tokens.push_back({op});
        }
    }
    bool done() const {
        return pos == tokens.size();
    }
    bool is(const std::string &s) const {
        return !done() && !tokens[pos].quoted && lower(tokens[pos].text) == lower(s);
    }
    bool take(const std::string &s) {
        if (is(s)) {
            ++pos;
            return true;
        }
        return false;
    }
    void expect(const std::string &s) {
        require(take(s), "syntax_error", "expected " + s);
    }
    Token next() {
        require(!done(), "syntax_error", "unexpected end of SQL");
        return tokens[pos++];
    }
    std::string identifier(bool fold = true) {
        Token t = next();
        require(t.identifier, "syntax_error", "expected identifier");
        return fold ? lower(t.text) : t.text;
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
        return sign + t.text;
    }
    int64_t integer() {
        std::string s = number();
        size_t end = 0;
        int64_t v;
        try {
            v = std::stoll(s, &end);
        } catch (...) {
            throw Error("value_out_of_range", "BIGINT out of range");
        }
        require(end == s.size(), "type_error", "expected BIGINT integer");
        return v;
    }
    uint32_t size_parameter() {
        int64_t v = integer();
        require(v > 0 && v <= 16777216, "value_out_of_range", "invalid type dimension");
        return static_cast<uint32_t>(v);
    }
    std::string vector_text() {
        if (!done() && tokens[pos].quoted)
            return next().text;
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
            std::string text = next().text;
            return vector_value(text, dim);
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
    static std::vector<float> vector_value(const std::string &text, uint32_t dim) {
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
            return t.text;
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
        __m512d d0 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i)),
                                   _mm512_loadu_pd(q + i));
        s0 = _mm512_fmadd_pd(d0, d0, s0);
        __m512d d1 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i + 8)),
                                   _mm512_loadu_pd(q + i + 8));
        s1 = _mm512_fmadd_pd(d1, d1, s1);
        __m512d d2 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i + 16)),
                                   _mm512_loadu_pd(q + i + 16));
        s2 = _mm512_fmadd_pd(d2, d2, s2);
        __m512d d3 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i + 24)),
                                   _mm512_loadu_pd(q + i + 24));
        s3 = _mm512_fmadd_pd(d3, d3, s3);
    }
    for (; i + 8 <= n; i += 8) {
        __m512d d0 = _mm512_sub_pd(_mm512_cvtps_pd(_mm256_loadu_ps(f + i)),
                                   _mm512_loadu_pd(q + i));
        s0 = _mm512_fmadd_pd(d0, d0, s0);
    }
    __m512d sum = _mm512_add_pd(_mm512_add_pd(s0, s1), _mm512_add_pd(s2, s3));
    alignas(64) double lane[8];
    _mm512_store_pd(lane, sum);
    double s = ((lane[0] + lane[1]) + (lane[2] + lane[3])) +
               ((lane[4] + lane[5]) + (lane[6] + lane[7]));
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
