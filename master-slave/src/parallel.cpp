#include "parallel.hpp"

#include <mpi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <random>
#include <ctime>
#include <map>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "logger.hpp"
#include "solutions.hpp"

namespace parallel {
namespace {

constexpr int TAG_JOB    = 101;
constexpr int TAG_RESULT = 102;
constexpr int TAG_ELITE_PUSH = 104;  // worker -> master: push elite
constexpr int TAG_ELITE_PULL = 105;  // worker -> master: pull request
constexpr int TAG_ELITE_REPLY = 106; // master -> worker: reply to pull request
constexpr int TAG_STATS = 107;       // worker -> master: neighborhood evaluation/selection counts

void send_string_impl(int dest, int tag, const std::string& payload)
{
    int size = static_cast<int>(payload.size());
    MPI_Send(&size, 1, MPI_INT, dest, tag, MPI_COMM_WORLD);
    if (size > 0) {
        MPI_Send(payload.data(), size, MPI_CHAR, dest, tag, MPI_COMM_WORLD);
    }
}

std::string recv_string_impl(int source, int tag)
{
    MPI_Status status{};
    int size = 0;
    MPI_Recv(&size, 1, MPI_INT, source, tag, MPI_COMM_WORLD, &status);
    std::string payload(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        MPI_Recv(payload.data(), size, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
    }
    return payload;
}

void send_job_start(int dest, std::size_t seed, int preset_index)
{
    nlohmann::json j;
    j["seed"]         = seed;
    j["preset_index"] = preset_index;
    send_string_impl(dest, TAG_JOB, j.dump());
}

void send_stop_signal(int dest, int tag)
{
    int size = -1;
    MPI_Send(&size, 1, MPI_INT, dest, tag, MPI_COMM_WORLD);
}

struct WorkerPreset {
    double tabu_size_factor;
    double gamma_1, gamma_2, gamma_3, gamma_4;
};
static constexpr WorkerPreset WORKER_PRESETS[4] = {
    {1.25, 0.5,  0.2,  0.1,  0.2 },  // Best-solution (max tabu, max γ1-γ2 gap, stable)
    {0.25, 0.5,  0.3,  0.1,  0.6 },  // Fast-search (min tabu, max γ4)
    {0.75, 0.5,  0.4,  0.3,  0.5 },  // Diversification (tight gammas, no discrimination)
    {0.75, 0.3,  0.2,  0.1,  0.3 },  // Baseline (paper)
};

static void apply_worker_hyperparams(Config& cfg, int preset_index)
{
    using WH = cli::WorkerHyperparams;
    if (cfg.worker_hyperparams == WH::Fixed) return;

    if (cfg.worker_hyperparams == WH::Preset) {
        const WorkerPreset& p = WORKER_PRESETS[preset_index % 4];
        cfg.tabu_size_factor = p.tabu_size_factor;
        cfg.gamma_1          = p.gamma_1;
        cfg.gamma_2          = p.gamma_2;
        cfg.gamma_3          = p.gamma_3;
        cfg.gamma_4          = p.gamma_4;
        return;
    }

    std::mt19937_64 rng(cfg.seed ? *cfg.seed
                                 : static_cast<std::uint64_t>(std::random_device{}()));

    auto draw_scaled_step = [&](int begin_unit, int end_unit, double scale) -> double {
        if (end_unit < begin_unit) std::swap(begin_unit, end_unit);
        return static_cast<double>(
            std::uniform_int_distribution<int>(begin_unit, end_unit)(rng)
        ) / scale;
    };

    std::vector<double> gammas;
    gammas.reserve(3);
    const double EPS = 1e-9;
    while (gammas.size() < 3) {
        double v = draw_scaled_step(1, 5, 10.0);
        bool dup = false;
        for (double x : gammas) {
            if (std::fabs(x - v) < EPS) { dup = true; break; }
        }
        if (!dup) gammas.push_back(v);
    }

    std::sort(gammas.begin(), gammas.end(), std::greater<double>());
    cfg.gamma_1 = gammas[0];
    cfg.gamma_2 = gammas[1];
    cfg.gamma_3 = gammas[2];

    cfg.gamma_4          = draw_scaled_step(2, 6, 10.0);
    cfg.tabu_size_factor = draw_scaled_step(1, 5, 4.0);
}

static void randomize_worker_adaptive_hyperparams(Config& cfg)
{
    if (!cfg.randomize_worker_adaptive_hyperparams) {
        return;
    }

    std::mt19937_64 rng(cfg.seed ? *cfg.seed
                                 : static_cast<std::uint64_t>(std::random_device{}()));

    const std::size_t adaptive_iterations_base = std::max<std::size_t>(1, cfg.adaptive_iterations);
    const std::size_t adaptive_pull_segments_base = std::max<std::size_t>(1, cfg.adaptive_pull_elite_segments);

    const std::size_t iter_min = std::max<std::size_t>(1, adaptive_iterations_base - 3);
    const std::size_t iter_max = std::max<std::size_t>(iter_min, adaptive_iterations_base + 3);
    const std::size_t segment_min = std::max<std::size_t>(1, adaptive_pull_segments_base - 2);
    const std::size_t segment_max = std::max<std::size_t>(segment_min, adaptive_pull_segments_base + 2);

    std::uniform_int_distribution<std::size_t> iter_dist(iter_min, iter_max);
    std::uniform_int_distribution<std::size_t> segment_dist(segment_min, segment_max);

    cfg.adaptive_iterations = iter_dist(rng);
    cfg.adaptive_pull_elite_segments = segment_dist(rng);
}

static bool recv_solution_or_stop(int source, int tag, Solution& solution)
{
    MPI_Status status{};
    int size = 0;
    MPI_Recv(&size, 1, MPI_INT, source, tag, MPI_COMM_WORLD, &status);
    if (size < 0) {
        return false;
    }

    std::string payload(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        MPI_Recv(payload.data(), size, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
    }
    solution = Solution::from_json(nlohmann::json::parse(payload));
    return true;
}

struct JobStart { std::size_t seed; int preset_index; };
JobStart recv_job_start(int source)
{
    nlohmann::json j = nlohmann::json::parse(recv_string_impl(source, TAG_JOB));
    return {j.value("seed", std::size_t{0}), j.value("preset_index", -1)};
}

void send_solution(int dest, int tag, const Solution& solution)
{
    send_string_impl(dest, tag, solution.to_json().dump());
}

Solution recv_solution(int source, int tag)
{
    return Solution::from_json(nlohmann::json::parse(recv_string_impl(source, tag)));
}

void send_stats(int dest,
                 const std::array<std::size_t, NEIGHBORHOOD_COUNT>& evaluations,
                 const std::array<std::size_t, NEIGHBORHOOD_COUNT>& selections)
{
    nlohmann::json j;
    j["evaluations"] = evaluations;
    j["selections"]  = selections;
    send_string_impl(dest, TAG_STATS, j.dump());
}

void recv_stats(int source,
                 std::array<std::size_t, NEIGHBORHOOD_COUNT>& evaluations,
                 std::array<std::size_t, NEIGHBORHOOD_COUNT>& selections)
{
    nlohmann::json j = nlohmann::json::parse(recv_string_impl(source, TAG_STATS));
    evaluations = j.at("evaluations").get<std::array<std::size_t, NEIGHBORHOOD_COUNT>>();
    selections  = j.at("selections").get<std::array<std::size_t, NEIGHBORHOOD_COUNT>>();
}

bool is_valid_solution_for_exchange(const Solution& s)
{
    if (!s.feasible) return false;
    try {
        s.verify();
        return true;
    } catch (...) {
        return false;
    }
}

static std::size_t
count_diff_elite(const Solution& a, const Solution& b)
{
    auto pack_edge = [](std::size_t u, std::size_t v) -> uint64_t {
        if (u > v) std::swap(u, v);
        return (static_cast<uint64_t>(u) << 32) | static_cast<uint64_t>(v);
    };

    std::unordered_map<uint64_t, std::size_t> ea, eb;
    auto collect_edge_counts = [&](const Solution& sol, std::unordered_map<uint64_t, std::size_t>& out) {
        out.clear();
        for (const auto& truck_vehicle : sol.truck_routes) {
            for (const auto& route : truck_vehicle) {
                const auto& customers = route->data().customers;
                if (customers.size() < 2) continue;
                for (std::size_t i = 0; i < customers.size(); ++i) {
                    std::size_t u = customers[i];
                    std::size_t v = customers[(i + 1) % customers.size()];
                    out[pack_edge(u, v)] += 1;
                }
            }
        }
        for (const auto& drone_vehicle : sol.drone_routes) {
            for (const auto& route : drone_vehicle) {
                const auto& customers = route->data().customers;
                if (customers.size() < 2) continue;
                for (std::size_t i = 0; i < customers.size(); ++i) {
                    std::size_t u = customers[i];
                    std::size_t v = customers[(i + 1) % customers.size()];
                    out[pack_edge(u, v)] += 1;
                }
            }
        }
    };

    collect_edge_counts(a, ea);
    collect_edge_counts(b, eb);

    std::size_t edge_loss_raw = 0;
    for (const auto& p : ea) {
        const std::size_t ca = p.second;
        std::size_t cb = 0;
        const auto itb = eb.find(p.first);
        if (itb != eb.end()) {
            cb = itb->second;
        }
        if (ca > cb) {
            edge_loss_raw += (ca - cb);
        }
    }

    return edge_loss_raw;
}

struct ElitePool {
    explicit ElitePool(std::size_t keep_count) : keep_count(keep_count) {}

    void consider(Solution candidate, int source_worker, bool prefer_pulled_flag)
    {
        if (!is_valid_solution_for_exchange(candidate)) {
            return;
        }

        if (solutions.size() < keep_count) {
            solutions.push_back({std::move(candidate), source_worker});
        } else {
            auto choose_replacement = [&](bool prefer_pulled_only) {
                std::size_t chosen_idx = 0;
                double min_diff = std::numeric_limits<double>::infinity();
                bool found = false;

                for (std::size_t i = 0; i < solutions.size(); ++i) {
                    if (prefer_pulled_only && solutions[i].pull_count == 0) {
                        continue;
                    }

                    double diff = static_cast<double>(count_diff_elite(candidate, solutions[i].sol));
                    if (!found || diff < min_diff) {
                        min_diff = diff;
                        chosen_idx = i;
                        found = true;
                    }
                }

                return found ? chosen_idx : std::size_t{0};
            };

            std::size_t most_similar_idx = choose_replacement(prefer_pulled_flag);
            if (prefer_pulled_flag && solutions[most_similar_idx].pull_count == 0) {
                most_similar_idx = choose_replacement(false);
            }

            solutions[most_similar_idx] = {std::move(candidate), source_worker};
        }
    }

    bool empty() const
    {
        return solutions.empty();
    }

    const Solution& pick_for_dispatch(int excluded_worker, std::mt19937& rng, cli::ElitePullStrategy strategy)
    {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < solutions.size(); ++i) {
            if (solutions[i].source_worker != excluded_worker) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            ++solutions.front().pull_count;
            return solutions.front().sol;
        }

        auto mark_and_return = [&](std::size_t idx) -> const Solution& {
            ++solutions[idx].pull_count;
            return solutions[idx].sol;
        };

        auto pick_random = [&]() -> const Solution& {
            std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
            return mark_and_return(candidates[dist(rng)]);
        };

        auto pick_topk = [&](std::size_t k) -> const Solution& {
            std::vector<std::size_t> sorted = candidates;
            std::sort(sorted.begin(), sorted.end(), [&](std::size_t lhs, std::size_t rhs) {
                return solutions[lhs].sol.cost() < solutions[rhs].sol.cost();
            });
            k = std::min(k, sorted.size());
            std::uniform_int_distribution<std::size_t> dist(0, k - 1);
            return mark_and_return(sorted[dist(rng)]);
        };

        auto pick_rank_based = [&]() -> const Solution& {
            std::vector<std::size_t> sorted = candidates;
            std::sort(sorted.begin(), sorted.end(), [&](std::size_t lhs, std::size_t rhs) {
                return solutions[lhs].sol.cost() < solutions[rhs].sol.cost();
            });
            std::vector<double> weights(sorted.size());
            for (std::size_t i = 0; i < sorted.size(); ++i)
                weights[i] = 1.0 / static_cast<double>(i + 1);
            std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
            return mark_and_return(sorted[dist(rng)]);
        };

        auto pick_pullcount_based = [&]() -> const Solution& {
            std::vector<double> weights;
            weights.reserve(candidates.size());
            for (std::size_t idx : candidates) {
                weights.push_back(1.0 / (1.0 + static_cast<double>(solutions[idx].pull_count)));
            }
            std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
            return mark_and_return(candidates[dist(rng)]);
        };

        auto pick_diverse = [&]() -> const Solution& {
            std::size_t best_idx = candidates.front();
            double best_score = std::numeric_limits<double>::lowest();
            for (std::size_t idx : candidates) {
                double total_score = 0.0;
                std::size_t count = 0;
                for (std::size_t other = 0; other < solutions.size(); ++other) {
                    if (other == idx) continue;
                    double diff = static_cast<double>(count_diff_elite(
                        solutions[idx].sol,
                        solutions[other].sol));
                    total_score += diff;
                    ++count;
                }
                double score = count > 0 ? total_score / static_cast<double>(count) : 0.0;
                if (score > best_score) {
                    best_score = score;
                    best_idx = idx;
                }
            }
            return mark_and_return(best_idx);
        };

        switch (strategy) {
            case cli::ElitePullStrategy::Random:    return pick_random();
            case cli::ElitePullStrategy::TopK: {
                // derive k from pool size (keep_count)
                std::size_t k = std::max<std::size_t>(1, keep_count / 2);
                return pick_topk(k);
            }
            case cli::ElitePullStrategy::Rank:      return pick_rank_based();
            case cli::ElitePullStrategy::PullCount: return pick_pullcount_based();
            case cli::ElitePullStrategy::Diverse:   return pick_diverse();
        }

        return pick_random();
    }

private:
    struct Entry {
        Solution sol;
        int source_worker;
        std::size_t pull_count = 0;
    };

    std::size_t keep_count;
    std::vector<Entry> solutions;
};

std::size_t compute_min_pull_elites_per_worker(const Config& cfg, int world_size)
{
    constexpr std::size_t kMinClamp = 1;
    constexpr std::size_t kMaxClamp = 30;

    const double scaled = cfg.min_pull_elites_per_worker_factor * std::sqrt(static_cast<double>(cfg.customers_count));
    const std::size_t derived = static_cast<std::size_t>(std::ceil(std::max(1.0, scaled)));
    return std::clamp(derived, kMinClamp, kMaxClamp);
}

std::size_t compute_elite_pool_size(const Config& cfg, int world_size)
{
    constexpr std::size_t kMinClamp = 2;
    constexpr std::size_t kMaxClamp = 20;

    const double scaled = cfg.elite_pool_factor * std::sqrt(static_cast<double>(cfg.customers_count)) * static_cast<double>(world_size - 1);
    const std::size_t derived = static_cast<std::size_t>(std::ceil(scaled));
    return std::clamp(derived, kMinClamp, kMaxClamp);
}

} // namespace

Solution run_master(int world_size)
{
    const auto t0 = std::chrono::steady_clock::now();
    const Config base_cfg = global_config();
    bool prefer_pulled_flag = base_cfg.prefer_pulled;
    // Keep a valid fallback so verify() never receives an empty solution.
    Solution best_solution = Solution::initialize();
    const std::size_t elite_keep_count = compute_elite_pool_size(base_cfg, world_size);
    const std::size_t min_pull_elites_per_worker = compute_min_pull_elites_per_worker(base_cfg, world_size);
    ElitePool elite_pool(elite_keep_count);
    const auto t1 = std::chrono::steady_clock::now();

    struct ConvergenceRecord { double time_ms, cost_min; };
    std::vector<ConvergenceRecord> conv_records;
    auto record_convergence = [&](const Solution& sol) {
        if (!base_cfg.save_convergence) return;
        double t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        conv_records.push_back({t_ms, sol.cost() / 60.0});
    };
    record_convergence(best_solution);

    std::size_t next_seed = base_cfg.seed ? *base_cfg.seed : 1;

    std::vector<bool> worker_running(static_cast<std::size_t>(world_size), false);
    std::vector<std::size_t> worker_pull_requests(static_cast<std::size_t>(world_size), 0);
    bool stop_after_min_pulls = false;
    std::size_t active_workers = 0;
    std::array<std::size_t, NEIGHBORHOOD_COUNT> total_neighborhood_evals{};
    std::array<std::size_t, NEIGHBORHOOD_COUNT> total_neighborhood_selects{};
    std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));

