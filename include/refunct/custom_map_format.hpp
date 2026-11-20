#pragma once
// Refunct TAS map v1 format. Portable, bounded JSON reader; no UE or Win32 dependency.
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refunct::custom_maps {
constexpr size_t kClusterCount = 31;
constexpr size_t kMapMaxBytes = 4 * 1024 * 1024;
constexpr size_t kMapMaxNodes = 20000;
struct Element {
    float x{}, y{}, z{}, pitch{}, yaw{}, roll{}, sizex{}, sizey{}, sizez{};
};
struct Cluster {
    float z{}, rise_speed{};
    std::array<std::vector<Element>, 6> elements;
};
struct Map { std::array<Cluster, kClusterCount> clusters{}; };
constexpr std::array<std::string_view, 6> kKinds = {
    "platforms", "cubes", "buttons", "lifts", "pipes", "springpads"
};
constexpr std::array<size_t, 6> kExpectedTotals = {251, 18, 37, 3, 9, 4};

namespace detail {
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string str;
    std::vector<Value> array;
    std::map<std::string, Value, std::less<>> object;
    const Value* field(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
};
class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}
    bool parse(Value& output, std::string& error) {
        if (input_.size() > kMapMaxBytes) return fail(error, "file is too large");
        if (input_.substr(0, 3) == "\xef\xbb\xbf") pos_ = 3;
        if (!value(output, 0, error)) return false;
        spaces();
        return pos_ == input_.size() || fail(error, "unexpected trailing data");
    }
private:
    std::string_view input_;
    size_t pos_ = 0, nodes_ = 0;
    bool fail(std::string& error, const char* reason) {
        error = std::string(reason) + " at byte " + std::to_string(pos_);
        return false;
    }
    void spaces() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' ||
               input_[pos_] == '\r' || input_[pos_] == '\n')) ++pos_;
    }
    bool accept(char c) { spaces(); if (pos_ < input_.size() && input_[pos_] == c) { ++pos_; return true; } return false; }
    bool quoted(std::string& result, std::string& error) {
        if (!accept('"')) return fail(error, "expected string");
        while (pos_ < input_.size()) {
            const unsigned char ch = static_cast<unsigned char>(input_[pos_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return fail(error, "control character in string");
            if (ch != '\\') { result.push_back(static_cast<char>(ch)); continue; }
            if (pos_ == input_.size()) return fail(error, "unfinished string escape");
            const char esc = input_[pos_++];
            switch (esc) {
                case '"': case '\\': case '/': result.push_back(esc); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (input_.size() - pos_ < 4) return fail(error, "bad unicode escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = input_[pos_++];
                        const int digit = h >= '0' && h <= '9' ? h - '0' :
                            h >= 'A' && h <= 'F' ? h - 'A' + 10 :
                            h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
                        if (digit < 0) return fail(error, "bad unicode escape");
                        code = (code << 4) | static_cast<unsigned>(digit);
                    }
                    // Map schema uses ASCII property names only. Preserve escaped
                    // ASCII; reject surrogate escapes rather than misinterpreting them.
                    if (code > 127) return fail(error, "non-ASCII escaped JSON key/value");
                    result.push_back(static_cast<char>(code));
                    break;
                }
                default: return fail(error, "invalid string escape");
            }
        }
        return fail(error, "unterminated string");
    }
    bool literal(std::string_view expected) {
        if (input_.substr(pos_, expected.size()) != expected) return false;
        pos_ += expected.size(); return true;
    }
    bool value(Value& out, int depth, std::string& error) {
        if (depth > 14 || ++nodes_ > kMapMaxNodes) return fail(error, "JSON too deeply nested or complex");
        spaces();
        if (pos_ >= input_.size()) return fail(error, "incomplete JSON");
        const char c = input_[pos_];
        if (c == '{') {
            out.type = Value::Type::Object; ++pos_; spaces();
            if (accept('}')) return true;
            do {
                std::string key;
                if (!quoted(key, error)) return false;
                if (!accept(':')) return fail(error, "expected colon");
                Value item;
                if (!value(item, depth + 1, error)) return false;
                if (!out.object.emplace(std::move(key), std::move(item)).second)
                    return fail(error, "duplicate object key");
                if (accept('}')) return true;
            } while (accept(','));
            return fail(error, "expected object comma or closing brace");
        }
        if (c == '[') {
            out.type = Value::Type::Array; ++pos_; spaces();
            if (accept(']')) return true;
            do {
                Value item;
                if (!value(item, depth + 1, error)) return false;
                out.array.push_back(std::move(item));
                if (accept(']')) return true;
            } while (accept(','));
            return fail(error, "expected array comma or closing bracket");
        }
        if (c == '"') { out.type = Value::Type::String; return quoted(out.str, error); }
        if (literal("null")) { out.type = Value::Type::Null; return true; }
        if (literal("true") || literal("false")) { out.type = Value::Type::Bool; return true; }
        const size_t begin = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return fail(error, "invalid number");
        if (input_[pos_] == '0') ++pos_;
        else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        } else return fail(error, "invalid number");
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_; const size_t start = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (pos_ == start) return fail(error, "invalid fraction");
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            const size_t start = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (pos_ == start) return fail(error, "invalid exponent");
        }
        double number = 0;
        const auto parsed = std::from_chars(input_.data() + begin, input_.data() + pos_, number);
        if (parsed.ec != std::errc{} || parsed.ptr != input_.data() + pos_ ||
            !std::isfinite(number)) return fail(error, "non-finite or invalid number");
        out.number = number; out.type = Value::Type::Number;
        return true;
    }
};
inline bool get_float(const Value& v, std::string_view key, float& out,
                      float limit, bool positive, std::string& error) {
    const Value* field = v.field(key);
    if (!field || field->type != Value::Type::Number ||
        !std::isfinite(field->number) ||
        (positive && field->number <= 0) ||
        std::abs(field->number) > limit) {
        error = "invalid or missing '" + std::string(key) + "'";
        return false;
    }
    out = static_cast<float>(field->number); return true;
}
} // namespace detail

