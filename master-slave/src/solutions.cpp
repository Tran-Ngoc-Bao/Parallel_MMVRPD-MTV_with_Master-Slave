#include "solutions.hpp"
#include "neighborhoods.hpp"
#include "clusterize.hpp"
#include "logger.hpp"
#include <algorithm>
#include <set>
#include <queue>
#include <random>
#include <unordered_set>
#include <chrono>
#include <numeric>
#include <iostream>
#include <cassert>
#include <cmath>
#include <array>

// -----------------------------------------------------------------------
// Penalty coefficients
// -----------------------------------------------------------------------
namespace penalty {
    std::atomic<double> coeff[4] = {
        {1.0}, {1.0}, {1.0}, {1.0}
    };
    void update(int i, double violation) {
        double v = coeff[i].load(std::memory_order_relaxed);
        v = (violation > 0.0) ? v * 1.5 : v / 1.5;
        coeff[i].store(std::clamp(v, 1.0, 1e3), std::memory_order_relaxed);
    }
}

// -----------------------------------------------------------------------
// All 6 neighborhoods used in tabu search
// -----------------------------------------------------------------------
static const Neighborhood NEIGHBORHOODS[] = {
    Neighborhood::Move10,
    Neighborhood::Move11,
    Neighborhood::Move20,
    Neighborhood::Move21,
    Neighborhood::Move22,
    Neighborhood::TwoOpt
};
static constexpr size_t NUM_NEIGHBORHOODS = 6;
static constexpr double TOLERANCE = 0.001;
static constexpr double SIGNIFICANT_PUSH_RELATIVE_THRESHOLD = 0.001;

// -----------------------------------------------------------------------
// Solution::make
// -----------------------------------------------------------------------
Solution Solution::make(
    std::vector<std::vector<LocalRc<TruckRoute>>> truck_routes,
    std::vector<std::vector<LocalRc<DroneRoute>>> drone_routes)
{
    const Config& cfg = global_config();
    Solution sol;
    sol.truck_routes = std::move(truck_routes);
    sol.drone_routes = std::move(drone_routes);

    double working_time           = 0.0;
    double energy_violation       = 0.0;
    double capacity_violation     = 0.0;
    double waiting_time_violation = 0.0;
    double fixed_time_violation   = 0.0;

    sol.truck_working_time.resize(sol.truck_routes.size(), 0.0);
    sol.drone_working_time.resize(sol.drone_routes.size(), 0.0);
    sol.truck_capacity_violation.resize(sol.truck_routes.size(), 0.0);
    sol.truck_waiting_violation.resize(sol.truck_routes.size(), 0.0);
    sol.drone_capacity_violation.resize(sol.drone_routes.size(), 0.0);
    sol.drone_waiting_violation.resize(sol.drone_routes.size(), 0.0);
    sol.drone_energy_violation.resize(sol.drone_routes.size(), 0.0);
    sol.drone_fixed_violation.resize(sol.drone_routes.size(), 0.0);

    for (size_t t = 0; t < sol.truck_routes.size(); ++t) {
        double sum = 0.0;
        double veh_cv = 0.0, veh_wv = 0.0;
        for (const auto& r : sol.truck_routes[t]) {
            sum                += r->working_time();
            capacity_violation += r->capacity_violation() / cfg.truck.capacity;
            waiting_time_violation += r->waiting_time_violation();
            veh_cv += r->capacity_violation() / cfg.truck.capacity;
            veh_wv += r->waiting_time_violation();
        }
        sol.truck_working_time[t]       = sum;
        sol.truck_capacity_violation[t] = veh_cv;
        sol.truck_waiting_violation[t]  = veh_wv;
        working_time = std::max(working_time, sum);
    }

    for (size_t d = 0; d < sol.drone_routes.size(); ++d) {
        double sum = 0.0;
        double veh_ev = 0.0, veh_cv = 0.0, veh_wv = 0.0, veh_fv = 0.0;
        for (const auto& r : sol.drone_routes[d]) {
            sum                  += r->working_time();
            energy_violation     += r->energy_violation;
            capacity_violation   += r->capacity_violation() / cfg.drone.capacity();
            waiting_time_violation += r->waiting_time_violation();
            fixed_time_violation += r->fixed_time_violation;
            veh_ev += r->energy_violation;
            veh_cv += r->capacity_violation() / cfg.drone.capacity();
            veh_wv += r->waiting_time_violation();
            veh_fv += r->fixed_time_violation;
        }
        sol.drone_working_time[d]       = sum;
        sol.drone_energy_violation[d]   = veh_ev;
        sol.drone_capacity_violation[d] = veh_cv;
        sol.drone_waiting_violation[d]  = veh_wv;
        sol.drone_fixed_violation[d]    = veh_fv;
        working_time = std::max(working_time, sum);
    }

    energy_violation       /= cfg.drone.battery();
    waiting_time_violation /= cfg.waiting_time_limit;
    fixed_time_violation   /= cfg.drone.fixed_time();  // x/infinity = 0 for Unlimited

    sol.working_time           = working_time;
    sol.energy_violation       = energy_violation;
    sol.capacity_violation     = capacity_violation;
    sol.waiting_time_violation = waiting_time_violation;
    sol.fixed_time_violation   = fixed_time_violation;
    sol.feasible = (energy_violation       == 0.0 &&
                    capacity_violation     == 0.0 &&
                    waiting_time_violation == 0.0 &&
                    fixed_time_violation   == 0.0);
    return sol;
}

