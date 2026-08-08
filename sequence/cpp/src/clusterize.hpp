#pragma once
#include <vector>
#include <cstddef>

// Angular sweep clustering: assigns customers to k clusters
// `customers` is modified in-place (sorted/rotated)
std::vector<std::vector<size_t>> clusterize(std::vector<size_t>& customers, size_t k);