inline bool Decode(std::string_view json, Map& map, std::string& error) {
    detail::Value root;
    if (!detail::Parser(json).parse(root, error)) return false;
    const auto* version = root.field("version");
    if (!version || version->type != detail::Value::Type::Number || version->number != 1) {
        error = "only Refunct TAS map version 1 is supported"; return false;
    }
    const auto* clusters = root.field("clusters");
    if (!clusters || clusters->type != detail::Value::Type::Array ||
        clusters->array.size() != kClusterCount) {
        error = "map must contain exactly 31 clusters"; return false;
    }
    std::array<size_t, 6> totals{};
    Map decoded;
    for (size_t i = 0; i < kClusterCount; ++i) {
        const auto& src = clusters->array[i];
        auto& dst = decoded.clusters[i];
        if (!detail::get_float(src, "z", dst.z, 10000000.f, false, error) ||
            !detail::get_float(src, "rise_speed", dst.rise_speed, 10000000.f, true, error)) {
            error += " in cluster " + std::to_string(i + 1); return false;
        }
        for (size_t kind = 0; kind < kKinds.size(); ++kind) {
            const auto* items = src.field(kKinds[kind]);
            if (!items || items->type != detail::Value::Type::Array ||
                items->array.size() > kExpectedTotals[kind]) {
                error = "invalid " + std::string(kKinds[kind]) + " list in cluster " +
                    std::to_string(i + 1); return false;
            }
            totals[kind] += items->array.size();
            for (const auto& item : items->array) {
                Element element;
                if (!detail::get_float(item,"x",element.x,10000000,false,error) ||
                    !detail::get_float(item,"y",element.y,10000000,false,error) ||
                    !detail::get_float(item,"z",element.z,10000000,false,error) ||
                    !detail::get_float(item,"pitch",element.pitch,36000,false,error) ||
                    !detail::get_float(item,"yaw",element.yaw,36000,false,error) ||
                    !detail::get_float(item,"roll",element.roll,36000,false,error) ||
                    !detail::get_float(item,"sizex",element.sizex,10000000,true,error) ||
                    !detail::get_float(item,"sizey",element.sizey,10000000,true,error) ||
                    !detail::get_float(item,"sizez",element.sizez,10000000,true,error)) {
                    error += " in " + std::string(kKinds[kind]) + " of cluster " +
                        std::to_string(i + 1); return false;
                }
                dst.elements[kind].push_back(element);
            }
        }
    }
    for (size_t kind = 0; kind < kKinds.size(); ++kind)
        if (totals[kind] != kExpectedTotals[kind]) {
            error = "expected " + std::to_string(kExpectedTotals[kind]) + " " +
                std::string(kKinds[kind]) + ", got " + std::to_string(totals[kind]);
            return false;
        }
    map = std::move(decoded);
    return true;
}
} // namespace refunct::custom_maps
