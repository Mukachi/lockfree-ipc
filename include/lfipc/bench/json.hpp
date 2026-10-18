// Minimal JSON writer for run output. Design: docs/design/001-clock.md
#pragma once

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lfipc::bench {

// Streams pretty-printed JSON into a string. The caller nests correctly;
// the writer handles commas, indentation, escaping and number formatting.
class json_writer {
public:
    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    // An array written on one line, elements separated by ", ". Containers
    // opened inside it are on one line too.
    void begin_inline_array() { open('[', true); }
    void end_array() { close(']'); }

    void key(std::string_view k) {
        separate();
        string(k);
        out_ += ": ";
        after_key_ = true;
    }

    void value(bool b) { scalar(b ? "true" : "false"); }
    void value(std::nullptr_t) { scalar("null"); }
    template <typename T>
    void value(const std::optional<T>& v) {
        if (v) value(*v);
        else value(nullptr);
    }
    void value(std::string_view s) {
        separate();
        string(s);
    }
    void value(const char* s) { value(std::string_view(s)); }
    template <std::integral T>
    void value(T v) {
        char buf[24];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        scalar(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
    }
    // Shortest form that reads back to the same double. NaN and infinity are
    // not JSON, so they are written as null.
    void value(double v) {
        if (!std::isfinite(v)) return scalar("null");
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        scalar(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
    }

    template <typename T>
    void member(std::string_view k, const T& v) {
        key(k);
        value(v);
    }

    const std::string& str() const noexcept { return out_; }

private:
    void separate() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!open_.empty()) {
            level& l = open_.back();
            if (l.compact) {
                if (!l.empty) out_ += ", ";
            } else {
                if (!l.empty) out_ += ',';
                out_ += '\n';
                out_.append(2 * open_.size(), ' ');
            }
            l.empty = false;
        }
    }
    void open(char c, bool compact = false) {
        separate();
        out_ += c;
        open_.push_back({true, compact || (!open_.empty() && open_.back().compact)});
    }
    void close(char c) {
        const level l = open_.back();
        open_.pop_back();
        if (!l.empty && !l.compact) {
            out_ += '\n';
            out_.append(2 * open_.size(), ' ');
        }
        out_ += c;
    }
    void scalar(std::string_view s) {
        separate();
        out_ += s;
    }
    void string(std::string_view s) {
        static constexpr char hex[] = "0123456789abcdef";
        out_ += '"';
        for (const char ch : s) {
            const auto u = static_cast<unsigned char>(ch);
            switch (ch) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\t': out_ += "\\t"; break;
                case '\r': out_ += "\\r"; break;
                default:
                    if (u < 0x20) {
                        out_ += "\\u00";
                        out_ += hex[u >> 4];
                        out_ += hex[u & 0xf];
                    } else {
                        out_ += ch;
                    }
            }
        }
        out_ += '"';
    }

    struct level {
        bool empty;    // no element written yet
        bool compact;  // written on one line
    };
    std::string out_;
    std::vector<level> open_;
    bool after_key_ = false;
};

}  // namespace lfipc::bench
