#include "json.h"

#include <cstdlib>
#include <cstring>

namespace clay {
namespace io {

namespace {

const JsonNode& null_node() {
    static const JsonNode kNull;
    return kNull;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// One UTF-16 code unit as UTF-8. Surrogate pairs are joined by the caller,
// which is the only place that can see both halves.
void append_utf8(std::string* out, std::uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool read_hex4(const char* d, std::size_t n, std::size_t i, std::uint32_t* out) {
    if (i + 4 > n) return false;
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
        const char c = d[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

}  // namespace

bool JsonDoc::fail(std::size_t at, const char* why) {
    error_ = std::string(why) + " at byte " + std::to_string(at);
    return false;
}

void JsonDoc::skip_ws(const char* d, std::size_t n, std::size_t* i) const {
    while (*i < n) {
        const char c = d[*i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++*i;
        else break;
    }
}

std::size_t JsonDoc::push(JsonNode&& node) {
    nodes_.push_back(std::move(node));
    return nodes_.size() - 1;
}

bool JsonDoc::parse(const char* data, std::size_t size) {
    nodes_.clear();
    error_.clear();
    if (!data || size == 0) return fail(0, "empty document");
    std::size_t i = 0;
    if (!parse_value(data, size, &i, 0, &root_)) return false;
    skip_ws(data, size, &i);
    if (i != size) return fail(i, "trailing data after the top-level value");
    return true;
}

bool JsonDoc::parse_string(const char* d, std::size_t n, std::size_t* i, std::string* out) {
    if (*i >= n || d[*i] != '"') return fail(*i, "expected a string");
    ++*i;
    out->clear();
    while (true) {
        if (*i >= n) return fail(*i, "unterminated string");
        const unsigned char c = static_cast<unsigned char>(d[*i]);
        if (c == '"') {
            ++*i;
            return true;
        }
        if (c < 0x20) return fail(*i, "raw control character in a string");
        if (c != '\\') {
            out->push_back(static_cast<char>(c));
            ++*i;
            continue;
        }
        ++*i;
        if (*i >= n) return fail(*i, "unterminated escape");
        const char e = d[*i];
        ++*i;
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!read_hex4(d, n, *i, &cp)) return fail(*i, "malformed \\u escape");
                *i += 4;
                // A high surrogate must be followed by its low half; anything
                // else is a lone surrogate, which is not encodable.
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (*i + 6 > n || d[*i] != '\\' || d[*i + 1] != 'u' ||
                        !read_hex4(d, n, *i + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return fail(*i, "unpaired high surrogate");
                    *i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail(*i, "unpaired low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default: return fail(*i - 1, "unknown escape");
        }
    }
}

bool JsonDoc::parse_value(const char* d, std::size_t n, std::size_t* i, int depth,
                          std::size_t* out) {
    if (depth > kMaxDepth) return fail(*i, "nesting deeper than this reader accepts");
    skip_ws(d, n, i);
    if (*i >= n) return fail(*i, "expected a value");
    const char c = d[*i];

    if (c == '"') {
        JsonNode node;
        node.type = JsonType::String;
        if (!parse_string(d, n, i, &node.text)) return false;
        *out = push(std::move(node));
        return true;
    }
    if (c == '{' || c == '[') {
        const bool object = c == '{';
        ++*i;
        JsonNode node;
        node.type = object ? JsonType::Object : JsonType::Array;
        // Reserve the slot BEFORE parsing children: a child push can reallocate
        // `nodes_`, so a reference taken now would dangle.
        const std::size_t self = push(std::move(node));
        skip_ws(d, n, i);
        if (*i < n && d[*i] == (object ? '}' : ']')) {
            ++*i;
            *out = self;
            return true;
        }
        while (true) {
            std::string key;
            if (object) {
                skip_ws(d, n, i);
                if (!parse_string(d, n, i, &key)) return false;
                skip_ws(d, n, i);
                if (*i >= n || d[*i] != ':') return fail(*i, "expected ':'");
                ++*i;
            }
            std::size_t child = 0;
            if (!parse_value(d, n, i, depth + 1, &child)) return false;
            if (object) nodes_[self].keys.push_back(std::move(key));
            nodes_[self].children.push_back(child);
            skip_ws(d, n, i);
            if (*i >= n) return fail(*i, object ? "unterminated object" : "unterminated array");
            if (d[*i] == ',') {
                ++*i;
                continue;
            }
            if (d[*i] == (object ? '}' : ']')) {
                ++*i;
                *out = self;
                return true;
            }
            return fail(*i, "expected ',' or a closing bracket");
        }
    }
    if (c == 't' || c == 'f' || c == 'n') {
        const char* lit = c == 't' ? "true" : (c == 'f' ? "false" : "null");
        const std::size_t len = std::strlen(lit);
        if (*i + len > n || std::memcmp(d + *i, lit, len) != 0)
            return fail(*i, "expected true, false or null");
        JsonNode node;
        node.type = c == 'n' ? JsonType::Null : JsonType::Bool;
        node.boolean = c == 't';
        *i += len;
        *out = push(std::move(node));
        return true;
    }
    // A number. Validated against JSON's grammar BEFORE strtod, because strtod
    // also accepts "nan", "inf" and hex — none of which JSON has, and all of
    // which would otherwise enter a mesh as a coordinate.
    {
        const std::size_t start = *i;
        if (*i < n && d[*i] == '-') ++*i;
        if (*i >= n || !is_digit(d[*i])) return fail(start, "expected a number");
        if (d[*i] == '0') ++*i;
        else while (*i < n && is_digit(d[*i])) ++*i;
        if (*i < n && d[*i] == '.') {
            ++*i;
            if (*i >= n || !is_digit(d[*i])) return fail(*i, "expected a digit after '.'");
            while (*i < n && is_digit(d[*i])) ++*i;
        }
        if (*i < n && (d[*i] == 'e' || d[*i] == 'E')) {
            ++*i;
            if (*i < n && (d[*i] == '+' || d[*i] == '-')) ++*i;
            if (*i >= n || !is_digit(d[*i])) return fail(*i, "expected a digit in the exponent");
            while (*i < n && is_digit(d[*i])) ++*i;
        }
        JsonNode node;
        node.type = JsonType::Number;
        const std::string text(d + start, *i - start);
        node.number = std::strtod(text.c_str(), nullptr);
        *out = push(std::move(node));
        return true;
    }
}

const JsonNode& JsonDoc::member(const JsonNode& node, const char* name) const {
    if (node.type != JsonType::Object) return null_node();
    for (std::size_t k = 0; k < node.keys.size(); ++k)
        if (node.keys[k] == name) return nodes_[node.children[k]];
    return null_node();
}

bool JsonDoc::has(const JsonNode& node, const char* name) const {
    if (node.type != JsonType::Object) return false;
    for (const std::string& k : node.keys)
        if (k == name) return true;
    return false;
}

const JsonNode& JsonDoc::element(const JsonNode& node, std::size_t i) const {
    if (node.type != JsonType::Array || i >= node.children.size()) return null_node();
    return nodes_[node.children[i]];
}

double JsonDoc::number_or(const JsonNode& node, const char* name, double fallback) const {
    const JsonNode& v = member(node, name);
    return v.type == JsonType::Number ? v.number : fallback;
}

std::string JsonDoc::string_or(const JsonNode& node, const char* name,
                               const std::string& fallback) const {
    const JsonNode& v = member(node, name);
    return v.type == JsonType::String ? v.text : fallback;
}

}  // namespace io
}  // namespace clay
