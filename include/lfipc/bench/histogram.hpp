// Log-linear latency histogram. Design: docs/design/002-histogram.md
#pragma once

#include <lfipc/bench/clock.hpp>
#include <lfipc/bench/rank.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace lfipc::bench {

// Values below 2^SubBits ticks are exact. Above, each power of two is split
// into 2^(SubBits-1) buckets, so a bucket is at most 2^(1-SubBits) of its
// value wide. Values from 2^MaxBits go to an overflow counter.
template <int SubBits = 10, int MaxBits = 40>
class histogram {
    static_assert(SubBits >= 2 && SubBits < MaxBits && MaxBits <= 63);

public:
    static constexpr int sub_bucket_bits = SubBits;
    static constexpr int max_bits = MaxBits;
    static constexpr std::size_t half = std::size_t{1} << (SubBits - 1);
    static constexpr std::size_t bucket_count = (std::size_t{MaxBits} - SubBits + 2) * half;

    // Counters are written here, so every page is mapped before the first sample (S1).
    histogram() : counts_(new std::uint64_t[bucket_count]) {
        std::fill(counts_.get(), counts_.get() + bucket_count, std::uint64_t{0});
    }

    // Branch-free index: the top set bit picks the power of two, the next
    // SubBits-1 bits the sub-bucket. OR-ing in `half` makes small values index
    // themselves without a branch, and keeps the bit scan off zero (with a
    // max() or a zero check, GCC emits a branch that mispredicts when values
    // straddle 2^SubBits).
    static constexpr std::size_t index(ticks v) noexcept {
        const int shift = static_cast<int>(std::bit_width(v | half)) - SubBits;
        return static_cast<std::size_t>(shift) * half + static_cast<std::size_t>(v >> shift);
    }

    // Largest value that maps to bucket i.
    static constexpr ticks upper_edge(std::size_t i) noexcept {
        const std::size_t group = i / half;
        if (group < 2) return i;
        const int shift = static_cast<int>(group) - 1;
        const ticks lower = static_cast<ticks>(half + i % half) << shift;
        return lower + (ticks{1} << shift) - 1;
    }

    void record(ticks v) noexcept {
        ++count_;
        min_ = std::min(min_, v);
        max_ = std::max(max_, v);
        if (v >> MaxBits) [[unlikely]] {
            ++overflow_;
            return;
        }
        ++counts_[index(v)];
    }

    void merge(const histogram& o) noexcept {
        for (std::size_t i = 0; i < bucket_count; ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        overflow_ += o.overflow_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t overflow() const noexcept { return overflow_; }
    ticks min() const noexcept { return count_ ? min_ : 0; }
    ticks max() const noexcept { return max_; }
    std::uint64_t at(std::size_t i) const noexcept { return counts_[i]; }

    // Nearest-rank percentile, reported as its bucket's upper edge (never below
    // the true value), capped at the exact max. Empty when fewer than 10 samples
    // lie above the rank (S5), or when there are no samples.
    std::optional<ticks> percentile(double p) const noexcept {
        if (count_ == 0) return std::nullopt;
        const std::size_t rank = nearest_rank(p, count_);
        if (count_ - rank < 10) return std::nullopt;
        return value_at_rank(rank);
    }

    // As percentile(), without the 10-sample rule. For tests and inspection.
    ticks value_at_rank(std::size_t rank) const noexcept {
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < bucket_count; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(upper_edge(i), max_);
        }
        return max_;  // the rank falls among the overflow samples
    }

    // Calls f(upper_edge, count) for every non-empty bucket, in order.
    template <typename F>
    void for_each_bucket(F&& f) const {
        for (std::size_t i = 0; i < bucket_count; ++i)
            if (counts_[i]) f(upper_edge(i), counts_[i]);
    }

private:
    std::unique_ptr<std::uint64_t[]> counts_;
    std::uint64_t count_ = 0;
    std::uint64_t overflow_ = 0;
    ticks min_ = std::numeric_limits<ticks>::max();
    ticks max_ = 0;
};

}  // namespace lfipc::bench
