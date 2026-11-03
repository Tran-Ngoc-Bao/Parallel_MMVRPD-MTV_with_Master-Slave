#include "neighborhoods.hpp"
#include "routes.hpp"
#include "solutions.hpp"
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <type_traits>

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

// Tabu note (inherited from the Rust version, kept): stored keys and intra-route keys
// are sorted, inter-route keys are checked unsorted, so multi-customer inter-route
// moves are only partly tabu. Sorting them was tried: worse with the current parameters.

// Passes a route type to a generic lambda (C++17 has no template lambdas).
template<typename T> struct Tag { using type = T; };

// Neighbour scoring without building the solution: same operations, same order as
// update_delta_inplace on the (at most two) changed vehicles, so the same cost to the
// bit. Only a neighbour that becomes the best one is built.
template<typename R> static RouteVals vals_of(const R& r);
template<> RouteVals vals_of(const TruckRoute& r) {
    RouteVals v;
    v.working_time           = r.working_time();
    v.capacity_violation     = r.capacity_violation();
    v.waiting_time_violation = r.waiting_time_violation();
    return v;
}
template<> RouteVals vals_of(const DroneRoute& r) {
    RouteVals v;
    v.working_time           = r.working_time();
    v.capacity_violation     = r.capacity_violation();
    v.waiting_time_violation = r.waiting_time_violation();
    v.energy_violation       = r.energy_violation;
    v.fixed_time_violation   = r.fixed_time_violation;
    return v;
}

// Per-vehicle sums, as in update_delta_inplace (cv already divided by the
// capacity route by route).
struct VehicleSum { double wt = 0, cv = 0, wv = 0, ev = 0, fv = 0; };

template<typename R> static VehicleSum sum_vehicle(const std::vector<RouteVals>& routes);
template<> VehicleSum sum_vehicle<TruckRoute>(const std::vector<RouteVals>& routes) {
    const Config& cfg = global_config();
    VehicleSum s;
    for (const auto& r : routes) {
        s.wt += r.working_time;
        s.cv += r.capacity_violation / cfg.truck.capacity;
        s.wv += r.waiting_time_violation;
    }
    return s;
}
template<> VehicleSum sum_vehicle<DroneRoute>(const std::vector<RouteVals>& routes) {
    const Config& cfg = global_config();
    VehicleSum s;
    for (const auto& r : routes) {
        s.wt += r.working_time;
        s.ev += r.energy_violation;
        s.cv += r.capacity_violation / cfg.drone.capacity();
        s.wv += r.waiting_time_violation;
        s.fv += r.fixed_time_violation;
    }
    return s;
}

// Totals of `base` with up to two vehicles replaced: (truck?, index, sums).
struct VehicleSub { bool truck; size_t index; VehicleSum sum; };

struct Totals {
    double working_time, energy_violation, capacity_violation,
           waiting_time_violation, fixed_time_violation;
    bool feasible;
};

// update_delta_inplace's totals for base with 1-2 vehicles replaced: sums restart from
// base's partial sum before the first replaced vehicle (same additions, same bits);
// the max working time uses base's three largest values.
class BaseTotals {
public:
    explicit BaseTotals(const Solution& base)
        : nt_(base.truck_working_time.size()), nd_(base.drone_working_time.size())
    {
        for (size_t t = 0; t < nt_; ++t) {
            wt_.push_back(base.truck_working_time[t]);
            cv_.push_back(base.truck_capacity_violation[t]);
            wv_.push_back(base.truck_waiting_violation[t]);
        }
        for (size_t d = 0; d < nd_; ++d) {
            wt_.push_back(base.drone_working_time[d]);
            cv_.push_back(base.drone_capacity_violation[d]);
            wv_.push_back(base.drone_waiting_violation[d]);
            ev_.push_back(base.drone_energy_violation[d]);
            fv_.push_back(base.drone_fixed_violation[d]);
        }
        auto prefix = [](const std::vector<double>& v) {
            std::vector<double> p(v.size() + 1, 0.0);
            for (size_t k = 0; k < v.size(); ++k) p[k + 1] = p[k] + v[k];
            return p;
        };
        cv_pre_ = prefix(cv_); wv_pre_ = prefix(wv_);
        ev_pre_ = prefix(ev_); fv_pre_ = prefix(fv_);

        // Positions of the three largest working times.
        for (size_t k = 0; k < wt_.size(); ++k) {
            size_t c = k;
            for (int r = 0; r < 3; ++r) {
                if (r >= ntop_) { top_[r] = c; ntop_ = r + 1; break; }
                if (wt_[c] > wt_[top_[r]]) std::swap(c, top_[r]);
            }
        }
    }

