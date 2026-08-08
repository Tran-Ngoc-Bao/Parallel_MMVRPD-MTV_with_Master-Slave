#pragma once
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include "routes.hpp"

// -----------------------------------------------------------------------
// Penalty coefficients – global atomics
// -----------------------------------------------------------------------
namespace penalty {
    extern std::atomic<double> coeff[4];
    inline double get(int i) { return coeff[i].load(std::memory_order_relaxed); }
    inline void   set(int i, double v) { coeff[i].store(v, std::memory_order_relaxed); }
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
        std::function<void(std::size_t iteration, const Solution& elite)> push_elite;
        std::function<bool(std::size_t iteration, Solution& pulled_elite)> pull_elite;
        std::function<bool()> should_stop;
    };

    // Factory – computes all derived fields (full recompute, always correct;
    // this is the reference implementation used everywhere update_delta_inplace
    // is not explicitly used).
    static Solution make(
        std::vector<std::vector<LocalRc<TruckRoute>>> truck_routes,
        std::vector<std::vector<LocalRc<DroneRoute>>> drone_routes);

    // Recomputes `scratch` in place for the given new route sets. `base`
    // must be the solution `scratch` was last synced from (i.e. the
    // unmodified original the candidate routes were derived from) so that,
    // per vehicle, an unchanged route list (same LocalRc pointers, checked
    // via operator==) can reuse base's cached working_time/violations
    // instead of re-deriving them. Vehicles whose route list did change are
    // always fully recomputed from their routes, exactly like make() would.
    // Produces the same working_time/feasible/etc. as calling
    // make(new_truck_routes, new_drone_routes) directly (mathematically
    // equivalent summation, so results may differ from make() by
    // floating-point noise on the order of 1e-13 relative error -- never
    // enough to change a feasible/violation-free vehicle's zero, since
    // those are exact 0.0 sums).
    static void update_delta_inplace(
        Solution& scratch,
        const Solution& base,
        std::vector<std::vector<LocalRc<TruckRoute>>> new_truck_routes,
        std::vector<std::vector<LocalRc<DroneRoute>>> new_drone_routes);

    double cost() const;

    size_t hamming_distance(const Solution& other) const;

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
