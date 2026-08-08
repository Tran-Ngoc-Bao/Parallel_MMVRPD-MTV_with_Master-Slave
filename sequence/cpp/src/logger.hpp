#pragma once
#include <string>
#include <fstream>
#include <chrono>
#include <optional>
#include <array>
#include "neighborhoods.hpp"

struct Solution;

struct Logger {
    size_t      _iteration   = 0;
    std::chrono::steady_clock::time_point _time_offset;
    std::string _outputs;
    std::string _problem;
    std::string _id;
    std::optional<std::ofstream> _writer;

    Logger();  // initializes, creates CSV file if logging enabled
    ~Logger() = default;

    void log(const Solution& solution,
             Neighborhood neighbor,
             const std::vector<std::vector<size_t>>& tabu_list);

    void finalize(const Solution& result,
                  size_t tabu_size,
                  size_t reset_after,
                  size_t actual_adaptive_iterations,
                  size_t total_adaptive_segments,
                  size_t last_improved,
                  double post_optimization,
                  double post_optimization_elapsed,
                  const std::array<std::size_t, NEIGHBORHOOD_COUNT>& neighborhood_evaluations = {},
                  const std::array<std::size_t, NEIGHBORHOOD_COUNT>& neighborhood_selections = {});
};