// -----------------------------------------------------------------------
// Solution::update_delta_inplace
// -----------------------------------------------------------------------
void Solution::update_delta_inplace(
    Solution& scratch,
    const Solution& base,
    std::vector<std::vector<LocalRc<TruckRoute>>> new_truck_routes,
    std::vector<std::vector<LocalRc<DroneRoute>>> new_drone_routes)
{
    const Config& cfg = global_config();

    scratch.truck_routes = std::move(new_truck_routes);
    scratch.drone_routes = std::move(new_drone_routes);

    auto same_routes = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i])) return false;
        return true;
    };

    for (size_t t = 0; t < scratch.truck_routes.size(); ++t) {
        if (same_routes(scratch.truck_routes[t], base.truck_routes[t])) {
            scratch.truck_working_time[t]       = base.truck_working_time[t];
            scratch.truck_capacity_violation[t] = base.truck_capacity_violation[t];
            scratch.truck_waiting_violation[t]  = base.truck_waiting_violation[t];
            continue;
        }
        double sum = 0.0, cv = 0.0, wv = 0.0;
        for (const auto& r : scratch.truck_routes[t]) {
            sum += r->working_time();
            cv  += r->capacity_violation() / cfg.truck.capacity;
            wv  += r->waiting_time_violation();
        }
        scratch.truck_working_time[t]       = sum;
        scratch.truck_capacity_violation[t] = cv;
        scratch.truck_waiting_violation[t]  = wv;
    }

    for (size_t d = 0; d < scratch.drone_routes.size(); ++d) {
        if (same_routes(scratch.drone_routes[d], base.drone_routes[d])) {
            scratch.drone_working_time[d]       = base.drone_working_time[d];
            scratch.drone_capacity_violation[d] = base.drone_capacity_violation[d];
            scratch.drone_waiting_violation[d]  = base.drone_waiting_violation[d];
            scratch.drone_energy_violation[d]   = base.drone_energy_violation[d];
            scratch.drone_fixed_violation[d]    = base.drone_fixed_violation[d];
            continue;
        }
        double sum = 0.0, ev = 0.0, cv = 0.0, wv = 0.0, fv = 0.0;
        for (const auto& r : scratch.drone_routes[d]) {
            sum += r->working_time();
            ev  += r->energy_violation;
            cv  += r->capacity_violation() / cfg.drone.capacity();
            wv  += r->waiting_time_violation();
            fv  += r->fixed_time_violation;
        }
        scratch.drone_working_time[d]       = sum;
        scratch.drone_energy_violation[d]   = ev;
        scratch.drone_capacity_violation[d] = cv;
        scratch.drone_waiting_violation[d]  = wv;
        scratch.drone_fixed_violation[d]    = fv;
    }

    double working_time = 0.0;
    for (double v : scratch.truck_working_time) working_time = std::max(working_time, v);
    for (double v : scratch.drone_working_time) working_time = std::max(working_time, v);

    double capacity_violation = 0.0, waiting_time_violation = 0.0,
           energy_violation = 0.0, fixed_time_violation = 0.0;
    for (double v : scratch.truck_capacity_violation) capacity_violation += v;
    for (double v : scratch.drone_capacity_violation) capacity_violation += v;
    for (double v : scratch.truck_waiting_violation)  waiting_time_violation += v;
    for (double v : scratch.drone_waiting_violation)  waiting_time_violation += v;
    for (double v : scratch.drone_energy_violation)   energy_violation += v;
    for (double v : scratch.drone_fixed_violation)    fixed_time_violation += v;

    energy_violation       /= cfg.drone.battery();
    waiting_time_violation /= cfg.waiting_time_limit;
    fixed_time_violation   /= cfg.drone.fixed_time();

    scratch.working_time           = working_time;
    scratch.energy_violation       = energy_violation;
    scratch.capacity_violation     = capacity_violation;
    scratch.waiting_time_violation = waiting_time_violation;
    scratch.fixed_time_violation   = fixed_time_violation;
    scratch.feasible = (energy_violation       == 0.0 &&
                         capacity_violation     == 0.0 &&
                         waiting_time_violation == 0.0 &&
                         fixed_time_violation   == 0.0);
}

// -----------------------------------------------------------------------
// cost()
// -----------------------------------------------------------------------
double Solution::cost() const {
    double p = 1.0
        + penalty::get(0) * energy_violation
        + penalty::get(1) * capacity_violation
        + penalty::get(2) * waiting_time_violation
        + penalty::get(3) * fixed_time_violation;
    const double exp = global_config().penalty_exponent;
    return working_time * (exp == 0.5 ? std::sqrt(p) : std::pow(p, exp));
}

// -----------------------------------------------------------------------
// hamming_distance
// -----------------------------------------------------------------------
size_t Solution::hamming_distance(const Solution& other) const {
    const Config& cfg = global_config();
    auto fill = [&](const Solution& s, std::vector<size_t>& repr) {
        for (const auto& routes : s.truck_routes)
            for (const auto& r : routes) {
                const auto& c = r->data().customers;
                for (size_t i = 1; i + 1 < c.size(); ++i) repr[c[i]] = c[i+1];
            }
        for (const auto& routes : s.drone_routes)
            for (const auto& r : routes) {
                const auto& c = r->data().customers;
                for (size_t i = 1; i + 1 < c.size(); ++i) repr[c[i]] = c[i+1];
            }
    };
    std::vector<size_t> a(cfg.customers_count+1, 0), b(cfg.customers_count+1, 0);
    fill(*this, a); fill(other, b);
    size_t count = 0;
    for (size_t i = 0; i <= cfg.customers_count; ++i) if (a[i] != b[i]) ++count;
    return count;
}

