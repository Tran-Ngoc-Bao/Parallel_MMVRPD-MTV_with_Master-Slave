#pragma once
#include <string>
#include <fstream>
#include <chrono>
#include <optional>
#include <vector>
#include "neighborhoods.hpp"

struct Solution;

struct Logger {
    size_t      _iteration   = 0;
    std::chrono::steady_clock::time_point _time_offset;
    std::string _outputs;
    std::string _problem;
    std::string _id;
    std::optional<std::ofstream> _writer;

    std::size_t total_evaluations = 0;

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
                  std::size_t total_evaluations = 0,
                  std::size_t total_elite_pushes = 0,
                  std::size_t accepted_elite_pushes = 0,
                  std::size_t evaluation_checkpoint_budget = 0,
                  const std::vector<double>& best_cost_by_evaluation_checkpoint = {},
                  std::size_t elite_pool_size = 0,
                  double elite_pool_diversity = 0.0,
                  const std::vector<double>& elite_pool_costs = {},
                  const std::vector<std::size_t>& worker_search_seeds = {},
                  const std::vector<std::size_t>& worker_coop_seeds = {},
                  std::size_t pull_offer_count = 0,
                  std::size_t pull_accept_count = 0,
                  std::size_t pull_request_count = 0,
                  const std::vector<std::size_t>& worker_pull_request_counts = {},
                  std::size_t pull_tolerance_satisfied_count = 0,
                  const std::vector<double>& best_solution_cost_by_evaluation_checkpoint = {});
};
