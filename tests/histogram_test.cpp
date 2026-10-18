// Tests for include/lfipc/bench/histogram.hpp. Spec: docs/design/002-histogram.md
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/overhead.hpp>

#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using lfipc::bench::ticks;
using hist = lfipc::bench::histogram<>;

constexpr double bound = 1.0 / (1 << (hist::sub_bucket_bits - 1));  // 2^(1-k)

// Bucket edges hold v, and the upper edge is within the error bound (S2).
bool bucket_ok(ticks v) {
    const std::size_t i = hist::index(v);
    if (i >= hist::bucket_count) return false;
    const ticks up = hist::upper_edge(i);
    const ticks lo = i ? hist::upper_edge(i - 1) + 1 : 0;
    return lo <= v && v <= up && static_cast<double>(up - v) <= bound * static_cast<double>(v);
}

long minor_faults() {
    rusage u;
    getrusage(RUSAGE_THREAD, &u);
    return u.ru_minflt;
}

const double published[] = {50.0, 90.0, 99.0, 99.9, 99.99, 99.999};

// Every published percentile against a sorted copy (S2).
bool percentiles_ok(std::vector<ticks> v, const char* name) {
    hist h;
    for (ticks x : v) h.record(x);
    std::sort(v.begin(), v.end());
    bool ok = true;
    for (double p : published) {
        const ticks exact = lfipc::bench::percentile_sorted(v, p);
        const ticks got = h.value_at_rank(lfipc::bench::nearest_rank(p, v.size()));
        const bool in_exact_range = exact < (ticks{1} << hist::sub_bucket_bits);
        const bool good = in_exact_range ? got == exact
                                         : got >= exact && static_cast<double>(got - exact) <= bound * static_cast<double>(exact);
        if (!good) std::printf("info  %s p%g: exact %llu, histogram %llu\n", name, p,
                               (unsigned long long)exact, (unsigned long long)got);
        ok = ok && good;
    }
    return ok && h.min() == v.front() && h.max() == v.back() && h.count() == v.size();
}

}  // namespace

int main() {
    // Layout: k = 10, range 2^40.
    check(hist::bucket_count == 16384, "16,384 buckets for k = 10, range 2^40");
    check(hist::index((ticks{1} << 40) - 1) == hist::bucket_count - 1, "top of range is the last bucket");

    // S2: every value below 2^20, exactly; below 2^k, each value its own bucket.
    bool all = true, exact = true;
    for (ticks v = 0; v < (ticks{1} << 20); ++v) {
        all = all && bucket_ok(v);
        if (v < (ticks{1} << hist::sub_bucket_bits)) exact = exact && hist::upper_edge(hist::index(v)) == v;
    }
    check(all, "S2 every value below 2^20 lies in its bucket, within 2^(1-k)");
    check(exact, "S2 values below 2^k are exact");

    // S2: both sides of every power of two up to the range, and random values across it.
    bool edges = true;
    for (int e = 1; e < hist::max_bits; ++e) {
        const ticks p2 = ticks{1} << e;
        edges = edges && bucket_ok(p2 - 1) && bucket_ok(p2) && bucket_ok(p2 + 1);
    }
    std::mt19937_64 rng(42);
    for (int i = 0; i < 1'000'000; ++i) edges = edges && bucket_ok(rng() >> (64 - hist::max_bits));
    check(edges, "S2 power-of-two edges and 1M random values across the range");

    // Buckets are contiguous: each upper edge is one below the next bucket's lower.
    bool contiguous = true;
    for (std::size_t i = 0; i + 1 < hist::bucket_count; ++i)
        contiguous = contiguous && hist::index(hist::upper_edge(i)) == i &&
                     hist::index(hist::upper_edge(i) + 1) == i + 1;
    check(contiguous, "buckets tile the range with no gaps or overlaps");

    // S2: percentiles against a sorted copy, on three shapes.
    {
        std::vector<ticks> uniform, lognormal, bimodal;
        std::uniform_int_distribution<ticks> u(0, 900);
        std::lognormal_distribution<double> ln(6.0, 1.5);
        std::normal_distribution<double> fast(300, 20), slow(50'000, 4'000);
        for (int i = 0; i < 2'000'000; ++i) {
            uniform.push_back(u(rng));
            lognormal.push_back(static_cast<ticks>(ln(rng)));
            bimodal.push_back(static_cast<ticks>(std::max(1.0, i % 100 ? fast(rng) : slow(rng))));
        }
        check(percentiles_ok(uniform, "uniform"), "S2 percentiles exact on values below 2^k");
        check(percentiles_ok(lognormal, "lognormal"), "S2 percentiles within bound, lognormal tail");
        check(percentiles_ok(bimodal, "bimodal"), "S2 percentiles within bound, bimodal");
    }

    // S3: overflow keeps count and max exact; a rank in the overflow reports max.
    {
        hist h;
        for (int i = 0; i < 20; ++i) h.record(100);
        h.record(ticks{1} << 41);
        h.record(ticks{1} << 50);
        check(h.count() == 22 && h.overflow() == 2, "S3 overflow counted");
        check(h.max() == (ticks{1} << 50) && h.min() == 100, "S3 min and max exact beyond the range");
        check(h.value_at_rank(22) == (ticks{1} << 50), "S3 a rank in the overflow reports the exact max");
        check(h.value_at_rank(10) == 100, "values in range unaffected");
    }

    // S4: merging equals recording everything into one.
    {
        hist one, a, b, c;
        for (int i = 0; i < 300'000; ++i) {
            const ticks v = rng() >> (64 - 42);  // some beyond the range
            one.record(v);
            (i % 3 == 0 ? a : i % 3 == 1 ? b : c).record(v);
        }
        a.merge(b);
        a.merge(c);
        bool same = a.count() == one.count() && a.overflow() == one.overflow() && a.min() == one.min() &&
                    a.max() == one.max();
        for (std::size_t i = 0; i < hist::bucket_count; ++i) same = same && a.at(i) == one.at(i);
        check(same, "S4 merge equals recording into one");
    }

    // S5: the 10-sample rule.
    {
        hist h;
        for (int i = 0; i < 5'000; ++i) h.record(static_cast<ticks>(i));
        check(!h.percentile(99.99) && h.percentile(99.0), "S5 p99.99 of 5,000 is null, p99 is not");
        hist g;
        for (int i = 0; i < 10'000; ++i) g.record(static_cast<ticks>(i));
        check(g.percentile(99.9).has_value(), "S5 p99.9 of 10,000 published: 10 samples above its rank");
        hist f;
        for (int i = 0; i < 9'999; ++i) f.record(static_cast<ticks>(i));
        check(!f.percentile(99.9), "S5 p99.9 of 9,999 is null: 9 above its rank");
        check(!hist().percentile(50.0) && hist().min() == 0, "empty histogram reports nothing");
    }

    // S1: no page faults while recording, across every bucket.
    {
        std::vector<ticks> values(2'000'000);
        for (auto& v : values) v = rng() >> (64 - hist::max_bits);
        hist h;
        const long before = minor_faults();
        for (ticks v : values) h.record(v);
        const long faults = minor_faults() - before;
        std::printf("info  minor page faults while recording 2M values: %ld\n", faults);
        check(faults == 0, "S1 recording causes no page faults after construction");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
