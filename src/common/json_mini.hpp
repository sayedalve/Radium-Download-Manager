// SPDX-License-Identifier: Apache-2.0
// Lightweight, RFC 8259 compliant JSON parser & builder for OpenIDM IPC & meta.json
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace openidm::json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    using Variant = std::variant<std::nullptr_t, bool, double, int64_t, std::string, Array, Object>;
    Variant data{nullptr};

    Value() : data(nullptr) {}
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int i) : data(static_cast<int64_t>(i)) {}
    Value(int64_t i) : data(i) {}
    Value(uint64_t i) : data(static_cast<int64_t>(i)) {}
    Value(uint32_t i) : data(static_cast<int64_t>(i)) {}
    Value(double d) : data(d) {}
    Value(const char* s) : data(std::string(s ? s : "")) {}
    Value(std::string s) : data(std::move(s)) {}
    Value(Array a) : data(std::move(a)) {}
    Value(Object o) : data(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_number() const { return std::holds_alternative<int64_t>(data) || std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }

    bool as_bool(bool def = false) const {
        if (auto* p = std::get_if<bool>(&data)) return *p;
        return def;
    }
    int64_t as_int64(int64_t def = 0) const {
        if (auto* p = std::get_if<int64_t>(&data)) return *p;
        if (auto* p = std::get_if<double>(&data)) return static_cast<int64_t>(*p);
        return def;
    }
    double as_double(double def = 0.0) const {
        if (auto* p = std::get_if<double>(&data)) return *p;
        if (auto* p = std::get_if<int64_t>(&data)) return static_cast<double>(*p);
        return def;
    }
    std::string as_string(const std::string& def = "") const {
        if (auto* p = std::get_if<std::string>(&data)) return *p;
        return def;
    }
    const Array& as_array() const {
        static const Array kEmpty;
        if (auto* p = std::get_if<Array>(&data)) return *p;
        return kEmpty;
    }
    const Object& as_object() const {
        static const Object kEmpty;
        if (auto* p = std::get_if<Object>(&data)) return *p;
        return kEmpty;
    }
    Object& mutable_object() {
        if (!is_object()) data = Object{};
        return std::get<Object>(data);
    }
    Array& mutable_array() {
        if (!is_array()) data = Array{};
        return std::get<Array>(data);
    }

    const Value& operator[](const std::string& key) const {
        static const Value kNull;
        if (auto* o = std::get_if<Object>(&data)) {
            auto it = o->find(key);
            if (it != o->end()) return it->second;
        }
        return kNull;
    }
    Value& operator[](const std::string& key) {
        return mutable_object()[key];
    }
    bool contains(const std::string& key) const {
        if (auto* o = std::get_if<Object>(&data)) {
            return o->find(key) != o->end();
        }
        return false;
    }

    static std::string EscapeString(const std::string& s) {
        std::ostringstream oss;
        oss << '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        oss << buf;
                    } else {
                        oss << static_cast<char>(c);
                    }
            }
        }
        oss << '"';
        return oss.str();
    }

    std::string dump() const {
        if (is_null()) return "null";
        if (auto* b = std::get_if<bool>(&data)) return *b ? "true" : "false";
        if (auto* i = std::get_if<int64_t>(&data)) return std::to_string(*i);
        if (auto* d = std::get_if<double>(&data)) {
            if (std::isnan(*d) || std::isinf(*d)) return "0";
            std::ostringstream oss;
            oss << *d;
            return oss.str();
        }
        if (auto* s = std::get_if<std::string>(&data)) return EscapeString(*s);
        if (auto* a = std::get_if<Array>(&data)) {
            std::ostringstream oss;
            oss << '[';
            for (size_t idx = 0; idx < a->size(); ++idx) {
                if (idx > 0) oss << ',';
                oss << (*a)[idx].dump();
            }
            oss << ']';
            return oss.str();
        }
        if (auto* o = std::get_if<Object>(&data)) {
            std::ostringstream oss;
            oss << '{';
            bool first = true;
            for (const auto& [k, v] : *o) {
                if (!first) oss << ',';
                first = false;
                oss << EscapeString(k) << ':' << v.dump();
            }
            oss << '}';
            return oss.str();
        }
        return "null";
    }
};