// -----------------------------------------------------------------------
// verify
// -----------------------------------------------------------------------
void Solution::verify() const {
    const Config& cfg = global_config();
    std::vector<bool> served(cfg.customers_count+1, false);
    served[0] = true;

    auto check = [&](const auto& vehicle_routes, bool sc, bool sr) {
        for (const auto& routes : vehicle_routes) {
            if (sr && routes.size() > 1)
                throw std::runtime_error("Vehicle has more than one route");
            for (const auto& r : routes) {
                const auto& c = r->data().customers;
                if (sc && c.size() != 3)
                    throw std::runtime_error("Route has more than one customer");
                if (c.front() != 0 || c.back() != 0)
                    throw std::runtime_error("Invalid route endpoints");
                for (size_t i = 1; i+1 < c.size(); ++i) {
                    if (served[c[i]]) throw std::runtime_error(
                        "Customer " + std::to_string(c[i]) + " served twice");
                    served[c[i]] = true;
                }
            }
        }
    };
    check(truck_routes, TruckRoute::single_customer(), TruckRoute::single_route());
    check(drone_routes, DroneRoute::single_customer(), DroneRoute::single_route());

    for (size_t i = 0; i <= cfg.customers_count; ++i)
        if (!served[i]) throw std::runtime_error(
            "Customer " + std::to_string(i) + " not served");
}

// -----------------------------------------------------------------------
// JSON serialization
// -----------------------------------------------------------------------
nlohmann::json Solution::to_json() const {
    nlohmann::json j;
    // truck_routes: array of array of array of size_t
    auto route_arr = [](const auto& vr) {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& routes : vr) {
            nlohmann::json va = nlohmann::json::array();
            for (const auto& r : routes)
                va.push_back(r->data().customers);
            a.push_back(va);
        }
        return a;
    };
    j["truck_routes"]          = route_arr(truck_routes);
    j["drone_routes"]          = route_arr(drone_routes);
    j["truck_working_time"]    = truck_working_time;
    j["drone_working_time"]    = drone_working_time;
    j["working_time"]          = working_time;
    j["energy_violation"]      = energy_violation;
    j["capacity_violation"]    = capacity_violation;
    j["waiting_time_violation"]= waiting_time_violation;
    j["fixed_time_violation"]  = fixed_time_violation;
    j["feasible"]              = feasible;
    return j;
}

Solution Solution::from_json(const nlohmann::json& j) {
    std::vector<std::vector<LocalRc<TruckRoute>>> tr;
    std::vector<std::vector<LocalRc<DroneRoute>>> dr;

    for (const auto& vr : j.at("truck_routes")) {
        tr.push_back({});
        for (const auto& r : vr)
            tr.back().push_back(TruckRoute::make(r.get<std::vector<size_t>>()));
    }
    for (const auto& vr : j.at("drone_routes")) {
        dr.push_back({});
        for (const auto& r : vr)
            dr.back().push_back(DroneRoute::make(r.get<std::vector<size_t>>()));
    }
    return Solution::make(std::move(tr), std::move(dr));
}

