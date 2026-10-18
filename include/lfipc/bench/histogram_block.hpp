// JSON block for a histogram. Design: docs/design/002-histogram.md
#pragma once

#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/json.hpp>

#include <optional>
#include <string_view>

namespace lfipc::bench {

// Writes `"<key>": {...}` into the object the caller has open. Percentiles and
// min/max in ns, converted here with the run's ticks_per_ns; buckets stay in
// ticks so a run can be re-analysed or merged later. No mean (S6).
template <int SubBits, int MaxBits>
void write_histogram_block(json_writer& w, const histogram<SubBits, MaxBits>& h, double ticks_per_ns,
                           std::string_view key = "histogram") {
    const auto ns = [ticks_per_ns](ticks t) { return static_cast<double>(t) / ticks_per_ns; };
    w.key(key);
    w.begin_object();
    w.member("sub_bucket_bits", SubBits);
    w.member("max_bits", MaxBits);
    w.member("count", h.count());
    w.member("overflow", h.overflow());
    w.member("min_ns", ns(h.min()));
    w.member("max_ns", ns(h.max()));

    w.key("percentiles_ns");
    w.begin_object();
    const struct {
        const char* name;
        double p;
    } published[] = {{"p50", 50.0},     {"p90", 90.0},       {"p99", 99.0},
                     {"p999", 99.9},    {"p9999", 99.99},    {"p99999", 99.999}};
    for (const auto& q : published) {
        const auto v = h.percentile(q.p);
        w.member(q.name, v ? std::optional<double>(ns(*v)) : std::nullopt);
    }
    w.end_object();

    w.key("buckets");
    w.begin_array();
    h.for_each_bucket([&w](ticks upper, std::uint64_t n) {
        w.begin_inline_array();
        w.value(upper);
        w.value(n);
        w.end_array();
    });
    w.end_array();
    w.end_object();
}

}  // namespace lfipc::bench
