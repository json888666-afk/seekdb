#pragma once
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
    static std::vector<float> vector_value(const std::string &text, uint32_t dim) {
        json a;
        try {
            a = json::parse(text);
        } catch (...) {
            throw Error("invalid_vector", "invalid vector array");
        }
        require(a.is_array() && a.size() == dim, "invalid_vector", "vector dimension mismatch");
        std::vector<float> v;
        v.reserve(dim);
        for (const auto &x : a) {
            require(x.is_number(), "invalid_vector", "vector entries must be numbers");
            double d = x.get<double>();
            float f = static_cast<float>(d);
            require(std::isfinite(d) && std::isfinite(f), "invalid_vector",
                    "vector must contain finite float32 values");
            v.push_back(f);
        }
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
            return vector_value(vector_text(), col.size);
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
} // namespace mini
