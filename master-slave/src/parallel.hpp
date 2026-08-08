#pragma once

struct Solution;

namespace parallel {

Solution run_master(int world_size);
void run_worker(int rank);

} // namespace parallel