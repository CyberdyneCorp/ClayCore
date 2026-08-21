#pragma once

// A minimal JSON reader, for glTF and nothing else (file-io spec).
//
// Dependency-free by the same rule the GLB WRITER follows, and error-code
// based because the core builds with -fno-exceptions. It parses into a flat
// arena of nodes rather than a tree of owning pointers, so a document is freed
// without walking a deep structure and a child reference is just an index.
//
// It reads UNTRUSTED input, so the limits are part of the contract rather than
// an afterthought: nesting is bounded (a file of ten thousand `[` would
// otherwise recurse until the stack ends), and every lookup is checked. What
// it does NOT do is as important — no duplicate-key merging, no comments, no
// trailing commas, no NaN or Infinity literals. glTF forbids all of them, and
// accepting them would mean this reader accepts files the format does not.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clay {
namespace io {

enum class JsonType : std::uint8_t { Null, Bool, Number, String, Array, Object };

struct JsonNode {
    JsonType type = JsonType::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                    // String nodes only
    std::vector<std::string> keys;       // Object only; parallel to children
    std::vector<std::size_t> children;   // Array and Object
};

class JsonDoc {
  public:
    // The deepest nesting accepted. glTF documents are three or four levels;
    // anything approaching this is an attack rather than an asset.
    static constexpr int kMaxDepth = 64;

    // Parses `size` bytes. On failure returns false and leaves `error` set to
    // a message naming the byte offset, which is what makes a malformed asset
    // diagnosable rather than merely rejected.
    bool parse(const char* data, std::size_t size);

    const std::string& error() const { return error_; }
    bool empty() const { return nodes_.empty(); }
    const JsonNode& root() const { return nodes_[root_]; }
    const JsonNode& at(std::size_t index) const { return nodes_[index]; }

    // Object member by name, or null when absent or when `node` is not an
    // object. Returning a sentinel rather than a pointer keeps every call site
    // free of a null check it would otherwise forget.
    const JsonNode& member(const JsonNode& node, const char* name) const;
    bool has(const JsonNode& node, const char* name) const;

    // Array element, or null when out of range or not an array.
    const JsonNode& element(const JsonNode& node, std::size_t i) const;

    // Typed reads with a default, so a missing or wrong-typed field is a
    // documented fallback rather than a branch at every call site.
    double number_or(const JsonNode& node, const char* name, double fallback) const;
    std::string string_or(const JsonNode& node, const char* name,
                          const std::string& fallback) const;

  private:
    bool parse_value(const char* d, std::size_t n, std::size_t* i, int depth, std::size_t* out);
    bool parse_string(const char* d, std::size_t n, std::size_t* i, std::string* out);
    bool fail(std::size_t at, const char* why);
    void skip_ws(const char* d, std::size_t n, std::size_t* i) const;
    std::size_t push(JsonNode&& node);

    std::vector<JsonNode> nodes_;
    std::size_t root_ = 0;
    std::string error_;
};

}  // namespace io
}  // namespace clay
