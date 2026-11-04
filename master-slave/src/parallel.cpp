#include "parallel.hpp"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
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
#include <optional>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <nlohmann/json.hpp>

#include "comm.hpp"
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

constexpr int MASTER = 0;

// One thread's end of the Comm: its rank plus the shared mailboxes.
struct Endpoint {
    Comm& comm;
    int   rank;
};

void send_string_impl(Endpoint& ep, int dest, int tag, std::string payload)
{
    ep.comm.send(ep.rank, dest, tag, std::move(payload));
}

std::string recv_string_impl(Endpoint& ep, int source, int tag)
{
    return ep.comm.recv(ep.rank, source, tag).payload;
}

void send_job_start(Endpoint& ep, int dest, std::size_t seed, std::size_t coop_seed)
{
    nlohmann::json j;
    j["seed"]         = seed;
    j["coop_seed"]    = coop_seed;
    send_string_impl(ep, dest, TAG_JOB, j.dump());
}

// No elite available to send, but the worker should keep running.
void send_empty_signal(Endpoint& ep, int dest, int tag)
{
    ep.comm.send(ep.rank, dest, tag, std::string{}, /*empty=*/true);
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

enum class PullReply { Solution, Empty };

static PullReply recv_pull_reply(Endpoint& ep, int source, int tag, Solution& solution)
{
    Comm::Message m = ep.comm.recv(ep.rank, source, tag);
    if (m.empty) return PullReply::Empty;
    solution = Solution::from_json(nlohmann::json::parse(m.payload));
    return PullReply::Solution;
}

struct JobStart { std::size_t seed; std::size_t coop_seed; };
JobStart recv_job_start(Endpoint& ep, int source)
{
    nlohmann::json j = nlohmann::json::parse(recv_string_impl(ep, source, TAG_JOB));
    return {j.value("seed", std::size_t{0}), j.value("coop_seed", std::size_t{0})};
}

void send_solution(Endpoint& ep, int dest, int tag, const Solution& solution)
{
    send_string_impl(ep, dest, tag, solution.to_json().dump());
}

void send_stats(Endpoint& ep, int dest, std::size_t total_evaluations, std::size_t push_count,
                 std::size_t pull_round_improved_count, std::size_t iterations)
{
    nlohmann::json j;
    j["total_evaluations"]        = total_evaluations;
    j["push_count"]               = push_count;
    j["pull_round_improved_count"] = pull_round_improved_count;
    j["iterations"]              = iterations;
    send_string_impl(ep, dest, TAG_STATS, j.dump());
}

void recv_stats(Endpoint& ep, int source, std::size_t& total_evaluations, std::size_t& push_count,
                 std::size_t& pull_round_improved_count, std::size_t& iterations)
{
    nlohmann::json j = nlohmann::json::parse(recv_string_impl(ep, source, TAG_STATS));
    pull_round_improved_count = j.at("pull_round_improved_count").get<std::size_t>();
    total_evaluations = j.at("total_evaluations").get<std::size_t>();
    push_count        = j.at("push_count").get<std::size_t>();
    iterations        = j.at("iterations").get<std::size_t>();
}

// Cost comparison tolerance for floating-point solution costs.
static bool is_significantly_better(double fx, double fe)
{
    return fx < fe - 1e-9 * std::max(1.0, fe);
}

// Structural distance of the elite pool: edge_distance under edge-crowding,
// td_distance otherwise.
static double structural_distance(const Solution& a, const Solution& b,
                                  cli::EliteReplaceStrategy replace_strategy)
{
    return replace_strategy == cli::EliteReplaceStrategy::EdgeCrowding
        ? static_cast<double>(a.edge_distance(b)) : a.td_distance(b);
}

// offered: true iff at least one elite from a worker other than the
// requester exists in the pool. solution: non-null iff that elite was
// actually sent (offered && accepted).
struct PickResult {
    bool offered = false;
    const Solution* solution = nullptr;
};

struct ElitePool {
    explicit ElitePool(std::size_t keep_count, std::uint64_t seed = std::random_device{}())
        : keep_count(keep_count), rng(seed) {}

    bool consider(Solution candidate, int source_worker, cli::EliteReplaceStrategy replace_strategy)
    {

        if (solutions.size() < keep_count) {
            // Reject exact duplicates even while the pool still has room.
            for (const auto& e : solutions) {
                if (candidate.is_structural_duplicate(e.sol)) return false;
            }
            solutions.push_back({std::move(candidate), source_worker, 0, next_seq++});
            return true;
        }

        auto find_replacement = [&](auto prefer) -> std::optional<std::size_t> {
            std::optional<std::size_t> best;
            double best_diff = std::numeric_limits<double>::infinity();

            for (std::size_t i = 0; i < solutions.size(); ++i) {
                double diff = structural_distance(candidate, solutions[i].sol, replace_strategy);
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

        std::optional<std::size_t> chosen;
        switch (replace_strategy) {
            case cli::EliteReplaceStrategy::TDCrowding:
            case cli::EliteReplaceStrategy::EdgeCrowding: {
                // Replace the pool's closest match to `candidate` (tie-break:
                // worst cost, then oldest), but only if significantly better.
                std::optional<std::size_t> target = find_replacement(
                    [&](std::size_t i, std::size_t than) {
                        double ci = solutions[i].sol.cost(), ct = solutions[than].sol.cost();
                        return ci != ct ? ci > ct : is_older(i, than);
                    });
                if (target && is_significantly_better(candidate.cost(), solutions[*target].sol.cost())) {
                    chosen = target;
                }
                break;
            }

            case cli::EliteReplaceStrategy::QualityOnly: {
                // Replace the worst-cost entry (tie-break: oldest), if the candidate beats it.
                std::size_t worst = 0;
                for (std::size_t i = 1; i < solutions.size(); ++i) {
                    double ci = solutions[i].sol.cost(), cw = solutions[worst].sol.cost();
                    if (ci > cw || (ci == cw && is_older(i, worst))) worst = i;
                }
                if (is_significantly_better(candidate.cost(), solutions[worst].sol.cost())) {
                    chosen = worst;
                }
                break;
            }

            case cli::EliteReplaceStrategy::RandomTarget: {
                std::uniform_int_distribution<std::size_t> dist(0, solutions.size() - 1);
                std::size_t idx = dist(rng);
                if (is_significantly_better(candidate.cost(), solutions[idx].sol.cost())) {
                    chosen = idx;
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

    // Average pairwise structural (td) distance across all elites currently
    // in the pool: div = 2/(m*(m-1)) * sum over all C(m,2) pairs. td_distance
    // is already normalized to [0,1], so no extra scaling is needed here.
    double diversity() const
    {
        const std::size_t m = solutions.size();
        if (m < 2) return 0.0;

        double sum = 0.0;
        for (std::size_t i = 0; i < m; ++i) {
            for (std::size_t j = i + 1; j < m; ++j) {
                sum += solutions[i].sol.td_distance(solutions[j].sol);
            }
        }
        return 2.0 * sum
             / (static_cast<double>(m) * static_cast<double>(m - 1));
    }

    // offered = false if the pool has no elite from another worker. Otherwise
    // solution is non-null unless Selective accept rejects it (quality vs.
    // requester's personal best, diversity vs. requester's current solution).
    PickResult pick_for_dispatch(int excluded_worker, std::mt19937& rng, cli::ElitePullStrategy strategy,
                                  cli::ElitePullAcceptStrategy accept_strategy,
                                  cli::EliteReplaceStrategy replace_strategy,
                                  const Solution* requester_personal_best,
                                  const Solution* requester_current)
    {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < solutions.size(); ++i) {
            if (solutions[i].source_worker != excluded_worker) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            return {false, nullptr};
        }

        auto mark_and_return = [&](std::size_t idx) -> const Solution* {
            ++solutions[idx].pull_count;
            return &solutions[idx].sol;
        };

        auto pick_random = [&]() -> const Solution* {
            std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
            return mark_and_return(candidates[dist(rng)]);
        };

        auto pick_topk = [&](std::size_t k) -> const Solution* {
            std::vector<std::size_t> sorted = candidates;
            std::sort(sorted.begin(), sorted.end(), [&](std::size_t lhs, std::size_t rhs) {
                return solutions[lhs].sol.cost() < solutions[rhs].sol.cost();
            });
            k = std::min(k, sorted.size());
            std::uniform_int_distribution<std::size_t> dist(0, k - 1);
            return mark_and_return(sorted[dist(rng)]);
        };

        auto pick_rank_based = [&]() -> const Solution* {
            std::vector<std::size_t> sorted = candidates;
            std::sort(sorted.begin(), sorted.end(), [&](std::size_t lhs, std::size_t rhs) {
                return solutions[lhs].sol.cost() < solutions[rhs].sol.cost();
            });
            // Rank 1 = lowest cost (best). weight(rank) = (m-rank+1) / (1+...+m).
            const std::size_t m = sorted.size();
            const double denom = static_cast<double>(m) * static_cast<double>(m + 1) / 2.0;
            std::vector<double> weights(m);
            for (std::size_t i = 0; i < m; ++i) {
                const std::size_t rank = i + 1;
                weights[i] = static_cast<double>(m - rank + 1) / denom;
            }
            std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
            return mark_and_return(sorted[dist(rng)]);
        };

        auto pick_pullcount_based = [&]() -> const Solution* {
            std::vector<double> weights;
            weights.reserve(candidates.size());
            for (std::size_t idx : candidates) {
                weights.push_back(1.0 / (1.0 + static_cast<double>(solutions[idx].pull_count)));
            }
            const double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
            for (double& w : weights) w /= sum;
            std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
            return mark_and_return(candidates[dist(rng)]);
        };

        const Solution* picked = nullptr;
        switch (strategy) {
            case cli::ElitePullStrategy::Random:    picked = pick_random(); break;
            case cli::ElitePullStrategy::TopK: {
                // derive k from the filtered candidate count, rounded up
                std::size_t k = (candidates.size() + 1) / 2;
                picked = pick_topk(k);
                break;
            }
            case cli::ElitePullStrategy::Rank:      picked = pick_rank_based(); break;
            case cli::ElitePullStrategy::PullCount: picked = pick_pullcount_based(); break;
            case cli::ElitePullStrategy::Off:       picked = pick_random(); break;
        }

        if (accept_strategy != cli::ElitePullAcceptStrategy::Selective) {
            return {true, picked};
        }

        // Selective: pull_count above is incremented regardless of the
        // accept/reject outcome below.
        const double cost_e = picked->cost();
        const double cost_personal_best = requester_personal_best->cost();
        const double quality_tolerance = 1.0 + global_config().elite_pull_quality_tolerance_pct / 100.0;
        const bool tolerance_satisfied = cost_e <= quality_tolerance * cost_personal_best;
        if (cost_e < cost_personal_best) {
            return {true, picked};
        }
        if (!tolerance_satisfied) {
            return {true, nullptr};
        }
        std::vector<double> d_values;
        d_values.reserve(candidates.size());
        for (std::size_t idx : candidates) {
            const double d = structural_distance(*requester_current, solutions[idx].sol, replace_strategy);
            if (d > 0.0) d_values.push_back(d);
        }
        if (!d_values.empty()) {
            const double mean_d = std::accumulate(d_values.begin(), d_values.end(), 0.0)
                                 / static_cast<double>(d_values.size());
            const double d_e = structural_distance(*requester_current, *picked, replace_strategy);
            if (d_e > mean_d) {
                return {true, picked};
            }
        }
        return {true, nullptr};
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
    std::mt19937_64 rng;
};

std::size_t compute_elite_pool_size(const Config& cfg, int world_size)
{
    // Non-zero override bypasses the elite_pool_factor formula entirely.
    if (cfg.elite_pool_size != 0) return cfg.elite_pool_size;

    constexpr std::size_t kMinClamp = 2;
    constexpr std::size_t kMaxClamp = 20;

    const double scaled = cfg.elite_pool_factor * std::sqrt(static_cast<double>(cfg.customers_count)) * static_cast<double>(world_size - 1);
    const std::size_t derived = static_cast<std::size_t>(std::ceil(scaled));
    return std::clamp(derived, kMinClamp, kMaxClamp);
}

// Master (rank 0, the calling thread): hands out the jobs, keeps the elite
// pool, answers pull requests and collects results and statistics.
static Solution run_master(Endpoint& ep)
{
    const int world_size = ep.comm.size();
    const auto t0 = std::chrono::steady_clock::now();
    const Config base_cfg = global_config();
    const cli::EliteReplaceStrategy elite_replace_strategy = base_cfg.elite_replace_strategy;
    std::optional<Solution> best_solution;
    const std::size_t elite_keep_count = compute_elite_pool_size(base_cfg, world_size);
    const std::size_t base_seed = base_cfg.seed ? *base_cfg.seed : 1;
    ElitePool elite_pool(elite_keep_count, base_seed);
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<std::size_t> worker_search_seeds(static_cast<std::size_t>(world_size - 1), 0);
    std::vector<std::size_t> worker_coop_seeds(static_cast<std::size_t>(world_size - 1), 0);
    // Lazily seeded from each worker's coop_seed on its first pull request,
    // then left to evolve across subsequent pulls (never re-seeded).
    std::unordered_map<int, std::mt19937> worker_pull_rngs;

    std::vector<bool> worker_running(static_cast<std::size_t>(world_size), false);
    std::size_t active_workers = 0;
    std::size_t total_evaluations = 0;
    std::size_t total_worker_iterations = 0;
    std::size_t total_push_count = 0;
    std::size_t accepted_push_count = 0;
    std::size_t pull_request_count = 0;
    std::size_t pull_offer_count = 0;
    std::size_t pull_accept_count = 0;
    // Per-worker request counts only -- offer/accept are tracked globally
    // above (that's the granularity exp2/c and exp2/d actually need).
    std::vector<std::size_t> worker_pull_request_counts_by_rank(static_cast<std::size_t>(world_size), 0);
    std::size_t total_pull_round_improved_count = 0;
    std::vector<std::size_t> worker_pull_round_improved_counts_by_rank(static_cast<std::size_t>(world_size), 0);

    const std::size_t evaluation_budget =
        base_cfg.max_evaluations * static_cast<std::size_t>(world_size - 1);
    std::vector<std::size_t> latest_worker_evals(static_cast<std::size_t>(world_size), 0);
    std::size_t next_checkpoint = 0;
    // Pool's own best cost (exp2/a) vs. true best solution found (exp2/d/e/f) -- can diverge under quality-only/random-target.
    std::vector<double> best_cost_by_checkpoint(9, 0.0);
    std::vector<double> best_solution_cost_by_checkpoint(9, 0.0);
    std::vector<double> diversity_by_checkpoint(9, 0.0);
    std::vector<bool> checkpoint_captured(9, false);

    // Time-based analog of the checkpoints above: an 8-way split of time_limit,
    // clocked from the last worker's first elite push (its constructed root)
    // rather than t1, so worker start-up/construction time isn't counted.
    const double time_checkpoint_limit = base_cfg.time_limit;
    std::size_t next_time_checkpoint = 0;
    std::vector<double> best_solution_cost_by_time_checkpoint(9, 0.0);
    std::vector<bool> time_checkpoint_captured(9, false);
    std::optional<double> best_initial_cost;
    std::optional<std::chrono::steady_clock::time_point> time_checkpoint_origin;
    std::vector<bool> worker_first_push_seen(static_cast<std::size_t>(world_size), false);
    std::size_t initial_push_reports = 0;

    auto current_running_total = [&]() {
        std::size_t total = 0;
        for (int r = 1; r < world_size; ++r) total += latest_worker_evals[static_cast<std::size_t>(r)];
        return total;
    };

    // Guards against an empty pool / no best_solution yet (would serialize
    // as JSON null) -- skips and retries on a later call instead.
    auto capture_checkpoint = [&](std::size_t idx) {
        if (checkpoint_captured[idx] || elite_pool.empty() || !best_solution) return;
        best_cost_by_checkpoint[idx] = elite_pool.best_cost();
        best_solution_cost_by_checkpoint[idx] = best_solution->cost();
        diversity_by_checkpoint[idx] = elite_pool.diversity();
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

    auto capture_time_checkpoint = [&](std::size_t idx) {
        if (time_checkpoint_captured[idx]) return;
        if (idx == 0) {
            // cp0 = best initial solution; only valid once every worker has
            // pushed its constructed root.
            if (initial_push_reports < static_cast<std::size_t>(world_size - 1)) return;
            best_solution_cost_by_time_checkpoint[0] = *best_initial_cost;
        } else {
            if (!best_solution) return;
            best_solution_cost_by_time_checkpoint[idx] = best_solution->cost();
        }
        time_checkpoint_captured[idx] = true;
    };

    auto capture_due_time_checkpoints = [&]() {
        if (time_checkpoint_limit <= 0.0 || !time_checkpoint_origin) return;
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - *time_checkpoint_origin).count();
        if (next_time_checkpoint == 0) {
            capture_time_checkpoint(0);
            if (!time_checkpoint_captured[0]) return;
            next_time_checkpoint = 1;
        }
        // Checkpoint 8 excluded here -- left to the post-loop catch-up so it
        // always reflects the true final best_solution.
        while (next_time_checkpoint <= 7 &&
               elapsed >= time_checkpoint_limit * static_cast<double>(next_time_checkpoint) / 8.0) {
            capture_time_checkpoint(next_time_checkpoint);
            ++next_time_checkpoint;
        }
    };

    auto start_worker = [&](int worker_rank) {
        const std::size_t search_seed = base_seed + static_cast<std::size_t>(worker_rank);
        const std::size_t coop_seed   = 10000 + base_seed + static_cast<std::size_t>(worker_rank);
        worker_search_seeds[static_cast<std::size_t>(worker_rank - 1)] = search_seed;
        worker_coop_seeds[static_cast<std::size_t>(worker_rank - 1)]   = coop_seed;
        send_job_start(ep, worker_rank, search_seed, coop_seed);
        worker_running[static_cast<std::size_t>(worker_rank)] = true;
        ++active_workers;
    };

    for (int worker_rank = 1; worker_rank < world_size; ++worker_rank) {
        start_worker(worker_rank);
    }

    while (active_workers > 0) {
        bool processed = false;
        capture_due_time_checkpoints();

        // Handle elite pushes from workers
        for (;;) {
            std::optional<Comm::Message> msg = ep.comm.try_recv_any(ep.rank, TAG_ELITE_PUSH);
            if (!msg) break;
            processed = true;
            const int worker_rank = msg->source;
            Solution elite = Solution::from_json(nlohmann::json::parse(msg->payload));
            // Time-limit runs only: a worker's first push is its constructed
            // root -> feeds cp0, and the last one to arrive starts the
            // time-checkpoint clock. (Eval-limit runs checkpoint differently.)
            if (time_checkpoint_limit > 0.0
                && !worker_first_push_seen[static_cast<std::size_t>(worker_rank)]) {
                worker_first_push_seen[static_cast<std::size_t>(worker_rank)] = true;
                ++initial_push_reports;
                const double root_cost = elite.cost();
                if (!best_initial_cost || root_cost < *best_initial_cost) best_initial_cost = root_cost;
                if (initial_push_reports == static_cast<std::size_t>(world_size - 1)) {
                    time_checkpoint_origin = std::chrono::steady_clock::now();
                }
            }
            if (!best_solution || elite.cost() < best_solution->cost()) {
                best_solution = elite;
            }
            if (elite_pool.consider(std::move(elite), worker_rank, elite_replace_strategy)) {
                ++accepted_push_count;
            }
        }

        // Handle evaluation-progress reports from workers
        for (;;) {
            std::optional<Comm::Message> msg = ep.comm.try_recv_any(ep.rank, TAG_EVAL_PROGRESS);
            if (!msg) break;
            processed = true;
            const int worker_rank = msg->source;
            nlohmann::json j = nlohmann::json::parse(msg->payload);
            latest_worker_evals[static_cast<std::size_t>(worker_rank)] =
                j.at("evaluations").get<std::size_t>();
            capture_due_checkpoints();
        }

        // Handle elite pull from workers
        for (;;) {
            std::optional<Comm::Message> msg = ep.comm.try_recv_any(ep.rank, TAG_ELITE_PULL);
            if (!msg) break;
            processed = true;
            const int worker_rank = msg->source;
            nlohmann::json pull_req = nlohmann::json::parse(msg->payload);
            const std::size_t worker_coop_seed = pull_req.value("seed", std::size_t{0});
            std::optional<Solution> requester_personal_best;
            if (pull_req.contains("personal_best")) {
                requester_personal_best = Solution::from_json(pull_req.at("personal_best"));
            }
            std::optional<Solution> requester_current;
            if (pull_req.contains("current")) {
                requester_current = Solution::from_json(pull_req.at("current"));
            }

            ++pull_request_count;
            ++worker_pull_request_counts_by_rank[static_cast<std::size_t>(worker_rank)];

            const Solution* elite_to_send = nullptr;
            if (!elite_pool.empty()) {
                auto it = worker_pull_rngs.try_emplace(
                    worker_rank, static_cast<unsigned>(worker_coop_seed)).first;
                PickResult pick = elite_pool.pick_for_dispatch(
                    worker_rank, it->second, base_cfg.elite_pull_strategy,
                    base_cfg.elite_pull_accept_strategy,
                    elite_replace_strategy,
                    requester_personal_best ? &*requester_personal_best : nullptr,
                    requester_current ? &*requester_current : nullptr);
                if (pick.offered) ++pull_offer_count;
                if (pick.solution) ++pull_accept_count;
                elite_to_send = pick.solution;
            }

            if (elite_to_send) {
                send_solution(ep, worker_rank, TAG_ELITE_REPLY, *elite_to_send);
            } else {
                send_empty_signal(ep, worker_rank, TAG_ELITE_REPLY);
            }
        }

        // Handle final results from workers
        for (;;) {
            std::optional<Comm::Message> msg = ep.comm.try_recv_any(ep.rank, TAG_RESULT);
            if (!msg) break;
            processed = true;
            const int worker_rank = msg->source;
            Solution result = Solution::from_json(nlohmann::json::parse(msg->payload));
            bool result_is_new_best = !best_solution || result.cost() < best_solution->cost();
            if (result_is_new_best) {
                best_solution = result;
            }
            elite_pool.consider(std::move(result), worker_rank, elite_replace_strategy);

            std::size_t worker_evaluations = 0;
            std::size_t worker_push_count = 0;
            std::size_t worker_pull_round_improved_count = 0;
            std::size_t worker_iterations = 0;
            recv_stats(ep, worker_rank, worker_evaluations, worker_push_count,
                       worker_pull_round_improved_count, worker_iterations);
            total_evaluations += worker_evaluations;
            total_push_count  += worker_push_count;
            total_pull_round_improved_count += worker_pull_round_improved_count;
            total_worker_iterations += worker_iterations;
            worker_pull_round_improved_counts_by_rank[static_cast<std::size_t>(worker_rank)] =
                worker_pull_round_improved_count;
            latest_worker_evals[static_cast<std::size_t>(worker_rank)] = worker_evaluations;
            capture_due_checkpoints();
            capture_due_time_checkpoints();

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

    if (time_checkpoint_limit > 0.0) {
        for (std::size_t idx = 0; idx <= 8; ++idx) {
            capture_time_checkpoint(idx);
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

    std::cerr << "Pull accept count: " << pull_accept_count << "/" << pull_offer_count
              << " (requests: " << pull_request_count << ")\n";

    std::vector<std::size_t> worker_pull_request_counts(static_cast<std::size_t>(world_size - 1), 0);
    std::vector<std::size_t> worker_pull_round_improved_counts(static_cast<std::size_t>(world_size - 1), 0);
    for (int worker_rank = 1; worker_rank < world_size; ++worker_rank) {
        worker_pull_request_counts[static_cast<std::size_t>(worker_rank - 1)] =
            worker_pull_request_counts_by_rank[static_cast<std::size_t>(worker_rank)];
        worker_pull_round_improved_counts[static_cast<std::size_t>(worker_rank - 1)] =
            worker_pull_round_improved_counts_by_rank[static_cast<std::size_t>(worker_rank)];
    }

    Logger logger;
    logger.finalize(*best_solution, 0, 0, 0, 0, 0, 0.0, 0.0,
                     total_evaluations, total_push_count, accepted_push_count,
                     evaluation_budget,
                     evaluation_budget > 0 ? best_cost_by_checkpoint : std::vector<double>{},
                     elite_pool.size(), elite_pool.diversity(), elite_pool.costs(),
                     worker_search_seeds, worker_coop_seeds,
                     pull_offer_count, pull_accept_count,
                     pull_request_count, worker_pull_request_counts,
                     evaluation_budget > 0 ? best_solution_cost_by_checkpoint : std::vector<double>{},
                     evaluation_budget > 0 ? diversity_by_checkpoint : std::vector<double>{},
                     total_pull_round_improved_count, worker_pull_round_improved_counts,
                     time_checkpoint_limit > 0.0 ? best_solution_cost_by_time_checkpoint
                                                 : std::vector<double>{},
                     time_checkpoint_limit,
                     total_worker_iterations);
    return *best_solution;
}

// Worker (rank >= 1, its own thread): one tabu search with its own config
// (seed, randomized adaptive hyperparameters), talking to the master only
// through its mailbox.
static void run_worker(Endpoint& ep, const Config& base_cfg)
{
    auto [seed, coop_seed] = recv_job_start(ep, MASTER);

    Config worker_cfg = base_cfg;
    worker_cfg.seed = seed;
    worker_cfg.disable_logging = true;
    randomize_worker_adaptive_hyperparams(worker_cfg);
    // The global config is per thread: this worker's own copy from now on.
    set_global_config(std::move(worker_cfg));
    const Config& cfg = global_config();
    Solution root = Solution::initialize();

    std::size_t push_count = 0;
    std::size_t pull_round_improved_count = 0;

    Solution::EliteHooks hooks;
    hooks.push_elite = [&](const Solution& elite) {
        send_solution(ep, MASTER, TAG_ELITE_PUSH, elite);
        ++push_count;
    };
    hooks.report_progress = [&](std::size_t total_evaluations) {
        nlohmann::json j;
        j["evaluations"] = total_evaluations;
        send_string_impl(ep, MASTER, TAG_EVAL_PROGRESS, j.dump());
    };
    hooks.pull_round_improved = [&]() { ++pull_round_improved_count; };

    if (cfg.elite_pull_strategy != cli::ElitePullStrategy::Off) {
        hooks.pull_elite = [&](std::size_t iteration, const Solution& personal_best, const Solution& current, Solution& pulled_elite) -> bool {
            nlohmann::json j;
            j["iteration"] = iteration;
            j["seed"]      = coop_seed;
            if (cfg.elite_pull_accept_strategy == cli::ElitePullAcceptStrategy::Selective) {
                j["personal_best"] = personal_best.to_json();
                j["current"]       = current.to_json();
            }
            send_string_impl(ep, MASTER, TAG_ELITE_PULL, j.dump());
            switch (recv_pull_reply(ep, MASTER, TAG_ELITE_REPLY, pulled_elite)) {
                case PullReply::Solution: return true;
                case PullReply::Empty:    return false;
            }
            return false;
        };
    }

    Logger logger;
    Solution result = Solution::tabu_search(root, logger, &hooks);
    send_solution(ep, MASTER, TAG_RESULT, result);
    send_stats(ep, MASTER, logger.total_evaluations, push_count, pull_round_improved_count,
               logger._iteration);
}

static void pin_current_thread(int cpu)
{
    bool ok = false;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
    if (!ok) std::cerr << "Warning: cannot pin a thread to CPU " << cpu << "\n";
}

// A failing worker cannot report to the master, which would then wait for
// its result forever; stop the whole program instead, as an MPI rank
// failure aborted the job.
[[noreturn]] static void abort_run(const std::string& who, const char* what)
{
    std::cerr << "Error in " << who << ": " << what << "\n";
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

} // namespace

Solution run(std::size_t workers, const std::vector<int>& cpus)
{
    Comm comm(static_cast<int>(workers) + 1);
    // Copied before any thread starts: workers build their own config from it.
    const Config base_cfg = global_config();

    // Every Logger creates the outputs directory; do it once here so the
    // workers do not race on it.
    if (!base_cfg.outputs.empty()) std::filesystem::create_directories(base_cfg.outputs);

    // Worker r (1..workers) runs on cpus[(r - 1) % size]; the master on
    // cpus[workers] when the list has a CPU left for it, else unpinned.
    if (cpus.size() > workers) pin_current_thread(cpus[workers]);

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t r = 1; r <= workers; ++r) {
        threads.emplace_back([&, r] {
            if (!cpus.empty()) pin_current_thread(cpus[(r - 1) % cpus.size()]);
            Endpoint ep{comm, static_cast<int>(r)};
            try { run_worker(ep, base_cfg); }
            catch (const std::exception& e) { abort_run("worker " + std::to_string(r), e.what()); }
            catch (...) { abort_run("worker " + std::to_string(r), "unknown exception"); }
        });
    }

    Endpoint ep{comm, MASTER};
    Solution best;
    try { best = run_master(ep); }
    catch (const std::exception& e) { abort_run("master", e.what()); }
    catch (...) { abort_run("master", "unknown exception"); }

    for (auto& t : threads) t.join();
    return best;
}

} // namespace parallel