    std::vector<int> preset_assignments(static_cast<std::size_t>(world_size - 1), -1);
    if (base_cfg.worker_hyperparams == cli::WorkerHyperparams::Preset) {
        const int n          = world_size - 1;
        const int n_baseline = n / 2;
        const int n_special  = n - n_baseline;   // ceil(n/2)
        const int base       = n_special / 3;
        const int remainder  = n_special % 3;

        // Special configs 0-2: evenly distributed among n_special workers
        std::vector<int> pool = {0, 1, 2};
        std::shuffle(pool.begin(), pool.end(), rng);

        int idx = 0;
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < base + (c < remainder ? 1 : 0); ++k)
                preset_assignments[static_cast<std::size_t>(idx++)] = pool[c];

        // Remaining n_baseline workers get Baseline (config 3)
        for (int k = 0; k < n_baseline; ++k)
            preset_assignments[static_cast<std::size_t>(idx++)] = 3;

        std::shuffle(preset_assignments.begin(), preset_assignments.end(), rng);
    }

    auto all_running_workers_reached_min_pull = [&]() {
        for (int worker_rank = 1; worker_rank < world_size; ++worker_rank) {
            const std::size_t idx = static_cast<std::size_t>(worker_rank);
            if (worker_running[idx] && worker_pull_requests[idx] < min_pull_elites_per_worker) {
                return false;
            }
        }
        return true;
    };

    auto start_worker = [&](int worker_rank) {
        const int preset_idx = preset_assignments[static_cast<std::size_t>(worker_rank - 1)];
        send_job_start(worker_rank, next_seed++, preset_idx);
        worker_running[static_cast<std::size_t>(worker_rank)] = true;
        ++active_workers;
    };

    for (int worker_rank = 1; worker_rank < world_size; ++worker_rank) {
        start_worker(worker_rank);
    }

    while (active_workers > 0) {
        bool processed = false;

        // Handle elite pushes from workers
        for (;;) {
            MPI_Status status{};
            int ready = 0;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_ELITE_PUSH, MPI_COMM_WORLD, &ready, &status);
            if (!ready) break;
            processed = true;
            const int worker_rank = status.MPI_SOURCE;
            Solution elite = recv_solution(worker_rank, TAG_ELITE_PUSH);
            if (is_valid_solution_for_exchange(elite)) {
                if (!best_solution.feasible || elite.cost() < best_solution.cost()) {
                    best_solution = elite;
                    record_convergence(best_solution);
                }
                elite_pool.consider(std::move(elite), worker_rank, prefer_pulled_flag);
            }
        }

        // Handle elite pull from workers
        for (;;) {
            MPI_Status status{};
            int ready = 0;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_ELITE_PULL, MPI_COMM_WORLD, &ready, &status);
            if (!ready) break;
            processed = true;
            const int worker_rank = status.MPI_SOURCE;
            (void)recv_string_impl(worker_rank, TAG_ELITE_PULL);

            const std::size_t worker_idx = static_cast<std::size_t>(worker_rank);
            ++worker_pull_requests[worker_idx];
            if (!stop_after_min_pulls && all_running_workers_reached_min_pull()) {
                stop_after_min_pulls = true;
            }

            if (stop_after_min_pulls) {
                send_stop_signal(worker_rank, TAG_ELITE_REPLY);
            } else if (elite_pool.empty()) {
                // No elite yet: send current best fallback instead of an empty solution.
                send_solution(worker_rank, TAG_ELITE_REPLY, best_solution);
            } else {
                const Solution& elite_to_send = elite_pool.pick_for_dispatch(
                    worker_rank, rng, base_cfg.elite_pull_strategy);
                send_solution(worker_rank, TAG_ELITE_REPLY, elite_to_send);
            }
        }

        // Handle final results from workers
        for (;;) {
            MPI_Status status{};
            int ready = 0;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_RESULT, MPI_COMM_WORLD, &ready, &status);
            if (!ready) break;
            processed = true;
            const int worker_rank = status.MPI_SOURCE;
            Solution result = recv_solution(worker_rank, TAG_RESULT);
            if (is_valid_solution_for_exchange(result) && (!best_solution.feasible || result.cost() < best_solution.cost())) {
                best_solution = result;
                record_convergence(best_solution);
            }
            if (is_valid_solution_for_exchange(result)) {
                elite_pool.consider(std::move(result), worker_rank, prefer_pulled_flag);
            }

            std::array<std::size_t, NEIGHBORHOOD_COUNT> worker_evals{};
            std::array<std::size_t, NEIGHBORHOOD_COUNT> worker_selects{};
            recv_stats(worker_rank, worker_evals, worker_selects);
            for (std::size_t i = 0; i < NEIGHBORHOOD_COUNT; ++i) {
                total_neighborhood_evals[i]   += worker_evals[i];
                total_neighborhood_selects[i] += worker_selects[i];
            }

            worker_running[static_cast<std::size_t>(worker_rank)] = false;
            --active_workers;
        }

        if (!processed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    const auto t2 = std::chrono::steady_clock::now();

    const double init_sec = std::chrono::duration<double>(t1 - t0).count();
    const double loop_sec = std::chrono::duration<double>(t2 - t1).count();
    const double total_sec = std::chrono::duration<double>(t2 - t0).count();
    std::cerr << std::fixed << std::setprecision(6)
              << "Timing (mode=parallel-master-slave, unit=s): init="
              << init_sec << " search=" << loop_sec
              << " total=" << total_sec << "\n";

    if (base_cfg.save_convergence && !conv_records.empty()) {
        namespace fs = std::filesystem;
        fs::path out(base_cfg.outputs);
        if (!fs::is_directory(out)) fs::create_directories(out);
        const std::string stem = fs::path(base_cfg.problem).stem().string();
        const char id_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 id_rng{std::random_device{}()};
        std::uniform_int_distribution<int> char_dist(0, 61);
        std::string run_id(8, ' ');
        for (auto& c : run_id) c = id_chars[char_dist(id_rng)];
        fs::path conv_path = out / (stem + "-" + run_id + "-convergence.csv");
        std::ofstream f(conv_path);
        if (f) {
            f << "sep=,\ntime_ms,cost_min\n";
            f << std::fixed;
            for (const auto& r : conv_records)
                f << std::setprecision(1) << r.time_ms << ','
                  << std::setprecision(6) << r.cost_min << '\n';
            std::cerr << "Convergence log written to " << conv_path.string() << "\n";
        }
    }

    Logger logger;
    logger.finalize(best_solution, 0, 0, 0, 0, 0, 0.0, 0.0,
                     total_neighborhood_evals, total_neighborhood_selects);
    return best_solution;
}