// -----------------------------------------------------------------------
// initialize()
// -----------------------------------------------------------------------
Solution Solution::initialize()
{
    const Config& cfg = global_config();

    std::vector<size_t> index;
    index.reserve(cfg.customers_count);
    for (size_t i = 1; i <= cfg.customers_count; ++i) index.push_back(i);

    auto clusters = clusterize(index, cfg.trucks_count);

    std::vector<std::vector<LocalRc<TruckRoute>>> truck_routes(cfg.trucks_count);
    std::vector<std::vector<LocalRc<DroneRoute>>> drone_routes(cfg.trucks_count);

    std::vector<size_t> clusters_mapping(cfg.customers_count+1, 0);
    for (size_t ci = 0; ci < clusters.size(); ++ci)
        for (size_t c : clusters[ci]) clusters_mapping[c] = ci;

    // Check feasibility helper
    auto feasible_check = [](const std::vector<std::vector<LocalRc<TruckRoute>>>& tr,
                              const std::vector<std::vector<LocalRc<DroneRoute>>>& dr) {
        return Solution::make(tr, dr).feasible;
    };

    // Determine which customers are servable by truck / drone
    std::vector<bool> truckable(cfg.customers_count+1, false);
    std::vector<bool> dronable_local(cfg.customers_count+1, false);
    truckable[0]       = true;
    dronable_local[0]  = true;

    if (cfg.trucks_count > 0) {
        for (size_t c = 1; c <= cfg.customers_count; ++c) {
            truck_routes[0].push_back(TruckRoute::make_single(c));
            truckable[c] = feasible_check(truck_routes, drone_routes);
            truck_routes[0].pop_back();
        }
    }
    if (cfg.drones_count > 0) {
        for (size_t c = 1; c <= cfg.customers_count; ++c) {
            if (cfg.dronable[c]) {
                drone_routes[0].push_back(DroneRoute::make_single(c));
                dronable_local[c] = feasible_check(truck_routes, drone_routes);
                drone_routes[0].pop_back();
            }
        }
    }

    for (size_t c = 1; c <= cfg.customers_count; ++c)
        if (!truckable[c] && !dronable_local[c])
            throw std::runtime_error("Customer " + std::to_string(c) +
                " cannot be served by neither trucks nor drones");

    // Priority-queue state
    struct State {
        double working_time;
        size_t vehicle;
        size_t parent;
        size_t index;
        bool   is_truck;
        bool operator>(const State& o) const {
            return working_time > o.working_time;
        }
    };

    // Min-heap (pop smallest working_time first)
    std::priority_queue<State, std::vector<State>, std::greater<State>> queue;

    // RNG
    std::mt19937_64 rng;
    if (cfg.seed) rng.seed(*cfg.seed);
    else          rng.seed(std::random_device{}());

    for (size_t ci = 0; ci < clusters.size(); ++ci) {
        auto& cluster = clusters[ci];
        if (cluster.empty()) continue;

        // Shuffle and push first truckable as truck candidate
        std::shuffle(cluster.begin(), cluster.end(), rng);
        for (size_t c : cluster) {
            if (truckable[c]) {
                queue.push({0.0, ci, 0, cluster.front(), true});
                break;
            }
        }

        // Sort by drone distance from depot, push first dronable
        std::sort(cluster.begin(), cluster.end(), [&](size_t a, size_t b){
            return cfg.drone_distances[0][a] < cfg.drone_distances[0][b];
        });
        for (size_t c : cluster) {
            if (dronable_local[c]) {
                queue.push({0.0, ci, 0, cluster[0], false});
                break;
            }
        }
    }

    std::set<size_t> global_set;
    for (size_t i = 1; i <= cfg.customers_count; ++i) global_set.insert(i);

    // Helper: push next truck candidate
    auto truck_next = [&](size_t parent, size_t vehicle) {
        double best_d = std::numeric_limits<double>::infinity();
        size_t best_c = 0;
        const auto& cl = clusters[clusters_mapping[parent]];
        for (size_t c : cl) {
            if (truckable[c] && global_set.count(c) &&
                cfg.truck_distances[parent][c] < best_d) {
                best_d = cfg.truck_distances[parent][c];
                best_c = c;
            }
        }
        if (best_c == 0) {
            for (size_t c : global_set) {
                if (truckable[c] && cfg.truck_distances[parent][c] < best_d) {
                    best_d = cfg.truck_distances[parent][c];
                    best_c = c;
                }
            }
        }
        if (best_c != 0) {
            double wt = Solution::make(truck_routes, drone_routes).truck_working_time[vehicle];
            queue.push({wt, vehicle, parent, best_c, true});
        }
    };

    auto drone_next = [&](size_t parent, size_t vehicle) {
        double best_d = std::numeric_limits<double>::infinity();
        size_t best_c = 0;
        const auto& cl = clusters[clusters_mapping[parent]];
        for (size_t c : cl) {
            if (dronable_local[c] && global_set.count(c) &&
                cfg.drone_distances[parent][c] < best_d) {
                best_d = cfg.drone_distances[parent][c];
                best_c = c;
            }
        }
        if (best_c == 0) {
            for (size_t c : global_set) {
                if (dronable_local[c] && cfg.drone_distances[parent][c] < best_d) {
                    best_d = cfg.drone_distances[parent][c];
                    best_c = c;
                }
            }
        }
        if (best_c != 0) {
            double wt = Solution::make(truck_routes, drone_routes).drone_working_time[vehicle];
            queue.push({wt, vehicle, parent, best_c, false});
        }
    };

    while (!global_set.empty()) {
        if (queue.empty())
            throw std::runtime_error(
                "Cannot construct initial solution – unservable customers remain");

        State packed = queue.top(); queue.pop();
        size_t v     = packed.vehicle;

        // Is the index still in the cluster?
        auto& cl = clusters[clusters_mapping[packed.index]];
        auto  it = std::find(cl.begin(), cl.end(), packed.index);

        if (it != cl.end()) {
            if (packed.is_truck) {
                if (packed.parent == 0)
                    truck_routes[v].push_back(TruckRoute::make_single(packed.index));
                else {
                    auto& rt = truck_routes[v].back();
                    rt = rt->route_push(packed.index);
                }
            } else {
                if (packed.parent == 0)
                    drone_routes[v].push_back(DroneRoute::make_single(packed.index));
                else {
                    auto& rt = drone_routes[v].back();
                    rt = rt->route_push(packed.index);
                }
            }

            if (feasible_check(truck_routes, drone_routes)) {
                cl.erase(it);
                global_set.erase(packed.index);

                if (packed.is_truck)
                    truck_next(packed.index, v);
                else
                    drone_next(cfg.single_drone_route ? 0 : packed.index, v);
            } else {
                // Undo
                if (packed.is_truck) {
                    if (packed.parent == 0)
                        truck_routes[v].pop_back();
                    else {
                        auto& rt = truck_routes[v].back();
                        rt = rt->route_pop();
                    }
                    if (!cfg.single_truck_route)
                        truck_next(0, v);
                } else {
                    if (packed.parent == 0)
                        drone_routes[v].pop_back();
                    else {
                        auto& rt = drone_routes[v].back();
                        rt = rt->route_pop();
                    }
                    drone_next(0, v);
                }
            }
        } else {
            // index no longer in cluster (already assigned)
            if (packed.is_truck)
                truck_next(packed.parent, v);
            else
                drone_next(cfg.single_drone_route ? 0 : packed.parent, v);
        }
    }

    // Resize drone routes to cfg.drones_count
    if (cfg.drones_count > 0) {
        std::vector<LocalRc<DroneRoute>> all_routes;
        for (auto& v : drone_routes)
            for (auto& r : v) all_routes.push_back(r);
        std::sort(all_routes.begin(), all_routes.end(),
                  [](const auto& a, const auto& b){
                      return a->working_time() > b->working_time();
                  });

        drone_routes.assign(cfg.drones_count, {});
        std::vector<double> wt(cfg.drones_count, 0.0);
        for (const auto& r : all_routes) {
            size_t best = std::min_element(wt.begin(), wt.end()) - wt.begin();
            drone_routes[best].push_back(r);
            wt[best] += r->working_time();
        }
    } else {
        drone_routes.clear();
    }

    return Solution::make(std::move(truck_routes), std::move(drone_routes));
}

