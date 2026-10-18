// Tests for include/lfipc/bench/json.hpp. Spec: docs/design/001-clock.md
#include <lfipc/bench/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using lfipc::bench::json_writer;

std::string one(double v) {
    json_writer w;
    w.value(v);
    return w.str();
}

bool round_trips(double v) {
    const std::string s = one(v);
    double back = 0;
    std::from_chars(s.data(), s.data() + s.size(), back);
    return back == v;
}

}  // namespace

int main() {
    {
        json_writer w;
        w.begin_object();
        w.end_object();
        check(w.str() == "{}", "empty object");
    }
    {
        json_writer w;
        w.begin_object();
        w.member("a", 1);
        w.key("b");
        w.begin_array();
        w.value(true);
        w.begin_object();
        w.member("c", "x");
        w.end_object();
        w.end_array();
        w.key("d");
        w.begin_array();
        w.end_array();
        w.end_object();
        const char* want =
            "{\n"
            "  \"a\": 1,\n"
            "  \"b\": [\n"
            "    true,\n"
            "    {\n"
            "      \"c\": \"x\"\n"
            "    }\n"
            "  ],\n"
            "  \"d\": []\n"
            "}";
        check(w.str() == want, "nesting, commas and indentation");
    }
    {
        json_writer w;
        w.value("q\"b\\s\nt\tc\x01");
        check(w.str() == "\"q\\\"b\\\\s\\nt\\tc\\u0001\"", "strings escaped");
    }
    {
        json_writer w;
        w.begin_object();
        w.key("b");
        w.begin_inline_array();
        w.begin_array();
        w.value(1);
        w.value(2);
        w.end_array();
        w.begin_array();
        w.end_array();
        w.end_array();
        w.member("n", nullptr);
        w.member("o", std::optional<double>{});
        w.member("v", std::optional<int>{7});
        w.end_object();
        check(w.str() == "{\n  \"b\": [[1, 2], []],\n  \"n\": null,\n  \"o\": null,\n  \"v\": 7\n}",
              "inline arrays, nested inside stay inline; null and optional");
    }
    check(round_trips(0.1) && round_trips(2.1974531) && round_trips(1e-300) && round_trips(-31.5),
          "doubles read back to the same value");
    check(one(std::nan("")) == "null" && one(std::numeric_limits<double>::infinity()) == "null",
          "NaN and infinity written as null");
    {
        json_writer w;
        w.begin_array();
        w.value(std::numeric_limits<std::uint64_t>::max());
        w.value(std::numeric_limits<std::int64_t>::min());
        w.end_array();
        check(w.str() == "[\n  18446744073709551615,\n  -9223372036854775808\n]",
              "64-bit integer limits exact");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
