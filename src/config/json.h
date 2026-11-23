#pragma once
// Minimal RFC 8259 JSON reader for configuration files. Not a general-purpose library:
// no writer, no streaming. Config files are user input, so every error reports line:column.
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;  // UTF-8
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;  // keeps file order; duplicate keys are rejected

    const Value* find(std::string_view key) const;
};

struct ParseResult {
    std::optional<Value> value;
    std::string error;  // "line L, column C: message" when value is empty
};

ParseResult parse(std::string_view text);

}  // namespace mc::json
