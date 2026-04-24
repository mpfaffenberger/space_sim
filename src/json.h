#pragma once
// -----------------------------------------------------------------------------
// json.h — tiny hand-rolled JSON parser.
//
// Scope: parses the small config files we hand-write for system definitions.
// Supports objects, arrays, strings, numbers (double), booleans, null, and
// `// line comments` (non-spec but useful for human-authored configs).
//
// NOT in scope: streaming parse, schema validation, emitting JSON, unicode
// escape normalisation beyond basic `\n \t \" \\` pairs. When any of those
// matter, swap in nlohmann/json — the Value API here is a subset of theirs.
//
// Error handling: on parse failure, parse() returns a null Value and prints
// a `line:col: msg` diagnostic to stderr. Typed accessors throw std::runtime
// _error on type mismatch — caller is expected to guard with is_*() or
// contains() if the field might be absent.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace json {

struct Value {
    enum Type : uint8_t { Null_, Bool_, Number_, String_, Object_, Array_ };
    Type type = Null_;

    // Separate fields rather than std::variant — keeps compile times low
    // (no heavy <variant> include, no recursive-variant shenanigans). The
    // memory overhead (empty string/map/vector per Value) is cents.
    bool        b = false;
    double      n = 0.0;
    std::string s;
    std::unordered_map<std::string, Value> obj;
    std::vector<Value>                      arr;

    // ---- type predicates ------------------------------------------------
    bool is_null()   const { return type == Null_;   }
    bool is_bool()   const { return type == Bool_;   }
    bool is_number() const { return type == Number_; }
    bool is_string() const { return type == String_; }
    bool is_object() const { return type == Object_; }
    bool is_array()  const { return type == Array_;  }

    // ---- typed accessors (throw on mismatch) ---------------------------
    bool                as_bool()   const;
    double              as_number() const;
    float               as_float()  const;     // narrowing cast of as_number
    int                 as_int()    const;
    uint32_t            as_u32()    const;
    const std::string&  as_string() const;
    const std::unordered_map<std::string, Value>& as_object() const;
    const std::vector<Value>&                      as_array()  const;

    // ---- object access --------------------------------------------------
    // Throws if `key` missing or `this` isn't an object. Use contains() /
    // find() if the key is optional.
    const Value& operator[](const std::string& key) const;
    const Value& operator[](size_t idx) const;

    bool         contains(const std::string& key) const;
    const Value* find(const std::string& key)     const;   // nullptr if absent

    // ---- defaulted accessors (return fallback on missing/wrong type) ---
    double      number_or(double  fallback) const;
    bool        bool_or  (bool    fallback) const;
    std::string string_or(const std::string& fallback) const;
};

// Parse a JSON text. Returns Null Value on failure (and logs to stderr).
Value parse(std::string_view text);

// Convenience: read file + parse. Returns Null Value on IO or parse failure.
Value parse_file(const std::string& path);

} // namespace json
