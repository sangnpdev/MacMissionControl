#include "config/json.h"

#include <charconv>
#include <cmath>

namespace mc::json {

const Value* Value::find(std::string_view key) const {
    for (auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    ParseResult run() {
        // Tolerate a UTF-8 BOM: Windows editors often write one.
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") pos_ = 3;
        Value v;
        if (!value(v, 0)) return {std::nullopt, error_};
        ws();
        if (pos_ != s_.size()) return fail("unexpected content after the JSON value");
        return {std::move(v), {}};
    }

private:
    ParseResult fail(const std::string& msg) {
        error(msg);
        return {std::nullopt, error_};
    }
    bool error(const std::string& msg) {
        if (!error_.empty()) return false;
        int line = 1, col = 1;
        for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        error_ = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + msg;
        return false;
    }
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return error("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return error("nesting too deep");
        ws();
        if (pos_ >= s_.size()) return error("unexpected end of input");
        switch (s_[pos_]) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"': out.type = Value::Type::String; return string(out.string);
        case 't': out.type = Value::Type::Bool; out.boolean = true; return literal("true");
        case 'f': out.type = Value::Type::Bool; out.boolean = false; return literal("false");
        case 'n': out.type = Value::Type::Null; return literal("null");
        default: out.type = Value::Type::Number; return number(out.number);
        }
    }

    bool object(Value& out, int depth) {
        out.type = Value::Type::Object;
        ++pos_;  // {
        ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return error("expected a string key");
            std::string key;
            if (!string(key)) return false;
            if (out.find(key)) return error("duplicate key \"" + key + "\"");
            ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return error("expected ':'");
            ++pos_;
            Value v;
            if (!value(v, depth + 1)) return false;
            out.object.emplace_back(std::move(key), std::move(v));
            ws();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return error("expected ',' or '}'");
        }
    }

    bool array(Value& out, int depth) {
        out.type = Value::Type::Array;
        ++pos_;  // [
        ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            Value v;
            if (!value(v, depth + 1)) return false;
            out.array.push_back(std::move(v));
            ws();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            return error("expected ',' or ']'");
        }
    }

    bool number(double& out) {
        // Validate the RFC 8259 grammar first; from_chars alone accepts forms JSON forbids.
        const std::size_t start = pos_;
        auto digit = [&] { return pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9'; };
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (!digit()) return error("invalid value");
        if (s_[pos_] == '0') ++pos_;
        else while (digit()) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digit()) return error("expected digits after '.'");
            while (digit()) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digit()) return error("expected exponent digits");
            while (digit()) ++pos_;
        }
        const auto r = std::from_chars(s_.data() + start, s_.data() + pos_, out);  // locale-independent
        if (r.ec != std::errc() || !std::isfinite(out)) { pos_ = start; return error("number out of range"); }
        return true;
    }

    bool hex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return error("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return error("invalid \\u escape");
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (pos_ >= s_.size()) return error("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') { ++pos_; return true; }
            if (c < 0x20) return error("control character in string");
            if (c != '\\') { out += char(c); ++pos_; continue; }
            if (++pos_ >= s_.size()) return error("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                    unsigned lo;
                    if (s_.substr(pos_, 2) != "\\u") return error("unpaired surrogate");
                    pos_ += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return error("invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return error("unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return error("invalid escape");
            }
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace

ParseResult parse(std::string_view text) { return Parser(text).run(); }

}  // namespace mc::json
