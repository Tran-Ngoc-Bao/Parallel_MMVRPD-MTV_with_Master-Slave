#include "neighborhoods.hpp"
#include "routes.hpp"
#include "solutions.hpp"
#include <limits>
#include <algorithm>
#include <stdexcept>

// -----------------------------------------------------------------------
// Helper: find the vehicle with the largest working time
// -----------------------------------------------------------------------
static std::pair<size_t, bool> find_decisive_vehicle(const Solution& sol) {
    double max_time = std::numeric_limits<double>::lowest();
    size_t vehicle  = 0;
    bool   is_truck = true;
    for (size_t t = 0; t < sol.truck_working_time.size(); ++t) {
        if (sol.truck_working_time[t] > max_time) {
            max_time = sol.truck_working_time[t];
            vehicle  = t;
            is_truck = true;
        }
    }
    for (size_t d = 0; d < sol.drone_working_time.size(); ++d) {
        if (sol.drone_working_time[d] > max_time) {
            max_time = sol.drone_working_time[d];
            vehicle  = d;
            is_truck = false;
        }
    }
    return {vehicle, is_truck};
}

// -----------------------------------------------------------------------
// Internal update helper
// -----------------------------------------------------------------------
struct IterState {
    const Solution& original;
    const TabuSet& tabu_list;
    double& aspiration_cost;
    double& min_cost;
    bool&   require_feasible;
    Solution& best_result;
    std::vector<size_t>& best_tabu;
    std::size_t& evaluations;
    // Reusable scratch solution for update_delta_inplace() (hot path used
    // by inter_route_internal/intra_route). Not touched by
    // inter_route_extract_internal/ejection_chain_internal, which still
    // build fresh Solutions via Solution::make().
    Solution& scratch;
};

