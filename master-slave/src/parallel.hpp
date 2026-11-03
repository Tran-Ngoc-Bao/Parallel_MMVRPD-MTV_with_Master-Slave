#pragma once

#include <cstddef>
#include <vector>

struct Solution;

namespace parallel {

// Runs the master-slave tabu search with `workers` worker threads plus the
// master in the calling thread, all on the global config of the calling
// thread. With a non-empty `cpus`, worker r (1..workers) is pinned to
// cpus[(r - 1) % cpus.size()] and the master to cpus[workers] when the list
// is longer than `workers`.
Solution run(std::size_t workers, const std::vector<int>& cpus);

} // namespace parallel
