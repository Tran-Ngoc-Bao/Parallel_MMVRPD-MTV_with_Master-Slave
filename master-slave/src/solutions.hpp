#pragma once
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include "routes.hpp"

// -----------------------------------------------------------------------
// Penalty coefficients – one set per thread: each master-slave worker
// adapts its own, as each MPI rank did in its own process.
// -----------------------------------------------------------------------
namespace penalty {
    extern thread_local double coeff[4];
    inline double get(int i) { return coeff[i]; }
    inline void   set(int i, double v) { coeff[i] = v; }
    void update(int i, double violation);
}

// Forward declare Logger (defined in logger.hpp)
struct Logger;

// -----------------------------------------------------------------------
// Solution
// -----------------------------------------------------------------------
struct Solution {
    std::vector<std::vector<LocalRc<TruckRoute>>> truck_routes;
    std::vector<std::vector<LocalRc<DroneRoute>>> drone_routes;

    std::vector<double> truck_working_time;
    std::vector<double> drone_working_time;

    // Per-vehicle cached breakdown (always kept in sync by make() and
    // update_delta_inplace()). Lets update_delta_inplace() skip re-deriving
    // working_time/violations for vehicles whose route list didn't change.
    std::vector<double> truck_capacity_violation;
    std::vector<double> truck_waiting_violation;
    std::vector<double> drone_capacity_violation;
    std::vector<double> drone_waiting_violation;
    std::vector<double> drone_energy_violation;
    std::vector<double> drone_fixed_violation;

    double working_time           = 0;
    double energy_violation       = 0;
    double capacity_violation     = 0;
    double waiting_time_violation = 0;
    double fixed_time_violation   = 0;
    bool   feasible               = false;

    struct EliteHooks {
        std::function<void(const Solution& elite)> push_elite;
        std::function<bool(std::size_t iteration, const Solution& personal_best, const Solution& current, Solution& pulled_elite)> pull_elite;
        std::function<void(std::size_t total_evaluations)> report_progress;
        // Called once per successful pull whose round (up to the next pull, or
        // search end) improved on the personal best held at pull time.
        std::function<void()> pull_round_improved;
    };

    static Solution make(
        std::vector<std::vector<LocalRc<TruckRoute>>> truck_routes,
        std::vector<std::vector<LocalRc<DroneRoute>>> drone_routes);

    static void update_delta_inplace(
        Solution& scratch,
        const Solution& base,
        std::vector<std::vector<LocalRc<TruckRoute>>> new_truck_routes,
        std::vector<std::vector<LocalRc<DroneRoute>>> new_drone_routes);

    // Same as above when only the listed vehicles changed (up to two per type,
    // NO_VEHICLE when unused); the others reuse base's values without comparing routes.
    static constexpr size_t NO_VEHICLE = static_cast<size_t>(-1);
    static void update_delta_inplace(
        Solution& scratch,
        const Solution& base,
        std::vector<std::vector<LocalRc<TruckRoute>>> new_truck_routes,
        std::vector<std::vector<LocalRc<DroneRoute>>> new_drone_routes,
        const size_t (&changed_truck)[2],
        const size_t (&changed_drone)[2]);

    double cost() const;

    // cost() of a solution with these totals (cost() itself uses it).
    static double penalized_cost(double working_time, double energy_violation,
                                 double capacity_violation, double waiting_time_violation,
                                 double fixed_time_violation);

    size_t hamming_distance(const Solution& other) const;

    // Structural distance in [0,1] combining mode-assignment, physical-vehicle
    // grouping, trip-partition, and visit-sequence differences.
    double td_distance(const Solution& other) const;

    // Asymmetric edge-multiset loss: sums, per consecutive-customer edge,
    // how much this solution's occurrence count exceeds `other`'s. Used by
    // the elite pool to find the existing entry closest to a candidate.
    size_t edge_distance(const Solution& other) const;

    // Canonical fingerprint: equal keys iff solutions agree on mode, vehicle
    // grouping, trip composition, and visit order. Ground truth for
    // duplicate checks, since td/edge_distance are float and can be off by a hair.
    std::string canonical_structural_key() const;
    bool is_structural_duplicate(const Solution& other) const;

    void verify() const;

    // Build initial solution from problem data
    static Solution initialize();

    // Destroy-and-repair
    Solution destroy_and_repair(const std::vector<std::vector<double>>& edge_records) const;

    // Main tabu search loop
    static Solution tabu_search(Solution root, Logger& logger, const EliteHooks* hooks = nullptr);

    // JSON serialization
    nlohmann::json to_json() const;
    static Solution from_json(const nlohmann::json& j);
};