class Parser {
public:
    explicit Parser(std::string_view src) : m_src(src) {}

    Value Parse() {
        SkipWs();
        Value v = ParseValue();
        SkipWs();
        if (m_pos != m_src.size()) {
            throw std::runtime_error("Trailing characters in JSON");
        }
        return v;
    }

private:
    std::string_view m_src;
    size_t m_pos{0};

    void SkipWs() {
        while (m_pos < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_pos]))) {
            ++m_pos;
        }
    }

    char Peek() const {
        if (m_pos >= m_src.size()) throw std::runtime_error("Unexpected end of JSON");
        return m_src[m_pos];
    }

    char Get() {
        char c = Peek();
        ++m_pos;
        return c;
    }

    Value ParseValue() {
        SkipWs();
        char c = Peek();
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') return Value(ParseString());
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') return ParseNull();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber();
        throw std::runtime_error("Invalid JSON value token");
    }

    Value ParseObject() {
        Get(); // '{'
        SkipWs();
        Object obj;
        if (Peek() == '}') {
            Get();
            return Value(std::move(obj));
        }
        while (true) {
            SkipWs();
            if (Peek() != '"') throw std::runtime_error("Expected string key in JSON object");
            std::string key = ParseString();
            SkipWs();
            if (Get() != ':') throw std::runtime_error("Expected ':' in JSON object");
            SkipWs();
            obj[std::move(key)] = ParseValue();
            SkipWs();
            char sep = Get();
            if (sep == '}') break;
            if (sep != ',') throw std::runtime_error("Expected ',' or '}' in JSON object");
        }
        return Value(std::move(obj));
    }

    Value ParseArray() {
        Get(); // '['
        SkipWs();
        Array arr;
        if (Peek() == ']') {
            Get();
            return Value(std::move(arr));
        }
        while (true) {
            SkipWs();
            arr.push_back(ParseValue());
            SkipWs();
            char sep = Get();
            if (sep == ']') break;
            if (sep != ',') throw std::runtime_error("Expected ',' or ']' in JSON array");
        }
        return Value(std::move(arr));
    }

    std::string ParseString() {
        if (Get() != '"') throw std::runtime_error("Expected '\"'");
        std::string out;
        while (true) {
            char c = Get();
            if (c == '"') break;
            if (c == '\\') {
                char esc = Get();
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (m_pos + 4 > m_src.size()) throw std::runtime_error("Invalid \\u escape");
                        std::string hex(m_src.substr(m_pos, 4));
                        m_pos += 4;
                        uint32_t cp = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
                        if (cp <= 0x7F) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Value ParseBool() {
        if (m_src.substr(m_pos, 4) == "true") {
            m_pos += 4;
            return Value(true);
        }
        if (m_src.substr(m_pos, 5) == "false") {
            m_pos += 5;
            return Value(false);
        }
        throw std::runtime_error("Invalid boolean literal");
    }

    Value ParseNull() {
        if (m_src.substr(m_pos, 4) == "null") {
            m_pos += 4;
            return Value(nullptr);
        }
        throw std::runtime_error("Invalid null literal");
    }

    Value ParseNumber() {
        size_t start = m_pos;
        if (m_src[m_pos] == '-') ++m_pos;
        bool is_float = false;
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                ++m_pos;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_float = true;
                ++m_pos;
            } else {
                break;
            }
        }
        std::string token(m_src.substr(start, m_pos - start));
        if (is_float) {
            return Value(std::stod(token));
        }
        return Value(static_cast<int64_t>(std::stoll(token)));
    }
};

inline Value Parse(std::string_view src) {
    return Parser(src).Parse();
}

} // namespace openidm::json