template<typename Tabu>
static bool internal_update(IterState& st, const Solution& sol, Tabu&& tabu)
{
    ++st.evaluations;
    if (st.require_feasible && !sol.feasible) return false;
    double cost = sol.cost();
    bool new_global = cost < st.aspiration_cost && sol.feasible;
    bool in_tabu    = st.tabu_list.contains(tabu);
    if (new_global || (!in_tabu && cost < st.min_cost)) {
        st.min_cost     = cost;
        st.best_result  = sol;
        st.best_tabu    = std::forward<Tabu>(tabu);
        if (new_global) {
            st.aspiration_cost  = cost;
            st.require_feasible = true;
        }
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------
// inter_route_internal<RI>
//   Iterates over routes of vehicle_i (type RI) and pairs them with every
//   route of every vehicle (both truck and drone).
// -----------------------------------------------------------------------
template<typename RI>
static void inter_route_internal(
    Neighborhood n,
    IterState&   st,
    size_t       vehicle_i,
    std::vector<std::vector<LocalRc<TruckRoute>>>& truck_cloned,
    std::vector<std::vector<LocalRc<DroneRoute>>>& drone_cloned)
{
    const auto& orig_ri = RouteHelper<RI>::get_routes(
        st.original.truck_routes, st.original.drone_routes);

    bool asymmetric = (n == Neighborhood::Move10 ||
                       n == Neighborhood::Move20 ||
                       n == Neighborhood::Move21);

    // Helper lambda: iterate route_j over type RJ
    auto iterate_j = [&](auto rj_tag) {
        using RJ = typename decltype(rj_tag)::type;
        const auto& orig_rj = RouteHelper<RJ>::get_routes(
            st.original.truck_routes, st.original.drone_routes);

        for (size_t ri_idx = 0; ri_idx < orig_ri[vehicle_i].size(); ++ri_idx) {
            const auto& route_i = orig_ri[vehicle_i][ri_idx];

            for (size_t vehicle_j = 0; vehicle_j < orig_rj.size(); ++vehicle_j) {
                for (size_t rj_idx = 0; rj_idx < orig_rj[vehicle_j].size(); ++rj_idx) {
                    const auto& route_j = orig_rj[vehicle_j][rj_idx];

                    // Skip same route (compare by first interior customer)
                    if (route_i->data().customers[1] == route_j->data().customers[1]) continue;

                    // Collect neighbors
                    using Tup = std::tuple<
                        std::optional<LocalRc<RI>>,
                        std::optional<LocalRc<RJ>>,
                        std::vector<size_t>>;
                    std::vector<Tup> neighbors = route_i->template inter_route<RJ>(route_j, n);

                    if (asymmetric) {
                        // route_j.inter_route(route_i, n) -> swap (1,0) -> (0,1)
                        auto rev = route_j->template inter_route<RI>(route_i, n);
                        for (auto& [a, b, t] : rev) {
                            neighbors.emplace_back(b, a, t);
                        }
                    }

                    for (auto& [new_ri, new_rj, tabu] : neighbors) {
                        // single_customer constraint
                        if (new_ri && RouteHelper<RI>::single_customer() &&
                            (*new_ri)->data().customers.size() != 3) continue;
                        if (new_rj && RouteHelper<RJ>::single_customer() &&
                            (*new_rj)->data().customers.size() != 3) continue;

                        // Determine index correction for swap_remove
                        size_t rj_idx_adj = rj_idx;

                        // Apply new_ri
                        {
                            auto& cr = RouteHelper<RI>::get_routes_mut(truck_cloned, drone_cloned);
                            if (new_ri) {
                                cr[vehicle_i][ri_idx] = *new_ri;
                            } else {
                                // Check if RI == RJ and same vehicle: swap_remove
                                // from route_i's vector may affect route_j's index
                                if constexpr (std::is_same_v<RI, RJ>) {
                                    if (vehicle_i == vehicle_j &&
                                        rj_idx == orig_rj[vehicle_j].size() - 1) {
                                        rj_idx_adj = ri_idx;
                                    }
                                }
                                swap_remove_elem(cr[vehicle_i], ri_idx);
                            }
                        }

                        // Apply new_rj
                        {
                            auto& cr = RouteHelper<RJ>::get_routes_mut(truck_cloned, drone_cloned);
                            if (new_rj) {
                                cr[vehicle_j][rj_idx_adj] = *new_rj;
                            } else {
                                swap_remove_elem(cr[vehicle_j], rj_idx_adj);
                            }
                        }

                        Solution::update_delta_inplace(st.scratch, st.original,
                            std::move(truck_cloned), std::move(drone_cloned));
                        internal_update(st, st.scratch, std::move(tabu));

                        // Restore
                        truck_cloned = std::move(st.scratch.truck_routes);
                        drone_cloned = std::move(st.scratch.drone_routes);

                        {
                            auto& cr = RouteHelper<RJ>::get_routes_mut(truck_cloned, drone_cloned);
                            if (new_rj) {
                                cr[vehicle_j][rj_idx_adj] = route_j;
                            } else {
                                swap_push(cr[vehicle_j], rj_idx_adj, route_j);
                            }
                        }
                        {
                            auto& cr = RouteHelper<RI>::get_routes_mut(truck_cloned, drone_cloned);
                            if (new_ri) {
                                cr[vehicle_i][ri_idx] = route_i;
                            } else {
                                swap_push(cr[vehicle_i], ri_idx, route_i);
                            }
                        }
                    }
                }
            }
        }
    };

    struct TruckTag { using type = TruckRoute; };
    struct DroneTag { using type = DroneRoute; };
    iterate_j(TruckTag{});
    iterate_j(DroneTag{});
}

// -----------------------------------------------------------------------
// inter_route_extract_internal<RI>
// -----------------------------------------------------------------------
template<typename RI>
static void inter_route_extract_internal(
    Neighborhood n,
    IterState&   st,
    size_t       vehicle_i,
    std::vector<std::vector<LocalRc<TruckRoute>>>& truck_cloned,
    std::vector<std::vector<LocalRc<DroneRoute>>>& drone_cloned)
{
    const auto& orig_ri = RouteHelper<RI>::get_routes(
        st.original.truck_routes, st.original.drone_routes);

    auto iterate_j = [&](auto rj_tag) {
        using RJ = typename decltype(rj_tag)::type;
        const auto& orig_rj = RouteHelper<RJ>::get_routes(
            st.original.truck_routes, st.original.drone_routes);

        for (size_t ri_idx = 0; ri_idx < orig_ri[vehicle_i].size(); ++ri_idx) {
            const auto& route_i = orig_ri[vehicle_i][ri_idx];

            auto extracted = route_i->template inter_route_extract<RJ>(n);

            for (auto& [new_ri, new_rj, tabu] : extracted) {
                if (RouteHelper<RJ>::single_customer() &&
                    new_rj->data().customers.size() != 3) continue;

                {
                    auto& cr = RouteHelper<RI>::get_routes_mut(truck_cloned, drone_cloned);
                    cr[vehicle_i][ri_idx] = new_ri;
                }

                for (size_t vehicle_j = 0; vehicle_j < orig_rj.size(); ++vehicle_j) {
                    if (RouteHelper<RJ>::single_route() &&
                        !orig_rj[vehicle_j].empty()) continue;

                    {
                        auto& cr = RouteHelper<RJ>::get_routes_mut(truck_cloned, drone_cloned);
                        cr[vehicle_j].push_back(new_rj);
                    }

                    Solution s = Solution::make(std::move(truck_cloned), std::move(drone_cloned));
                    internal_update(st, s, tabu);

                    truck_cloned = std::move(s.truck_routes);
                    drone_cloned = std::move(s.drone_routes);

                    {
                        auto& cr = RouteHelper<RJ>::get_routes_mut(truck_cloned, drone_cloned);
                        cr[vehicle_j].pop_back();
                    }
                }

                {
                    auto& cr = RouteHelper<RI>::get_routes_mut(truck_cloned, drone_cloned);
                    cr[vehicle_i][ri_idx] = route_i;
                }
            }
        }
    };

    struct TruckTag { using type = TruckRoute; };
    struct DroneTag { using type = DroneRoute; };
    iterate_j(TruckTag{});
    iterate_j(DroneTag{});
}

// -----------------------------------------------------------------------
// ejection_chain_internal
// -----------------------------------------------------------------------
static void ejection_chain_internal(IterState& st)
{
    auto [truck_routes, drone_routes] = AnyRoute::from_solution(st.original);
    // Helper that treats trucks first, then drones
    size_t num_trucks = truck_routes.size();
    size_t num_drones = drone_routes.size();
    size_t total      = num_trucks + num_drones;

    auto vehicle_routes = [&](size_t v) -> std::vector<AnyRoute>& {
        return v < num_trucks ? truck_routes[v] : drone_routes[v - num_trucks];
    };

    auto same_route = [&](size_t vi, size_t ri, size_t vj, size_t rj) {
        return vehicle_routes(vi)[ri].customers()[1] ==
               vehicle_routes(vj)[rj].customers()[1];
    };

    for (size_t vi = 0; vi < total; ++vi) {
        for (size_t ri = 0; ri < vehicle_routes(vi).size(); ++ri) {
            for (size_t vj = 0; vj < total; ++vj) {
                for (size_t rj = 0; rj < vehicle_routes(vj).size(); ++rj) {
                    if (same_route(vi, ri, vj, rj)) continue;
                    for (size_t vk = 0; vk < total; ++vk) {
                        for (size_t rk = 0; rk < vehicle_routes(vk).size(); ++rk) {
                            if (same_route(vj, rj, vk, rk)) continue;
                            if (same_route(vk, rk, vi, ri)) continue;

                            const AnyRoute& ari = vehicle_routes(vi)[ri];
                            const AnyRoute& arj = vehicle_routes(vj)[rj];
                            const AnyRoute& ark = vehicle_routes(vk)[rk];

                            auto neighbors = ari.inter_route_3_any(arj, ark,
                                                Neighborhood::EjectionChain);

                            for (auto& [ni_opt, nj, nk, tabu] : neighbors) {
                                if (!ni_opt) continue; // avoid changing route config

                                // Build new route sets
                                auto new_tr = truck_routes;
                                auto new_dr = drone_routes;

                                auto& nvr_k = (vk < num_trucks)
                                    ? new_tr[vk] : new_dr[vk - num_trucks];
                                nvr_k[rk] = nk;

                                auto& nvr_j = (vj < num_trucks)
                                    ? new_tr[vj] : new_dr[vj - num_trucks];
                                nvr_j[rj] = nj;

                                auto& nvr_i = (vi < num_trucks)
                                    ? new_tr[vi] : new_dr[vi - num_trucks];
                                if (ni_opt) {
                                    nvr_i[ri] = *ni_opt;
                                } else {
                                    swap_remove_elem(nvr_i, ri);
                                }

                                Solution s = AnyRoute::to_solution(
                                    std::move(new_tr), std::move(new_dr));

                                if (internal_update(st, s, std::move(tabu))) {
                                    // Update local view from new best
                                    auto [ntr, ndr] = AnyRoute::from_solution(s);
                                    truck_routes = std::move(ntr);
                                    drone_routes = std::move(ndr);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------
// Public: inter_route
// -----------------------------------------------------------------------
namespace neighborhoods {

std::pair<Solution, std::vector<size_t>> inter_route(
    Neighborhood n,
    const Solution& solution,
    const TabuSet& tabu_list,
    double aspiration_cost,
    std::size_t& evaluations)
{
    evaluations = 0;
    auto [vehicle_i, is_truck] = find_decisive_vehicle(solution);

    auto truck_cloned = solution.truck_routes;
    auto drone_cloned = solution.drone_routes;

    double min_cost         = std::numeric_limits<double>::max();
    bool   require_feasible = false;
    Solution best           = solution;
    std::vector<size_t> best_tabu;
    Solution scratch        = solution;

    IterState st{solution, tabu_list, aspiration_cost,
                 min_cost, require_feasible, best, best_tabu, evaluations, scratch};

    if (n == Neighborhood::EjectionChain) {
        ejection_chain_internal(st);
    } else {
        if (is_truck)
            inter_route_internal<TruckRoute>(n, st, vehicle_i, truck_cloned, drone_cloned);
        else
            inter_route_internal<DroneRoute>(n, st, vehicle_i, truck_cloned, drone_cloned);

        if (is_truck)
            inter_route_extract_internal<TruckRoute>(n, st, vehicle_i, truck_cloned, drone_cloned);
        else
            inter_route_extract_internal<DroneRoute>(n, st, vehicle_i, truck_cloned, drone_cloned);
    }

    return {best, best_tabu};
}

// -----------------------------------------------------------------------
// Public: intra_route
// -----------------------------------------------------------------------
std::pair<Solution, std::vector<size_t>> intra_route(
    Neighborhood n,
    const Solution& solution,
    const TabuSet& tabu_list,
    double aspiration_cost,
    std::size_t& evaluations)
{
    evaluations = 0;
    Solution best = solution;
    std::vector<size_t> best_tabu;

    if (n == Neighborhood::EjectionChain) return {best, best_tabu};

    auto [vehicle, is_truck] = find_decisive_vehicle(solution);

    auto truck_cloned = solution.truck_routes;
    auto drone_cloned = solution.drone_routes;

    double min_cost         = std::numeric_limits<double>::max();
    bool   require_feasible = false;
    Solution scratch        = solution;

    IterState st{solution, tabu_list, aspiration_cost,
                 min_cost, require_feasible, best, best_tabu, evaluations, scratch};

    auto do_search = [&](auto& routes, auto& cloned) {
        if (vehicle >= routes.size()) return;
        for (size_t i = 0; i < routes[vehicle].size(); ++i) {
            const auto& route = routes[vehicle][i];
            auto neighbors = route->intra_route(n);
            for (auto& [new_route, tabu] : neighbors) {
                cloned[vehicle][i] = new_route;
                Solution::update_delta_inplace(st.scratch, st.original,
                    std::move(truck_cloned), std::move(drone_cloned));
                internal_update(st, st.scratch, std::move(tabu));
                truck_cloned = std::move(st.scratch.truck_routes);
                drone_cloned = std::move(st.scratch.drone_routes);
                cloned[vehicle][i] = route;
            }
        }
    };

    if (is_truck) do_search(solution.truck_routes, truck_cloned);
    else          do_search(solution.drone_routes, drone_cloned);

    return {best, best_tabu};
}

// -----------------------------------------------------------------------
// Public: search
// -----------------------------------------------------------------------
bool search(
    Neighborhood n,
    const Solution& solution,
    TabuSet& tabu_list,
    size_t tabu_size,
    double aspiration_cost,
    Solution& out_result,
    std::size_t& evaluations)
{
    std::size_t intra_evals = 0, inter_evals = 0;
    auto [intra_sol, intra_tabu] = intra_route(n, solution, tabu_list, aspiration_cost, intra_evals);
    auto [inter_sol, inter_tabu] = inter_route(n, solution, tabu_list, aspiration_cost, inter_evals);
    evaluations = intra_evals + inter_evals;

    Solution result;
    std::vector<size_t> tabu;

    if (intra_tabu.empty() && inter_tabu.empty()) return false;

    if (intra_tabu.empty()) {
        result = std::move(inter_sol); tabu = std::move(inter_tabu);
    } else if (inter_tabu.empty()) {
        result = std::move(intra_sol); tabu = std::move(intra_tabu);
    } else if (intra_sol.cost() < inter_sol.cost()) {
        result = std::move(intra_sol); tabu = std::move(intra_tabu);
    } else {
        result = std::move(inter_sol); tabu = std::move(inter_tabu);
    }

    std::sort(tabu.begin(), tabu.end());
    tabu_list.touch(tabu, tabu_size);

    out_result = std::move(result);
    return true;
}

} // namespace neighborhoods