// -----------------------------------------------------------------------
// destroy_and_repair
// -----------------------------------------------------------------------
Solution Solution::destroy_and_repair(
    const std::vector<std::vector<double>>& edge_records) const
{
    const Config& cfg = global_config();
    std::mt19937_64 rng;
    if (cfg.seed) rng.seed(*cfg.seed + 1);
    else          rng.seed(std::random_device{}());

    // Score each customer by edge_records sum
    std::vector<double> scores(cfg.customers_count+1, 0.0);
    for (const auto& routes : truck_routes) {
        for (const auto& r : routes) {
            const auto& c = r->data().customers;
            for (size_t i = 1; i+1 < c.size(); ++i)
                scores[c[i]] = edge_records[c[i-1]][c[i]] + edge_records[c[i]][c[i+1]];
        }
    }
    for (const auto& routes : drone_routes) {
        for (const auto& r : routes) {
            const auto& c = r->data().customers;
            for (size_t i = 1; i+1 < c.size(); ++i)
                scores[c[i]] = edge_records[c[i-1]][c[i]] + edge_records[c[i]][c[i+1]];
        }
    }

    std::vector<size_t> ordered;
    ordered.reserve(cfg.customers_count);
    for (size_t i = 1; i <= cfg.customers_count; ++i) ordered.push_back(i);
    std::sort(ordered.begin(), ordered.end(),
              [&](size_t a, size_t b){ return scores[a] < scores[b]; });

    size_t destroy_count = static_cast<size_t>(
        static_cast<double>(cfg.customers_count) * cfg.destroy_rate);

    std::unordered_set<size_t> to_destroy_set;
    while (to_destroy_set.size() < destroy_count) {
        size_t n = ordered.size();
        std::uniform_int_distribution<size_t> dist(0, n-1);
        size_t r = dist(rng);
        size_t idx = r * r / n;
        to_destroy_set.insert(ordered[idx]);
    }

    auto cur_tr = truck_routes;
    auto cur_dr = drone_routes;

    // Destroy
    for (auto& routes : cur_tr) {
        size_t i = 0;
        while (i < routes.size()) {
            std::vector<size_t> buf;
            for (size_t c : routes[i]->data().customers)
                if (!to_destroy_set.count(c)) buf.push_back(c);
            if (buf.size() > 2) { routes[i] = TruckRoute::make(buf); ++i; }
            else swap_remove_elem(routes, i);
        }
    }
    for (auto& routes : cur_dr) {
        size_t i = 0;
        while (i < routes.size()) {
            std::vector<size_t> buf;
            for (size_t c : routes[i]->data().customers)
                if (!to_destroy_set.count(c)) buf.push_back(c);
            if (buf.size() > 2) { routes[i] = DroneRoute::make(buf); ++i; }
            else swap_remove_elem(routes, i);
        }
    }

    // Repair: insert each destroyed customer
    std::vector<size_t> to_destroy_vec(to_destroy_set.begin(), to_destroy_set.end());
    std::shuffle(to_destroy_vec.begin(), to_destroy_vec.end(), rng);

    // Temporarily set all penalty coefficients to max during repair
    double old_p[4];
    for (int i = 0; i < 4; ++i) {
        old_p[i] = penalty::get(i);
        penalty::set(i, 1e3);
    }

    for (size_t cust : to_destroy_vec) {
        double min_cost = std::numeric_limits<double>::max();
        struct Insert { bool is_truck; bool append; size_t vehicle; size_t route; size_t pos; };
        Insert best{true, true, 0, 0, 0};

        // Try trucks
        for (size_t t = 0; t < cur_tr.size(); ++t) {
            if (!cfg.single_truck_route || cur_tr[t].empty()) {
                cur_tr[t].push_back(TruckRoute::make_single(cust));
                auto tmp = Solution::make(std::move(cur_tr), std::move(cur_dr));
                if (tmp.cost() < min_cost) { min_cost = tmp.cost(); best = {true,true,t,0,0}; }
                cur_tr = std::move(tmp.truck_routes); cur_dr = std::move(tmp.drone_routes);
                cur_tr[t].pop_back();
            }
            for (size_t ri = 0; ri < cur_tr[t].size(); ++ri) {
                auto recover = cur_tr[t][ri];
                auto buf = recover->data().customers;
                buf.insert(buf.begin()+1, cust);
                for (size_t pos = 1; pos < recover->data().customers.size(); ++pos) {
                    cur_tr[t][ri] = TruckRoute::make(buf);
                    auto tmp = Solution::make(std::move(cur_tr), std::move(cur_dr));
                    if (tmp.cost() < min_cost) { min_cost = tmp.cost(); best = {true,false,t,ri,pos}; }
                    cur_tr = std::move(tmp.truck_routes); cur_dr = std::move(tmp.drone_routes);
                    std::swap(buf[pos], buf[pos+1]);
                }
                cur_tr[t][ri] = recover;
            }
        }

        // Try drones
        if (cfg.dronable[cust]) {
            for (size_t d = 0; d < cur_dr.size(); ++d) {
                cur_dr[d].push_back(DroneRoute::make_single(cust));
                auto tmp = Solution::make(std::move(cur_tr), std::move(cur_dr));
                if (tmp.cost() < min_cost) { min_cost = tmp.cost(); best = {false,true,d,0,0}; }
                cur_tr = std::move(tmp.truck_routes); cur_dr = std::move(tmp.drone_routes);
                cur_dr[d].pop_back();

                if (!cfg.single_drone_route) {
                    for (size_t ri = 0; ri < cur_dr[d].size(); ++ri) {
                        auto recover = cur_dr[d][ri];
                        auto buf = recover->data().customers;
                        buf.insert(buf.begin()+1, cust);
                        for (size_t pos = 1; pos < recover->data().customers.size(); ++pos) {
                            cur_dr[d][ri] = DroneRoute::make(buf);
                            auto tmp = Solution::make(std::move(cur_tr), std::move(cur_dr));
                            if (tmp.cost() < min_cost) { min_cost = tmp.cost(); best = {false,false,d,ri,pos}; }
                            cur_tr = std::move(tmp.truck_routes); cur_dr = std::move(tmp.drone_routes);
                            std::swap(buf[pos], buf[pos+1]);
                        }
                        cur_dr[d][ri] = recover;
                    }
                }
            }
        }

        // Apply best insertion
        if (best.is_truck) {
            if (best.append) {
                cur_tr[best.vehicle].push_back(TruckRoute::make_single(cust));
            } else {
                auto buf = cur_tr[best.vehicle][best.route]->data().customers;
                buf.insert(buf.begin() + static_cast<ptrdiff_t>(best.pos), cust);
                cur_tr[best.vehicle][best.route] = TruckRoute::make(buf);
            }
        } else {
            if (best.append) {
                cur_dr[best.vehicle].push_back(DroneRoute::make_single(cust));
            } else {
                auto buf = cur_dr[best.vehicle][best.route]->data().customers;
                buf.insert(buf.begin() + static_cast<ptrdiff_t>(best.pos), cust);
                cur_dr[best.vehicle][best.route] = DroneRoute::make(buf);
            }
        }
    }

    for (int i = 0; i < 4; ++i) penalty::set(i, old_p[i]);

    return Solution::make(std::move(cur_tr), std::move(cur_dr));
}