    Totals with(const VehicleSub* subs, size_t nsubs) const
    {
        const Config& cfg = global_config();
        size_t pos[2];   // position in trucks-then-drones order
        for (size_t k = 0; k < nsubs; ++k) pos[k] = subs[k].truck ? subs[k].index : nt_ + subs[k].index;
        auto sub_at = [&](size_t k) -> const VehicleSum* {
            for (size_t s = 0; s < nsubs; ++s) if (pos[s] == k) return &subs[s].sum;
            return nullptr;
        };

        // Working time: max of the vehicles not replaced, then of the replaced ones.
        double working_time = 0.0;
        for (int r = 0; r < ntop_; ++r) {
            if (sub_at(top_[r])) continue;
            working_time = wt_[top_[r]];
            break;
        }
        for (size_t s = 0; s < nsubs; ++s) working_time = std::max(working_time, subs[s].sum.wt);

        // Trucks-then-drones sums (capacity, waiting time).
        size_t first = pos[0];
        for (size_t s = 1; s < nsubs; ++s) first = std::min(first, pos[s]);
        double capacity_violation = cv_pre_[first], waiting_time_violation = wv_pre_[first];
        for (size_t k = first; k < wt_.size(); ++k) {
            const VehicleSum* s = sub_at(k);
            capacity_violation += s ? s->cv : cv_[k];
        }
        for (size_t k = first; k < wt_.size(); ++k) {
            const VehicleSum* s = sub_at(k);
            waiting_time_violation += s ? s->wv : wv_[k];
        }

        // Drones-only sums (energy, fixed time).
        size_t first_d = nd_;
        for (size_t s = 0; s < nsubs; ++s) if (!subs[s].truck) first_d = std::min(first_d, subs[s].index);
        double energy_violation = ev_pre_[first_d], fixed_time_violation = fv_pre_[first_d];
        for (size_t d = first_d; d < nd_; ++d) {
            const VehicleSum* s = sub_at(nt_ + d);
            energy_violation += s ? s->ev : ev_[d];
        }
        for (size_t d = first_d; d < nd_; ++d) {
            const VehicleSum* s = sub_at(nt_ + d);
            fixed_time_violation += s ? s->fv : fv_[d];
        }

        energy_violation       /= cfg.drone.battery();
        waiting_time_violation /= cfg.waiting_time_limit;
        fixed_time_violation   /= cfg.drone.fixed_time();

        return {working_time, energy_violation, capacity_violation, waiting_time_violation,
                fixed_time_violation,
                energy_violation == 0.0 && capacity_violation == 0.0 &&
                waiting_time_violation == 0.0 && fixed_time_violation == 0.0};
    }

private:
    size_t nt_, nd_;
    std::vector<double> wt_, cv_, wv_, ev_, fv_;               // trucks then drones (ev, fv: drones)
    std::vector<double> cv_pre_, wv_pre_, ev_pre_, fv_pre_;    // partial sums from 0.0
    size_t top_[3] = {0, 0, 0};
    int    ntop_ = 0;
};

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
    constexpr bool ri_truck = std::is_same_v<RI, TruckRoute>;

    bool asymmetric = (n == Neighborhood::Move10 ||
                       n == Neighborhood::Move20 ||
                       n == Neighborhood::Move21);

    // Scratch reused across neighbours.
    std::vector<size_t> bi, bj;                 // routes being generated
    std::vector<RouteVals> vals_i, vals_j;      // RouteVals of vehicle_i / vehicle_j routes
    std::vector<RouteVals> list_i, list_j;      // the same after the move
    std::vector<size_t> tabu_buf;
    vals_i.clear();
    for (const auto& r : orig_ri[vehicle_i]) vals_i.push_back(vals_of(*r));
    const BaseTotals base_totals(st.original);

    auto swap_remove_vals = [](std::vector<RouteVals>& v, size_t idx) {
        if (idx != v.size() - 1) v[idx] = v.back();
        v.pop_back();
    };

    // Helper lambda: iterate route_j over type RJ
    auto iterate_j = [&](auto rj_tag) {
        using RJ = typename decltype(rj_tag)::type;
        constexpr bool rj_truck = std::is_same_v<RJ, TruckRoute>;
        const auto& orig_rj = RouteHelper<RJ>::get_routes(
            st.original.truck_routes, st.original.drone_routes);

        for (size_t ri_idx = 0; ri_idx < orig_ri[vehicle_i].size(); ++ri_idx) {
            const auto& route_i = orig_ri[vehicle_i][ri_idx];

            for (size_t vehicle_j = 0; vehicle_j < orig_rj.size(); ++vehicle_j) {
                const bool same_list = std::is_same_v<RI, RJ> && vehicle_i == vehicle_j;
                vals_j.clear();
                for (const auto& r : orig_rj[vehicle_j]) vals_j.push_back(vals_of(*r));

                for (size_t rj_idx = 0; rj_idx < orig_rj[vehicle_j].size(); ++rj_idx) {
                    const auto& route_j = orig_rj[vehicle_j][rj_idx];

                    // Skip same route (compare by first interior customer)
                    if (route_i->data().customers[1] == route_j->data().customers[1]) continue;

                    // One neighbour: new route_i (ri: nullptr if removed) and
                    // new route_j (rj: nullptr if removed).
                    auto consider = [&](const RouteVals* vi, const std::vector<size_t>* ri,
                                        const RouteVals* vj, const std::vector<size_t>* rj,
                                        const size_t* tabu, size_t tabu_size) {
                        // single_customer constraint
                        if (ri && RouteHelper<RI>::single_customer() && ri->size() != 3) return;
                        if (rj && RouteHelper<RJ>::single_customer() && rj->size() != 3) return;

                        // The route lists after the move, as applied below to
                        // truck_cloned / drone_cloned (swap_remove included).
                        size_t rj_idx_adj = rj_idx;
                        VehicleSub subs[2];
                        size_t nsubs = 0;
                        list_i = vals_i;
                        if (vi) list_i[ri_idx] = *vi;
                        else {
                            if (same_list && rj_idx == vals_i.size() - 1) rj_idx_adj = ri_idx;
                            swap_remove_vals(list_i, ri_idx);
                        }
                        std::vector<RouteVals>& lj = same_list ? list_i : list_j;
                        if (!same_list) lj = vals_j;
                        if (vj) lj[rj_idx_adj] = *vj;
                        else    swap_remove_vals(lj, rj_idx_adj);
                        subs[nsubs++] = {ri_truck, vehicle_i, sum_vehicle<RI>(list_i)};
                        if (!same_list) subs[nsubs++] = {rj_truck, vehicle_j, sum_vehicle<RJ>(list_j)};
                        const Totals tot = base_totals.with(subs, nsubs);

                        // internal_update(), on the totals
                        ++st.evaluations;
                        if (st.require_feasible && !tot.feasible) return;
                        const double cost = Solution::penalized_cost(
                            tot.working_time, tot.energy_violation, tot.capacity_violation,
                            tot.waiting_time_violation, tot.fixed_time_violation);
                        const bool new_global = cost < st.aspiration_cost && tot.feasible;
                        // Unsorted key: see the tabu note at the top of this file.
                        tabu_buf.assign(tabu, tabu + tabu_size);
                        const bool in_tabu = !new_global && st.tabu_list.contains(tabu_buf);
                        if (!(new_global || (!in_tabu && cost < st.min_cost))) return;

                        // New best neighbour: build it as before.
                        {
                            auto& cr = RouteHelper<RI>::get_routes_mut(truck_cloned, drone_cloned);
                            if (ri) cr[vehicle_i][ri_idx] = RouteHelper<RI>::make(*ri);
                            else    swap_remove_elem(cr[vehicle_i], ri_idx);
                        }
                        {
                            auto& cr = RouteHelper<RJ>::get_routes_mut(truck_cloned, drone_cloned);
                            if (rj) cr[vehicle_j][rj_idx_adj] = RouteHelper<RJ>::make(*rj);
                            else    swap_remove_elem(cr[vehicle_j], rj_idx_adj);
                        }
                        size_t changed_truck[2] = {Solution::NO_VEHICLE, Solution::NO_VEHICLE};
                        size_t changed_drone[2] = {Solution::NO_VEHICLE, Solution::NO_VEHICLE};
                        (ri_truck ? changed_truck : changed_drone)[0] = vehicle_i;
                        (rj_truck ? changed_truck : changed_drone)[1] = vehicle_j;
                        Solution::update_delta_inplace(st.scratch, st.original,
                            std::move(truck_cloned), std::move(drone_cloned),
                            changed_truck, changed_drone);
                        st.min_cost    = cost;
                        st.best_result = st.scratch;
                        st.best_tabu   = tabu_buf;
                        if (new_global) {
                            st.aspiration_cost  = cost;
                            st.require_feasible = true;
                        }

                        // Restore
                        truck_cloned = std::move(st.scratch.truck_routes);
                        drone_cloned = std::move(st.scratch.drone_routes);
                        {
                            auto& cr = RouteHelper<RJ>::get_routes_mut(truck_cloned, drone_cloned);
                            if (rj) cr[vehicle_j][rj_idx_adj] = route_j;
                            else    swap_push(cr[vehicle_j], rj_idx_adj, route_j);
                        }
                        {
                            auto& cr = RouteHelper<RI>::get_routes_mut(truck_cloned, drone_cloned);
                            if (ri) cr[vehicle_i][ri_idx] = route_i;
                            else    swap_push(cr[vehicle_i], ri_idx, route_i);
                        }
                    };

                    route_i->template inter_route_visit<RJ>(route_j, n, bi, bj,
                        [&](const RouteVals* vd, const std::vector<size_t>* rd,
                            const RouteVals* vt, const std::vector<size_t>* rt,
                            const size_t* tabu, size_t tabu_size) {
                            consider(vd, rd, vt, rt, tabu, tabu_size);
                        });
                    if (asymmetric) {
                        // route_j.inter_route(route_i, n) -> swap (1,0) -> (0,1)
                        route_j->template inter_route_visit<RI>(route_i, n, bi, bj,
                            [&](const RouteVals* vd, const std::vector<size_t>* rd,
                                const RouteVals* vt, const std::vector<size_t>* rt,
                                const size_t* tabu, size_t tabu_size) {
                                consider(vt, rt, vd, rd, tabu, tabu_size);
                            });
                    }
                }
            }
        }
    };

    iterate_j(Tag<TruckRoute>{});
    iterate_j(Tag<DroneRoute>{});
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
                    internal_update(st, s, tabu);   // NOTE: tabu NOT moved here — it's reused in inner loop over vehicle_j

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

    iterate_j(Tag<TruckRoute>{});
    iterate_j(Tag<DroneRoute>{});
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

    // Only this vehicle's route list changes.
    const size_t changed_truck[2] = {is_truck ? vehicle : Solution::NO_VEHICLE, Solution::NO_VEHICLE};
    const size_t changed_drone[2] = {is_truck ? Solution::NO_VEHICLE : vehicle, Solution::NO_VEHICLE};

    auto do_search = [&](auto& routes, auto& cloned) {
        if (vehicle >= routes.size()) return;
        for (size_t i = 0; i < routes[vehicle].size(); ++i) {
            const auto& route = routes[vehicle][i];
            auto neighbors = route->intra_route(n);
            for (auto& [new_route, tabu] : neighbors) {
                cloned[vehicle][i] = new_route;
                Solution::update_delta_inplace(st.scratch, st.original,
                    std::move(truck_cloned), std::move(drone_cloned),
                    changed_truck, changed_drone);
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
