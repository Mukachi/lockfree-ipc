// Nearest-rank percentile index, shared by every percentile in the harness.
// Design: docs/design/001-clock.md, docs/design/002-histogram.md
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace lfipc::bench {

// 1-based nearest rank of percentile p in (0, 100] over n > 0 samples.
// Callers handle n == 0 themselves; there is no rank to return.
inline std::size_t nearest_rank(double p, std::size_t n) {
    // p * n first, and a small guard: 99.9 / 100 * 1000 is 999.0000000000001 in
    // floating point, and a bare ceil() would step to the next rank.
    const double exact = p * static_cast<double>(n) / 100.0;
    const auto rank = static_cast<std::size_t>(std::ceil(exact - 1e-9));
    return std::clamp<std::size_t>(rank, 1, n);
}

}  // namespace lfipc::bench
