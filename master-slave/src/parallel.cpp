#include "parallel.hpp"

#include <mpi.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <list>
#include <string>
#include <thread>
#include <random>
#include <ctime>
#include <map>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <optional>

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
constexpr int TAG_STATS = 107;       // worker -> master: total evaluations and elite-push counts
constexpr int TAG_EVAL_PROGRESS = 108; // worker -> master: running evaluations count, sent every 1/128 of its budget

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

// No elite available to send, but the worker should keep running.
void send_empty_signal(int dest, int tag)
{
    int size = -2;
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

enum class PullReply { Solution, Empty, Stop };

// Sentinels: size == -1 means Stop, size == -2 means Empty (see send_stop_signal/send_empty_signal).
static PullReply recv_pull_reply(int source, int tag, Solution& solution)
{
    MPI_Status status{};
    int size = 0;
    MPI_Recv(&size, 1, MPI_INT, source, tag, MPI_COMM_WORLD, &status);
    if (size == -1) return PullReply::Stop;
    if (size == -2) return PullReply::Empty;

    std::string payload(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        MPI_Recv(payload.data(), size, MPI_CHAR, source, tag, MPI_COMM_WORLD, &status);
    }
    solution = Solution::from_json(nlohmann::json::parse(payload));
    return PullReply::Solution;
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

struct PendingSend {
    int size = 0;
    std::string payload;
    MPI_Request req_size    = MPI_REQUEST_NULL;
    MPI_Request req_payload = MPI_REQUEST_NULL;
};

void reap_completed_sends(std::list<PendingSend>& pending)
{
    for (auto it = pending.begin(); it != pending.end(); ) {
        int done_size = 0, done_payload = 0;
        MPI_Test(&it->req_size, &done_size, MPI_STATUS_IGNORE);
        MPI_Test(&it->req_payload, &done_payload, MPI_STATUS_IGNORE);
        if (done_size && done_payload) it = pending.erase(it);
        else ++it;
    }
}

void send_solution_async(int dest, int tag, const Solution& solution,
                          std::list<PendingSend>& pending)
{
    reap_completed_sends(pending);

    pending.emplace_back();
    PendingSend& p = pending.back();
    p.payload = solution.to_json().dump();
    p.size    = static_cast<int>(p.payload.size());
    MPI_Isend(&p.size, 1, MPI_INT, dest, tag, MPI_COMM_WORLD, &p.req_size);
    if (p.size > 0) {
        MPI_Isend(p.payload.data(), p.size, MPI_CHAR, dest, tag, MPI_COMM_WORLD, &p.req_payload);
    }
}

void wait_all_pending_sends(std::list<PendingSend>& pending)
{
    for (auto& p : pending) {
        MPI_Wait(&p.req_size, MPI_STATUS_IGNORE);
        MPI_Wait(&p.req_payload, MPI_STATUS_IGNORE);
    }
    pending.clear();
}

void send_stats(int dest, std::size_t total_evaluations, std::size_t push_count)
{
    nlohmann::json j;
    j["total_evaluations"] = total_evaluations;
    j["push_count"]        = push_count;
    send_string_impl(dest, TAG_STATS, j.dump());
}

void recv_stats(int source, std::size_t& total_evaluations, std::size_t& push_count)
{
    nlohmann::json j = nlohmann::json::parse(recv_string_impl(source, TAG_STATS));
    total_evaluations = j.at("total_evaluations").get<std::size_t>();
    push_count        = j.at("push_count").get<std::size_t>();
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

    bool consider(Solution candidate, int source_worker, cli::EliteReplaceStrategy replace_strategy)
    {
        if (solutions.size() < keep_count) {
            solutions.push_back({std::move(candidate), source_worker, 0, next_seq++});
            return true;
        }

        auto find_replacement = [&](auto eligible, auto prefer) -> std::optional<std::size_t> {
            std::optional<std::size_t> best;
            double best_diff = std::numeric_limits<double>::infinity();

            for (std::size_t i = 0; i < solutions.size(); ++i) {
                if (!eligible(i)) continue;
                double diff = static_cast<double>(count_diff_elite(candidate, solutions[i].sol));
                if (!best || diff < best_diff || (diff == best_diff && prefer(i, *best))) {
                    best_diff = diff;
                    best = i;
                }
            }
            return best;
        };

        auto is_older = [&](std::size_t i, std::size_t than) {
            return solutions[i].insertion_seq < solutions[than].insertion_seq;
        };
        auto always_eligible = [](std::size_t) { return true; };

        std::optional<std::size_t> chosen;
        switch (replace_strategy) {
            case cli::EliteReplaceStrategy::SimilarityOnly:
                // Tie-break: replace the oldest of the equally-similar entries.
                chosen = find_replacement(always_eligible, is_older);
                break;

            case cli::EliteReplaceStrategy::SimilarityQuality:
                // Tie-break: worst cost first, then oldest.
                chosen = find_replacement(
                    [&](std::size_t i) { return solutions[i].sol.cost() > candidate.cost(); },
                    [&](std::size_t i, std::size_t than) {
                        double ci = solutions[i].sol.cost(), ct = solutions[than].sol.cost();
                        return ci != ct ? ci > ct : is_older(i, than);
                    });
                break;

            case cli::EliteReplaceStrategy::SimilarityPullFirst: {
                chosen = find_replacement(
                    [&](std::size_t i) { return solutions[i].pull_count > 0; }, is_older);
                if (!chosen) {
                    chosen = find_replacement(always_eligible, is_older);
                }
                break;
            }
        }

        if (chosen) {
            solutions[*chosen] = {std::move(candidate), source_worker, 0, next_seq++};
            return true;
        }
        return false;
    }

    bool empty() const
    {
        return solutions.empty();
    }

    double best_cost() const
    {
        double m = std::numeric_limits<double>::infinity();
        for (const auto& e : solutions) m = std::min(m, e.sol.cost());
        return m;
    }

    std::size_t size() const
    {
        return solutions.size();
    }

    std::vector<double> costs() const
    {
        std::vector<double> result;
        result.reserve(solutions.size());
        for (const auto& e : solutions) result.push_back(e.sol.cost());
        return result;
    }

    // Average pairwise Hamming distance across all elites currently in the
    // pool: div = 2/(m*(m-1)) * sum over all C(m,2) pairs.
    // Normalized to [0,1] by customers_count
    double diversity() const
    {
        const std::size_t m = solutions.size();
        if (m < 2) return 0.0;
        const std::size_t customers_count = global_config().customers_count;
        if (customers_count == 0) return 0.0;

        std::size_t sum = 0;
        for (std::size_t i = 0; i < m; ++i) {
            for (std::size_t j = i + 1; j < m; ++j) {
                sum += solutions[i].sol.hamming_distance(solutions[j].sol);
            }
        }
        return 2.0 * static_cast<double>(sum)
             / (static_cast<double>(m) * static_cast<double>(m - 1) * static_cast<double>(customers_count));
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

        switch (strategy) {
            case cli::ElitePullStrategy::Random:    return pick_random();
            case cli::ElitePullStrategy::TopK: {
                // derive k from pool size (keep_count)
                std::size_t k = std::max<std::size_t>(1, keep_count / 2);
                return pick_topk(k);
            }
            case cli::ElitePullStrategy::Rank:      return pick_rank_based();
            case cli::ElitePullStrategy::PullCount: return pick_pullcount_based();
            case cli::ElitePullStrategy::Off:       break;
        }

        return pick_random();
    }

private:
    struct Entry {
        Solution sol;
        int source_worker;
        std::size_t pull_count = 0;
        std::size_t insertion_seq = 0;
    };

    std::size_t keep_count;
    std::vector<Entry> solutions;
    std::size_t next_seq = 0;
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
    const std::size_t derived = static_cast<std::size_t>(std::round(scaled));
    return std::clamp(derived, kMinClamp, kMaxClamp);
}

} // namespace

Solution run_master(int world_size)
{
    const auto t0 = std::chrono::steady_clock::now();
    const Config base_cfg = global_config();
    const cli::EliteReplaceStrategy elite_replace_strategy = base_cfg.elite_replace_strategy;
    std::optional<Solution> best_solution;
    const std::size_t elite_keep_count = compute_elite_pool_size(base_cfg, world_size);
    const std::size_t min_pull_elites_per_worker = compute_min_pull_elites_per_worker(base_cfg, world_size);
    ElitePool elite_pool(elite_keep_count);
    const auto t1 = std::chrono::steady_clock::now();

    std::size_t next_seed = base_cfg.seed ? *base_cfg.seed : 1;

    std::vector<bool> worker_running(static_cast<std::size_t>(world_size), false);
    std::vector<std::size_t> worker_pull_requests(static_cast<std::size_t>(world_size), 0);
    bool stop_after_min_pulls = false;
    std::size_t active_workers = 0;
    std::size_t total_evaluations = 0;
    std::size_t total_push_count = 0;
    std::size_t accepted_push_count = 0;
    std::mt19937 rng(base_cfg.seed ? static_cast<unsigned>(*base_cfg.seed) : 1u);

    const std::size_t evaluation_budget =
        base_cfg.max_evaluations * static_cast<std::size_t>(world_size - 1);
    std::vector<std::size_t> latest_worker_evals(static_cast<std::size_t>(world_size), 0);
    std::size_t next_checkpoint = 0;
    std::vector<double> best_cost_by_checkpoint(9, 0.0);
    std::vector<bool> checkpoint_captured(9, false);
    std::size_t best_solution_evaluations = 0;

    auto current_running_total = [&]() {
        std::size_t total = 0;
        for (int r = 1; r < world_size; ++r) total += latest_worker_evals[static_cast<std::size_t>(r)];
        return total;
    };

    auto record_new_best = [&]() {
        best_solution_evaluations = current_running_total();
    };

    // Guards against an empty pool (best_cost() == +infinity, which would
    // serialize as JSON null) -- skips and retries on a later call instead.
    auto capture_checkpoint = [&](std::size_t idx) {
        if (checkpoint_captured[idx] || elite_pool.empty()) return;
        best_cost_by_checkpoint[idx] = elite_pool.best_cost();
        checkpoint_captured[idx] = true;
    };

    auto capture_due_checkpoints = [&]() {
        if (evaluation_budget == 0) return;
        std::size_t running_total = current_running_total();
        if (next_checkpoint == 0) {
            if (running_total == 0) return;
            capture_checkpoint(0);
            if (!checkpoint_captured[0]) return;  // pool still empty, retry later
            next_checkpoint = 1;
        }
        // Checkpoint 8 excluded here -- left to the post-loop catch-up so
        // it always reflects the true final best_solution.
        while (next_checkpoint <= 7 &&
               running_total >= evaluation_budget * next_checkpoint / 8) {
            capture_checkpoint(next_checkpoint);
            ++next_checkpoint;
        }
    };

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
            if (!best_solution || elite.cost() < best_solution->cost()) {
                best_solution = elite;
                record_new_best();
            }
            if (elite_pool.consider(std::move(elite), worker_rank, elite_replace_strategy)) {
                ++accepted_push_count;
            }
        }

        // Handle evaluation-progress reports from workers
        for (;;) {
            MPI_Status status{};
            int ready = 0;
            MPI_Iprobe(MPI_ANY_SOURCE, TAG_EVAL_PROGRESS, MPI_COMM_WORLD, &ready, &status);
            if (!ready) break;
            processed = true;
            const int worker_rank = status.MPI_SOURCE;
            nlohmann::json j = nlohmann::json::parse(recv_string_impl(worker_rank, TAG_EVAL_PROGRESS));
            latest_worker_evals[static_cast<std::size_t>(worker_rank)] =
                j.at("evaluations").get<std::size_t>();
            capture_due_checkpoints();
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
                send_empty_signal(worker_rank, TAG_ELITE_REPLY);
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
            bool result_is_new_best = !best_solution || result.cost() < best_solution->cost();
            if (result_is_new_best) {
                best_solution = result;
            }
            elite_pool.consider(std::move(result), worker_rank, elite_replace_strategy);

            std::size_t worker_evaluations = 0;
            std::size_t worker_push_count = 0;
            recv_stats(worker_rank, worker_evaluations, worker_push_count);
            total_evaluations += worker_evaluations;
            total_push_count  += worker_push_count;
            latest_worker_evals[static_cast<std::size_t>(worker_rank)] = worker_evaluations;
            if (result_is_new_best) record_new_best();
            capture_due_checkpoints();

            worker_running[static_cast<std::size_t>(worker_rank)] = false;
            --active_workers;
        }

        if (!processed) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    if (evaluation_budget > 0) {
        for (std::size_t idx = 0; idx <= 8; ++idx) {
            capture_checkpoint(idx);
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

    Logger logger;
    logger.finalize(*best_solution, 0, 0, 0, 0, 0, 0.0, 0.0,
                     total_evaluations, total_push_count, accepted_push_count,
                     evaluation_budget,
                     evaluation_budget > 0 ? best_cost_by_checkpoint : std::vector<double>{},
                     elite_pool.size(), elite_pool.diversity(), elite_pool.costs(),
                     best_solution_evaluations);
    return *best_solution;
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

    std::list<PendingSend> pending_pushes;
    std::size_t push_count = 0;

    Solution::EliteHooks hooks;
    hooks.push_elite = [&](const Solution& elite) {
        send_solution_async(0, TAG_ELITE_PUSH, elite, pending_pushes);
        ++push_count;
    };
    bool stop_requested = false;
    hooks.should_stop = [&]() {
        return stop_requested;
    };
    hooks.report_progress = [&](std::size_t total_evaluations) {
        nlohmann::json j;
        j["evaluations"] = total_evaluations;
        send_string_impl(0, TAG_EVAL_PROGRESS, j.dump());
    };

    if (worker_cfg.elite_pull_strategy != cli::ElitePullStrategy::Off) {
        hooks.pull_elite = [&](std::size_t iteration, Solution& pulled_elite) -> bool {
            nlohmann::json j;
            j["iteration"] = iteration;
            send_string_impl(0, TAG_ELITE_PULL, j.dump());
            switch (recv_pull_reply(0, TAG_ELITE_REPLY, pulled_elite)) {
                case PullReply::Solution: return true;
                case PullReply::Stop:     stop_requested = true; return false;
                case PullReply::Empty:    return false;
            }
            return false;
        };
    }

    Logger logger;
    Solution result = Solution::tabu_search(root, logger, &hooks);
    wait_all_pending_sends(pending_pushes);
    send_solution(0, TAG_RESULT, result);
    send_stats(0, logger.total_evaluations, push_count);
}

} // namespace parallel