// -----------------------------------------------------------------------
// tabu_search
// -----------------------------------------------------------------------
Solution Solution::tabu_search(Solution root, Logger& logger, const EliteHooks* hooks)
{
    const Config& cfg = global_config();
    double gamma_1 = cfg.gamma_1;
    double gamma_2 = cfg.gamma_2;
    double gamma_3 = cfg.gamma_3;
    double gamma_4 = cfg.gamma_4;

    size_t total_vehicle = 0;
    for (const auto& r : root.truck_routes) if (!r.empty()) ++total_vehicle;
    for (const auto& r : root.drone_routes) if (!r.empty()) ++total_vehicle;
    if (total_vehicle == 0) total_vehicle = 1;

    double base     = static_cast<double>(cfg.customers_count) / total_vehicle;
    size_t tabu_sz  = static_cast<size_t>(cfg.tabu_size_factor * base);
    size_t adap_its = static_cast<size_t>(cfg.adaptive_iterations * base);
    size_t reset_after = cfg.fix_iteration
        ? std::numeric_limits<size_t>::max() / 2
        : static_cast<size_t>(cfg.reset_after_factor * base);

    Solution result = root;

    // RNG
    std::mt19937_64 rng;
    if (cfg.seed) rng.seed(*cfg.seed + 2);
    else          rng.seed(std::random_device{}());

    struct AdaptiveState {
        size_t segment             = 0;
        size_t segment_reset       = 0;
        size_t last_improved_seg   = 0;
        size_t pull_count          = 0;
        std::vector<double> scores;
        std::vector<double> weights;
        std::vector<uint32_t> occurences;
    };
    AdaptiveState adaptive;
    adaptive.scores.assign(NUM_NEIGHBORHOODS, 0.0);
    adaptive.weights.assign(NUM_NEIGHBORHOODS, 1.0);
    adaptive.occurences.assign(NUM_NEIGHBORHOODS, 0);

    if (cfg.dry_run) {
        logger.finalize(result, tabu_sz, reset_after, adap_its,
                        0, 0, 0.0, 0.0);
        return result;
    }

    Solution current = root;
    std::vector<std::vector<double>> edge_records(
        cfg.customers_count+1,
        std::vector<double>(cfg.customers_count+1,
                            std::numeric_limits<double>::max()));

    std::vector<Solution> elite_set;
    elite_set.push_back(result);

    std::vector<TabuSet> tabu_lists(NUM_NEIGHBORHOODS);
    size_t neighborhood_idx = 0;
    size_t last_improved    = 0;
    std::size_t total_evals = 0;

    const std::size_t eval_report_interval =
        cfg.max_evaluations > 0 ? std::max<std::size_t>(1, cfg.max_evaluations / 128) : 0;
    std::size_t next_eval_report = eval_report_interval;
    auto report_progress_if_due = [&]() {
        if (eval_report_interval == 0 || !hooks || !hooks->report_progress) return;
        while (total_evals >= next_eval_report) {
            hooks->report_progress(total_evals);
            next_eval_report += eval_report_interval;
        }
    };
    if (eval_report_interval > 0 && hooks && hooks->report_progress) {
        hooks->report_progress(0);
    }

    double last_pushed_cost = root.cost();
    if (hooks && hooks->push_elite) {
        hooks->push_elite(root);
    }

    auto record_new = [&](const Solution& nb, size_t iteration, size_t segment, bool allow_push = true) {
        if (nb.cost() + TOLERANCE < result.cost() && nb.feasible) {
            result       = nb;
            last_improved = iteration;
            adaptive.last_improved_seg = segment;

            // edge_records/elite_set only feed destroy_and_repair, which never
            // runs under Adaptive -- skip updating them in that case.
            if (cfg.strategy != cli::Strategy::Adaptive) {
                for (const auto& routes : nb.truck_routes)
                    for (const auto& r : routes) {
                        const auto& c = r->data().customers;
                        for (size_t i = 0; i+1 < c.size(); ++i)
                            edge_records[c[i]][c[i+1]] =
                                std::min(edge_records[c[i]][c[i+1]], nb.working_time);
                    }

                if (cfg.max_elite_size > 0) {
                    if (elite_set.size() == cfg.max_elite_size) {
                        // Remove least diverse
                        size_t min_idx = 0;
                        size_t min_hd  = elite_set[0].hamming_distance(result);
                        for (size_t i = 1; i < elite_set.size(); ++i) {
                            size_t hd = elite_set[i].hamming_distance(result);
                            if (hd < min_hd) { min_hd = hd; min_idx = i; }
                        }
                        swap_remove_elem(elite_set, min_idx);
                    }
                    elite_set.push_back(nb);
                }
            }

            bool should_push = false;
            switch (cfg.elite_push_strategy) {
                case cli::ElitePushStrategy::NewBest:
                    should_push = true;
                    break;
                case cli::ElitePushStrategy::SignificantBest:
                    should_push = last_pushed_cost <= 0.0
                        || (last_pushed_cost - result.cost()) / last_pushed_cost
                               >= SIGNIFICANT_PUSH_RELATIVE_THRESHOLD;
                    break;
                case cli::ElitePushStrategy::SegmentBest:
                    // Pushed at segment boundaries instead of on every improvement.
                    should_push = false;
                    break;
            }

            if (allow_push && should_push && hooks && hooks->push_elite) {
                hooks->push_elite(result);
                last_pushed_cost = result.cost();
            }
        }
    };

    auto update_violations = [](const Solution& s) {
        penalty::update(0, s.energy_violation);
        penalty::update(1, s.capacity_violation);
        penalty::update(2, s.waiting_time_violation);
        penalty::update(3, s.fixed_time_violation);
    };
    size_t max_iter = cfg.fix_iteration
        ? *cfg.fix_iteration
        : std::numeric_limits<size_t>::max() / 2;

    auto search_start = std::chrono::steady_clock::now();

    for (size_t iteration = 1; iteration <= max_iter; ++iteration) {
        // Stop-condition priority: max-evaluations, then time-limit, then
        // non-improving segments (checked further below via elite_set /
        // hooks->should_stop).
        if (cfg.max_evaluations > 0 && total_evals >= cfg.max_evaluations) break;

        if (cfg.time_limit > 0.0) {
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - search_start).count();
            if (elapsed >= cfg.time_limit) break;
        }

        if (cfg.verbose) {
            auto segments_before_reset = [&]() -> size_t {
                if (cfg.adaptive_fixed_segments)
                    return adaptive.segment > adaptive.segment_reset + cfg.adaptive_pull_elite_segments
                        ? adaptive.segment - (adaptive.segment_reset + cfg.adaptive_pull_elite_segments)
                        : 0;
                return cfg.adaptive_pull_elite_segments >
                    (adaptive.segment - std::max(adaptive.segment_reset, adaptive.last_improved_seg))
                    ? cfg.adaptive_pull_elite_segments -
                      (adaptive.segment - std::max(adaptive.segment_reset, adaptive.last_improved_seg))
                    : 0;
            };

            if (cfg.strategy == cli::Strategy::Adaptive) {
                std::cerr << "Iteration #" << iteration
                          << " (segments before reset " << segments_before_reset() << "): "
                          << current.cost() << "/" << result.cost()
                          << " elite " << elite_set.size() << "/" << cfg.max_elite_size
                          << "     \r";
            } else {
                size_t since = iteration - last_improved;
                size_t rem   = reset_after - (since % reset_after);
                std::cerr << "Iteration #" << iteration
                          << " (reset in " << rem << "): "
                          << current.cost() << "/" << result.cost()
                          << " elite " << elite_set.size() << "/" << cfg.max_elite_size
                          << "     \r";
            }
        }

        Neighborhood nb = NEIGHBORHOODS[neighborhood_idx];
        Solution old_current = current;

        Solution neighbor;
        std::size_t nb_evals = 0;
        bool found = neighborhoods::search(
            nb, current, tabu_lists[neighborhood_idx],
            tabu_sz, result.cost(), neighbor, nb_evals);
        total_evals += nb_evals;
        report_progress_if_due();

        if (found) {
            if (neighbor.feasible) {
                if (neighbor.cost() + TOLERANCE < result.cost())
                    adaptive.scores[neighborhood_idx] += gamma_1;
                else if (neighbor.cost() < current.cost())
                    adaptive.scores[neighborhood_idx] += gamma_2;
                else
                    adaptive.scores[neighborhood_idx] += gamma_3;
            }
            record_new(neighbor, iteration, adaptive.segment);
            current = std::move(neighbor);
        }

        ++adaptive.occurences[neighborhood_idx];

        // Check end-of-segment
        bool eos = false;
        if (cfg.adaptive_fixed_iterations)
            eos = iteration > 0 && iteration % adap_its == 0;
        else
            eos = iteration != last_improved
               && (iteration - last_improved) % adap_its == 0;

        if (eos) {
            ++adaptive.segment;

            if (cfg.elite_push_strategy == cli::ElitePushStrategy::SegmentBest
                && hooks && hooks->push_elite
                && result.cost() + TOLERANCE < last_pushed_cost) {
                hooks->push_elite(result);
                last_pushed_cost = result.cost();
            }
        }

        // Check reset / elite pull condition
        if (cfg.strategy == cli::Strategy::Adaptive) {
            bool do_pull_elite = false;
            if (cfg.adaptive_fixed_segments)
                do_pull_elite = adaptive.segment >= adaptive.segment_reset + cfg.adaptive_pull_elite_segments;
            else
                do_pull_elite = adaptive.segment >=
                    std::max(adaptive.segment_reset, adaptive.last_improved_seg)
                    + cfg.adaptive_pull_elite_segments;

            if (do_pull_elite) {
                bool attempted = false;
                bool pulled = false;
                if (hooks && hooks->pull_elite) {
                    attempted = true;
                    Solution pulled_elite;
                    pulled = hooks->pull_elite(iteration, pulled_elite);
                    if (pulled) {
                        record_new(pulled_elite, iteration, adaptive.segment, /*allow_push=*/false);
                        current = std::move(pulled_elite);
                    }
                }

                if (cfg.max_evaluations == 0 &&
                    hooks && hooks->should_stop && hooks->should_stop()) {
                    break;
                }

                // Reset search state on every completed pull attempt, even if
                // master had no elite to send -- only an actual Stop (handled
                // by the break above) skips this.
                if (attempted) {
                    for (auto& tl : tabu_lists) {
                        tl.clear();
                    }
                    adaptive.segment_reset = adaptive.segment;
                    adaptive.weights.assign(NUM_NEIGHBORHOODS, 1.0);
                    std::fill(adaptive.scores.begin(), adaptive.scores.end(), 0.0);
                    std::fill(adaptive.occurences.begin(), adaptive.occurences.end(), 0);
                }
                if (pulled) {
                    ++adaptive.pull_count;
                }
            }

            if (cfg.ejection_chain_iterations > 0 && do_pull_elite) {
                TabuSet ec_tabu;
                for (size_t ei = 0; ei < cfg.ejection_chain_iterations; ++ei) {
                    Solution ec_neighbor;
                    std::size_t ec_evals = 0;
                    bool ec_found = neighborhoods::search(
                        Neighborhood::EjectionChain, current, ec_tabu,
                        cfg.ejection_chain_iterations + 1, result.cost(), ec_neighbor, ec_evals);
                    total_evals += ec_evals;
                    report_progress_if_due();
                    if (ec_found) {
                        current = ec_neighbor;
                        record_new(current, iteration, adaptive.segment);
                    }
                    update_violations(current);
                    logger.log(current, Neighborhood::EjectionChain, ec_tabu.entries());
                }
            } else {
                update_violations(current);
                logger.log(current, nb, tabu_lists[neighborhood_idx].entries());
            }
        } else {
            bool do_reset = iteration != last_improved
                        && (iteration - last_improved) % reset_after == 0;

            if (do_reset) {
                adaptive.segment_reset = adaptive.segment;
                adaptive.weights.assign(NUM_NEIGHBORHOODS, 1.0);

                if (elite_set.empty()) {
                    if (cfg.max_evaluations > 0 || cfg.time_limit > 0.0) elite_set.push_back(result);
                    else break;
                }

                std::uniform_int_distribution<size_t> pick(0, elite_set.size()-1);
                size_t i = pick(rng);
                Solution picked = swap_remove_elem(elite_set, i);
                current = picked.destroy_and_repair(edge_records);
                for (auto& tl : tabu_lists) tl.clear();
            }

            if (do_reset && cfg.ejection_chain_iterations > 0) {
                TabuSet ec_tabu;
                for (size_t ei = 0; ei < cfg.ejection_chain_iterations; ++ei) {
                    Solution ec_neighbor;
                    std::size_t ec_evals = 0;
                    bool ec_found = neighborhoods::search(
                        Neighborhood::EjectionChain, current, ec_tabu,
                        cfg.ejection_chain_iterations + 1, result.cost(), ec_neighbor, ec_evals);
                    total_evals += ec_evals;
                    report_progress_if_due();
                    if (ec_found) {
                        current = ec_neighbor;
                        record_new(current, iteration, adaptive.segment);
                    }
                    update_violations(current);
                    logger.log(current, Neighborhood::EjectionChain, ec_tabu.entries());
                }
            } else {
                update_violations(current);
                logger.log(current, nb, tabu_lists[neighborhood_idx].entries());
            }
        }

        // Advance neighborhood selection
        switch (cfg.strategy) {
        case cli::Strategy::Random:
            neighborhood_idx =
                std::uniform_int_distribution<size_t>(0, NUM_NEIGHBORHOODS-1)(rng);
            break;
        case cli::Strategy::Cyclic:
            neighborhood_idx = (neighborhood_idx + 1) % NUM_NEIGHBORHOODS;
            break;
        case cli::Strategy::Vns:
            if (iteration == last_improved) {
                neighborhood_idx = 0;
            } else {
                neighborhood_idx = (neighborhood_idx + 1) % NUM_NEIGHBORHOODS;
                if (neighborhood_idx != 0) current = old_current;
            }
            break;
        case cli::Strategy::Adaptive:
            if (eos) {
                for (size_t ni = 0; ni < NUM_NEIGHBORHOODS; ++ni) {
                    if (adaptive.occurences[ni] > 0) {
                        adaptive.weights[ni] = (1 - gamma_4) * adaptive.weights[ni]
                            + gamma_4 * adaptive.scores[ni] / adaptive.occurences[ni];
                    }
                    adaptive.scores[ni]     = 0.0;
                    adaptive.occurences[ni] = 0;
                }
            }
            neighborhood_idx = static_cast<size_t>(
                std::discrete_distribution<size_t>(
                    adaptive.weights.begin(),
                    adaptive.weights.end())(rng));
            break;
        }
    }

    if (cfg.verbose) std::cerr << "\n";

    // Budget exhausted: push the final personal best regardless of push
    // strategy if it's better than the last elite we pushed, so the final
    // archive never misses it just because a threshold/segment never came.
    if (hooks && hooks->push_elite && result.cost() + TOLERANCE < last_pushed_cost) {
        hooks->push_elite(result);
        last_pushed_cost = result.cost();
    }

    // post_optimization stub (not implemented, matches Rust comment-out)
    double post_opt = 0.0, post_opt_elapsed = 0.0;

    logger.finalize(result, tabu_sz, reset_after, adap_its,
                    adaptive.segment, last_improved,
                    post_opt, post_opt_elapsed, total_evals);
    return result;
}