void run_worker(int /*rank*/)
{
    const Config base_cfg = global_config();

    auto [seed, preset_index] = recv_job_start(0);

    Config worker_cfg = base_cfg;
    worker_cfg.seed = seed;
    worker_cfg.disable_logging = true;
    apply_worker_hyperparams(worker_cfg, preset_index);
    randomize_worker_adaptive_hyperparams(worker_cfg);
    set_global_config(worker_cfg);
    Solution root = Solution::initialize();

    Solution::EliteHooks hooks;
    hooks.push_elite = [&](std::size_t, const Solution& elite) {
        if (is_valid_solution_for_exchange(elite)) {
            send_solution(0, TAG_ELITE_PUSH, elite);
        }
    };
    bool stop_requested = false;
    hooks.should_stop = [&]() {
        return stop_requested;
    };

    hooks.pull_elite = [&](std::size_t iteration, Solution& pulled_elite) -> bool {
        nlohmann::json j;
        j["iteration"] = iteration;
        send_string_impl(0, TAG_ELITE_PULL, j.dump());
        if (!recv_solution_or_stop(0, TAG_ELITE_REPLY, pulled_elite)) {
            stop_requested = true;
            return false;
        }
        return is_valid_solution_for_exchange(pulled_elite);
    };

    Logger logger;
    Solution result = Solution::tabu_search(root, logger, &hooks);
    send_solution(0, TAG_RESULT, result);
    send_stats(0, logger.neighborhood_evaluations, logger.neighborhood_selections);
}

} // namespace parallel